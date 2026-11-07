// 文件管理：上传(raw body)/下载/列表(排序)/删除。
// 文件是 admin 自有资源（非 relay 状态），IO 直接在 admin worker 线程完成；
// 审计照记。文件名白名单防目录穿越，写文件 tmp+rename 原子替换。
#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <vector>

using nlohmann::json;

// 百分号解码：浏览器会对文件名做 encodeURIComponent（中文/空格等）
static std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() &&
            isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            auto hex = [](char c) {
                return (c <= '9') ? (c - '0') : (tolower((unsigned char)c) - 'a' + 10);
            };
            out.push_back((char)((hex(in[i + 1]) << 4) | hex(in[i + 2])));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
}

// 文件名白名单：禁止 / \ .. 开头点 和控制字符，允许 UTF-8（中文/空格等），最长 128 字节
static bool validFileName(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    if (s[0] == '.') return false;
    if (s.find("..") != std::string::npos) return false;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '/' || c == '\\') return false;
    }
    return true;
}


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
        if (!validFileName(name)) continue;
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
        arr.push_back(j);
    }
    ctx->send(arr.dump());
}

void AdminServer::handleFileDownload(const HttpContextPtr& ctx) {
    std::string name = urlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !validFileName(name)) {
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
    std::string body;
    body.resize(n);
    if (n > 0 && fread(&body[0], 1, n, fp) != (size_t)n) body.clear();
    fclose(fp);
    ctx->response->content_type = APPLICATION_OCTET_STREAM;
    ctx->response->headers["Content-Disposition"] = "attachment; filename=\"" + name + "\"";
    ctx->response->body = body;
}

void AdminServer::handleFileUpload(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = urlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !validFileName(name)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file name\"}");
        return;
    }
    // 先看 Content-Length，避免把超限文件读进内存后才报错
    std::string clen = ctx->header("Content-Length");
    size_t max_ul = (size_t)settingInt("file.max_upload_mb", 32) * 1024 * 1024;
    if (!clen.empty() && strtoull(clen.c_str(), NULL, 10) > max_ul) {
        ctx->response->status_code = HTTP_STATUS_PAYLOAD_TOO_LARGE;
        ctx->send("{\"error\":\"file too large (max 32MiB)\"}");
        return;
    }
    const std::string& body = ctx->body();
    if (body.size() > max_ul) {
        ctx->response->status_code = HTTP_STATUS_PAYLOAD_TOO_LARGE;
        ctx->send("{\"error\":\"file too large (max 32MiB)\"}");
        return;
    }
    mkdir(cfg_.file_dir.c_str(), 0700);
    std::string path = cfg_.file_dir + "/" + name;
    std::string tmp = path + ".uploading";
    FILE* fp = fopen(tmp.c_str(), "wb");
    if (!fp) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"write failed\"}");
        return;
    }
    size_t written = body.empty() ? 0 : fwrite(body.data(), 1, body.size(), fp);
    fclose(fp);
    if (written != body.size()) {
        unlink(tmp.c_str());
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"short write\"}");
        return;
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"rename failed\"}");
        return;
    }
    audit(role, "files.upload", name + " size=" + std::to_string(body.size()));
    json j;
    j["name"] = name;
    j["size"] = body.size();
    ctx->send(j.dump());
}

void AdminServer::handleFileDelete(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = urlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !validFileName(name)) {
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
