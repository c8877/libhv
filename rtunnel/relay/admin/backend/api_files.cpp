// 文件管理：上传(raw body)/下载/列表(排序)/删除。
// 文件是 admin 自有资源（非 relay 状态），IO 直接在 admin worker 线程完成；
// 审计照记。文件名白名单防目录穿越，写文件 tmp+rename 原子替换。
#include "admin_server.hpp"
#include "cpputil/hstring.h"
#include "hash_util.hpp"
#include "proxy_util.hpp"
#include "cpputil/json.hpp"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <vector>

using nlohmann::json;


struct FileEnt {
    std::string name;
    std::string rel;      // 相对仓库根的路径（不含前导 /）
    uint64_t    size;
    uint64_t    mtime_ms;
    bool        dir;
};

static void walkDir(const std::string& dir, const std::string& rel, const std::string& needle,
                    std::vector<FileEnt>& out, int& budget) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* de;
    while ((de = readdir(d)) != NULL && budget > 0) {
        std::string name = de->d_name;
        if (!adminValidFileName(name)) continue;
        std::string path = dir + "/" + name;
        struct stat st{};
        if (lstat(path.c_str(), &st) != 0) continue;
        bool isDir = S_ISDIR(st.st_mode);
        if (!isDir && !S_ISREG(st.st_mode)) continue;
        std::string child = rel.empty() ? name : rel + "/" + name;
        std::string low = name;
        hv::tolower(low);
        if (low.find(needle) != std::string::npos) {
            out.push_back({name, child, isDir ? 0 : (uint64_t)st.st_size,
                           (uint64_t)st.st_mtime * 1000, isDir});
            budget--;
        }
        if (isDir) walkDir(path, child, needle, out, budget);
    }
    closedir(d);
}

static bool listDir(const std::string& dir, const std::string& rel, std::vector<FileEnt>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        std::string name = de->d_name;
        if (!adminValidFileName(name)) continue;
        struct stat st;
        std::string path = dir + "/" + name;
        // 不跟随符号链接，避免仓库里被放入指向根目录之外的链接
        if (lstat(path.c_str(), &st) != 0) continue;
        bool isDir = S_ISDIR(st.st_mode);
        if (!isDir && !S_ISREG(st.st_mode)) continue;
        std::string child = rel.empty() ? name : rel + "/" + name;
        out.push_back({name, child, isDir ? 0 : (uint64_t)st.st_size,
                       (uint64_t)st.st_mtime * 1000, isDir});
    }
    closedir(d);
    return true;
}

void AdminServer::handleFiles(const HttpContextPtr& ctx) {
    if (cfg_.file_dir.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"--admin-file-dir not configured\"}");
        return;
    }
    std::string dir = urlDecode(ctx->param("dir"));
    std::string abs;
    if (!adminRepoPath(cfg_.file_dir, dir, abs)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad dir\"}");
        return;
    }
    std::string rel = dir;
    if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    std::string sort = ctx->param("sort", "name");
    std::string order = ctx->param("order", "asc");
    std::string q = ctx->param("q");
    std::vector<FileEnt> files;
    // 递归搜索：按名称匹配子目录中的条目，最多返回 500 条
    if (!q.empty() && ctx->param("recursive") == "1") {
        std::string needle = q;
        hv::tolower(needle);
        int budget = 500;
        walkDir(abs, rel, needle, files, budget);
        std::sort(files.begin(), files.end(), [](const FileEnt& a, const FileEnt& b) {
            if (a.dir != b.dir) return a.dir;
            return a.rel < b.rel;
        });
        json arr = json::array();
        for (const auto& f : files) {
            json j;
            j["name"] = f.name;
            j["path"] = "/" + f.rel;
            j["type"] = f.dir ? "dir" : "file";
            j["size"] = f.size;
            j["mtime_ms"] = f.mtime_ms;
            j["sha256"] = "";
            arr.push_back(j);
        }
        ctx->send(arr.dump());
        return;
    }
    if (!listDir(abs, rel, files)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto cmp = [&](const FileEnt& a, const FileEnt& b) {
        if (a.dir != b.dir) return a.dir;   // 目录始终排在文件前
        bool r;
        if (sort == "size") r = a.size < b.size;
        else if (sort == "mtime") r = a.mtime_ms < b.mtime_ms;
        else r = a.name < b.name;
        return order == "desc" ? !r : r;
    };
    std::sort(files.begin(), files.end(), cmp);

    json arr = json::array();
    for (const auto& f : files) {
        json j;
        j["name"] = f.name;
        j["type"] = f.dir ? "dir" : "file";
        j["size"] = f.size;
        j["mtime_ms"] = f.mtime_ms;
        std::string sha;
        if (!f.dir) {
            if (!db_.fileMetaGet(f.rel, (long long)f.size, (long long)f.mtime_ms, sha)) {
                sha = adminSha256File(abs + "/" + f.name);
                if (!sha.empty()) db_.fileMetaPut(f.rel, (long long)f.size, (long long)f.mtime_ms, sha);
            }
        }
        j["sha256"] = sha;
        arr.push_back(j);
    }
    ctx->send(arr.dump());
}

