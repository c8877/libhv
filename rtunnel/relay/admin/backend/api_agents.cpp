// A/C 连接列表与设备详情（在 relay 线程内取快照，JSON 序列化后回 admin 线程）
#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <algorithm>

using nlohmann::json;

static void fillConnCommon(RelayConn* c, json& j) {
    j["remote_addr"] = c->remote_addr;
    j["connected_at_ms"] = c->connected_at_ms;
    j["last_seen_ms"] = c->last_seen_ms;
    j["bytes_in"] = c->bytes_in;
    j["bytes_out"] = c->bytes_out;
    int nstream = 0;
    for (RelayStream* s = c->streams; s; s = (c->role == rtnp::ROLE_AGENT ? s->next_c : s->next_a)) nstream++;
    j["streams"] = nstream;
}

void AdminServer::handleAgents(const HttpContextPtr& ctx) {
    bool all = (currentActor().role == "admin" || currentActor().role == "manager");
    std::vector<std::string> mine = myDevids();
    std::string body = callRelay([this, all, mine]() {
        json arr = json::array();
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (c->role != rtnp::ROLE_AGENT || !c->authed) continue;
            if (!all && std::find(mine.begin(), mine.end(), std::string(c->devid)) == mine.end()) continue;
            json j;
            j["devid"] = c->devid;
            j["online"] = true;   // 注册表在册即在线
            fillConnCommon(c, j);
            arr.push_back(j);
        }
        return arr.dump();
    });
    ctx->send(body);
}

void AdminServer::handleAgentDetail(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    int found = 0;
    std::string body = callRelay([this, &devid, &found]() {
        RelayConn* c = relay_->findAgent(devid.c_str());
        if (c == NULL) return std::string("{}");
        found = 1;
        json j;
        j["devid"] = c->devid;
        j["online"] = true;
        fillConnCommon(c, j);
        j["streams"] = json::array();
        for (RelayStream* s = c->streams; s; s = s->next_c) {
            json st;
            st["a_user"] = s->a ? s->a->user : "";
            st["a_sid"] = s->a_sid;
            st["c_sid"] = s->c_sid;
            st["opened"] = (bool)s->opened;
            st["opened_at_ms"] = s->opened_at_ms;
            st["a_closed"] = (bool)s->a_closed;
            st["c_closed"] = (bool)s->c_closed;
            j["streams"].push_back(st);
        }
        return j.dump();
    });
    if (!found) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device not online\"}");
        return;
    }
    ctx->send(body);
}

void AdminServer::handleClients(const HttpContextPtr& ctx) {
    auto actor = currentActor();
    std::string body = callRelay([this, actor]() {
        json arr = json::array();
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (c->role != rtnp::ROLE_CLIENT || !c->authed || c->internal) continue;
            if ((actor.role == "operator" || actor.role == "viewer") && actor.username != c->user) continue;
            json j;
            j["user"] = c->user;
            fillConnCommon(c, j);
            arr.push_back(j);
        }
        return arr.dump();
    });
    ctx->send(body);
}
