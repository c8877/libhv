// 会话列表与概览（在 relay 线程内取快照）
#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <algorithm>
#include "hversion.h"

using nlohmann::json;

void AdminServer::handleStreams(const HttpContextPtr& ctx) {
    bool all = (currentActor().role == "admin" || currentActor().role == "manager");
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
                arr.push_back(st);
            }
        }
        return arr.dump();
    });
    ctx->send(body);
}

void AdminServer::handleOverview(const HttpContextPtr& ctx) {
    std::string body = callRelay([this]() {
        json j;
        int nagent = 0, nclient = 0, nstream = 0;
        uint64_t in = 0, out = 0;
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (!c->authed) continue;
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
        return j.dump();
    });
    ctx->send(body);
}

// Prometheus 文本指标（text/plain）
void AdminServer::handleMetrics(const HttpContextPtr& ctx) {
    std::string body = callRelay([this]() {
        int nagent = 0, nclient = 0, nstream = 0;
        uint64_t in = 0, out = 0;
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (!c->authed) continue;
            if (c->role == rtnp::ROLE_AGENT) {
                nagent++;
                for (RelayStream* s = c->streams; s; s = s->next_c) nstream++;
            } else if (c->role == rtnp::ROLE_CLIENT) {
                if (!c->internal) nclient++;
            }
            in += c->bytes_in;
            out += c->bytes_out;
        }
        uint64_t uptime = (hloop_now_ms(relay_->loop) - relay_->started_at_ms) / 1000;
        std::string s;
        s += "# HELP rtunnel_agents online agents\n# TYPE rtunnel_agents gauge\n";
        s += "rtunnel_agents " + std::to_string(nagent) + "\n";
        s += "# HELP rtunnel_clients online clients\n# TYPE rtunnel_clients gauge\n";
        s += "rtunnel_clients " + std::to_string(nclient) + "\n";
        s += "# HELP rtunnel_streams active streams\n# TYPE rtunnel_streams gauge\n";
        s += "rtunnel_streams " + std::to_string(nstream) + "\n";
        s += "# HELP rtunnel_bytes_in_total bytes received from peers\n# TYPE rtunnel_bytes_in_total counter\n";
        s += "rtunnel_bytes_in_total " + std::to_string(in) + "\n";
        s += "# HELP rtunnel_bytes_out_total bytes forwarded to peers\n# TYPE rtunnel_bytes_out_total counter\n";
        s += "rtunnel_bytes_out_total " + std::to_string(out) + "\n";
        s += "# HELP rtunnel_uptime_seconds relay uptime\n# TYPE rtunnel_uptime_seconds gauge\n";
        s += "rtunnel_uptime_seconds " + std::to_string(uptime) + "\n";
        s += "# HELP rtunnel_revoked_identities revoked SAN count\n# TYPE rtunnel_revoked_identities gauge\n";
        s += "rtunnel_revoked_identities " + std::to_string(relay_->revoked_sans.size()) + "\n";
        return s;
    });
    ctx->response->content_type = TEXT_PLAIN;
    ctx->response->body = body;
}
