// 分发任务：创建、查询、重试、取消、延长 + 后台调度
#include "admin_server.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <chrono>

#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#endif

using nlohmann::json;

static std::string distSha256(const std::string& path) {
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
    return std::string("no-openssl");
#endif
}

static long long nowMs() { return AdminDb::nowMs(); }

void AdminServer::handleDistCreate(const HttpContextPtr& ctx, const AdminActor& actor) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::string file = req.value("file", "");
    std::string sub = req.value("dir", "");
    std::vector<std::string> devids;
    if (req.contains("devids") && req["devids"].is_array()) {
        for (auto& v : req["devids"]) if (v.is_string()) devids.push_back(v.get<std::string>());
    }
    long long now = nowMs();
    long long start = req.value("start_at_ms", now);
    long long deadline = req.value("deadline_at_ms", start + 24LL * 3600 * 1000);
    int max_retries = req.value("max_retries", 3);
    if (file.empty() || file.find("..") != std::string::npos || file.find('/') != std::string::npos ||
        sub.size() > 128 || sub.find("..") != std::string::npos || (!sub.empty() && sub[0] == '/')) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file or dir\"}");
        return;
    }
    if (devids.empty() || devids.size() > 100) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"devids required (1-100)\"}");
        return;
    }
    if (deadline <= start || deadline - start > 30LL * 86400 * 1000) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad time window (max 30 days)\"}");
        return;
    }
    if (max_retries < 0 || max_retries > 10) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"max_retries 0-10\"}");
        return;
    }
    std::string path = cfg_.file_dir + "/" + file;
    struct stat st{};
    if (cfg_.file_dir.empty() || stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"file not found\"}");
        return;
    }
    std::string sha = distSha256(path);
    std::string batch = "b" + std::to_string(now) + "-" + AdminDb::randomHex(3);
    std::vector<long long> ids;
    std::string bad;
    for (auto& devid : devids) {
        bool online = callRelay([this, &devid]() {
            return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
        }) == "1";
        if (!online) { bad += (bad.empty() ? "" : ",") + devid; continue; }
        std::string base = callRelay([this, &devid]() { return relay_->fileDirOf(devid.c_str()); });
        std::string target = base.empty() ? ("/" + sub) : (base + (sub.empty() ? "" : "/" + sub));
        std::string status = (start <= now) ? "queued" : "scheduled";
        long long id = 0;
        if (!db_.distInsert(batch, devid, file, (long long)st.st_size, sha, sub, target,
                            status, start, deadline, max_retries, actor.username, &id)) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"db error\"}");
            return;
        }
        ids.push_back(id);
    }
    if (ids.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"all devices offline: " + bad + "\"}");
        return;
    }
    audit(actor.username, "distribution.create", batch + " file=" + file + " n=" + std::to_string(ids.size()));
    json out;
    out["batch_id"] = batch;
    out["tasks"] = ids;
    out["size"] = (long long)st.st_size;
    if (!bad.empty()) out["skipped_offline"] = bad;
    ctx->response->status_code = HTTP_STATUS_ACCEPTED;
    ctx->send(out.dump());
}

void AdminServer::handleDistList(const HttpContextPtr& ctx) {
    std::string status = ctx->param("status");
    std::string q = ctx->param("q");
    std::string batch = ctx->param("batch");
    int page = atoi(ctx->param("page").c_str());
    int per = atoi(ctx->param("per_page").c_str());
    if (page < 1) page = 1;
    if (per < 1 || per > 100) per = 20;
    std::string arr;
    int total = 0;
    db_.distList(status, q, batch, page, per, arr, total);
    ctx->send("{\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
              ",\"per_page\":" + std::to_string(per) + ",\"items\":" + arr + "}");
}

void AdminServer::handleDistSummary(const HttpContextPtr& ctx) {
    std::string j;
    if (!db_.distSummary(j)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send(j);
}

void AdminServer::handleDistGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string j;
    if (!db_.distGet(id, j)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    ctx->send(j);
}

void AdminServer::handleDistRetry(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string js;
    if (!db_.distGet(id, js)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto j = json::parse(js);
    long long now = nowMs();
    if (j.value("status", "") != "failed") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"only failed tasks can retry\"}");
        return;
    }
    if (now >= j.value("deadline_at_ms", 0LL)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"deadline passed, extend not available for failed\"}");
        return;
    }
    int retry = j.value("retry_count", 0) + 1;
    db_.distUpdate(id, "queued", "", j.value("sent", 0LL), 0, 0, 0, retry);
    audit(actor.username, "distribution.retry", std::to_string(id));
    ctx->send("{\"ok\":true,\"retry_count\":" + std::to_string(retry) + "}");
}

void AdminServer::handleDistCancel(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string js;
    if (!db_.distGet(id, js)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto j = json::parse(js);
    std::string st = j.value("status", "");
    if (st != "scheduled" && st != "queued") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"only scheduled/queued can cancel\"}");
        return;
    }
    db_.distUpdate(id, "canceled", "canceled", j.value("sent", 0LL), 0, nowMs(), 0,
                   j.value("retry_count", 0));
    audit(actor.username, "distribution.cancel", std::to_string(id));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDistExtend(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string js;
    if (!db_.distGet(id, js)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto j = json::parse(js);
    auto req = json::parse(ctx->body(), nullptr, false);
    long long nd = req.is_discarded() ? 0 : req.value("deadline_at_ms", 0LL);
    long long now = nowMs();
    long long old = j.value("deadline_at_ms", 0LL);
    long long start = j.value("start_at_ms", 0LL);
    if (j.value("status", "") != "timeout" || j.value("extend_count", 0) != 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"only timeout tasks can extend once\"}");
        return;
    }
    if (nd <= now || nd > old + 24LL * 3600 * 1000 || nd > start + 30LL * 86400 * 1000) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad new deadline\"}");
        return;
    }
    db_.distExtend(id, nd);
    audit(actor.username, "distribution.extend", std::to_string(id) + " -> " + std::to_string(nd));
    ctx->send("{\"ok\":true,\"deadline_at_ms\":" + std::to_string(nd) + "}");
}

