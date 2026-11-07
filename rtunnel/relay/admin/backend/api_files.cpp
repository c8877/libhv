// 文件管理：上传(raw body)/下载/列表(排序)/删除。
// 文件是 admin 自有资源（非 relay 状态），IO 直接在 admin worker 线程完成；
// 审计照记。文件名白名单防目录穿越，写文件 tmp+rename 原子替换。
#include "admin_server.hpp"
#include "hash_util.hpp"
#include "cpputil/json.hpp"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <vector>

using nlohmann::json;


struct FileEnt {
    std::string name;
    uint64_t    size;
    uint64_t    mtime_ms;
};

static bool listDir(const std::string& dir, std::vector<FileEnt>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* de;
    while ((de = readdir(d)) != NULL) {
        std::string name = de->d_name;
        if (!adminValidFileName(name)) continue;
        struct stat st;
        std::string path = dir + "/" + name;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        out.push_back({name, (uint64_t)st.st_size, (uint64_t)st.st_mtime * 1000});
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
    std::string sort = ctx->param("sort", "name");
    std::string order = ctx->param("order", "asc");
    std::vector<FileEnt> files;
    listDir(cfg_.file_dir, files);
    auto cmp = [&](const FileEnt& a, const FileEnt& b) {
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
        j["size"] = f.size;
        j["mtime_ms"] = f.mtime_ms;
        std::string sha;
        if (!db_.fileMetaGet(f.name, (long long)f.size, (long long)f.mtime_ms, sha)) {
            sha = adminSha256File(cfg_.file_dir + "/" + f.name);
            if (!sha.empty()) db_.fileMetaPut(f.name, (long long)f.size, (long long)f.mtime_ms, sha);
        }
        j["sha256"] = sha;
        arr.push_back(j);
    }
    ctx->send(arr.dump());
}

void AdminServer::handleFileDownload(const HttpContextPtr& ctx) {
    std::string name = adminUrlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !adminValidFileName(name)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file name\"}");
        return;
    }
    std::string path = cfg_.file_dir + "/" + name;
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

void AdminServer::handleFileDelete(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = adminUrlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !adminValidFileName(name)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file name\"}");
        return;
    }
    std::string path = cfg_.file_dir + "/" + name;
    if (unlink(path.c_str()) != 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    audit(role, "files.delete", name);
    json j;
    j["name"] = name;
    j["deleted"] = true;
    ctx->send(j.dump());
}
