#include <algorithm>
// 账号/会话/设备归属 API（SQLite）
#include "common/logging.hpp"
#include "admin_server.hpp"
#include "admin_util.hpp"
#include "cpputil/json.hpp"

#include <ctime>
#include <mutex>
#include <unordered_map>

using nlohmann::json;

extern thread_local AdminActor tl_actor;


// 登录失败限流：username -> (fails, lock_until)
static std::mutex g_login_mu;
static std::unordered_map<std::string, std::pair<int, time_t>> g_login_fail;
static std::unordered_map<std::string, std::pair<int, time_t>> g_ip_fail;

AdminActor AdminServer::currentActor() const { return tl_actor; }

namespace {
bool roleKnown(const std::string& r) {
    return r == "admin" || r == "manager" || r == "operator" || r == "viewer";
}
// actor 能管理的目标账号（admin 不可被管理）；有 account.write 资源即可管理非超管账号
bool canManage(const AdminActor& a, const DbUser& t) {
    if (t.role == "admin") return false;
    if (a.role == "admin" || a.super) return roleKnown(t.role);
    if (a.perms.count("account.write")) return true;
    if (a.role == "manager") return t.role == "operator" || t.role == "viewer";
    return false;
}
bool canCreateRole(const AdminActor& a, const std::string& r) {
    if (r == "admin") return false;
    if (a.role == "admin" || a.super) return true;
    if (a.perms.count("account.write")) return r == "operator" || r == "viewer";
    if (a.role == "manager") return r == "operator" || r == "viewer";
    return false;
}
} // namespace

bool AdminServer::tokenGroupDevids(std::vector<std::string>& out) {
    // 令牌组：以 actor.token_scoped 表示
    if (!tl_actor.is_client && tl_actor.token_scoped) {
        out.assign(tl_actor.devices.begin(), tl_actor.devices.end());
        return true;
    }
    return false;
}

bool AdminServer::clientGroupDevids(std::vector<std::string>& out) {
    if (!tl_actor.is_client) return false;
    out.assign(tl_actor.devices.begin(), tl_actor.devices.end());
    return true;
}

bool AdminServer::tokenScoped() {
    return tl_actor.token_scoped || (tl_actor.is_client && tl_actor.token_id > 0);
}

// 只按设备列表过滤（全部设备 / 超级权限不限制）
bool AdminServer::scopeRestricted() {
    // 接入账号的「全部设备」只在租户内成立，列表仍要按租户过滤
    if (tl_actor.is_client) return true;
    return !(tl_actor.super || tl_actor.all_devices);
}

std::vector<std::string> AdminServer::myDevids() {
    std::vector<std::string> v;
    if (tl_actor.is_client) {
        std::vector<std::string> tdev;
        db_.tenantDevices(tl_actor.tenant_id, tdev);
        if (tl_actor.super || tl_actor.all_devices) return tdev;
        for (const auto& d : tdev)
            if (tl_actor.devices.count(d)) v.push_back(d);
        return v;
    }
    if (tl_actor.super || tl_actor.all_devices) return v;
    v.assign(tl_actor.devices.begin(), tl_actor.devices.end());
    return v;
}

bool AdminServer::ownsDevid(const std::string& devid) {
    if (tl_actor.is_client) {
        long long t = 0;
        if (!db_.deviceTenant(devid, t) || t != tl_actor.tenant_id) return false;
    }
    if (tl_actor.super || tl_actor.all_devices) return true;
    return tl_actor.devices.count(devid) > 0;
}

void AdminServer::handleSetupStatus(const HttpContextPtr& ctx) {
    json j;
    j["initialized"] = db_.ok() && db_.initialized();
    j["db"] = db_.ok();
    ctx->send(j.dump());
}

