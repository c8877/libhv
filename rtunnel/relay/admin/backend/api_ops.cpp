// 操作面：踢会话 / 设备下线 / 审计查询
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/json.hpp"

#include <signal.h>

using nlohmann::json;

void AdminServer::handleKickStream(const HttpContextPtr& ctx, const std::string& role) {
    uint32_t a_sid = (uint32_t)atoi(ctx->param("a_sid").c_str());
    bool all = !scopeRestricted();
    std::vector<std::string> mine = myDevids();
    int found = 0;
    callRelay([this, a_sid, &found, all, mine]() {
        for (RelayConn* ag = relay_->agents; ag; ag = ag->reg_next) {
            for (RelayStream* s = ag->streams; s; s = s->next_c) {
                if (s->a_sid != a_sid) continue;
                if (!all && std::find(mine.begin(), mine.end(), std::string(ag->devid)) == mine.end()) {
                    found = 2;
                    return std::string();
                }
                found = relay_->kickStream(a_sid) ? 1 : 0;
                return std::string();
            }
        }
        return std::string();
    });
    if (found == 2) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    if (!found) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"stream not found\"}");
        return;
    }
    audit(role, "streams.kick", "a_sid=" + std::to_string(a_sid));
    json j;
    j["a_sid"] = a_sid;
    j["kicked"] = true;
    ctx->send(j.dump());
}

void AdminServer::handleKickAgent(const HttpContextPtr& ctx, const std::string& role) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    int found = 0;
    callRelay([this, &devid, &found]() {
        found = relay_->kickAgent(devid.c_str()) ? 1 : 0;
        return std::string();
    });
    if (!found) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device not online\"}");
        return;
    }
    audit(role, "agents.kick", devid);
    json j;
    j["devid"] = devid;
    j["kicked"] = true;
    ctx->send(j.dump());
}

void AdminServer::handleAudit(const HttpContextPtr& ctx) {
    // 默认最近 200 条；页面一次要一屏以上的历史时传 limit（上限 ring 的 1000 条）
    int limit = atoi(ctx->param("limit").c_str());
    if (limit <= 0) limit = 200;
    if (limit > 1000) limit = 1000;
    ctx->send(audit_.tailJson(limit));
}

void AdminServer::handleShutdown(const HttpContextPtr& ctx, const std::string& role) {
    audit(role, "shutdown", "requested");
    ctx->send("{\"draining\":true}");
    // 在 relay 线程复用 SIGTERM 的 drain 路径；drain 期间 admin 线程仍可写回响应
    callRelay([this]() {
        relay_->onShutdown(SIGTERM);
        return std::string();
    });
}
