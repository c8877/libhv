// 中继列表同步任务调度：到点释放批次、下发快照、等 ACK、失败闸门与重试
#include "admin_server.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <cstdio>
#include <map>
#include <set>
#include <string>

using nlohmann::json;


// "1.4.98" / "1.4.98-armv7l" → 至少 1.4.98
static bool versionAtLeast(const std::string& v, int maj, int min, int patch) {
    int a = 0, b = 0, c = 0;
    int n = sscanf(v.c_str(), "%d.%d.%d", &a, &b, &c);
    if (n < 2) return false;
    if (a != maj) return a > maj;
    if (b != min) return b > min;
    return c >= patch;
}

void AdminServer::relaySyncLoop() {
    while (!waitStop(1000)) {
        try {
            relaySyncTick();
        } catch (...) {
            logf_("[relay-sync] tick exception");
        }
    }
}

void AdminServer::relaySyncTick() {
    long long now = AdminDb::nowMs();

    // ---- 任务级：批量信息（状态 + 快照），同时决定哪些任务还接受下发
    struct TaskInfo {
        std::string status;
        long long rev = 0;
        std::string payload;
        bool payload_loaded = false;
    };
    std::map<std::string, TaskInfo> tasks;
    {
        std::string js;
        if (db_.relayTaskActive(js)) {
            auto arr = json::parse(js, nullptr, false);
            if (!arr.is_discarded() && arr.is_array()) {
                for (auto& j : arr) {
                    TaskInfo t;
                    t.status = j.value("status", std::string());
                    t.rev = j.value("rev", 0LL);
                    tasks[j.value("batch", std::string())] = t;
                }
            }
        }
    }

    // ---- 逐设备行
    std::string dj;
    if (db_.relayTaskDue(dj)) {
        auto rows = json::parse(dj, nullptr, false);
        if (!rows.is_discarded() && rows.is_array()) {
            std::string acks_js = callRelay([this]() { return relay_->relayListAcksJson(); });
            auto acks = json::parse(acks_js, nullptr, false);
            if (acks.is_discarded() || !acks.is_object()) acks = json::object();

            // 在线表每 tick 只取一次；设备多时避免逐行往返
            std::set<std::string> onlineSet;
            bool onlineLoaded = false;
            auto isOnline = [&](const std::string& d) {
                if (!onlineLoaded) {
                    auto arr = json::parse(callRelay([this]() { return relay_->onlineAgentDevids(); }),
                                           nullptr, false);
                    if (!arr.is_discarded() && arr.is_array()) {
                        for (auto& v : arr) {
                            if (v.is_string()) onlineSet.insert(v.get<std::string>());
                        }
                    }
                    onlineLoaded = true;
                }
                return onlineSet.count(d) > 0;
            };

            // 版本按设备缓存：一个 tick 内每台最多查一次
            std::map<std::string, std::string> verCache;
            auto versionOf = [&](const std::string& d) {
                auto it = verCache.find(d);
                if (it != verCache.end()) return it->second;
                std::string v = callRelay([this, d]() { return relay_->agentVersion(d.c_str()); });
                verCache[d] = v;
                return v;
            };

            for (auto& r : rows) {
                long long id = r.value("id", 0LL);
                std::string batch = r.value("batch", std::string());
                std::string devid = r.value("devid", std::string());
                std::string st = r.value("status", std::string());
                int retry = r.value("retry_count", 0);
                int maxr = r.value("max_retries", 1);
                long long deadline = r.value("deadline_at_ms", 0LL);
                long long row_rev = r.value("rev", 0LL);

                // 任务快照（发送内容与 rev 都来自快照，列表后续修改不影响已建任务）
                auto ti = tasks.find(batch);
                // paused 不算活跃：暂停后不再释放/补发任何一行，等人工继续
                bool task_active = ti != tasks.end() &&
                                   (ti->second.status == "scheduled" || ti->second.status == "running");
                if (ti != tasks.end() && !ti->second.payload_loaded) {
                    db_.relayTaskPayload(batch, ti->second.payload, ti->second.rev);
                    ti->second.payload_loaded = true;
                }

                // ACK 命中同一 rev 即视为已对齐（含设备重连后的自动对齐）
                auto ackIt = acks.find(devid);
                bool ack_hit = false;
                bool ack_ok = false;
                std::string ack_err;
                long long ack_rev = 0;
                if (ackIt != acks.end() && ackIt->is_object()) {
                    ack_rev = (*ackIt).value("rev", 0LL);
                    ack_ok = (*ackIt).value("ok", false);
                    ack_err = (*ackIt).value("error", std::string());
                    ack_hit = (ack_rev == row_rev && row_rev != 0);
                }

                // 已对齐：不用再发，直接收敛（设备可能在上一次自动下发里就采纳了这份列表）
                if (ack_hit && st != "success" && st != "failed" && st != "timeout" && st != "cancelled") {
                    db_.distUpdate(id, ack_ok ? "success" : "failed",
                                   ack_ok ? std::string() : ack_err, 0, 0, now, 0, retry);
                    logf_("[relay-sync] %s %s already aligned (rev=%lld ok=%d)", batch.c_str(), devid.c_str(),
                          ack_rev, ack_ok ? 1 : 0);
                    continue;
                }

                if (st == "pending") {
                    if (!task_active) continue;   // 任务已收尾，不再补发旧快照
                    if (isOnline(devid)) db_.distUpdate(id, "queued", "", 0, 0, 0, 0, retry);
                    continue;
                }

                if (st == "scheduled") {
                    if (task_active && now >= r.value("start_at_ms", 0LL)) {
                        db_.distUpdate(id, "queued", "", 0, 0, 0, 0, retry);
                    }
                    continue;
                }

                if (st == "running") {
                    long long started = r.value("started_ms", 0LL);
                    if (started > 0 && now - started > 15000) {
                        int nr = retry + 1;
                        long long backoff = nr == 1 ? 5000 : (nr == 2 ? 15000 : 30000);
                        if (nr <= maxr && (deadline == 0 || now + backoff < deadline)) {
                            db_.distUpdate(id, "queued", "ack timeout", 0, 0, 0, now + backoff, nr);
                        } else {
                            db_.distUpdate(id, "failed", "ack timeout", 0, 0, now, 0, nr);
                        }
                    }
                    continue;
                }

                if (st != "queued") continue;
                long long next_retry = r.value("next_retry_ms", 0LL);
                if (now < next_retry) continue;
                if (deadline > 0 && now >= deadline) {
                    db_.distUpdate(id, "timeout", "deadline exceeded", 0, 0, now, 0, retry);
                    continue;
                }
                if (!task_active) continue;
                if (!isOnline(devid)) {
                    db_.distUpdate(id, "pending", "device offline", 0, 0, 0, 0, retry);
                    logf_("[relay-sync] %s %s offline -> pending", batch.c_str(), devid.c_str());
                    continue;
                }
                std::string ver = versionOf(devid);
                if (!versionAtLeast(ver, 1, 4, 98)) {
                    db_.distUpdate(id, "failed",
                                   "agent " + (ver.empty() ? std::string("unknown") : ver) +
                                       " 不支持 RELAY_LIST（需 >= 1.4.98）",
                                   0, 0, now, 0, retry);
                    continue;
                }
                if (ti == tasks.end() || ti->second.payload.empty()) {
                    db_.distUpdate(id, "failed", "task snapshot missing", 0, 0, now, 0, retry);
                    continue;
                }
                std::string payload = ti->second.payload;
                bool sent = callRelay([this, devid, payload]() {
                    return relay_->sendRelayListJson(devid.c_str(), payload) ? "1" : "0";
                }) == "1";
                if (!sent) {
                    db_.distUpdate(id, "pending", "send failed", 0, 0, 0, 0, retry);
                    logf_("[relay-sync] %s %s send failed -> pending", batch.c_str(), devid.c_str());
                    continue;
                }
                db_.relayItemMarkRunning(id, now);
                logf_("[relay-sync] %s %s sent (rev=%lld)", batch.c_str(), devid.c_str(), row_rev);
            }
        }
    }

    // ---- 任务级推进
    for (auto& kv : tasks) {
        const std::string& batch = kv.first;
        TaskInfo& ti = kv.second;
        if (ti.status == "scheduled") {
            std::string js, err;
            if (!db_.relayTaskGet(batch, js, err)) continue;
            auto t = json::parse(js, nullptr, false);
            if (t.is_discarded()) continue;
            long long start = t.value("start_at_ms", 0LL);
            if (now >= start) {
                db_.relayTaskSetStatus(batch, "running", 0);
                db_.relayTaskSetWave(batch, 1, 0);
                logf_("[relay-sync] task %s started (wave 1)", batch.c_str());
            }
            continue;
        }
        if (ti.status != "running") continue;

        std::string js, err;
        if (!db_.relayTaskGet(batch, js, err)) continue;
        auto t = json::parse(js, nullptr, false);
        if (t.is_discarded()) continue;
        int wave = t.value("current_wave", 0);
        if (wave < 1) wave = 1;
        int total_waves = t.value("total_waves", 1);
        int interval_s = t.value("wave_interval_s", 0);
        int threshold = t.value("fail_threshold_pct", 0);
        long long wave_done = t.value("wave_done_ms", 0LL);
        long long deadline = t.value("deadline_at_ms", 0LL);

        int success = 0, failed = 0, timeout = 0, pending = 0, active = 0, total = 0;
        db_.relayTaskWaveStats(batch, wave, success, failed, timeout, pending, active, total);

        // 截止时间到了：未完成的（含待上线、未释放批次）一起收尾
        if (deadline > 0 && now >= deadline && (active > 0 || pending > 0 || wave < total_waves)) {
            int n = 0;
            db_.relayTaskCancelPending(batch, n);
            db_.relayTaskSetStatus(batch, "failed", now);
            logf_("[relay-sync] task %s deadline exceeded, cancelled %d", batch.c_str(), n);
            continue;
        }
        if (active > 0) continue;
        if (wave_done == 0) {
            int denom = success + failed + timeout;
            int rate = denom > 0 ? (failed + timeout) * 100 / denom : 0;
            // 还有后续批次时才需要暂停闸门；最后一批直接收敛成最终状态
            if (rate > threshold && wave < total_waves) {
                db_.relayTaskSetWave(batch, wave, now);
                db_.relayTaskSetStatus(batch, "paused", 0);
                logf_("[relay-sync] task %s paused: wave %d failure %d%% > %d%%", batch.c_str(), wave, rate,
                      threshold);
                continue;
            }
            db_.relayTaskSetWave(batch, wave, now);
            logf_("[relay-sync] task %s wave %d done (%d/%d ok)", batch.c_str(), wave, success,
                  success + failed + timeout);
            continue;
        }
        if (wave >= total_waves) {
            int os = 0, of = 0, ot = 0, op = 0, oa = 0, otot = 0;
            db_.relayTaskOverall(batch, os, of, ot, op, oa, otot);
            std::string final;
            if (of + ot > 0) final = (os > 0) ? "partial" : "failed";
            else if (op > 0) final = "partial";   // 没有失败，但仍有设备没上线/没确认，不能算成功
            else final = "success";
            db_.relayTaskSetStatus(batch, final, now);
            logf_("[relay-sync] task %s finished: %s (ok=%d fail=%d timeout=%d pending=%d)", batch.c_str(),
                  final.c_str(), os, of, ot, op);
            continue;
        }
        long long wait_ms = (long long)interval_s * 1000;
        if (now - wave_done >= wait_ms) {
            std::string e;
            if (!db_.relayTaskReleaseWave(batch, wave + 1, now, e)) {
                logf_("[relay-sync] task %s release wave %d failed: %s", batch.c_str(), wave + 1, e.c_str());
                continue;
            }
            db_.relayTaskSetWave(batch, wave + 1, 0);
            logf_("[relay-sync] task %s released wave %d/%d", batch.c_str(), wave + 1, total_waves);
        }
    }
}
