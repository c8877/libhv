// 分发任务调度器：到点执行、并发控制、重试/超时
#include "admin_server.hpp"
#include "hash_util.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <chrono>
#include <thread>

using nlohmann::json;

static long long nowMs() { return AdminDb::nowMs(); }

// ---------------------------------------------------------------- 调度

void AdminServer::distLoop() {
    while (!waitStop(1000)) {
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
