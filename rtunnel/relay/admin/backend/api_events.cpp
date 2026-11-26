// 事件流（SSE）与分钟级指标查询。
//
// SSE 不跨线程写 hio：每个连接在自己的 loop 上起一个 1s 定时器，从进程内 EventBus
// 拉增量再写回自己的 io。这样既不用锁 io，也天然支持 Last-Event-ID 续传（seq 即事件 id）。
#include "admin_server.hpp"
#include "api_reply.hpp"

#include "cpputil/json.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>

using nlohmann::json;

namespace {

const int  kSsePollMs = 1000;         // 增量拉取间隔
const int  kSseBeatMs = 15000;        // 心跳注释，防止中间设备掐空闲连接
const int  kSseMaxClients = 32;       // 单 relay 最多 32 个事件流连接
const long long kSseMaxLifeMs = 6LL * 3600 * 1000;   // 单连接最长 6 小时，之后让客户端重连

std::mutex g_sse_mu;
int        g_sse_clients = 0;

// 权限快照：定时器里没有请求上下文，绝不能读 tl_actor（会被同线程其它请求覆盖）
struct SseConn {
    AdminServer* srv = nullptr;
    hv::HttpResponseWriter* writer = nullptr;
    hio_t*     io = nullptr;
    hloop_t*   loop = nullptr;
    htimer_t*  timer = nullptr;
    long long  cursor = 0;
    // 无 Last-Event-ID 时只推「连接之后发生的事」：relay 队列到总线最多有 500ms 搬运延迟，
    // 光靠 cursor 会把连接前产生的旧事件当成新的推出去（受限账号会看到别人的审计）。
    long long  since_ms = 0;
    // 客户端断开后 hv 会回收 io 并把同一块内存给下一个连接；指针不变但 id 会变。
    // 不认这个就会把旧连接的事件写到新连接的 socket 上（串流）。
    uint32_t   io_id = 0;
    uint64_t   last_beat = 0;
    uint64_t   opened_at = 0;
    bool       restricted = false;      // 受限账号：只推自己设备的设备类事件
    bool       super = false;
    bool       all_devices = false;
    bool       is_client = false;
    long long  tenant_id = 0;
    std::set<std::string> devices;
    std::string who;
};

}  // namespace

namespace {

void sseFree(SseConn* c) {
    if (c == nullptr) return;
    if (c->timer) {
        htimer_del(c->timer);
        c->timer = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(g_sse_mu);
        if (g_sse_clients > 0) --g_sse_clients;
    }
    delete c;
}

// 与 AdminServer::ownsDevid 同语义，但身份来自快照而不是 thread_local
bool sseDevidAllowed(const SseConn* c, const std::string& devid, AdminDb* db) {
    if (devid.empty()) return !c->restricted;
    if (c->is_client) {
        long long t = 0;
        if (db == nullptr || !db->deviceTenant(devid, t) || t != c->tenant_id) return false;
    }
    if (!c->restricted) return true;
    if (c->super || c->all_devices) return true;
    return c->devices.count(devid) > 0;
}

// 事件是否对当前订阅者可见：受限账号只收自己设备的事件
bool sseAllow(const SseConn* c, const std::string& type, const json& data, AdminDb* db) {
    if (!c->restricted) return true;
    auto it = data.find("devid");
    if (it != data.end() && it->is_string()) return sseDevidAllowed(c, it->get<std::string>(), db);
    // 审计事件：target_type=device 时按目标设备过滤，其余一律不给
    if (type == "audit") {
        std::string tt = data.value("target_type", std::string());
        if (tt == "device") return sseDevidAllowed(c, data.value("target_id", std::string()), db);
        return false;
    }
    if (type == "bus.dropped") return true;
    return false;
}

int sseWrite(SseConn* c, const std::string& chunk) {
    if (c == nullptr || c->writer == nullptr) return -1;
    if (c->io == nullptr || hio_is_closed(c->io)) return -1;
    return c->writer->WriteBody(chunk);
}

std::string sseFrame(long long seq, const char* type, const std::string& data) {
    std::string s;
    s.reserve(data.size() + 64);
    if (seq > 0) {
        s += "id: ";
        s += std::to_string(seq);
        s += "\n";
    }
    if (type && *type) {
        s += "event: ";
        s += type;
        s += "\n";
    }
    s += "data: ";
    s += data;
    s += "\n\n";
    return s;
}

}  // namespace