void AdminServer::handleFileDownload(const HttpContextPtr& ctx) {
    std::string name = urlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !adminValidFileName(name)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file name\"}");
        return;
    }
    std::string dir = urlDecode(ctx->param("dir"));
    std::string rel, path;
    if (!adminRepoPath(cfg_.file_dir, dir, rel)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad dir\"}");
        return;
    }
    path = rel + "/" + name;
    struct stat lst{};
    if (lstat(path.c_str(), &lst) != 0 || !S_ISREG(lst.st_mode)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    size_t max_dl = (size_t)settingInt("file.max_download_mb", 32) * 1024 * 1024;
    if (n < 0 || (size_t)n > max_dl) {
        fclose(fp);
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"file too large\"}");
        return;
    }
    ctx->response->content_type = APPLICATION_OCTET_STREAM;
    ctx->response->headers["Content-Disposition"] = "attachment; filename=\"" + name + "\"";
    hv::HttpResponseWriter* w = ctx->writer.get();
    if (!w) {
        fclose(fp);
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"no writer\"}");
        return;
    }

    // 单段 Range，支持断点续传
    long long start = 0, end = (long long)n - 1;
    bool partial = false;
    std::string rh = ctx->header("Range");
    if (!rh.empty() && rh.rfind("bytes=", 0) == 0) {
        std::string spec = rh.substr(6);
        size_t dash = spec.find('-');
        if (dash == std::string::npos) {
            fclose(fp);
            ctx->response->status_code = HTTP_STATUS_RANGE_NOT_SATISFIABLE;
            ctx->send("{\"error\":\"bad range\"}");
            return;
        }
        std::string a = spec.substr(0, dash), b = spec.substr(dash + 1);
        if (a.empty()) {
            long long suffix = atoll(b.c_str());
            if (suffix <= 0) {
                fclose(fp);
                ctx->response->status_code = HTTP_STATUS_RANGE_NOT_SATISFIABLE;
                ctx->send("{\"error\":\"bad range\"}");
                return;
            }
            start = n > suffix ? (long long)n - suffix : 0;
        } else {
            start = atoll(a.c_str());
            if (!b.empty()) end = atoll(b.c_str());
        }
        if (start < 0 || start >= n || end < start) {
            fclose(fp);
            ctx->response->status_code = HTTP_STATUS_RANGE_NOT_SATISFIABLE;
            ctx->send("{\"error\":\"range not satisfiable\"}");
            return;
        }
        if (end >= n) end = (long long)n - 1;
        partial = true;
    }

    long long len = end - start + 1;
    w->Begin();
    w->WriteStatus(partial ? HTTP_STATUS_PARTIAL_CONTENT : HTTP_STATUS_OK);
    w->WriteHeader("Accept-Ranges", "bytes");
    if (partial) {
        std::string cr = "bytes " + std::to_string(start) + "-" + std::to_string(end) +
                         "/" + std::to_string(n);
        w->WriteHeader("Content-Range", cr.c_str());
    }
    w->EndHeaders("Content-Length", len);
    fseek(fp, (long)start, SEEK_SET);
    char buf[64 * 1024];
    long long left = len;
    while (left > 0) {
        size_t want = left > (long long)sizeof(buf) ? sizeof(buf) : (size_t)left;
        size_t got = fread(buf, 1, want, fp);
        if (got == 0) break;
        if (w->WriteBody(buf, (int)got) < 0) break;
        left -= (long long)got;
    }
    fclose(fp);
    w->End();
}

