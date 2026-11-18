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
    int apply_timeout_ms = settingInt("ota.apply_timeout_s", 180) * 1000;
    if (apply_timeout_ms < 30000) apply_timeout_ms = 30000;

    dist_active.clear();
    dist_busy_dev.clear();
    int running = 0;
    for (auto& j : arr) {
        std::string st = j.value("status", "");
        if (st == "running" || st == "verifying" || st == "installing" || st == "confirming") {
            dist_active[j["id"].get<long long>()] = (uint32_t)j.value("relay_tid", 0);
            dist_busy_dev.insert(j.value("devid", std::string()));
            if (st == "running") running++;
        }
    }

    auto finishRetry = [&](const json& j, const std::string& err, long long sent, const char* statusNow) {
        long long id = j["id"].get<long long>();
        int retry = j.value("retry_count", 0);
        int maxr = j.value("max_retries", 3);
        long long deadline = j.value("deadline_at_ms", 0LL);
        int nr = retry + 1;
        long long backoff = nr == 1 ? 30000 : (nr == 2 ? 60000 : 120000);
        const char* final_status = NULL;
        if (std::string(statusNow) == "timeout" || now >= deadline) {
            db_.distUpdate(id, "timeout", err, sent, 0, now, 0, retry);
            final_status = "timeout";
        } else if (nr <= maxr && now + backoff < deadline) {
            db_.distUpdate(id, "queued", err, sent, 0, 0, now + backoff, nr);
        } else {
            db_.distUpdate(id, "failed", err, sent, 0, now, 0, nr);
            final_status = "failed";
        }
        if (final_status) {
            notifyTaskFailed(id, j.value("devid", std::string()), final_status, err,
                             j.value("kind", std::string("file")));
        }
    };

    // 仓库文件可能带子目录，传输名只用最后一段
    auto baseOfPath = [](const std::string& p) {
        size_t i = p.rfind('/');
        return i == std::string::npos ? p : p.substr(i + 1);
    };
    // 设备侧文件绝对路径：分发目标目录 + 文件名
    auto devicePath = [&](const json& j) {
        std::string dir = j.value("target_path", std::string());
        std::string sub = j.value("sub_dir", std::string());
        if (dir.empty()) dir = "/" + sub;
        if (dir.size() > 1 && dir.back() == '/') dir.pop_back();
        return dir + "/" + baseOfPath(j.value("file_name", std::string()));
    };

    for (auto& j : arr) {
        std::string st = j.value("status", "");
        long long id = j["id"].get<long long>();
        long long deadline = j.value("deadline_at_ms", 0LL);
        long long sent = j.value("sent", 0LL);
        std::string devid = j.value("devid", std::string());
        std::string kind = j.value("kind", std::string("file"));
        std::string version = j.value("target_version", std::string());

        if (st == "running") {
            uint32_t tid = (uint32_t)j.value("relay_tid", 0);
            std::string p = callRelay([this, tid]() { return relay_->pushProgress(tid); });
            auto pg = json::parse(p, nullptr, false);
            if (!pg.is_discarded()) {
                sent = pg.value("sent", sent);
                db_.distProgress(id, sent);
                if (pg.value("done", false)) {
                    if (pg.value("ok", false)) {
                        if (kind == "ota") {
                            // 传输完成 -> 进入版本校验/安装阶段（不再新开传输）
                            uint32_t atid = 0;
                            std::string err;
                            std::string path = devicePath(j);
                            std::string ok = callRelay([this, &devid, &path, &version, &j, &err, &atid]() {
                                return relay_->startOtaApply(devid.c_str(), path.c_str(), version,
                                        (uint64_t)j.value("file_size", 0LL),
                                        j.value("file_sha", std::string()), err, atid) ? "1" : "0";
                            });
                            dist_active.erase(id);
                            dist_busy_dev.erase(devid);
                            running--;
                            if (ok != "1") {
                                finishRetry(j, err.empty() ? "ota start failed" : err, sent, "failed");
                            } else {
                                db_.distSetPhase(id, "verifying", "", sent, atid, now);
                                logf_("[dist] ota verify %s version=%s tid=%u", devid.c_str(),
                                      version.c_str(), atid);
                            }
                            continue;
                        }
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

        if (st == "verifying" || st == "installing") {
            uint32_t tid = (uint32_t)j.value("relay_tid", 0);
            long long started = j.value("started_ms", 0LL);
            std::string p = callRelay([this, tid]() { return relay_->otaApplyProgress(tid); });
            auto pg = json::parse(p, nullptr, false);
            if (pg.is_discarded() || !pg.value("found", false)) {
                dist_active.erase(id);
                finishRetry(j, "relay lost ota apply", sent, "failed");
                continue;
            }
            std::string phase = pg.value("phase", std::string());
            if (st == "verifying" && (phase == "verified" || phase == "installed")) {
                // 先把“版本校验通过”落库，下一 tick 再进入安装/确认，阶段可在任务页看到
                db_.distSetPhase(id, "installing", "", sent, tid, started ? started : now);
                continue;
            }
            if (pg.value("done", false)) {
                dist_active.erase(id);
                if (pg.value("ok", false)) {
                    db_.distSetPhase(id, "confirming", "", sent, tid, now);
                } else {
                    finishRetry(j, pg.value("error", std::string("install failed")), sent, "failed");
                }
                continue;
            }
            if (started > 0 && now - started > apply_timeout_ms) {
                callRelay([this, tid]() { return relay_->cancelOtaApply(tid) ? "1" : "0"; });
                dist_active.erase(id);
                finishRetry(j, "ota apply timeout", sent, "timeout");
            }
            continue;
        }

        if (st == "confirming") {
            long long started = j.value("started_ms", 0LL);
            uint32_t tid = (uint32_t)j.value("relay_tid", 0);
            if (tid) {
                // exec 失败时 agent 会补发失败 ACK，立即收敛不必等满超时
                std::string ap = callRelay([this, tid]() { return relay_->otaApplyProgress(tid); });
                auto ag = json::parse(ap, nullptr, false);
                if (!ag.is_discarded() && ag.value("done", false) && !ag.value("ok", false)) {
                    dist_active.erase(id);
                    dist_busy_dev.erase(devid);
                    finishRetry(j, ag.value("error", std::string("restart failed")), sent, "failed");
                    continue;
                }
            }
            std::string cur = callRelay([this, &devid]() { return relay_->agentVersion(devid.c_str()); });
            if (!cur.empty() && cur == version) {
                db_.distUpdate(id, "success", "", j.value("file_size", 0LL), 0, now, 0,
                               j.value("retry_count", 0));
                dist_active.erase(id);
                dist_busy_dev.erase(devid);
                continue;
            }
            if (started > 0 && now - started > apply_timeout_ms) {
                std::string err = "version not confirmed";
                if (!cur.empty()) err += " (agent reports " + cur + ")";
                dist_active.erase(id);
                dist_busy_dev.erase(devid);
                finishRetry(j, err, sent, "failed");
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

            if (kind == "ota") {
                if (!online) {
                    // 设备不在线不消耗重试次数，等它上线
                    db_.distUpdate(id, "queued", "device offline", sent, 0, 0,
                                   now + 60000, j.value("retry_count", 0));
                    continue;
                }
                int cap = atoi(callRelay([this, &devid]() {
                    return std::to_string(relay_->agentOtaCap(devid.c_str()));
                }).c_str());
                if (cap == 0) {
                    db_.distUpdate(id, "unsupported", "agent does not support ota", sent, 0, now, 0,
                                   j.value("retry_count", 0));
                    continue;
                }
                if (cap < 0) {
                    db_.distUpdate(id, "queued", "", sent, 0, 0, now + 60000,
                                   j.value("retry_count", 0));
                    continue;
                }
            } else if (!online) {
                finishRetry(j, "device offline", sent, "failed");
                continue;
            }

            std::string path;
            if (!adminRepoPath(cfg_.file_dir, j.value("file_name", std::string()), path)) {
                finishRetry(j, "bad repo file path", sent, "failed");
                continue;
            }
            std::string sub = j.value("sub_dir", std::string());
            uint32_t tid = 0;
            std::string err;
            std::string ok = callRelay([this, &devid, &path, &sub, &j, &err, &tid, &baseOfPath]() {
                std::string fname = baseOfPath(j.value("file_name", std::string()));
                return relay_->startFilePush(devid.c_str(), path.c_str(), sub.c_str(),
                        fname.c_str(),
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
