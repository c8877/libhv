// 接入账号与 Token API
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "common/token_util.hpp"
#include "cpputil/json.hpp"

#include <cstdlib>

using nlohmann::json;

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
    ctx->send(out);
}

void AdminServer::handleClientAccountGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.clientAccountGet(id, out, err)) { apiNotFound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handleClientAccountCreate(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string username = req.value("username", "");
    long long id = 0;
    std::string err;
    if (!db_.clientAccountCreate(username, req.value("display_name", ""), req.value("note", ""),
                                 role, id, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "client_account.create", username);
    refreshTokenCache();
    json out; out["id"] = id; out["username"] = username;
    ctx->send(out.dump());
}

void AdminServer::handleClientAccountUpdate(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string status = req.value("status", "active");
    std::string err;
    if (!db_.clientAccountUpdate(id, req.value("display_name", ""), req.value("note", ""), status, err)) {
        apiNotFound(ctx, err);
        return;
    }
    audit(role, "client_account.update", std::to_string(id) + " status=" + status);
    refreshTokenCache();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientAccountDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.clientAccountDelete(id, err)) { apiNotFound(ctx, err); return; }
    audit(role, "client_account.delete", std::to_string(id));
    refreshTokenCache();
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientTokens(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.clientTokenList(id, out, err)) { apiBad(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handleClientTokenCreate(const HttpContextPtr& ctx, const std::string& role) {
    long long account_id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
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
    spec = specFromJson(req, spec);
    // 未显式传 status 时保持原状态，避免改限额时把停用的 Token 意外启用
    if (req.contains("status") && req["status"].is_string()) status = req["status"].get<std::string>();
    if (!db_.clientTokenUpdate(id, spec, status, err)) { apiNotFound(ctx, err); return; }
    audit(role, "client_token.update", std::to_string(id) + " status=" + status);
    refreshTokenCache();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientTokenRotate(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string plain, out, err;
    if (!db_.clientTokenRotate(id, plain, out, err)) { apiNotFound(ctx, err); return; }
    audit(role, "client_token.rotate", std::to_string(id));
    refreshTokenCache();
    json j = json::parse(out, nullptr, false);
    if (j.is_discarded()) j = json::object();
    j["token"] = plain;
    ctx->send(j.dump());
}

void AdminServer::handleClientTokenDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.clientTokenDelete(id, err)) { apiNotFound(ctx, err); return; }
    audit(role, "client_token.delete", std::to_string(id));
    refreshTokenCache();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientAccountEffective(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string aj, err;
    if (!db_.clientAccountGet(id, aj, err)) { apiNotFound(ctx, err); return; }
    json a = json::parse(aj, nullptr, false);
    std::string user = a.is_discarded() ? "" : a.value("username", "");
    std::string out;
    if (!db_.permEffectiveClient(user, out, err)) { apiBad(ctx, err); return; }
    ctx->send(out);
}