void AdminServer::handleFilePut(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = urlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !adminValidFileName(name)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file name\"}");
        return;
    }
    const std::string& body = ctx->body();
    size_t max_edit = (size_t)settingInt("file.max_edit_mb", 4) * 1024 * 1024;
    if (body.size() > max_edit) {
        ctx->response->status_code = HTTP_STATUS_PAYLOAD_TOO_LARGE;
        ctx->send("{\"error\":\"file too large for inline edit (max " +
                  std::to_string(settingInt("file.max_edit_mb", 4)) + "MiB)\"}");
        return;
    }
    mkdir(cfg_.file_dir.c_str(), 0700);
    std::string dir = urlDecode(ctx->param("dir"));
    std::string base, path;
    if (!adminRepoPath(cfg_.file_dir, dir, base)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad dir\"}");
        return;
    }
    path = base + "/" + name;
    struct stat pst{};
    if (stat(base.c_str(), &pst) != 0 || !S_ISDIR(pst.st_mode)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"目录不存在\"}");
        return;
    }
    // 写同目录隐藏临时文件，写完 fsync 再 rename，避免半截文件
    std::string tmp = base + "/." + name + ".edit";
    FILE* fp = fopen(tmp.c_str(), "wb");
    if (!fp) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"write failed\"}");
        return;
    }
    size_t written = body.empty() ? 0 : fwrite(body.data(), 1, body.size(), fp);
    if (written != body.size() || fflush(fp) != 0 || fsync(fileno(fp)) != 0) {
        fclose(fp);
        unlink(tmp.c_str());
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"write failed\"}");
        return;
    }
    fclose(fp);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"rename failed\"}");
        return;
    }
    struct stat st{};
    long long sz = (stat(path.c_str(), &st) == 0) ? (long long)st.st_size : 0;
    long long mtime_ms = (stat(path.c_str(), &st) == 0) ? (long long)st.st_mtime * 1000 : 0;
    std::string sha = adminSha256File(path);
    std::string metaKey = (dir.empty() || dir == "/") ? name : (dir + "/" + name);
    if (!metaKey.empty() && metaKey[0] == '/') metaKey.erase(0, 1);
    if (!sha.empty()) db_.fileMetaPut(metaKey, sz, mtime_ms, sha);
    audit(role, "files.edit", metaKey + " size=" + std::to_string(body.size()));
    json j;
    j["name"] = name;
    j["size"] = sz;
    j["mtime_ms"] = mtime_ms;
    j["sha256"] = sha;
    ctx->send(j.dump());
}

// 递归删除；不跟随符号链接
static bool removeTree(const std::string& path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str()) == 0;
    DIR* d = opendir(path.c_str());
    if (!d) return false;
    struct dirent* de;
    bool ok = true;
    while ((de = readdir(d)) != NULL) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        if (!removeTree(path + "/" + n)) ok = false;
    }
    closedir(d);
    if (!ok) return false;
    return rmdir(path.c_str()) == 0;
}

