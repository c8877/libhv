// 账号/会话/设备归属 API（SQLite）
#include "common/logging.hpp"
#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <ctime>
#include <mutex>
#include <unordered_map>

using nlohmann::json;

extern thread_local AdminActor tl_actor;

static std::string cookieVal(const HttpContextPtr& ctx, const std::string& key) {
    const HttpCookie& ck = ctx->request->GetCookie(key.c_str());
    if (!ck.name.empty()) return ck.value;
    std::string c = ctx->header("Cookie");
    size_t pos = 0;
    while (pos < c.size()) {
        size_t semi = c.find(';', pos);
        std::string item = c.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = item.substr(0, eq);
            while (!k.empty() && (k[0] == ' ' || k[0] == '\t')) k.erase(0, 1);
            if (k == key) return item.substr(eq + 1);
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return "";
}

// 登录失败限流：username -> (fails, lock_until)
static std::mutex g_login_mu;
static std::unordered_map<std::string, std::pair<int, time_t>> g_login_fail;

AdminActor AdminServer::currentActor() const { return tl_actor; }

namespace {
bool roleKnown(const std::string& r) {
    return r == "admin" || r == "manager" || r == "operator" || r == "viewer";
}
// actor 能管理的目标账号（admin 不可被管理）
bool canManage(const AdminActor& a, const DbUser& t) {
    if (t.role == "admin") return false;
    if (a.role == "admin") return roleKnown(t.role);
    if (a.role == "manager") return t.role == "operator" || t.role == "viewer";
    return false;
}
bool canCreateRole(const AdminActor& a, const std::string& r) {
    if (a.role == "admin") return r == "manager" || r == "operator" || r == "viewer";
    if (a.role == "manager") return r == "operator" || r == "viewer";
    return false;
}
} // namespace

std::vector<std::string> AdminServer::myDevids() {
    std::vector<std::string> v;
    if ((tl_actor.role == "operator" || tl_actor.role == "viewer") && db_.ok()) {
        db_.ownedDevids(tl_actor.id, v);
    }
    return v;
}

bool AdminServer::ownsDevid(const std::string& devid) {
    if (tl_actor.role != "operator" && tl_actor.role != "viewer") return true;
    long long owner = 0;
    if (!db_.ok() || !db_.deviceOwner(devid, owner)) return false;
    return owner == tl_actor.id;
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
    {
        std::lock_guard<std::mutex> lk(g_login_mu);
        auto it = g_login_fail.find(user);
        if (it != g_login_fail.end() && it->second.second > time(NULL)) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"locked, try later\"}");
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
        ctx->response->status_code = HTTP_STATUS_UNAUTHORIZED;
        ctx->send("{\"error\":\"bad username or password\"}");
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_login_mu);
        g_login_fail.erase(user);
    }
    std::string sid = AdminDb::randomHex(32);
    int hours = settingInt("auth.session_hours", 12);
    if (sid.empty() || !db_.createSession(AdminDb::sha256Hex(sid), u.id, AdminDb::nowMs() + (long long)hours * 3600 * 1000)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"session create failed\"}");
        return;
    }
    ctx->response->headers["Set-Cookie"] =
        "rt_sid=" + sid + "; Path=/; HttpOnly; SameSite=Strict; Max-Age=" + std::to_string(hours * 3600);
    json j;
    j["username"] = u.username;
    j["role"] = u.role;
    audit(u.username, "account.login", u.username);
    ctx->send(j.dump());
}

void AdminServer::handleLogout(const HttpContextPtr& ctx) {
    std::string sid = cookieVal(ctx, "rt_sid");
    if (!sid.empty() && db_.ok()) db_.dropSession(AdminDb::sha256Hex(sid));
    ctx->response->headers["Set-Cookie"] = "rt_sid=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleMe(const HttpContextPtr& ctx) {
    std::string role;
    if (requireRole(ctx, ROLE_VIEWER, role)) return;
    json j;
    j["id"] = tl_actor.id;
    j["username"] = tl_actor.username;
    j["role"] = tl_actor.role.empty() ? role : tl_actor.role;
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
    if (requireRole(ctx, ROLE_OPERATOR, role)) return;
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
    } else if (canManage(actor, target)) {
        // 上级可重置下级密码
    } else {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    std::string err;
    if (!db_.setPassword(id, newp, err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    db_.dropUserSessions(id);
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
    bool allowed = canManage(actor, target);
    if (!allowed || id == actor.id) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    db_.setStatus(id, status);
    if (status == "disabled") db_.dropUserSessions(id);
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
    bool allowed = canManage(actor, target);
    if (!allowed || id == actor.id) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    db_.removeUser(id);
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
    if (!canManage(actor, target) || id == actor.id || !canCreateRole(actor, role)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    if (!db_.setRole(id, role)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    audit(actor.username, "account.role", target.username + " -> " + role);
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDeviceList(const HttpContextPtr& ctx) {
    std::string role;
    if (requireRole(ctx, ROLE_OPERATOR, role)) return;
    std::vector<std::pair<std::string, long long>> ds;
    db_.deviceList(ds);
    std::vector<DbUser> us;
    db_.listUsers(us);
    std::unordered_map<long long, std::string> names;
    for (auto& u : us) names[u.id] = u.username;
    json arr = json::array();
    for (auto& d : ds) {
        json j;
        j["devid"] = d.first;
        j["owner_id"] = d.second;
        j["owner"] = d.second ? names[d.second] : "";
        arr.push_back(j);
    }
    ctx->send(arr.dump());
}

void AdminServer::handleDeviceBind(const HttpContextPtr& ctx, const AdminActor& actor) {
    if (actor.role != "admin" && actor.role != "manager") {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    std::string devid = ctx->param("devid");
    json req = json::parse(ctx->body(), nullptr, false);
    std::string owner = req.value("username", "");
    long long owner_id = 0;
    if (!owner.empty()) {
        DbUser u;
        if (!db_.findUser(owner, u)) {
            ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
            ctx->send("{\"error\":\"no such account\"}");
            return;
        }
        if (u.role == "superadmin") {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"cannot bind to superadmin\"}");
            return;
        }
        owner_id = u.id;
    }
    if (owner_id) db_.bindDevice(devid, owner_id);
    else db_.unbindDevice(devid);
    audit(actor.username, "device.bind", devid + " -> " + (owner.empty() ? "(none)" : owner));
    ctx->send("{\"ok\":true}");
}
