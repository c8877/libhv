// 会话列表与概览（在 relay 线程内取快照）
#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <algorithm>
#include "hversion.h"

using nlohmann::json;

namespace {

bool devAllowed(const std::vector<std::string>& mine, bool scoped, const char* devid) {
    if (!scoped) return true;
    return std::find(mine.begin(), mine.end(), std::string(devid ? devid : "")) != mine.end();
}

// 绑定了权限组的 atk Token 只统计组内设备：agent 连接按 devid 过滤，
// client 连接只有在与组内 agent 存在会话时才计入。
bool clientRelevant(RelayConn* c, const std::vector<std::string>& mine, bool scoped) {
    if (!scoped) return true;
    for (RelayStream* s = c->streams; s; s = s->next_c)
        if (s->a && devAllowed(mine, scoped, s->a->devid)) return true;
    return false;
}

} // namespace

void AdminServer::handleStreams(const HttpContextPtr& ctx) {
    bool all = !scopeRestricted();
    std::vector<std::string> mine = myDevids();
    std::string body = callRelay([this, all, mine]() {
        json arr = json::array();
        // 按 agent 侧遍历，避免一条流双算
        for (RelayConn* c = relay_->agents; c; c = c->reg_next) {
            if (!all && std::find(mine.begin(), mine.end(), std::string(c->devid)) == mine.end()) continue;
            for (RelayStream* s = c->streams; s; s = s->next_c) {
                json st;
                st["c_devid"] = c->devid;
                st["c_sid"] = s->c_sid;
                st["a_user"] = s->a ? s->a->user : "";
                st["a_sid"] = s->a_sid;
                st["state"] = !s->opened ? "OPENING"
                              : (s->a_closed || s->c_closed) ? "HALF_CLOSED" : "OPEN";
                st["opened_at_ms"] = s->opened_at_ms;
                st["last_active_ms"] = s->last_active_ms;
                arr.push_back(st);
            }
        }
        return arr.dump();
    });
    ctx->send(body);
}

void AdminServer::handleOverview(const HttpContextPtr& ctx) {
    // 受限账号（未获全部设备）只看自己范围内的设备与会话
    bool scoped = scopeRestricted();
    std::vector<std::string> mine = scoped ? myDevids() : std::vector<std::string>();
    std::string body = callRelay([this, scoped, mine]() {
        json j;
        int nagent = 0, nclient = 0, nstream = 0;
        uint64_t in = 0, out = 0;
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (!c->authed) continue;
            if (c->role == rtnp::ROLE_AGENT && !devAllowed(mine, scoped, c->devid)) continue;
            if (c->role == rtnp::ROLE_CLIENT && !clientRelevant(c, mine, scoped)) continue;
            if (c->role == rtnp::ROLE_AGENT) {
                nagent++;
                for (RelayStream* s = c->streams; s; s = s->next_c) nstream++;
            } else if (c->role == rtnp::ROLE_CLIENT) {
                if (!c->internal) nclient++;
            }
            in += c->bytes_in;
            out += c->bytes_out;
        }
        j["agents"] = nagent;
        j["clients"] = nclient;
        j["streams"] = nstream;
        j["bytes_in"] = in;
        j["bytes_out"] = out;
        j["uptime_ms"] = hloop_now_ms(relay_->loop) - relay_->started_at_ms;
        j["version"] = hv_version();
        j["mtls"] = (bool)relay_->require_mtls;
        j["allow_all"] = (bool)relay_->allow_all;
        j["scoped"] = scoped;
        return j.dump();
    });
    ctx->send(body);
}