void AdminServer::handleFileDelete(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = urlDecode(ctx->param("name"));
    std::string dir = urlDecode(ctx->param("dir"));
    if (cfg_.file_dir.empty() || !adminValidFileName(name)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file name\"}");
        return;
    }
    std::string base, path;
    if (!adminRepoPath(cfg_.file_dir, dir, base)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad dir\"}");
        return;
    }
    path = base + "/" + name;
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    bool isDir = S_ISDIR(st.st_mode);
    if (isDir && ctx->param("recursive") != "1") {
        ctx->response->status_code = HTTP_STATUS_CONFLICT;
        ctx->send("{\"error\":\"目录需要 recursive=1 才能删除\"}");
        return;
    }
    if (!removeTree(path)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"delete failed\"}");
        return;
    }
    std::string rel = dir;
    if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    std::string metaKey = rel.empty() ? name : (rel + "/" + name);
    audit(role, "files.delete", metaKey);
    json j;
    j["name"] = name;
    j["type"] = isDir ? "dir" : "file";
    j["deleted"] = true;
    ctx->send(j.dump());
}

void AdminServer::handleFileMkdir(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = urlDecode(ctx->param("name"));
    std::string dir = urlDecode(ctx->param("dir"));
    if (cfg_.file_dir.empty() || !adminValidFileName(name)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad dir name\"}");
        return;
    }
    std::string base, path;
    if (!adminRepoPath(cfg_.file_dir, dir, base)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad dir\"}");
        return;
    }
    path = base + "/" + name;
    if (mkdir(path.c_str(), 0755) != 0) {
        ctx->response->status_code = HTTP_STATUS_CONFLICT;
        ctx->send("{\"error\":\"目录已存在或无法创建\"}");
        return;
    }
    std::string rel = dir;
    if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
    std::string relNew = rel.empty() ? name : (rel + "/" + name);
    audit(role, "files.mkdir", relNew);
    json j;
    j["name"] = name;
    j["type"] = "dir";
    j["path"] = "/" + relNew;
    ctx->send(j.dump());
}

void AdminServer::handleFileMove(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = urlDecode(ctx->param("name"));
    std::string dir = urlDecode(ctx->param("dir"));
    std::string to = urlDecode(ctx->param("to"));
    if (cfg_.file_dir.empty() || !adminValidFileName(name) || to.empty() ||
        !adminValidRepoPath(to)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad source or target\"}");
        return;
    }
    std::string fromBase, from, toPath;
    if (!adminRepoPath(cfg_.file_dir, dir, fromBase) ||
        !adminRepoPath(cfg_.file_dir, to, toPath)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad source or target\"}");
        return;
    }
    from = fromBase + "/" + name;
    struct stat fromSt{};
    if (lstat(from.c_str(), &fromSt) != 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"源不存在\"}");
        return;
    }
    struct stat dstSt{};
    if (lstat(toPath.c_str(), &dstSt) == 0) {
        ctx->response->status_code = HTTP_STATUS_CONFLICT;
        ctx->send("{\"error\":\"目标已存在\"}");
        return;
    }
    // 目录不能移动到自身内部
    if (S_ISDIR(fromSt.st_mode) && toPath.compare(0, from.size() + 1, from + "/") == 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"不能移动到自身内部\"}");
        return;
    }
    std::string parent = toPath.substr(0, toPath.rfind('/'));
    struct stat pst{};
    if (parent.empty() || stat(parent.c_str(), &pst) != 0 || !S_ISDIR(pst.st_mode)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"目标目录不存在\"}");
        return;
    }
    if (rename(from.c_str(), toPath.c_str()) != 0) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"移动失败\"}");
        return;
    }
    std::string relTo = to;
    if (!relTo.empty() && relTo[0] == '/') relTo.erase(0, 1);
    audit(role, "files.move", std::string(fromBase) + "/" + name + " -> " + relTo);
    size_t slash = relTo.rfind('/');
    json j;
    j["name"] = slash == std::string::npos ? relTo : relTo.substr(slash + 1);
    j["path"] = "/" + relTo;
    ctx->send(j.dump());
}
