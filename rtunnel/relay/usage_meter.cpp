// 用量计量：relay 只在内存里累计，管理面 worker 周期性调用 takeUsageEvents()
// 取走并落库（usage_events + usage_daily）。这里不碰 SQLite，也不做任何阻塞操作。
#include "relay_server.hpp"

#include "cpputil/json.hpp"

#include <string.h>
#include <time.h>

using nlohmann::json;

namespace {
const size_t kUsagePendingMax = 100000;   // 管理面长期不在时兜底，丢最旧事件
const long long kOnlineFlushMs = 3600LL * 1000;   // 在线时长最长一小时落一行
// 字节流水节流：攒够 4 MiB 或满 10 分钟才落一行，避免每秒一条把库写爆。
// 配额执行用的是内存计数，不受这里的节流影响。
const long long kBytesFlushBytes = 4LL << 20;
const long long kBytesFlushMs = 600LL * 1000;
}  // namespace

std::string RelayServer::usageLocalDay(long long ts_ms) const {
    time_t t = (time_t)(ts_ms / 1000);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[16] = {0};
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return std::string(buf);
}

void RelayServer::usagePushRaw(const char* subject_type, const std::string& subject_id,
                               const char* metric, long long qty, const std::string& meta,
                               const std::string& day, long long tenant_id) {
    if (subject_id.empty() || metric == nullptr || qty == 0) return;
    if (usage_pending.size() >= kUsagePendingMax) {
        ++usage_dropped;
        return;
    }
    long long now = (long long)time(NULL) * 1000;
    json e;
    e["ts_ms"] = now;
    e["day"] = day.empty() ? usageLocalDay(now) : day;
    e["tenant_id"] = tenant_id;
    e["subject_type"] = subject_type;
    e["subject_id"] = subject_id;
    e["metric"] = metric;
    e["qty"] = qty;
    if (!meta.empty()) e["meta"] = meta;
    e["source"] = "relay";
    usage_pending.push_back(e.dump());
}

// 把某令牌尚未上报的字节落一行；force=true 用于会话关闭（每次会话至少结算一次）
void RelayServer::usageFlushTokenBytes(long long token_id, bool force) {
    auto it = token_usage.find(token_id);
    if (it == token_usage.end()) return;
    TokenUsage& u = it->second;
    long long delta = u.used_bytes - u.reported_bytes;
    if (delta <= 0) {
        if (delta < 0) u.reported_bytes = u.used_bytes;   // 周期归零/回退，直接对齐
        return;
    }
    long long now = (long long)time(NULL) * 1000;
    if (!force && delta < kBytesFlushBytes && u.last_report_ms != 0 &&
        now - u.last_report_ms < kBytesFlushMs) {
        return;
    }
    usagePushRaw("token", std::to_string(token_id), "bytes", delta, "", usageLocalDay(now), u.tenant_id);
    u.reported_bytes = u.used_bytes;
    u.last_report_ms = now;
}

// 会话结束：一条 sessions（计次）+ 一条 session_seconds（计时长）。
// 只统计真正建立成功的流；管理台内部链路（token_id=0，终端/代理）不计入客户用量。
void RelayServer::usageOnStreamClose(RelayStream* s) {
    if (s == nullptr || !s->opened || s->a == nullptr || s->c == nullptr) return;
    if (s->a->token_id <= 0) return;   // 管理台内部链路（终端/代理）不属于客户用量
    usageFlushTokenBytes(s->a->token_id, true);   // 会话结束即结算该令牌的流量
    long long now_mono = (long long)hloop_now_ms(loop);
    long long dur_ms = now_mono - (long long)s->opened_at_ms;
    if (dur_ms < 0) dur_ms = 0;
    long long now_wall = (long long)time(NULL) * 1000;

    json meta;
    meta["token_id"] = s->a->token_id;
    meta["account_id"] = s->a->account_id;
    meta["user"] = s->a->user;
    meta["devid"] = s->c->devid;
    meta["port"] = (int)s->port;
    meta["started_ms"] = now_wall - dur_ms;
    meta["ended_ms"] = now_wall;
    meta["bytes_up"] = (long long)s->bytes_up;
    meta["bytes_down"] = (long long)s->bytes_down;
    std::string m = meta.dump();
    std::string subject = std::to_string(s->a->token_id);
    std::string today = usageLocalDay(now_wall);

    usagePushRaw("token", subject, "sessions", 1, m, today, s->a->tenant_id);
    long long secs = (dur_ms + 500) / 1000;
    if (secs > 0) usagePushRaw("token", subject, "session_seconds", secs, m, today, s->a->tenant_id);
}

// 把某设备的在线时长累加落一行；erase=false 只清零保留条目
void RelayServer::usageFlushOnline(const std::string& devid, bool erase) {
    auto it = usage_online.find(devid);
    if (it == usage_online.end()) return;
    long long secs = it->second.ms / 1000;
    if (secs > 0) {
        long long tenant = 1;   // 未登记的设备按默认租户
        auto dt = acl_device_tenant.find(devid);
        if (dt != acl_device_tenant.end()) tenant = dt->second;
        usagePushRaw("device", devid, "online_seconds", secs, "", it->second.day, tenant);
        it->second.ms -= secs * 1000;
    }
    if (erase) usage_online.erase(it);
}

// 设备断开：把最后一个不足一小时的尾巴冲掉，避免掉线后这段时间永远不入账
void RelayServer::usageOnAgentClose(RelayConn* c) {
    if (c == nullptr || !c->authed || c->devid[0] == '\0') return;
    if (strncmp(c->devid, "a:", 2) == 0) return;
    uint64_t now = hloop_now_ms(loop);
    if (c->usage_online_ms > 0 && now > c->usage_online_ms) {
        auto& acc = usage_online[c->devid];
        long long today_ts = (long long)time(NULL) * 1000;
        if (acc.day.empty()) acc.day = usageLocalDay(today_ts);
        acc.ms += (long long)(now - c->usage_online_ms);
        c->usage_online_ms = now;
    }
    usageFlushOnline(c->devid, true);
}

std::string RelayServer::takeUsageEvents() {
    long long now_wall = (long long)time(NULL) * 1000;
    std::string today = usageLocalDay(now_wall);
    uint64_t now_mono = hloop_now_ms(loop);

    // Token 字节：按阈值/时间节流上报；会话关闭时另有强制结算
    for (auto& kv : token_usage) {
        usageFlushTokenBytes(kv.first, false);
    }

    // 设备在线：按设备累加，跨天或满一小时才落一行
    for (RelayConn* c = agents; c; c = c->reg_next) {
        if (!c->authed || c->devid[0] == '\0') continue;
        if (strncmp(c->devid, "a:", 2) == 0) continue;
        if (c->usage_online_ms == 0) { c->usage_online_ms = now_mono; continue; }
        if (now_mono <= c->usage_online_ms) continue;
        long long d = (long long)(now_mono - c->usage_online_ms);
        c->usage_online_ms = now_mono;
        auto& acc = usage_online[c->devid];
        if (acc.day != today) {
            usageFlushOnline(c->devid, false);   // 先按旧的一天结算
            acc.day = today;
        }
        acc.ms += d;
        if (acc.ms >= kOnlineFlushMs) usageFlushOnline(c->devid, false);
    }

    std::string out = "[";
    for (size_t i = 0; i < usage_pending.size(); ++i) {
        if (i) out += ",";
        out += usage_pending[i];
    }
    out += "]";
    usage_pending.clear();
    return out;
}