// Prometheus 文本指标（text/plain）
void AdminServer::handleMetrics(const HttpContextPtr& ctx) {
    bool scoped = scopeRestricted();
    std::vector<std::string> mine = scoped ? myDevids() : std::vector<std::string>();
    std::string body = callRelay([this, scoped, mine]() {
        int nagent = 0, nclient = 0, nstream = 0;
        uint64_t in = 0, out = 0;
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (!c->authed) continue;
            if (c->role == rtnp::ROLE_AGENT && !devAllowed(mine, scoped, c->devid)) continue;
            if (c->role == rtnp::ROLE_CLIENT && !clientRelevant(c, mine, scoped)) continue;
            if (c->role == rtnp::ROLE_AGENT) {
                nagent++;
                for (RelayStream* s = c->streams; s; s = s->next_c) nstream++;
            } else if (c->role == rtnp::ROLE_CLIENT) {
                if (!c->internal) nclient++;
            }
            in += c->bytes_in;
            out += c->bytes_out;
        }
        int pending = 0;
        for (PendingOpen* p = relay_->pending; p; p = p->next) ++pending;
        uint64_t uptime = (hloop_now_ms(relay_->loop) - relay_->started_at_ms) / 1000;
        const RelayCounters& st = relay_->stats;
        std::string s;
        s += "# HELP rtunnel_agents online agents\n# TYPE rtunnel_agents gauge\n";
        s += "rtunnel_agents " + std::to_string(nagent) + "\n";
        s += "# HELP rtunnel_clients online clients\n# TYPE rtunnel_clients gauge\n";
        s += "rtunnel_clients " + std::to_string(nclient) + "\n";
        s += "# HELP rtunnel_streams active streams\n# TYPE rtunnel_streams gauge\n";
        s += "rtunnel_streams " + std::to_string(nstream) + "\n";
        s += "# HELP rtunnel_pending_opens OPEN_SHELL waiting for on-demand agent\n# TYPE rtunnel_pending_opens gauge\n";
        s += "rtunnel_pending_opens " + std::to_string(pending) + "\n";
        s += "# HELP rtunnel_connection_bytes_in bytes buffered from current connections\n# TYPE rtunnel_connection_bytes_in gauge\n";
        s += "rtunnel_connection_bytes_in " + std::to_string(in) + "\n";
        s += "# HELP rtunnel_connection_bytes_out bytes buffered to current connections\n# TYPE rtunnel_connection_bytes_out gauge\n";
        s += "rtunnel_connection_bytes_out " + std::to_string(out) + "\n";
        if (!scoped) {
        s += "# HELP rtunnel_connections_total accepted RTNP connections\n# TYPE rtunnel_connections_total counter\n";
        s += "rtunnel_connections_total{role=\"agent\"} " + std::to_string(st.conns_agent_total) + "\n";
        s += "rtunnel_connections_total{role=\"client\"} " + std::to_string(st.conns_client_total) + "\n";
        s += "# HELP rtunnel_auth_failed_total rejected handshakes / AUTH frames\n# TYPE rtunnel_auth_failed_total counter\n";
        s += "rtunnel_auth_failed_total " + std::to_string(st.auth_failed_total) + "\n";
        s += "# HELP rtunnel_frames_in_total frames received from peers\n# TYPE rtunnel_frames_in_total counter\n";
        s += "rtunnel_frames_in_total " + std::to_string(st.frames_in_total) + "\n";
        s += "# HELP rtunnel_frames_forwarded_total frames relayed to the other side\n# TYPE rtunnel_frames_forwarded_total counter\n";
        s += "rtunnel_frames_forwarded_total " + std::to_string(st.frames_forwarded_total) + "\n";
        s += "# HELP rtunnel_frame_errors_total bad frames / protocol errors\n# TYPE rtunnel_frame_errors_total counter\n";
        s += "rtunnel_frame_errors_total " + std::to_string(st.frame_errors_total) + "\n";
        s += "# HELP rtunnel_bytes_in_total bytes received from peers\n# TYPE rtunnel_bytes_in_total counter\n";
        s += "rtunnel_bytes_in_total " + std::to_string(st.bytes_in_total) + "\n";
        s += "# HELP rtunnel_bytes_forwarded_total bytes relayed to peers\n# TYPE rtunnel_bytes_forwarded_total counter\n";
        s += "rtunnel_bytes_forwarded_total " + std::to_string(st.bytes_forwarded_total) + "\n";
        s += "# HELP rtunnel_streams_opened_total streams opened successfully\n# TYPE rtunnel_streams_opened_total counter\n";
        s += "rtunnel_streams_opened_total " + std::to_string(st.streams_opened_total) + "\n";
        s += "# HELP rtunnel_streams_open_failed_total OPEN rejected / failed\n# TYPE rtunnel_streams_open_failed_total counter\n";
        s += "rtunnel_streams_open_failed_total " + std::to_string(st.streams_open_failed_total) + "\n";
        s += "# HELP rtunnel_streams_idle_reset_total streams reclaimed by idle timeout\n# TYPE rtunnel_streams_idle_reset_total counter\n";
        s += "rtunnel_streams_idle_reset_total " + std::to_string(st.streams_idle_reset_total) + "\n";
        s += "# HELP rtunnel_uptime_seconds relay uptime\n# TYPE rtunnel_uptime_seconds gauge\n";
        s += "rtunnel_uptime_seconds " + std::to_string(uptime) + "\n";
        s += "# HELP rtunnel_revoked_identities revoked SAN count\n# TYPE rtunnel_revoked_identities gauge\n";
        s += "rtunnel_revoked_identities " + std::to_string(relay_->revoked_sans.size()) + "\n";
        }  // !scoped：全局累计计数不暴露给按设备收窄的 Token
        return s;
    });

    // 管理面指标：HTTP 派发耗时、SQLite 语句耗时、告警队列深度
    // 同样只给未按设备收窄的调用方，避免把全局管理面数据带出权限范围。
    if (scoped) {
        ctx->response->content_type = TEXT_PLAIN;
        ctx->response->body = body;
        return;
    }
    DbStats ds;
    db_.stats(ds);
    int alert_q = 0;
    {
        std::lock_guard<std::mutex> lk(alert_mu_);
        alert_q = (int)alert_q_.size();
    }
    uint64_t reqs = http_req_total_.load(std::memory_order_relaxed);
    uint64_t us = http_us_total_.load(std::memory_order_relaxed);
    body += "# HELP rtunnel_admin_http_requests_total admin HTTP requests\n# TYPE rtunnel_admin_http_requests_total counter\n";
    body += "rtunnel_admin_http_requests_total " + std::to_string(reqs) + "\n";
    body += "# HELP rtunnel_admin_http_responses_total admin HTTP responses by status class\n# TYPE rtunnel_admin_http_responses_total counter\n";
    body += "rtunnel_admin_http_responses_total{class=\"2xx\"} " + std::to_string(http_req_2xx_.load()) + "\n";
    body += "rtunnel_admin_http_responses_total{class=\"4xx\"} " + std::to_string(http_req_4xx_.load()) + "\n";
    body += "rtunnel_admin_http_responses_total{class=\"5xx\"} " + std::to_string(http_req_5xx_.load()) + "\n";
    body += "# HELP rtunnel_admin_http_handler_inflight handlers currently executing\n# TYPE rtunnel_admin_http_handler_inflight gauge\n";
    body += "rtunnel_admin_http_handler_inflight " + std::to_string(http_inflight_.load()) + "\n";
    body += "# HELP rtunnel_admin_http_handler_seconds_total handler dispatch time\n# TYPE rtunnel_admin_http_handler_seconds_total counter\n";
    body += "rtunnel_admin_http_handler_seconds_total ";
    body += std::to_string((double)us / 1e6) + "\n";
    body += "# HELP rtunnel_admin_http_slow_total requests taking >= 1s to dispatch\n# TYPE rtunnel_admin_http_slow_total counter\n";
    body += "rtunnel_admin_http_slow_total " + std::to_string(http_slow_total_.load()) + "\n";
    body += "# HELP rtunnel_admin_db_queries_total SQLite statements executed\n# TYPE rtunnel_admin_db_queries_total counter\n";
    body += "rtunnel_admin_db_queries_total " + std::to_string(ds.queries) + "\n";
    body += "# HELP rtunnel_admin_db_seconds_total SQLite statement time\n# TYPE rtunnel_admin_db_seconds_total counter\n";
    body += "rtunnel_admin_db_seconds_total " + std::to_string((double)ds.total_us / 1e6) + "\n";
    body += "# HELP rtunnel_admin_db_max_seconds slowest SQLite statement\n# TYPE rtunnel_admin_db_max_seconds gauge\n";
    body += "rtunnel_admin_db_max_seconds " + std::to_string((double)ds.max_us / 1e6) + "\n";
    body += "# HELP rtunnel_admin_db_slow_total SQLite statements >= 100ms\n# TYPE rtunnel_admin_db_slow_total counter\n";
    body += "rtunnel_admin_db_slow_total " + std::to_string(ds.slow_queries) + "\n";
    body += "# HELP rtunnel_admin_alert_queue queued alert notifications\n# TYPE rtunnel_admin_alert_queue gauge\n";
    body += "rtunnel_admin_alert_queue " + std::to_string(alert_q) + "\n";
    body += "# HELP rtunnel_admin_dist_active active distribution tasks\n# TYPE rtunnel_admin_dist_active gauge\n";
    body += "rtunnel_admin_dist_active " + std::to_string(dist_active_count_.load()) + "\n";

    ctx->response->content_type = TEXT_PLAIN;
    ctx->response->body = body;
}
