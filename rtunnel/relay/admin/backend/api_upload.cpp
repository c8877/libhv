// 文件上传：分片、状态、整包，及 .uploading 清理
#include "admin_server.hpp"
#include "hash_util.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctime>

using nlohmann::json;

void AdminServer::handleFileUpload(const HttpContextPtr& ctx, const std::string& role) {
    std::string name = adminUrlDecode(ctx->param("name"));
    if (cfg_.file_dir.empty() || !adminValidFileName(name)) {
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
        std::string sha = adminSha256File(path);
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
    std::string name = adminUrlDecode(ctx->param("name"));
    std::string uid = ctx->param("upload");
    if (cfg_.file_dir.empty() || !adminValidFileName(name) || uid.empty() ||
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


void AdminServer::cleanupUploads() {
    if (cfg_.file_dir.empty()) return;
    DIR* d = opendir(cfg_.file_dir.c_str());
    if (!d) return;
    time_t cutoff = time(NULL) - 3600;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string n = e->d_name;
        if (n.size() < 11 || n.compare(n.size() - 10, 10, ".uploading") != 0) continue;
        std::string path = cfg_.file_dir + "/" + n;
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && st.st_mtime < cutoff) unlink(path.c_str());
    }
    closedir(d);
}
