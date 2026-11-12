// agent OTA：任务创建/查询/重试/取消 + 后台调度
#include "admin_server.hpp"
#include "hash_util.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <sys/stat.h>
#include <unistd.h>

using nlohmann::json;

static long long nowMs() { return AdminDb::nowMs(); }

static bool otaVersionValid(const std::string& v) {
    if (v.empty() || v.size() > 40) return false;
    for (char c : v) {
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+')) return false;
    }
    return true;
}

// ---------------------------------------------------------------- API

void AdminServer::handleOtaCreate(const HttpContextPtr& ctx, const AdminActor& actor) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::string file = req.value("file", "");
    std::string version = req.value("version", "");
    std::vector<std::string> devids;
    if (req.contains("devids") && req["devids"].is_array()) {
        for (auto& v : req["devids"]) if (v.is_string()) devids.push_back(v.get<std::string>());
    }
    long long now = nowMs();
    long long deadline = req.value("deadline_at_ms", now + 3600LL * 1000);
    int max_retries = req.value("max_retries", 2);
    int force = req.value("force", false) ? 1 : 0;
    if (file.empty() || file.find("..") != std::string::npos || file.find('/') != std::string::npos) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file\"}");
        return;
    }
    if (!otaVersionValid(version)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad version\"}");
        return;
    }
    if (devids.empty() || devids.size() > 200) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"devids required (1-200)\"}");
        return;
    }
    if (deadline <= now || deadline - now > 7LL * 86400 * 1000) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad deadline (max 7 days)\"}");
        return;
    }
    if (max_retries < 0 || max_retries > 5) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"max_retries 0-5\"}");
        return;
    }
    std::string path = cfg_.file_dir + "/" + file;
    struct stat st{};
    if (cfg_.file_dir.empty() || stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"file not found\"}");
        return;
    }
    if (st.st_size > 64LL * 1024 * 1024) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"agent binary too large (max 64MB)\"}");
        return;
    }
    std::string sha = adminSha256File(path);
    std::string batch = "o" + std::to_string(now) + "-" + AdminDb::randomHex(3);
    std::vector<long long> ids;
    std::string unsupported;
    for (auto& devid : devids) {
        int cap = atoi(callRelay([this, &devid]() {
            return std::to_string(relay_->agentOtaCap(devid.c_str()));
        }).c_str());
        std::string status = cap == 0 ? "unsupported" : "queued";
        long long id = 0;
        if (!db_.otaInsert(batch, devid, version, file, (long long)st.st_size, sha, status,
                           deadline, max_retries, actor.username, force, &id)) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"db error\"}");
            return;
        }
        if (cap == 0) {
            db_.otaUpdate(id, "unsupported", "agent does not support ota", 0, 0, now, 0, 0);
            unsupported += (unsupported.empty() ? "" : ",") + devid;
        }
        ids.push_back(id);
    }
    audit(actor.username, "ota.create",
          batch + " version=" + version + " file=" + file + " n=" + std::to_string(ids.size()));
    json out;
    out["batch_id"] = batch;
    out["tasks"] = ids;
    out["size"] = (long long)st.st_size;
    if (!unsupported.empty()) out["unsupported"] = unsupported;
    ctx->response->status_code = HTTP_STATUS_ACCEPTED;
    ctx->send(out.dump());
}