void AdminServer::handleSetup(const HttpContextPtr& ctx) {
    if (!db_.ok()) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db not available\"}");
        return;
    }
    if (db_.initialized()) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"already initialized\"}");
        return;
    }
    json req = json::parse(ctx->body(), nullptr, false);
    std::string token = req.value("token", "");
    std::string user = req.value("username", "");
    std::string pass = req.value("password", "");
    if (cfg_.token.empty() || token != cfg_.token) {
        ctx->response->status_code = HTTP_STATUS_UNAUTHORIZED;
        ctx->send("{\"error\":\"bad bootstrap token\"}");
        return;
    }
    std::string err;
    if (!db_.createUser(user, pass, "admin", err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    audit("setup", "account.create", "admin " + user);
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleLogin(const HttpContextPtr& ctx) {
    if (!db_.ok()) {
        ctx->response->status_code = HTTP_STATUS_SERVICE_UNAVAILABLE;
        ctx->send("{\"error\":\"db not available\"}");
        return;
    }
    json req = json::parse(ctx->body(), nullptr, false);
    std::string user = req.value("username", "");
    std::string pass = req.value("password", "");
    std::string ip = ctx->request->client_addr.ip;
    {
        std::lock_guard<std::mutex> lk(g_login_mu);
        // 失败表只用于短期锁定：定期清掉过期项，避免被随机用户名撑大
        if (g_login_fail.size() > 4096 || g_ip_fail.size() > 4096) {
            time_t now = time(NULL);
            for (auto i = g_login_fail.begin(); i != g_login_fail.end();) {
                if (i->second.second <= now) i = g_login_fail.erase(i);
                else ++i;
            }
            for (auto i = g_ip_fail.begin(); i != g_ip_fail.end();) {
                if (i->second.second <= now) i = g_ip_fail.erase(i);
                else ++i;
            }
            if (g_login_fail.size() > 8192) g_login_fail.clear();
            if (g_ip_fail.size() > 8192) g_ip_fail.clear();
        }
        auto it = g_login_fail.find(user);
        if (it != g_login_fail.end() && it->second.second > time(NULL)) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"locked, try later\"}");
            return;
        }
        auto ii = g_ip_fail.find(ip);
        if (ii != g_ip_fail.end() && ii->second.second > time(NULL)) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"too many attempts, try later\"}");
            return;
        }
    }
    DbUser u;
    std::string hash;
    bool ok = db_.findUser(user, u, &hash) && u.status == "active" &&
              AdminDb::verifyPassword(pass, hash);
    if (!ok) {
        std::lock_guard<std::mutex> lk(g_login_mu);
        auto& st = g_login_fail[user];
        int lf = settingInt("auth.lock_fails", 5);
        int lm = settingInt("auth.lock_minutes", 5);
        if (++st.first >= lf) { st.first = 0; st.second = time(NULL) + lm * 60; }
        if (!ip.empty()) {
            auto& is = g_ip_fail[ip];
            if (++is.first >= lf * 3) { is.first = 0; is.second = time(NULL) + lm * 60; }
        }
        ctx->response->status_code = HTTP_STATUS_UNAUTHORIZED;
        ctx->send("{\"error\":\"bad username or password\"}");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_login_mu);
        g_login_fail.erase(user);
        if (!ip.empty()) g_ip_fail.erase(ip);
    }
    std::string sid = AdminDb::randomHex(32);
    int hours = settingInt("auth.session_hours", 12);
    if (sid.empty() || !db_.createSession(AdminDb::sha256Hex(sid), u.id, AdminDb::nowMs() + (long long)hours * 3600 * 1000)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"session create failed\"}");
        return;
    }
    bool https = adminReqIsHttps(ctx);
    ctx->response->headers["Set-Cookie"] =
        "rt_sid=" + sid + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" +
        std::to_string(hours * 3600) + (https ? "; Secure" : "");
    if (https && ctx->request && ctx->request->scheme == "https") {
        // 本地 TLS 终止：顺便告诉浏览器以后只走 HTTPS
        ctx->response->headers["Strict-Transport-Security"] = "max-age=31536000";
    }
    json j;
    j["username"] = u.username;
    j["role"] = u.role;
    audit(u.username, "account.login", u.username);
    ctx->send(j.dump());
}

