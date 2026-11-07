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
#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#endif
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


static std::string fileSha256(const std::string& path) {
#if defined(WITH_OPENSSL)
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return std::string();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) EVP_DigestUpdate(ctx, buf, n);
    unsigned char md[32];
    unsigned int mdlen = 0;
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    EVP_MD_CTX_free(ctx);
    fclose(fp);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < mdlen; ++i) { out += hex[md[i] >> 4]; out += hex[md[i] & 0xF]; }
    return out;
#else
    (void)path;
    return std::string();
#endif
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
        std::string sha;
        if (!db_.fileMetaGet(f.name, (long long)f.size, (long long)f.mtime_ms, sha)) {
            sha = fileSha256(cfg_.file_dir + "/" + f.name);
            if (!sha.empty()) db_.fileMetaPut(f.name, (long long)f.size, (long long)f.mtime_ms, sha);
        }
        j["sha256"] = sha;
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

void AdminServer::handleFileUpload(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = urlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !validFileName(name)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file name\"}");
        return;
    }
    size_t max_ul = (size_t)settingInt("file.max_upload_mb", 32) * 1024 * 1024;
    mkdir(cfg_.file_dir.c_str(), 0700);
    std::string path = cfg_.file_dir + "/" + name;
    std::string uid = ctx->param("upload");
    if (!uid.empty() &&
        (uid.size() > 64 || uid.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad upload id\"}");
        return;
    }
    bool chunked_req = (ctx->param("append") == "1" || ctx->param("finalize") == "1");
    if (chunked_req && uid.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"missing upload id\"}");
        return;
    }
    std::string tmp = path + (uid.empty() ? ".uploading" : "." + uid + ".uploading");

    // 分片上传：?append=1&offset=N 追加到临时文件；?finalize=1 落盘
    if (ctx->param("finalize") == "1") {
        if (rename(tmp.c_str(), path.c_str()) != 0) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"no upload in progress\"}");
            return;
        }
        struct stat st{};
        long long sz = (stat(path.c_str(), &st) == 0) ? (long long)st.st_size : 0;
        std::string sha = fileSha256(path);
        if (!sha.empty()) db_.fileMetaPut(name, sz, (long long)st.st_mtime * 1000, sha);
        ctx->send("{\"name\":\"" + name + "\",\"size\":" + std::to_string(sz) +
                  ",\"sha256\":\"" + sha + "\"}");
        return;
    }
    if (ctx->param("append") == "1") {
        size_t off = (size_t)strtoull(ctx->param("offset", "0").c_str(), NULL, 10);
        const std::string& body = ctx->body();
        if (off + body.size() > max_ul) {
            unlink(tmp.c_str());
            ctx->response->status_code = HTTP_STATUS_PAYLOAD_TOO_LARGE;
            ctx->send("{\"error\":\"file too large (max " +
                      std::to_string(settingInt("file.max_upload_mb", 32)) + "MiB)\"}");
            return;
        }
        FILE* fp = fopen(tmp.c_str(), off ? "r+b" : "wb");
        if (!fp) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"write failed\"}");
            return;
        }
        if (off) {
            fseek(fp, 0, SEEK_END);
            if ((size_t)ftell(fp) != off) {
                fclose(fp);
                unlink(tmp.c_str());
                ctx->response->status_code = HTTP_STATUS_CONFLICT;
                ctx->send("{\"error\":\"offset mismatch\"}");
                return;
            }
        }
        size_t written = body.empty() ? 0 : fwrite(body.data(), 1, body.size(), fp);
        long long sz = (long long)ftell(fp);
        fclose(fp);
        if (written != body.size()) {
            unlink(tmp.c_str());
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"short write\"}");
            return;
        }
        ctx->send("{\"name\":\"" + name + "\",\"received\":" + std::to_string(sz) + "}");
        return;
    }

    std::string clen = ctx->header("Content-Length");
    if (!clen.empty() && strtoull(clen.c_str(), NULL, 10) > max_ul) {
        ctx->response->status_code = HTTP_STATUS_PAYLOAD_TOO_LARGE;
        ctx->send("{\"error\":\"file too large (max " +
                  std::to_string(settingInt("file.max_upload_mb", 32)) + "MiB)\"}");
        return;
    }
    const std::string& body = ctx->body();
    const size_t MAX_SINGLE = 8 * 1024 * 1024;
    if (body.size() > MAX_SINGLE) {
        ctx->response->status_code = HTTP_STATUS_PAYLOAD_TOO_LARGE;
        ctx->send("{\"error\":\"use chunked upload for files > 8MiB\"}");
        return;
    }
    if (body.size() > max_ul) {
        ctx->response->status_code = HTTP_STATUS_PAYLOAD_TOO_LARGE;
        ctx->send("{\"error\":\"file too large (max " +
                  std::to_string(settingInt("file.max_upload_mb", 32)) + "MiB)\"}");
        return;
    }
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

void AdminServer::handleUploadStatus(const HttpContextPtr& ctx) {
    std::string name = urlDecode(ctx->param("name"));
    std::string uid = ctx->param("upload");
    if (cfg_.file_dir.empty() || !validFileName(name) || uid.empty() ||
        uid.size() > 64 || uid.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad request\"}");
        return;
    }
    struct stat st{};
    long long sz = 0;
    std::string tmp = cfg_.file_dir + "/" + name + "." + uid + ".uploading";
    if (stat(tmp.c_str(), &st) == 0) sz = (long long)st.st_size;
    ctx->send("{\"received\":" + std::to_string(sz) + "}");
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