void AdminServer::handleOtaList(const HttpContextPtr& ctx) {
    std::string status = ctx->param("status");
    std::string q = ctx->param("q");
    std::string batch = ctx->param("batch");
    int page = atoi(ctx->param("page").c_str());
    int per = atoi(ctx->param("per_page").c_str());
    if (page < 1) page = 1;
    if (per < 1 || per > 100) per = 20;
    std::string arr;
    int total = 0;
    if (!db_.otaList(status, q, batch, page, per, arr, total)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send("{\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
              ",\"per_page\":" + std::to_string(per) + ",\"items\":" + arr + "}");
}

void AdminServer::handleOtaSummary(const HttpContextPtr& ctx) {
    std::string j;
    if (!db_.otaSummary(j)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send(j);
}

void AdminServer::handleOtaGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string j;
    if (!db_.otaGet(id, j)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    ctx->send(j);
}

void AdminServer::handleOtaRetry(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string j;
    if (!db_.otaGet(id, j)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    if (!db_.otaReset(id)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    audit(actor.username, "ota.retry", std::to_string(id));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleOtaCancel(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string j;
    if (!db_.otaGet(id, j)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto t = json::parse(j, nullptr, false);
    std::string status = t.is_discarded() ? "" : t.value("status", "");
    uint32_t tid = (uint32_t)(t.is_discarded() ? 0 : t.value("relay_tid", 0LL));
    if (status == "running" && tid) {
        callRelay([this, tid]() { return relay_->cancelOtaPush(tid) ? "1" : "0"; });
    }
    db_.otaUpdate(id, "canceled", "canceled by " + actor.username,
                  t.is_discarded() ? 0 : t.value("sent", 0LL), 0, nowMs(), 0,
                  t.is_discarded() ? 0 : t.value("retry_count", 0));
    audit(actor.username, "ota.cancel", std::to_string(id));
    ctx->send("{\"ok\":true}");
}

// ---------------------------------------------------------------- 调度

void AdminServer::otaLoop() {
    while (!waitStop(1000)) {
        try { otaTick(); } catch (...) { logf_("[ota] tick exception"); }
    }
}

void AdminServer::otaTick() {
    std::string js;
    if (!db_.otaDue(js)) return;
    auto arr = json::parse(js, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return;

    long long now = nowMs();
    int maxc = settingInt("ota.max_concurrent", 1);
    if (maxc < 1) maxc = 1;
    if (maxc > 32) maxc = 32;
    int apply_timeout_ms = settingInt("ota.apply_timeout_s", 180) * 1000;
    if (apply_timeout_ms < 30000) apply_timeout_ms = 30000;

    ota_active.clear();
    ota_busy_dev.clear();
    int running = 0;
    for (auto& j : arr) {
        std::string st = j.value("status", "");
        if (st == "running" || st == "applying") {
            ota_active[j["id"].get<long long>()] = (uint32_t)j.value("relay_tid", 0);
            ota_busy_dev.insert(j.value("devid", std::string()));
            if (st == "running") running++;
        }
    }

    auto finishRetry = [&](const json& j, const std::string& err, long long sent, const char* statusNow) {
        long long id = j["id"].get<long long>();
        int retry = j.value("retry_count", 0);
        int maxr = j.value("max_retries", 2);
        long long deadline = j.value("deadline_at_ms", 0LL);
        int nr = retry + 1;
        long long backoff = nr == 1 ? 30000 : (nr == 2 ? 60000 : 120000);
        if (std::string(statusNow) == "timeout" || now >= deadline) {
            db_.otaUpdate(id, "timeout", err, sent, 0, now, 0, retry);
        } else if (nr <= maxr && now + backoff < deadline) {
            db_.otaUpdate(id, "queued", err, sent, 0, 0, now + backoff, nr);
        } else {
            db_.otaUpdate(id, "failed", err, sent, 0, now, 0, nr);
        }
    };

    for (auto& j : arr) {
        std::string st = j.value("status", "");
        long long id = j["id"].get<long long>();
        long long deadline = j.value("deadline_at_ms", 0LL);
        long long sent = j.value("sent", 0LL);
        std::string devid = j.value("devid", std::string());
        std::string version = j.value("version", std::string());

        if (st == "running") {
            uint32_t tid = (uint32_t)j.value("relay_tid", 0);
            std::string p = callRelay([this, tid]() { return relay_->otaProgress(tid); });
            auto pg = json::parse(p, nullptr, false);
            bool found = !pg.is_discarded() && pg.value("found", false);
            if (found && !pg.value("done", false)) {
                long long s = pg.value("sent", sent);
                db_.otaProgress(id, s);
                if (now >= deadline) {
                    callRelay([this, tid]() { return relay_->cancelOtaPush(tid) ? "1" : "0"; });
                    ota_active.erase(id);
                    ota_busy_dev.erase(devid);
                    running--;
                    finishRetry(j, "deadline exceeded", s, "timeout");
                }
                continue;
            }
            if (found && pg.value("ok", false) && pg.value("applied", false)) {
                // 二进制已替换，等 agent 以新版本重连（SYSINFO 上报 version）
                db_.otaMarkApplying(id, (long long)pg.value("size", sent), tid, now);
                ota_active.erase(id);
                running--;
                continue;
            }
            std::string err = found ? pg.value("error", std::string("ota failed"))
                                    : std::string("relay lost transfer");
            if (found) {
                callRelay([this, tid]() { return relay_->cancelOtaPush(tid) ? "1" : "0"; });
            }
            ota_active.erase(id);
            ota_busy_dev.erase(devid);
            running--;
            finishRetry(j, err, sent, "failed");
            continue;
        }

        if (st == "applying") {
            // agent 可能补发一条失败 ACK（exec 失败已回滚），以 relay 最新结果为准
            uint32_t tid = (uint32_t)j.value("relay_tid", 0);
            if (tid) {
                std::string p = callRelay([this, tid]() { return relay_->otaProgress(tid); });
                auto pg = json::parse(p, nullptr, false);
                if (!pg.is_discarded() && pg.value("done", false) && !pg.value("ok", false)) {
                    std::string err = pg.value("error", std::string("restart failed"));
                    db_.otaUpdate(id, "failed", err, sent, 0, now, 0, j.value("retry_count", 0));
                    ota_active.erase(id);
                    ota_busy_dev.erase(devid);
                    continue;
                }
            }
            std::string ver = callRelay([this, &devid]() { return relay_->agentVersion(devid.c_str()); });
            if (!ver.empty() && ver == version) {
                db_.otaUpdate(id, "success", "", j.value("file_size", 0LL), 0, now, 0,
                              j.value("retry_count", 0));
                ota_active.erase(id);
                ota_busy_dev.erase(devid);
                continue;
            }
            long long started = j.value("started_ms", 0LL);
            if (started > 0 && now - started > apply_timeout_ms) {
                std::string err = "version not confirmed";
                if (!ver.empty()) err += " (agent reports " + ver + ")";
                db_.otaUpdate(id, "failed", err, sent, 0, now, 0, j.value("retry_count", 0));
                ota_active.erase(id);
                ota_busy_dev.erase(devid);
            }
            continue;
        }

        if (st != "queued") continue;
        if (now < j.value("next_retry_ms", 0LL)) continue;
        if (now >= deadline) {
            db_.otaUpdate(id, "timeout", "deadline exceeded", sent, 0, now, 0,
                          j.value("retry_count", 0));
            continue;
        }
        if (running >= maxc || ota_busy_dev.count(devid)) continue;

        bool online = callRelay([this, &devid]() {
            return relay_->agentOnline(devid.c_str()) ? "1" : "0";
        }) == "1";
        int cap = atoi(callRelay([this, &devid]() {
            return std::to_string(relay_->agentOtaCap(devid.c_str()));
        }).c_str());
        if (cap == 0) {
            db_.otaUpdate(id, "unsupported", "agent does not support ota", sent, 0, now, 0,
                          j.value("retry_count", 0));
            continue;
        }
        if (!online || cap < 0) {
            // 离线或能力未知：不消耗重试次数，1 分钟后再看
            db_.otaUpdate(id, "queued", online ? "" : "device offline", sent, 0, 0,
                          now + 60000, j.value("retry_count", 0));
            continue;
        }
        std::string cur = callRelay([this, &devid]() { return relay_->agentVersion(devid.c_str()); });
        if (!j.value("force", 0) && !cur.empty() && cur == version) {
            db_.otaUpdate(id, "success", "already at version", j.value("file_size", 0LL), 0, now, 0,
                          j.value("retry_count", 0));
            continue;
        }

        std::string path = cfg_.file_dir + "/" + j.value("file_name", std::string());
        uint32_t tid = 0;
        std::string err;
        std::string ok = callRelay([this, &devid, &path, &version, &j, &err, &tid]() {
            return relay_->startOtaPush(devid.c_str(), path.c_str(), version,
                    (uint64_t)j.value("file_size", 0LL), j.value("file_sha", std::string()),
                    err, tid) ? "1" : "0";
        });
        if (ok != "1") {
            finishRetry(j, err.empty() ? "start failed" : err, sent, "failed");
            continue;
        }
        db_.otaUpdate(id, "running", "", sent, tid, 0, now, j.value("retry_count", 0));
        ota_active[id] = tid;
        ota_busy_dev.insert(devid);
        running++;
        logf_("[ota] start %s version=%s tid=%u", devid.c_str(), version.c_str(), tid);
    }
}