void AdminServer::handleLogout(const HttpContextPtr& ctx) {
    std::string sid = adminCookieValue(ctx, "rt_sid");
    if (!sid.empty() && db_.ok()) db_.dropSession(AdminDb::sha256Hex(sid));
    ctx->response->headers["Set-Cookie"] = "rt_sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0" +
        std::string(adminReqIsHttps(ctx) ? "; Secure" : "");
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleMe(const HttpContextPtr& ctx) {
    std::string role;
    if (requirePerm(ctx, "auth", role)) return;
    json j;
    j["id"] = tl_actor.id;
    j["username"] = tl_actor.username;
    j["role"] = tl_actor.role.empty() ? role : tl_actor.role;
    j["super"] = tl_actor.super;
    j["all_devices"] = tl_actor.all_devices;
    j["global_scope"] = tl_actor.global_scope;
    {
        // 只回传真正生效的资源：全局资源需要「全局权限」，否则前端会显示点不动的入口
        nlohmann::json ps = nlohmann::json::array();
        for (const auto& k : tl_actor.perms) {
            if (!tl_actor.super && permIsGlobal(k) && !tl_actor.global_scope) continue;
            ps.push_back(k);
        }
        j["perms"] = ps;
    }
    {
        std::map<std::string, std::string> p;
        db_.getUserPrefs(tl_actor.id, p);
        std::string dn = p.count("display_name") ? p["display_name"] : tl_actor.username;
        j["display_name"] = dn.empty() ? tl_actor.username : dn;
        j["avatar_color"] = p.count("avatar_color") ? p["avatar_color"] : "";
    }
    ctx->send(j.dump());
}

void AdminServer::handleUsers(const HttpContextPtr& ctx) {
    std::string role;
    if (requirePerm(ctx, "account.read", role)) return;
    std::vector<DbUser> us;
    db_.listUsers(us);
    json arr = json::array();
    for (auto& u : us) {
        if (tl_actor.role != "admin" && u.role != "operator" && u.role != "viewer") continue;
        json j;
        j["id"] = u.id;
        j["username"] = u.username;
        j["role"] = u.role;
        j["status"] = u.status;
        j["created_ms"] = u.created_ms;
        {
            std::vector<long long> gids;
            db_.permGroupsOfUser(u.id, gids);
            j["perm_groups"] = gids;
        }
        arr.push_back(j);
    }
    ctx->send(arr.dump());
}

void AdminServer::handleUserCreate(const HttpContextPtr& ctx, const AdminActor& actor) {
    json req = json::parse(ctx->body(), nullptr, false);
    std::string user = req.value("username", "");
    std::string pass = req.value("password", "");
    std::string role = req.value("role", "viewer");
    if (!canCreateRole(actor, role)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"cannot create this role\"}");
        return;
    }
    // 建账号 = 把该角色的内置组授予对方：组的授权面必须在自己范围内
    DbPermScope rs;
    if (db_.permRoleScope(role, rs)) {
        std::string why = grantDenied(rs);
        if (!why.empty()) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"" + why + "\"}");
            return;
        }
    }
    std::string err;
    if (!db_.createUser(user, pass, role, err)) {
        logf_("[admin] create user '%s' failed: %s", user.c_str(), err.c_str());
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    audit(actor.username, "account.create", role + " " + user);
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleUserPassword(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    std::string oldp = req.value("old_password", "");
    std::string newp = req.value("new_password", "");
    if ((int)newp.size() < settingInt("auth.password_min", 8)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"password too short\"}");
        return;
    }
    DbUser target;
    if (!db_.findUserById(id, target)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"no such user\"}");
        return;
    }
    if (actor.id == id) {
        DbUser me;
        std::string hash;
        if (!db_.findUser(actor.username, me, &hash) || !AdminDb::verifyPassword(oldp, hash)) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"old password wrong\"}");
            return;
        }
    } else {
        DbPermScope ts;
        db_.permScopeOfUser(id, ts);
        // 重置他人密码等同于接管账号：目标当前权限必须在自己的范围内
        if (!hasPerm("account.write") || !manageDenied(ts).empty()) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"forbidden\"}");
            return;
        }
    }
    std::string err;
    if (!db_.setPassword(id, newp, err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    db_.dropUserSessions(id);
    kickConsoleWsOfUser(id, "password changed");
    audit(actor.username, "account.password", target.username);
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleUserStatus(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    std::string status = req.value("status", "");
    if (status != "active" && status != "disabled") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad status\"}");
        return;
    }
    DbUser target;
    if (!db_.findUserById(id, target)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"no such user\"}");
        return;
    }
    DbPermScope ts;
    db_.permScopeOfUser(id, ts);
    bool allowed = canManage(actor, target) && manageDenied(ts).empty();
    if (!allowed || id == actor.id) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    db_.setStatus(id, status);
    if (status == "disabled") {
        db_.dropUserSessions(id);
        kickConsoleWsOfUser(id, "account disabled");
    }
    audit(actor.username, "account.status", target.username + " " + status);
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleUserDelete(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    DbUser target;
    if (!db_.findUserById(id, target)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"no such user\"}");
        return;
    }
    DbPermScope ts;
    db_.permScopeOfUser(id, ts);
    bool allowed = canManage(actor, target) && manageDenied(ts).empty();
    if (!allowed || id == actor.id) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    db_.removeUser(id);
    kickConsoleWsOfUser(id, "account deleted");
    audit(actor.username, "account.delete", target.username);
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleUserRole(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    std::string role = req.value("role", "");
    if (!roleKnown(role)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad role\"}");
        return;
    }
    DbUser target;
    if (!db_.findUserById(id, target)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"no such user\"}");
        return;
    }
    DbPermScope before;
    db_.permScopeOfUser(id, before);
    DbPermScope rs;
    bool have_role_scope = db_.permRoleScope(role, rs);
    std::string why = manageDenied(before);
    if (why.empty() && have_role_scope) why = grantDenied(rs);
    if (!canManage(actor, target) || id == actor.id || !canCreateRole(actor, role) || !why.empty()) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"" + (why.empty() ? std::string("forbidden") : why) + "\"}");
        return;
    }
    if (!db_.setRole(id, role)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    audit(actor.username, "account.role", target.username + " -> " + role);
    // 角色决定未分组时的回落权限，改完让该账号的 WS 重新握手
    kickConsoleWsOfUser(id, "role changed");
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDeviceList(const HttpContextPtr& ctx) {
    std::string role;
    if (requirePerm(ctx, "device.view", role)) return;
    std::vector<std::string> ds;
    db_.deviceList(ds);
    json arr = json::array();
    for (auto& d : ds) {
        if (scopeRestricted() && !ownsDevid(d)) continue;
        json j;
        j["devid"] = d;
        arr.push_back(j);
    }
    ctx->send(arr.dump());
}

