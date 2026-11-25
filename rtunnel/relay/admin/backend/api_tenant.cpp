// 租户 API：计费与设备归属边界。默认租户 id=1，升级前的数据都在它下面。
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/json.hpp"

#include <string>

using nlohmann::json;

extern thread_local AdminActor tl_actor;

void AdminServer::handleTenants(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.tenantList(out)) { apiBad(ctx, "db error"); return; }
    // 接入令牌只看得到自己所属的租户，避免跨租户泄露名称与设备规模
    if (!tl_actor.is_client) { ctx->send(out); return; }
    json doc = json::parse(out, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) { apiBad(ctx, "db error"); return; }
    json arr = json::array();
    for (auto& t : doc.value("items", json::array())) {
        if (t.value("id", (long long)0) == tl_actor.tenant_id) arr.push_back(t);
    }
    json resp;
    resp["items"] = arr;
    ctx->send(resp.dump());
}

void AdminServer::handleTenantCreate(const HttpContextPtr& ctx, const std::string& role) {
    if (tl_actor.is_client) { apiError(ctx, HTTP_STATUS_FORBIDDEN, "接入令牌不能管理租户"); return; }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    long long id = 0;
    std::string err;
    if (!db_.tenantCreate(req.value("name", ""), req.value("note", ""), actorLabel(), id, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "tenant.create", std::to_string(id) + " " + req.value("name", ""));
    json out;
    out["ok"] = true;
    out["id"] = id;
    ctx->send(out.dump());
}

void AdminServer::handleTenantUpdate(const HttpContextPtr& ctx, const std::string& role) {
    if (tl_actor.is_client) { apiError(ctx, HTTP_STATUS_FORBIDDEN, "接入令牌不能管理租户"); return; }
    long long id = atoll(ctx->param("id").c_str());
    if (id <= 0) { apiBad(ctx, "bad id"); return; }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    // 缺省字段表示保持原值（-1），避免只改名称/状态时把套餐清空
    int max_devices = -1;
    int max_sessions = -1;
    long long quota_bytes = -1;
    if (req.contains("max_devices") && req["max_devices"].is_number())
        max_devices = req["max_devices"].get<int>();
    if (req.contains("max_sessions") && req["max_sessions"].is_number())
        max_sessions = req["max_sessions"].get<int>();
    if (req.contains("quota_bytes") && req["quota_bytes"].is_number())
        quota_bytes = req["quota_bytes"].get<long long>();
    else if (req.contains("quota_gb") && req["quota_gb"].is_number())
        quota_bytes = (long long)(req["quota_gb"].get<double>() * 1073741824.0 + 0.5);
    std::string err;
    if (!db_.tenantUpdate(id, req.value("name", ""), req.value("note", ""),
                          req.value("status", std::string("active")), max_devices, max_sessions,
                          quota_bytes, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "tenant.update", std::to_string(id) + " " + req.value("status", std::string("active")));
    // 停用/启用租户会改变其下全部账号的有效状态，重新下发令牌策略（停用会断开在线连接）
    refreshTokenCache();
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}