void AdminServer::sseTickCb(htimer_t* t) {
    SseConn* c = (SseConn*)hevent_userdata(t);
    if (c == nullptr) return;
    if (c->io == nullptr || hio_is_closed(c->io) || (c->io_id != 0 && hio_id(c->io) != c->io_id)) {
        sseFree(c);   // 连接已关闭，或 io 结构体已被回收复用
        return;
    }
    uint64_t now = hloop_now_ms(c->loop);
    if (now - c->opened_at > (uint64_t)kSseMaxLifeMs) {
        sseWrite(c, ": bye\n\n");
        sseFree(c);
        return;
    }

    std::vector<EventEntry> evs;
    bool lost = false;
    long long latest = c->srv->bus_.drain(c->cursor, evs, lost);
    if (lost) {
        json j;
        j["reason"] = "cursor expired";
        j["oldest_seq"] = latest;
        if (sseWrite(c, sseFrame(0, "bus.lost", j.dump())) < 0) {
            sseFree(c);
            return;
        }
    }
    for (const auto& e : evs) {
        // 续传（带 Last-Event-ID）要历史；新连接只要连接之后的事件，
        // 留 1s 容差吸收同毫秒与时序抖动。
        if (c->since_ms > 0 && e.ts_ms > 0 && e.ts_ms < c->since_ms - 1000) continue;
        json data = json::parse(e.data, nullptr, false);
        if (data.is_discarded()) data = json::object();
        if (!sseAllow(c, e.type, data, &c->srv->db_)) continue;
        data["ts"] = e.ts_ms;
        if (sseWrite(c, sseFrame(e.seq, e.type.c_str(), data.dump())) < 0) {
            sseFree(c);
            return;
        }
    }
    c->cursor = latest;

    if (now - c->last_beat >= (uint64_t)kSseBeatMs) {
        c->last_beat = now;
        if (sseWrite(c, ": ping\n\n") < 0) {
            sseFree(c);
            return;
        }
    }
}

void AdminServer::handleEvents(const HttpContextPtr& ctx) {
    if (ctx->writer == nullptr) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "streaming unavailable");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_sse_mu);
        if (g_sse_clients >= kSseMaxClients) {
            apiError(ctx, HTTP_STATUS_TOO_MANY_REQUESTS,
                     "事件流连接已达上限 " + std::to_string(kSseMaxClients) + "，请稍后再试");
            return;
        }
        ++g_sse_clients;
    }

    AdminActor a = currentActor();
    auto* c = new SseConn;
    c->srv = this;
    c->writer = ctx->writer.get();
    c->io = ctx->writer->io();
    c->io_id = c->io ? hio_id(c->io) : 0;
    c->loop = c->io ? hevent_loop(c->io) : nullptr;
    if (c->loop == nullptr) {
        sseFree(c);
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "no event loop");
        return;
    }
    c->who = actorLabel();
    c->super = a.super;
    c->all_devices = a.all_devices;
    c->is_client = a.is_client;
    c->tenant_id = a.tenant_id;
    c->devices = a.devices;
    c->restricted = a.is_client ? true : !(a.super || a.all_devices);
    c->opened_at = hloop_now_ms(c->loop);
    c->last_beat = c->opened_at;

    // 断点续传：优先 Last-Event-ID，其次 ?last=，都没有就只推新事件
    long long cursor = 0;
    std::string le = ctx->header("Last-Event-ID");
    if (le.empty()) le = ctx->param("last");
    if (!le.empty()) cursor = atoll(le.c_str());
    if (cursor <= 0) {
        cursor = bus_.head();
        c->since_ms = AdminDb::nowMs();   // 新连接：只要这之后产生的
    }
    c->cursor = cursor;

    ctx->response->headers["Cache-Control"] = "no-cache";
    ctx->response->headers["X-Accel-Buffering"] = "no";   // nginx 不要缓冲
    ctx->writer->Begin();
    if (ctx->writer->EndHeaders("Content-Type", "text/event-stream") < 0) {
        sseFree(c);
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "streaming unavailable");
        return;
    }
    json hello;
    hello["cursor"] = c->cursor;
    hello["who"] = c->who;
    hello["restricted"] = c->restricted;
    if (sseWrite(c, sseFrame(0, "hello", hello.dump())) < 0) {
        sseFree(c);
        return;
    }

    c->timer = htimer_add(c->loop, &AdminServer::sseTickCb, kSsePollMs, INFINITE);
    if (c->timer == nullptr) {
        sseFree(c);
        return;
    }
    hevent_set_userdata(c->timer, c);
    // 返回 NEXT：hv 不再接管这个连接，直到我们自己 End/close。ctx->writer 已写过 header，
    // HttpHandler 也会自动把状态改成 NEXT。
}

void AdminServer::handleMetricsHistory(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (devid.empty()) {
        apiBad(ctx, "devid required");
        return;
    }
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    long long now = AdminDb::nowMs();
    long long from = 0, to = 0;
    int step = 60;
    for (const auto& kv : ctx->request->query_params) {
        if (kv.first == "from") from = atoll(kv.second.c_str());
        else if (kv.first == "to") to = atoll(kv.second.c_str());
        else if (kv.first == "step") step = atoi(kv.second.c_str());
    }
    if (to <= 0) to = now;
    if (from <= 0) from = to - 6LL * 3600 * 1000;   // 默认近 6 小时
    if (from > to) {
        apiBad(ctx, "from 必须早于 to");
        return;
    }
    // 时间戳兼容秒：小于 1e12 当秒处理
    if (from < 100000000000LL) from *= 1000;
    if (to < 100000000000LL) to *= 1000;
    if (step <= 0) step = 60;

    std::string out, err;
    if (!db_.metricsQuery(devid, from, to, step, out, err)) {
        if (err == "range too large (max 31 days)") {
            apiBad(ctx, err);
        } else {
            apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err);
        }
        return;
    }
    ctx->send(out);
}
