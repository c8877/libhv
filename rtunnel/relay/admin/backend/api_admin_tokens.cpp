// 管理台账号 API Token（atk-）API。
// 归属账号沿用 users 表；设备范围通过设备分组绑定（subject_groups.subject_type='atk'）。
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/json.hpp"

#include <cstdlib>

using nlohmann::json;

namespace {

// 不能拿 API Token 再去管理 Token，避免凭据自我复制
bool viaToken(const AdminActor& a) { return a.token_id > 0; }

bool canTouch(const AdminActor& a, long long owner_id) {
    if (viaToken(a)) return false;
    if (a.role == "admin") return true;
    return a.id == owner_id;   // manager 只能管理自己的 Token
}

long long expiresFrom(const json& req, long long fallback) {
    if (req.contains("expires_at_ms") && req["expires_at_ms"].is_number())
        return req["expires_at_ms"].get<long long>();
    if (req.contains("expires_in_days") && req["expires_in_days"].is_number()) {
        long long days = req["expires_in_days"].get<long long>();
        return days > 0 ? AdminDb::nowMs() + days * 86400000LL : 0;
    }
    return fallback;
}

} // namespace

void AdminServer::handleAdminTokens(const HttpContextPtr& ctx, const AdminActor& actor) {
    if (viaToken(actor)) { apiError(ctx, HTTP_STATUS_FORBIDDEN, "API Token 不能管理 Token"); return; }
    long long uid = actor.role == "admin" ? 0 : actor.id;
    std::string out;
    if (!db_.adminTokenList(uid, ctx->param("q"), out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleAdminTokenCreate(const HttpContextPtr& ctx, const AdminActor& actor) {
    if (viaToken(actor)) { apiError(ctx, HTTP_STATUS_FORBIDDEN, "API Token 不能创建 Token"); return; }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }

    long long target = req.value("user_id", 0LL);
    if (target <= 0) target = actor.id;
    if (actor.role != "admin" && target != actor.id) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "只能为自己的账号创建 Token");
        return;
    }
    DbUser u;
    if (!db_.findUserById(target, u)) { apiNotFound(ctx, "账号不存在"); return; }
    if (actor.role != "admin" && u.role == "admin") {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能为管理员账号创建 Token");
        return;
    }

    DbTokenSpec spec;
    spec.name = req.value("name", "");
    spec.expires_at_ms = expiresFrom(req, 0);
    std::string plain, out, err;
    if (!db_.adminTokenCreate(target, u.username, spec, actor.username,
                              settingInt("admin_token.max_per_user", 20), plain, out, err)) {
        apiBad(ctx, err);
        return;
    }
    json j = json::parse(out, nullptr, false);
    if (j.is_discarded()) j = json::object();
    audit(actor.username, "admin_token.create",
          "user=" + u.username + " name=" + spec.name + " prefix=" + j.value("token_prefix", ""));
    j["token"] = plain;   // 仅此一次返回明文
    ctx->send(j.dump());
}

void AdminServer::handleAdminTokenUpdate(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    DbTokenSpec spec;
    long long owner = 0;
    std::string uname, status, err;
    if (!db_.adminTokenSpec(id, spec, owner, uname, &status, err)) { apiNotFound(ctx, err); return; }
    if (!canTouch(actor, owner)) { apiError(ctx, HTTP_STATUS_FORBIDDEN, "无权管理该 Token"); return; }
    if (status == "revoked") { apiBad(ctx, "Token 已吊销，只能轮换或删除"); return; }

    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    if (req.contains("name") && req["name"].is_string()) spec.name = req["name"].get<std::string>();
    spec.expires_at_ms = expiresFrom(req, spec.expires_at_ms);
    if (req.contains("status") && req["status"].is_string()) {
        std::string s = req["status"].get<std::string>();
        if (s != "active" && s != "disabled") { apiBad(ctx, "bad status"); return; }
        status = s;
    }
    if (!db_.adminTokenUpdate(id, spec, status, err)) { apiNotFound(ctx, err); return; }
    audit(actor.username, "admin_token.update", "user=" + uname + " id=" + std::to_string(id) +
          " status=" + status);
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleAdminTokenRotate(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    DbTokenSpec spec;
    long long owner = 0;
    std::string uname, status, err;
    if (!db_.adminTokenSpec(id, spec, owner, uname, &status, err)) { apiNotFound(ctx, err); return; }
    if (!canTouch(actor, owner)) { apiError(ctx, HTTP_STATUS_FORBIDDEN, "无权管理该 Token"); return; }
    if (status == "revoked") { apiBad(ctx, "Token 已吊销，不能轮换"); return; }

    std::string plain, out;
    if (!db_.adminTokenRotate(id, plain, out, err)) { apiNotFound(ctx, err); return; }
    json j = json::parse(out, nullptr, false);
    if (j.is_discarded()) j = json::object();
    audit(actor.username, "admin_token.rotate",
          "user=" + uname + " id=" + std::to_string(id) + " prefix=" + j.value("token_prefix", ""));
    j["token"] = plain;
    ctx->send(j.dump());
}

void AdminServer::handleAdminTokenDelete(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    DbTokenSpec spec;
    long long owner = 0;
    std::string uname, status, err;
    if (!db_.adminTokenSpec(id, spec, owner, uname, &status, err)) { apiNotFound(ctx, err); return; }
    if (!canTouch(actor, owner)) { apiError(ctx, HTTP_STATUS_FORBIDDEN, "无权管理该 Token"); return; }
    if (!db_.adminTokenDelete(id, err)) { apiNotFound(ctx, err); return; }
    audit(actor.username, "admin_token.revoke", "user=" + uname + " id=" + std::to_string(id));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleAdminTokenGroups(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    DbTokenSpec spec;
    long long owner = 0;
    std::string uname, status, err;
    if (!db_.adminTokenSpec(id, spec, owner, uname, &status, err)) { apiNotFound(ctx, err); return; }
    if (!canTouch(actor, owner)) { apiError(ctx, HTTP_STATUS_FORBIDDEN, "无权查看该 Token"); return; }
    std::string out;
    if (!db_.adminTokenGroups(id, out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleAdminTokenGroupsSave(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    DbTokenSpec spec;
    long long owner = 0;
    std::string uname, status, err;
    if (!db_.adminTokenSpec(id, spec, owner, uname, &status, err)) { apiNotFound(ctx, err); return; }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::vector<long long> ids;
    if (req.contains("groups")) {
        for (auto& v : req["groups"]) if (v.is_number_integer()) ids.push_back(v.get<long long>());
    }
    if (!db_.adminTokenSetGroups(id, ids, err)) { apiBad(ctx, err); return; }
    audit(actor.username, "admin_token.groups",
          "user=" + uname + " id=" + std::to_string(id) + " groups=" + std::to_string(ids.size()));
    ctx->send("{\"ok\":true}");
}