// ---------------------------------------------------------------- 调度

void AdminServer::distLoop() {
    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        try { distTick(); } catch (...) { logf_("[dist] tick exception"); }
    }
}

void AdminServer::distTick() {
    std::string js;
    if (!db_.distDue(js)) return;
    auto arr = json::parse(js, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return;

    long long now = nowMs();
    dist_active.clear();
    dist_busy_dev.clear();
    for (auto& j : arr) {
        if (j.value("status", "") == "running") {
            long long id = j["id"].get<long long>();
            dist_active[id] = (uint32_t)j.value("relay_tid", 0);
            dist_busy_dev.insert(j.value("devid", std::string()));
        }
    }
    int running = (int)dist_active.size();

    auto finishRetry = [&](const json& j, const std::string& err, long long sent, const char* statusNow) {
        long long id = j["id"].get<long long>();
        int retry = j.value("retry_count", 0);
        int maxr = j.value("max_retries", 3);
        long long deadline = j.value("deadline_at_ms", 0LL);
        int nr = retry + 1;
        long long backoff = nr == 1 ? 30000 : (nr == 2 ? 60000 : 120000);
        if (std::string(statusNow) == "timeout" || now >= deadline) {
            db_.distUpdate(id, "timeout", err, sent, 0, now, 0, retry);
        } else if (nr <= maxr && now + backoff < deadline) {
            db_.distUpdate(id, "queued", err, sent, 0, 0, now + backoff, nr);
        } else {
            db_.distUpdate(id, "failed", err, sent, 0, now, 0, nr);
        }
    };

    for (auto& j : arr) {
        std::string st = j.value("status", "");
        long long id = j["id"].get<long long>();
        long long deadline = j.value("deadline_at_ms", 0LL);
        long long sent = j.value("sent", 0LL);
        std::string devid = j.value("devid", std::string());

        if (st == "running") {
            uint32_t tid = (uint32_t)j.value("relay_tid", 0);
            std::string p = callRelay([this, tid]() { return relay_->pushProgress(tid); });
            auto pg = json::parse(p, nullptr, false);
            if (!pg.is_discarded()) {
                sent = pg.value("sent", sent);
                db_.distProgress(id, sent);
                if (pg.value("done", false)) {
                    if (pg.value("ok", false)) {
                        db_.distUpdate(id, "success", "", j.value("file_size", 0LL), 0, now, 0,
                                       j.value("retry_count", 0));
                        dist_active.erase(id);
                        dist_busy_dev.erase(devid);
                        running--;
                        continue;
                    }
                    std::string err = pg.value("error", std::string("transfer failed"));
                    callRelay([this, tid]() { return relay_->cancelFilePush(tid) ? "1" : "0"; });
                    dist_active.erase(id);
                    dist_busy_dev.erase(devid);
                    running--;
                    finishRetry(j, err, sent, "failed");
                    continue;
                }
            }
            if (now >= deadline) {
                callRelay([this, tid]() { return relay_->cancelFilePush(tid) ? "1" : "0"; });
                dist_active.erase(id);
                dist_busy_dev.erase(devid);
                running--;
                finishRetry(j, "deadline exceeded", sent, "timeout");
            }
            continue;
        }

        if (st == "scheduled") {
            if (now >= j.value("start_at_ms", 0LL)) {
                db_.distUpdate(id, "queued", "", sent, 0, 0, 0, j.value("retry_count", 0));
            }
            continue;
        }

        if (st == "queued") {
            long long nextRetry = j.value("next_retry_ms", 0LL);
            if (now < nextRetry) continue;
            if (now >= deadline) {
                db_.distUpdate(id, "timeout", "deadline exceeded", sent, 0, now, 0,
                               j.value("retry_count", 0));
                continue;
            }
            if (running >= 3 || dist_busy_dev.count(devid)) continue;
            bool online = callRelay([this, &devid]() {
                return relay_->agentOnline(devid.c_str()) ? "1" : "0";
            }) == "1";
            if (!online) { finishRetry(j, "device offline", sent, "failed"); continue; }

            std::string path = cfg_.file_dir + "/" + j.value("file_name", std::string());
            std::string sub = j.value("sub_dir", std::string());
            uint32_t tid = 0;
            std::string err;
            std::string ok = callRelay([this, &devid, &path, &sub, &j, &err, &tid]() {
                return relay_->startFilePush(devid.c_str(), path.c_str(), sub.c_str(),
                        j.value("file_name", std::string()).c_str(),
                        (uint64_t)j.value("file_size", 0LL), j.value("file_sha", std::string()),
                        err, tid) ? "1" : "0";
            });
            if (ok != "1") { finishRetry(j, err.empty() ? "start failed" : err, sent, "failed"); continue; }
            db_.distUpdate(id, "running", "", sent, tid, 0, 0, j.value("retry_count", 0));
            dist_active[id] = tid;
            dist_busy_dev.insert(devid);
            running++;
        }
    }
}
