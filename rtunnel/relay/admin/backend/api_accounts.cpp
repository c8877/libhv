// 接入账号与 Token API
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "common/token_util.hpp"
#include "cpputil/json.hpp"

#include <cstdlib>

using nlohmann::json;

extern thread_local AdminActor tl_actor;

namespace {
// 从请求体读取 token 限额；缺省字段保留 def
DbTokenSpec specFromJson(const json& req, DbTokenSpec def) {
    if (!req.is_object()) return def;
    if (req.contains("name") && req["name"].is_string()) def.name = req["name"].get<std::string>();
    if (req.contains("expires_at_ms") && req["expires_at_ms"].is_number())
        def.expires_at_ms = req["expires_at_ms"].get<long long>();
    if (req.contains("expires_in_days") && req["expires_in_days"].is_number()) {
        long long days = req["expires_in_days"].get<long long>();
        def.expires_at_ms = days > 0 ? AdminDb::nowMs() + days * 86400000LL : 0;
    }
    if (req.contains("quota_gb") && req["quota_gb"].is_number()) {
        def.quota_bytes = (long long)(req["quota_gb"].get<double>() * 1024.0 * 1024.0 * 1024.0);
    }
    if (req.contains("quota_bytes") && req["quota_bytes"].is_number())
        def.quota_bytes = req["quota_bytes"].get<long long>();
    if (req.contains("quota_period") && req["quota_period"].is_string())
        def.quota_period = req["quota_period"].get<std::string>();
    if (req.contains("rate_per_min") && req["rate_per_min"].is_number())
        def.rate_per_min = req["rate_per_min"].get<int>();
    if (req.contains("burst") && req["burst"].is_number())
        def.burst = req["burst"].get<int>();
    if (req.contains("max_sessions") && req["max_sessions"].is_number())
        def.max_sessions = req["max_sessions"].get<int>();
    if (req.contains("rate_kbps") && req["rate_kbps"].is_number())
        def.rate_kbps = req["rate_kbps"].get<int>();
    if (req.contains("quota_action") && req["quota_action"].is_string())
        def.quota_action = req["quota_action"].get<std::string>();
    return def;
}
} // namespace

void AdminServer::handleClientAccounts(const HttpContextPtr& ctx) {
    std::string q = ctx->param("q");
    std::string out;
    if (!db_.clientAccountList(q, out)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    if (tl_actor.is_client) {
        // 接入令牌只看得到同租户账号
        json arr = json::parse(out, nullptr, false);
        if (!arr.is_discarded() && arr.is_array()) {
            json keep = json::array();
            for (auto& a : arr)
                if (a.value("tenant_id", (long long)1) == tl_actor.tenant_id) keep.push_back(a);
            ctx->send(keep.dump());
            return;
        }
    }
    ctx->send(out);
}

void AdminServer::handleClientAccountGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string td = accountTenantDenied(id);
    if (!td.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, td); return; }
    std::string out, err;
    if (!db_.clientAccountGet(id, out, err)) { apiNotFound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handleClientAccountCreate(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string username = req.value("username", "");
    long long tenant_id = req.value("tenant_id", (long long)1);
    // 接入令牌只能在自租户内建账号，不能通过 tenant_id 跳到别的租户
    if (tl_actor.is_client) tenant_id = tl_actor.tenant_id;
    if (!db_.tenantExists(tenant_id)) { apiBad(ctx, "租户不存在"); return; }
    long long id = 0;
    std::string err;
    if (!db_.clientAccountCreate(username, req.value("display_name", ""), req.value("note", ""),
                                 tenant_id, role, id, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "client_account.create", username);
    refreshTokenCache();
    refreshGroupAcl();
    json out; out["id"] = id; out["username"] = username;
    ctx->send(out.dump());
}

void AdminServer::handleClientAccountUpdate(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string denied = accountTenantDenied(id);
    if (denied.empty()) denied = currentActor().super ? std::string() : accountDenied(id);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    std::string username, cur_status = "active";
    long long cur_tenant = 1;
    {
        std::string aj, aerr;
        if (db_.clientAccountGet(id, aj, aerr)) {
            auto a = json::parse(aj, nullptr, false);
            if (!a.is_discarded()) {
                username = a.value("username", std::string());
                cur_status = a.value("status", std::string("active"));
                cur_tenant = a.value("tenant_id", (long long)1);
            }
        }
    }
    // 未显式传 status 时保持原状态，避免改备注把停用账号放回来
    std::string status = cur_status;
    if (req.contains("status") && req["status"].is_string()) status = req["status"].get<std::string>();
    long long tenant_id = req.value("tenant_id", cur_tenant);
    if (tl_actor.is_client) tenant_id = tl_actor.tenant_id;
    if (!db_.tenantExists(tenant_id)) { apiBad(ctx, "租户不存在"); return; }
    std::string err;
    if (!db_.clientAccountUpdate(id, req.value("display_name", ""), req.value("note", ""), status,
                                 tenant_id, err)) {
        apiNotFound(ctx, err);
        return;
    }
    audit(role, "client_account.update", std::to_string(id) + " status=" + status);
    refreshTokenCache();
    refreshGroupAcl();
    if (status != "active" && !username.empty())
        kickClients({username}, -1, "account disabled");
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientAccountDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string denied = accountTenantDenied(id);
    if (denied.empty()) denied = currentActor().super ? std::string() : accountDenied(id);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    std::string username;
    {
        std::string aj, aerr;
        if (db_.clientAccountGet(id, aj, aerr)) {
            auto a = json::parse(aj, nullptr, false);
            if (!a.is_discarded()) username = a.value("username", std::string());
        }
    }
    std::string err;
    if (!db_.clientAccountDelete(id, err)) { apiNotFound(ctx, err); return; }
    audit(role, "client_account.delete", std::to_string(id));
    refreshTokenCache();
    refreshGroupAcl();
    if (!username.empty()) kickClients({username}, -1, "account deleted");
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientAccountKick(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string denied = accountTenantDenied(id);
    if (denied.empty()) denied = currentActor().super ? std::string() : accountDenied(id);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    std::string aj, err;
    if (!db_.clientAccountGet(id, aj, err)) { apiNotFound(ctx, err); return; }
    auto a = json::parse(aj, nullptr, false);
    std::string username = a.is_discarded() ? "" : a.value("username", std::string());
    if (username.empty()) { apiBad(ctx, "account not found"); return; }
    int n = kickClients({username}, -1, "manual kick");
    audit(role, "client_account.kick", username + " closed=" + std::to_string(n));
    json out;
    out["ok"] = true;
    out["closed"] = n;
    ctx->send(out.dump());
}

void AdminServer::handleClientTokens(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string td = accountTenantDenied(id);
    if (!td.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, td); return; }
    std::string out, err;
    if (!db_.clientTokenList(id, out, err)) { apiBad(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handleClientTokenCreate(const HttpContextPtr& ctx, const std::string& role) {
    long long account_id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    // 令牌继承归属账号的权限：账号超出自己范围就不能给它签令牌
    std::string denied = accountTenantDenied(account_id);
    if (denied.empty()) denied = currentActor().super ? std::string() : accountDenied(account_id);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    DbTokenSpec spec = specFromJson(req, DbTokenSpec{});
    std::string plain, out, err;
    if (!db_.clientTokenCreate(account_id, spec, role, settingInt("token.max_per_account", 20),
                               plain, out, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "client_token.create", "account=" + std::to_string(account_id) +
          " name=" + spec.name);
    refreshTokenCache();
    refreshGroupAcl();
    json j = json::parse(out, nullptr, false);
    if (j.is_discarded()) j = json::object();
    j["token"] = plain;   // 仅此一次返回明文
    ctx->send(j.dump());
}

void AdminServer::handleClientTokenUpdate(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    // 更新时未提供的字段保持原值：先读当前值
    DbTokenSpec spec;
    long long account_id = 0;
    std::string status, err;
    if (!db_.clientTokenSpec(id, spec, account_id, &status, err)) { apiNotFound(ctx, err); return; }
    std::string denied = accountTenantDenied(account_id);
    if (denied.empty()) denied = currentActor().super ? std::string() : accountDenied(account_id);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    spec = specFromJson(req, spec);
    // 未显式传 status 时保持原状态，避免改限额时把停用的 Token 意外启用
    if (req.contains("status") && req["status"].is_string()) status = req["status"].get<std::string>();
    if (!db_.clientTokenUpdate(id, spec, status, err)) { apiNotFound(ctx, err); return; }
    audit(role, "client_token.update", std::to_string(id) + " status=" + status);
    refreshTokenCache();
    refreshGroupAcl();
    // 令牌状态或权限组可能已变：已有会话必须重新认证
    kickClients({}, id, "token updated");
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientTokenRotate(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    DbTokenSpec spec;
    long long account_id = 0;
    std::string err;
    if (!db_.clientTokenSpec(id, spec, account_id, nullptr, err)) { apiNotFound(ctx, err); return; }
    // 轮换会返回新明文，等价于重新签发：同样要求归属账号在自己范围内
    std::string denied = accountTenantDenied(account_id);
    if (denied.empty()) denied = currentActor().super ? std::string() : accountDenied(account_id);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    std::string plain, out;
    if (!db_.clientTokenRotate(id, plain, out, err)) { apiNotFound(ctx, err); return; }
    audit(role, "client_token.rotate", std::to_string(id));
    refreshTokenCache();
    refreshGroupAcl();
    kickClients({}, id, "token rotated");
    json j = json::parse(out, nullptr, false);
    if (j.is_discarded()) j = json::object();
    j["token"] = plain;
    ctx->send(j.dump());
}

void AdminServer::handleClientTokenDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    DbTokenSpec spec;
    long long account_id = 0;
    std::string err;
    if (!db_.clientTokenSpec(id, spec, account_id, nullptr, err)) { apiNotFound(ctx, err); return; }
    std::string denied = accountTenantDenied(account_id);
    if (denied.empty()) denied = currentActor().super ? std::string() : accountDenied(account_id);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    if (!db_.clientTokenDelete(id, err)) { apiNotFound(ctx, err); return; }
    audit(role, "client_token.delete", std::to_string(id));
    refreshTokenCache();
    refreshGroupAcl();
    kickClients({}, id, "token revoked");
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientAccountEffective(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string td = accountTenantDenied(id);
    if (!td.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, td); return; }
    std::string aj, err;
    if (!db_.clientAccountGet(id, aj, err)) { apiNotFound(ctx, err); return; }
    json a = json::parse(aj, nullptr, false);
    std::string user = a.is_discarded() ? "" : a.value("username", "");
    std::string out;
    if (!db_.permEffectiveClient(user, out, err)) { apiBad(ctx, err); return; }
    ctx->send(out);
}
