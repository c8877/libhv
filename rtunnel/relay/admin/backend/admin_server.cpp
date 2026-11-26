#include "admin_server.hpp"
#include "admin_tls.hpp"
#include "admin_util.hpp"
#include "admin_web_assets.hpp"
#include "common/logging.hpp"
#include "common/token_util.hpp"
#include "cpputil/json.hpp"

using nlohmann::json;
#include <thread>
#include <chrono>
#include <cctype>

#include <chrono>
#include <future>
#include <memory>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ---------------------------------------------------------------- 跨线程调用

// 在 relay 线程执行 fn 并同步等结果。relay 状态只在 relay 线程被访问。
std::string AdminServer::callRelay(std::function<std::string()> fn) {
    if (relay_->loop == NULL) return "{\"error\":\"relay not running\"}";

    // fn 可能按引用捕获调用方栈上的变量（含输出参数）。等待超时后调用方会返回，
    // 此时若 relay 线程再执行 fn 就是悬空引用；用 abandoned 标记让回调跳过它。
    // 回调与超时路径用同一把锁互斥：fn 已经开始执行时，超时方必须等它结束才能返回。
    struct Task {
        std::function<std::string()> fn;
        std::promise<std::string>    promise;
        std::mutex                   mu;
        bool                         abandoned = false;
        bool                         done = false;
    };
    auto task = std::make_shared<Task>();
    task->fn = std::move(fn);
    std::future<std::string> fut = task->promise.get_future();

    // holder 保证事件被执行前 task 一直存活（即使等待方超时返回）
    auto* holder = new std::shared_ptr<Task>(task);
    hevent_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = (hevent_type_e)(HEVENT_TYPE_CUSTOM + 1);
    ev.cb = [](hevent_t* e) {
        auto* holder = (std::shared_ptr<Task>*)e->userdata;
        Task* t = holder->get();
        std::string result;
        {
            std::lock_guard<std::mutex> lk(t->mu);
            if (t->abandoned) {
                result = "{\"error\":\"relay timeout\"}";
            } else {
                try {
                    result = t->fn();
                } catch (...) {
                    result = "{\"error\":\"internal\"}";
                }
                t->done = true;
            }
        }
        t->promise.set_value(result);
        delete holder;
    };
    ev.userdata = holder;
    hloop_post_event(relay_->loop, &ev);

    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        std::unique_lock<std::mutex> lk(task->mu);
        if (!task->done) {
            task->abandoned = true;
            return "{\"error\":\"relay timeout\"}";
        }
        // 刚好在超时与取锁之间完成，返回真实结果
        lk.unlock();
        return fut.get();
    }
    return fut.get();
}

// ---------------------------------------------------------------- 鉴权与角色

thread_local AdminActor tl_actor;


std::string AdminServer::roleOf(const HttpContextPtr& ctx) {
    tl_actor = AdminActor{};
    if (ctx->request) tl_actor.ip = ctx->request->client_addr.ip;
    // AI 调用方用 X-Actor: ai:<模型/会话> 自报身份，只进审计标签，不参与授权。
    // 限制前缀与长度，避免被伪造成普通账号名。
    {
        std::string xa = ctx->header("X-Actor");
        if (xa.rfind("ai:", 0) == 0 && xa.size() <= 80) tl_actor.ai_actor = xa;
    }

    // 1) 会话 Cookie
    std::string sid = adminCookieValue(ctx, "rt_sid");
    if (!sid.empty() && db_.ok()) {
        DbUser u;
        bool got = db_.getSession(AdminDb::sha256Hex(sid), u, AdminDb::nowMs());
        if (got) {
            tl_actor.id = u.id;
            tl_actor.username = u.username;
            tl_actor.role = u.role;
            loadActorPerms();
            return u.role;
        }
    }

    // 2) 管理台 API Token（atk-）：不受数据库初始化状态影响
    bool initialized = db_.ok() && db_.initialized();
    std::string auth = ctx->header("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.compare(0, prefix.size(), prefix) != 0) return "";
    std::string tok = auth.substr(prefix.size());

    if (rtutil::isAdminToken(tok) && db_.ok()) {
        DbUser u;
        long long token_id = 0;
        std::string err;
        std::string ip = ctx->request ? ctx->request->client_addr.ip : "";
        if (db_.adminTokenAuth(AdminDb::sha256Hex(tok), AdminDb::nowMs(), ip, u, token_id, err)) {
            tl_actor.id = u.id;
            tl_actor.username = u.username;
            tl_actor.role = u.role;
            tl_actor.token_id = token_id;
            loadActorPerms();
            return u.role;
        }
        return "";
    }

    // 2b) 接入账号 Token（rtk_）：继承所在权限组的资源
    if (rtutil::isClientToken(tok) && db_.ok()) {
        long long account_id = 0, token_id = 0;
        std::string uname, err;
        std::string ip = ctx->request ? ctx->request->client_addr.ip : "";
        if (db_.clientTokenAuth(AdminDb::sha256Hex(tok), AdminDb::nowMs(), ip, account_id, uname, token_id,
                                err)) {
            tl_actor.username = uname;
            tl_actor.role = "client";
            tl_actor.token_id = token_id;
            tl_actor.is_client = true;
            tl_actor.client_id = account_id;
            long long t = 0;
            if (db_.tenantOfClient(account_id, t)) tl_actor.tenant_id = t;
            loadActorPerms();
            return "client";
        }
        return "";
    }

    // 3) 首次初始化前，或显式配置了紧急 token 时，允许 Bearer
    if (!cfg_.token.empty() && tok == cfg_.token) {
        bool allow = !initialized;
        if (allow) {
            // 初始化前 token 只放行 setup 接口
            if (ctx->request->path.find("/api/v1/setup") != 0) allow = false;
        }
        if (allow) {
            tl_actor.username = "bootstrap";
            tl_actor.role = "admin";
            tl_actor.super = true;
            return "admin";
        }
    }
    return "";
}

void AdminServer::loadActorPerms() {
    tl_actor.perms.clear();
    tl_actor.devices.clear();
    tl_actor.ports.clear();
    tl_actor.super = false;
    tl_actor.all_devices = false;
    tl_actor.global_scope = false;
    tl_actor.token_scoped = false;
    DbPermScope sc;
    if (!db_.ok()) return;   // 权限只在库里求值，库不可用就 fail closed
    if (tl_actor.is_client) {
        if (tl_actor.token_id > 0) db_.permScopeOfClientToken(tl_actor.token_id, tl_actor.client_id, sc);
        else db_.permScopeOfClient(tl_actor.client_id, sc);
    } else if (tl_actor.token_id > 0 && tl_actor.id > 0) {
        db_.permScopeOfAdminToken(tl_actor.token_id, tl_actor.id, sc);
    } else if (tl_actor.id > 0) {
        db_.permScopeOfUser(tl_actor.id, sc);
    } else {
        return;   // 只有 bootstrap 初始化令牌走这里，它的 super 在 roleOf 里显式设置
    }
    tl_actor.super = sc.super;
    tl_actor.all_devices = sc.all_devices;
    // 全部设备本身就意味着能影响整个部署；显式 global_scope 用于受限设备的委派管理员
    tl_actor.global_scope = sc.super || sc.all_devices || sc.global_scope;
    tl_actor.token_scoped = sc.token_groups;
    tl_actor.perms = sc.resources;
    tl_actor.devices = sc.devices;
    tl_actor.ports = sc.ports;
}

std::string AdminServer::grantDenied(const DbPermScope& want) const {
    if (tl_actor.super) return std::string();
    if (want.super) return "只有超级管理员可以操作超级权限组";
    for (const auto& r : want.resources) {
        if (!tl_actor.perms.count(r)) return "不能授予自己没有的资源: " + r;
        // 全局键会影响范围外设备/整机配置，需要自己拥有「全局权限」
        if (permIsGlobal(r) && !tl_actor.global_scope)
            return "不能授予全局资源（需要全局权限）: " + r;
    }
    if (want.global_scope && !tl_actor.global_scope) return "不能授予全局权限";
    if (want.all_devices && !tl_actor.all_devices) return "不能授予全部设备";
    if (!tl_actor.all_devices) {
        for (const auto& d : want.devices) {
            if (!tl_actor.devices.count(d)) return "不能授予自己无权访问的设备: " + d;
        }
    }
    if (!adminPortsCovered(tl_actor.ports, want.ports)) return "不能授予超出自己范围的端口: " + want.ports;
    return std::string();
}

std::string AdminServer::manageDenied(const DbPermScope& target) const {
    if (tl_actor.super) return std::string();
    if (target.super) return "只有超级管理员可以操作超级权限组";
    for (const auto& r : target.resources) {
        if (!tl_actor.perms.count(r))
            return "目标当前权限高于你的权限，不能修改: " + r;
        if (permIsGlobal(r) && !tl_actor.global_scope)
            return "目标拥有全局权限，不能修改: " + r;
    }
    if (target.global_scope && !tl_actor.global_scope) return "目标拥有全局权限，不能修改";
    if (target.all_devices && !tl_actor.all_devices) return "目标拥有全部设备权限，不能修改";
    if (!tl_actor.all_devices) {
        for (const auto& d : target.devices) {
            if (!tl_actor.devices.count(d))
                return "目标当前设备超出你的范围，不能修改: " + d;
        }
    }
    if (!adminPortsCovered(tl_actor.ports, target.ports))
        return "目标当前端口范围超出你的范围，不能修改: " + target.ports;
    return std::string();
}

bool AdminServer::subjectScope(const std::string& type, const std::string& sid, DbPermScope& out) {
    if (type == "user") {
        DbUser u;
        if (!db_.findUserById(atoll(sid.c_str()), u)) return false;
        return db_.permScopeOfUser(u.id, out);
    }
    if (type == "client") return db_.permScopeOfClient(atoll(sid.c_str()), out);
    if (type == "atk") {
        DbTokenSpec spec;
        long long uid = 0;
        std::string uname, terr;
        if (!db_.adminTokenSpec(atoll(sid.c_str()), spec, uid, uname, nullptr, terr)) return false;
        DbUser u;
        if (!db_.findUserById(uid, u)) return false;
        return db_.permScopeOfAdminToken(atoll(sid.c_str()), uid, out);
    }
    if (type == "rtk") {
        DbTokenSpec spec;
        long long aid = 0;
        std::string terr;
        if (!db_.clientTokenSpec(atoll(sid.c_str()), spec, aid, nullptr, terr)) return false;
        return db_.permScopeOfClientToken(atoll(sid.c_str()), aid, out);
    }
    return false;
}

std::string AdminServer::accountDenied(long long account_id) {
    DbPermScope sc;
    if (!db_.permScopeOfClient(account_id, sc)) return "接入账号不存在";
    return manageDenied(sc);
}

std::string AdminServer::accountTenantDenied(long long account_id) {
    if (!tl_actor.is_client) return std::string();   // 控制台账号可以跨租户运维
    long long t = 0;
    if (!db_.tenantOfClient(account_id, t)) return "接入账号不存在";
    if (t != tl_actor.tenant_id) return "不在本租户";
    return std::string();
}

std::string AdminServer::subjectTenantDenied(const std::string& type, const std::string& id) {
    if (!tl_actor.is_client) return std::string();
    if (type == "client") return accountTenantDenied(atoll(id.c_str()));
    if (type == "rtk") {
        DbTokenSpec spec;
        long long acc = 0;
        std::string err;
        if (!db_.clientTokenSpec(atoll(id.c_str()), spec, acc, nullptr, err)) return "令牌不存在";
        return accountTenantDenied(acc);
    }
    return "接入令牌不能操作该对象";
}

void AdminServer::kickSubjectShrunk(const std::string& type, const std::string& sid,
                                    const DbPermScope& before, const char* reason) {
    DbPermScope after;
    if (!subjectScope(type, sid, after)) return;
    if (dbPermCovers(after, before)) return;   // 扩容/改名不打断
    if (type == "client") {
        kickClientsById(atoll(sid.c_str()), reason);
    } else if (type == "rtk") {
        kickClients({}, atoll(sid.c_str()), reason);
    } else if (type == "atk") {
        kickConsoleWsOfToken(atoll(sid.c_str()), reason);
    }
}

std::string AdminServer::grantGroupsDenied(const std::vector<long long>& ids) {
    for (long long id : ids) {
        DbPermScope gs;
        if (!db_.permGroupScope(id, gs)) return "权限组不存在: " + std::to_string(id);
        std::string why = grantDenied(gs);
        if (!why.empty()) return why;
    }
    return std::string();
}

namespace {
// 运营商侧资源：权限组/字典/中继/证书/系统设置/atk 管理属于平台自身运维，
// 接入令牌（客户）即使被误授也不能用，避免跨租户看到别人的设备与配置。
bool permIsOperatorKey(const std::string& k) {
    static const char* kPrefix[] = {"group.", "dict.",   "relay.",  "cert.",
                                    "settings.", "token.", "playbook."};
    for (const char* p : kPrefix)
        if (k.rfind(p, 0) == 0) return true;
    // 价目与审批是运营方动作：接入令牌可以看本租户费用，但不能改单价或放行设备写操作
    return k == "system.shutdown" || k == "billing.write" || k == "op.approve";
}
}  // namespace

bool AdminServer::hasPerm(const std::string& key) const {
    if (tl_actor.super) return true;
    if (tl_actor.is_client && permIsOperatorKey(key)) return false;
    if (!tl_actor.perms.count(key)) return false;
    // 全局资源需要组显式开启「全局权限」（全部设备与 super 自动视为开启），
    // 避免受限设备的组通过 relay/dict 等全局键影响范围外的设备。
    if (permIsGlobal(key) && !tl_actor.global_scope) return false;
    return true;
}

int AdminServer::requirePerm(const HttpContextPtr& ctx, const std::string& need, std::string& role) {
    role = roleOf(ctx);
    if (role.empty()) {
        ctx->response->status_code = HTTP_STATUS_UNAUTHORIZED;
        ctx->response->content_type = APPLICATION_JSON;
        ctx->response->body = "{\"error\":\"unauthorized\"}";
        return 401;
    }
    if (need.empty() || need == "auth" || hasPerm(need)) return 0;
    ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
    ctx->response->content_type = APPLICATION_JSON;
    ctx->response->body = "{\"error\":\"forbidden\",\"need\":\"" + need + "\"}";
    return 403;
}

bool AdminServer::wsAuthorize(const HttpRequestPtr& req, AdminActor& actor, std::string& role) {
    actor = AdminActor{};
    role.clear();
    // 鉴权失败时不能沿用同一 worker 线程上一个请求的身份（hasPerm 读 tl_actor）
    tl_actor = AdminActor{};
    std::string origin = req->GetHeader("Origin");
    if (!origin.empty()) {
        // 只比较 scheme://authority 的 authority 部分，必须与 Host 完全一致；
        // 子串匹配会把 evil-admin.example 当成 admin.example 的合法来源。
        std::string host = req->GetHeader("Host");
        size_t sp = origin.find("://");
        std::string authority = (sp == std::string::npos) ? std::string() : origin.substr(sp + 3);
        size_t slash = authority.find('/');
        if (slash != std::string::npos) authority = authority.substr(0, slash);
        auto lower = [](std::string v) {
            for (char& c : v) c = (char)tolower((unsigned char)c);
            return v;
        };
        if (host.empty() || sp == std::string::npos || lower(authority) != lower(host)) {
            logf_("[admin] ws origin mismatch: %s host=%s", origin.c_str(), host.c_str());
            return false;
        }
    }
    const HttpCookie& ck = req->GetCookie("rt_sid");
    if (!ck.name.empty() && db_.ok()) {
        DbUser u;
        if (db_.getSession(AdminDb::sha256Hex(ck.value), u, AdminDb::nowMs())) {
            actor.id = u.id; actor.username = u.username; actor.role = u.role;
            role = u.role;
            tl_actor = actor;   // 供随后的 ownsDevid() 使用，避免沿用上一个请求的身份
            loadActorPerms();
            actor = tl_actor;
            return true;
        }
    }
    // 非浏览器客户端：只接受 Authorization 头。浏览器走上面的会话 Cookie，
    // 不要再用 ?atk= 传令牌——查询串会进反向代理日志和访问记录。
    std::string auth = req->GetHeader("Authorization");
    const std::string prefix = "Bearer ";
    std::vector<std::string> cand;
    if (auth.compare(0, prefix.size(), prefix) == 0) cand.push_back(auth.substr(prefix.size()));
    for (const auto& tok : cand) {
        if (rtutil::isAdminToken(tok) && db_.ok()) {
            DbUser u;
            long long token_id = 0;
            std::string err;
            std::string ip = req->client_addr.ip;
            if (db_.adminTokenAuth(AdminDb::sha256Hex(tok), AdminDb::nowMs(), ip, u, token_id, err)) {
                actor.id = u.id; actor.username = u.username; actor.role = u.role;
                actor.token_id = token_id;
                role = u.role;
                tl_actor = actor;
                loadActorPerms();
                actor = tl_actor;
                return true;
            }
            return false;
        }
        if (rtutil::isClientToken(tok) && db_.ok()) {
            long long account_id = 0, token_id = 0;
            std::string uname, err;
            if (db_.clientTokenAuth(AdminDb::sha256Hex(tok), AdminDb::nowMs(), req->client_addr.ip,
                                    account_id, uname, token_id, err)) {
                actor.username = uname;
                actor.role = "client";
                actor.token_id = token_id;
                actor.is_client = true;
                actor.client_id = account_id;
                role = "client";
                tl_actor = actor;
                loadActorPerms();
                actor = tl_actor;
                return true;
            }
            return false;
        }
    }
    // 初始化前仅放行 /api/v1/setup*
    if (auth.compare(0, prefix.size(), prefix) == 0) {
        std::string tok = auth.substr(prefix.size());
        bool initialized = db_.ok() && db_.initialized();
        if (!cfg_.token.empty() && tok == cfg_.token && !initialized) {
            actor.id = 0; actor.username = "bootstrap"; actor.role = "admin";
            role = "admin";
            tl_actor = actor;
            return true;
        }
    }
    return false;
}

std::string AdminServer::actorLabel() const {
    AdminActor a = currentActor();
    if (a.username.empty()) return std::string();
    std::string who = a.is_client ? "client:" + a.username : a.username;
    if (a.id > 0) who += "#" + std::to_string(a.id);
    if (a.token_id > 0) who += "/token" + std::to_string(a.token_id);
    if (!a.role.empty()) who += "/" + a.role;
    if (!a.ip.empty()) who += "/" + a.ip;
    // 模型调用：把标签放最前面，审计可按 actor_type=ai 过滤
    if (!a.ai_actor.empty()) who = a.ai_actor + "|" + who;
    return who;
}

void AdminServer::audit(const std::string& role, const std::string& action, const std::string& detail) {
    // 审计主体：优先用当前请求的真实账号（含 id / 令牌 / 来源），没有请求上下文时才退回角色
    std::string who = actorLabel();
    if (who.empty()) who = role.empty() ? "anonymous" : role;
    audit_.write(who, action, detail);

    // 事件流只带概要（detail 可能很长）：完整内容仍在 audit.log/.jsonl
    nlohmann::json j;
    j["actor"] = who;
    j["action"] = action;
    if (!detail.empty()) {
        j["detail"] = detail.size() > 200 ? detail.substr(0, 200) + "…" : detail;
    }
    bus_.publish("audit", j.dump());
}

void AdminServer::emitEvent(const std::string& type, const std::string& data_json) {
    bus_.publish(type, data_json);
}

void AdminServer::auditEx(const std::string& action, const std::string& target_type,
                          const std::string& target_id, const std::string& result,
                          const std::string& detail, const std::string& op_id) {
    std::string who = actorLabel();
    if (who.empty()) who = "anonymous";
    AuditEntry e;
    e.actor = who;
    e.action = action;
    e.target_type = target_type;
    e.target_id = target_id;
    e.result = result.empty() ? "ok" : result;
    e.detail = detail;
    e.op_id = op_id;
    audit_.writeEntry(e);

    // 同一条审计也进事件流：AI/页面不需要轮询 /audit 就能看到「谁在做什么」
    nlohmann::json j;
    j["actor"] = e.actor;
    j["action"] = e.action;
    j["target_type"] = e.target_type;
    j["target_id"] = e.target_id;
    j["result"] = e.result;
    if (!e.detail.empty()) j["detail"] = e.detail;
    if (!e.op_id.empty()) j["op_id"] = e.op_id;
    bus_.publish("audit", j.dump());
}

int AdminServer::kickClients(const std::vector<std::string>& users, long long token_id, const char* reason) {
    if (users.empty() && token_id <= 0) return 0;
    std::string why = reason ? reason : "revoked";
    std::string out = callRelay([this, users, token_id, why]() {
        return std::to_string(relay_->kickClientSessions(users, token_id, why.c_str()));
    });
    int n = atoi(out.c_str());
    if (n > 0) logf_("[admin] %s: closed %d client session(s)", why.c_str(), n);
    return n;
}

void AdminServer::kickClientsById(long long account_id, const char* reason) {
    std::string aj, err;
    if (!db_.clientAccountGet(account_id, aj, err)) return;
    auto a = nlohmann::json::parse(aj, nullptr, false);
    if (a.is_discarded()) return;
    std::string user = a.value("username", std::string());
    if (!user.empty()) kickClients({user}, -1, reason);
}

// ---------------------------------------------------------------- 管理台 WS 会话
// 管理台的终端/串口/文件/服务 WS 都跑在独立线程，登录态在握手时只查一次。
// 这里登记每条的所属账号（和 atk 令牌），撤权/停用/吊销后主动断开，避免旧连接
// 继续用已经撤销的权限操作设备。

void AdminServer::registerConsoleWs(const WebSocketChannelPtr& ws, const AdminActor& a, const char* kind) {
    if (!ws) return;
    ConsoleWsEntry e;
    e.ws = ws;
    e.user_id = a.id;
    e.token_id = a.token_id;
    e.username = a.username;
    e.kind = kind ? kind : "";
    e.opened_ms = AdminDb::nowMs();
    std::lock_guard<std::mutex> lk(console_ws_mu_);
    console_ws_[ws.get()] = std::move(e);
}

void AdminServer::unregisterConsoleWs(const hv::WebSocketChannel* ws) {
    if (!ws) return;
    std::lock_guard<std::mutex> lk(console_ws_mu_);
    console_ws_.erase(ws);
}

// 关闭匹配的会话。先在锁内摘出来再关：close() 会同步触发 onclose → unregister，
// 持锁关会自锁。
static int closeConsoleWs(std::vector<WebSocketChannelPtr>& victims, const char* reason) {
    int n = 0;
    for (auto& ws : victims) {
        if (!ws) continue;
        logf_("[admin] console ws closed: %s", reason ? reason : "revoked");
        ++n;
        if (ws->isConnected()) ws->close();
    }
    return n;
}

int AdminServer::kickConsoleWsOfUser(long long user_id, const char* reason) {
    if (user_id <= 0) return 0;
    std::vector<WebSocketChannelPtr> victims;
    {
        std::lock_guard<std::mutex> lk(console_ws_mu_);
        for (auto it = console_ws_.begin(); it != console_ws_.end();) {
            if (it->second.user_id != user_id) { ++it; continue; }
            if (auto ws = it->second.ws.lock()) victims.push_back(std::move(ws));
            it = console_ws_.erase(it);
        }
    }
    return closeConsoleWs(victims, reason);
}

int AdminServer::kickConsoleWsOfToken(long long token_id, const char* reason) {
    if (token_id <= 0) return 0;
    std::vector<WebSocketChannelPtr> victims;
    {
        std::lock_guard<std::mutex> lk(console_ws_mu_);
        for (auto it = console_ws_.begin(); it != console_ws_.end();) {
            if (it->second.token_id != token_id) { ++it; continue; }
            if (auto ws = it->second.ws.lock()) victims.push_back(std::move(ws));
            it = console_ws_.erase(it);
        }
    }
    return closeConsoleWs(victims, reason);
}

std::map<long long, DbPermScope> AdminServer::snapshotUserScopes() {
    std::map<long long, DbPermScope> out;
    std::vector<DbUser> us;
    if (!db_.ok() || !db_.listUsers(us)) return out;
    for (auto& u : us) {
        DbPermScope sc;
        db_.permScopeOfUser(u.id, sc);
        out[u.id] = std::move(sc);
    }
    return out;
}

void AdminServer::kickConsoleWsShrunk(const std::map<long long, DbPermScope>& before, const char* reason) {
    if (before.empty()) return;
    int n = 0;
    for (auto& kv : before) {
        DbUser u;
        if (!db_.findUserById(kv.first, u) || u.status != "active") {
            n += kickConsoleWsOfUser(kv.first, reason);
            continue;
        }
        DbPermScope after;
        db_.permScopeOfUser(u.id, after);
        // 新范围覆盖旧范围 = 没缩；否则断开
        if (!dbPermCovers(after, kv.second)) n += kickConsoleWsOfUser(kv.first, reason);
    }
    if (n > 0) logf_("[admin] console ws: %d session(s) closed (%s)", n, reason ? reason : "revoked");
}

// ---------------------------------------------------------------- 前端

namespace {

// Accept-Encoding: gzip / gzip;q=0.5 / br, gzip;q=0 / *
bool adminAcceptsEncoding(const std::string& ae, const char* want) {
    size_t pos = 0;
    while (pos < ae.size()) {
        size_t comma = ae.find(',', pos);
        std::string item = ae.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b == std::string::npos) {
            if (comma == std::string::npos) break;
            pos = comma + 1;
            continue;
        }
        item = item.substr(b, e - b + 1);
        size_t semi = item.find(';');
        std::string token = item.substr(0, semi);
        for (auto& ch : token) ch = (char)tolower((unsigned char)ch);
        if (token == want || token == "*") {
            if (semi != std::string::npos) {
                size_t q = item.find("q=", semi);
                if (q != std::string::npos && atof(item.c_str() + q + 2) <= 0) return false;
            }
            return true;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

}  // namespace

int AdminServer::serveFrontend(const HttpContextPtr& ctx, const char* name, const char* ctype,
                               int source) {
    ctx->response->headers["Cache-Control"] = "no-store";
    if (name == nullptr || strstr(name, "..") != nullptr) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        return 404;
    }
    auto read_file = [](const std::string& path, std::string& body) -> bool {
        // 目录也能 fopen 成功，且 ftell 返回 LONG_MAX；必须先确认是普通文件，
        // 否则 body.resize() 会抛 length_error 把整个 relay 带崩（/assets/ 即可触发）。
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return false;
        if (st.st_size <= 0 || st.st_size > 64 * 1024 * 1024) return false;
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) return false;
        long n = (long)st.st_size;
        body.resize((size_t)n);
        bool ok = fread(&body[0], 1, (size_t)n, fp) == (size_t)n;
        fclose(fp);
        if (!ok) body.clear();
        return ok;
    };
    std::string ae = ctx->header("Accept-Encoding");
    const bool br_ok = adminAcceptsEncoding(ae, "br");
    const bool gzip_ok = adminAcceptsEncoding(ae, "gzip");

    const bool try_disk = (source != kAdminWebEmbeddedOnly) && !cfg_.docroot.empty();
    const bool try_embedded = (source != kAdminWebDiskOnly);

    // 1) 磁盘目录优先：开发时用 --admin-docroot 指到 web/dist 即可热改
    if (try_disk) {
        std::string path = cfg_.docroot + "/" + name;
        // 构建期生成的 .br/.gz 预压缩文件，按 Accept-Encoding 直接返回
        const char* encoding = NULL;
        const char* ext = NULL;
        if (br_ok) {
            encoding = "br"; ext = ".br";
        } else if (gzip_ok) {
            encoding = "gzip"; ext = ".gz";
        }
        if (encoding) {
            std::string body;
            if (read_file(path + ext, body)) {
                ctx->response->content_type = http_content_type_enum(ctype);
                ctx->response->headers["Content-Encoding"] = encoding;
                ctx->response->headers["Vary"] = "Accept-Encoding";
                ctx->response->body = std::move(body);
                return 200;
            }
        }
        std::string body;
        if (read_file(path, body)) {
            ctx->response->content_type = http_content_type_enum(ctype);
            ctx->response->body = std::move(body);
            return 200;
        }
    }

    // 2) 磁盘没有（或没配 docroot）：回退到编译期内嵌的前端
    //    rtunnel-relay 里这个查询永远返回 false；rtunnel-relay-embedded 带资源表。
    AdminWebAsset asset;
    if (try_embedded && adminWebAssetGet(name, gzip_ok, asset)) {
        ctx->response->content_type = http_content_type_enum(asset.ctype.c_str());
        if (asset.gzip) {
            ctx->response->headers["Content-Encoding"] = "gzip";
            ctx->response->headers["Vary"] = "Accept-Encoding";
        }
        ctx->response->body = std::move(asset.body);
        return 200;
    }

    if (source == kAdminWebBoth && cfg_.docroot.empty() && !adminWebAssetsAvailable()) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->response->content_type = http_content_type_enum("text/plain");
        ctx->response->body =
            "admin frontend not deployed: start relay with --admin-docroot <web/dist>, "
            "or use rtunnel-relay-embedded (built with -DRTUNNEL_ADMIN_WEB=ON)\n";
        return 404;
    }
    ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
    return 404;
}

// ---------------------------------------------------------------- 后台 worker

void AdminServer::spawnWorker(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(worker_mu_);
    workers_.emplace_back(std::move(fn));
}

bool AdminServer::waitStop(int ms) {
    std::unique_lock<std::mutex> lk(worker_mu_);
    return worker_cv_.wait_for(lk, std::chrono::milliseconds(ms),
                               [this]() { return stopping_.load(); });
}

void AdminServer::joinWorkers() {
    std::vector<std::thread> ws;
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        ws.swap(workers_);
    }
    for (auto& t : ws) {
        if (t.joinable()) t.join();
    }
}

// ---------------------------------------------------------------- 路由与启动

int AdminServer::start(const AdminConfig& cfg) {
    cfg_ = cfg;
    stopping_.store(false);

    {
        std::string db_err;
        if (db_.open(cfg_.db_path, db_err)) {
            loadSettings();
            db_.permSeed();
            db_.distRecover();
            logf_("[admin] db: %s", cfg_.db_path.c_str());
            // 启动时把下发表（relay_list）装进 relay（此时 loop 还没跑，仍在 relay 线程）
            {
                std::string spec;
                if (!relayListSpec(spec)) spec.clear();
                std::string rl_err;
                if (!relay_->setRelayList(spec, settingInt("relay.allow_leave", 0) != 0, rl_err)) {
                    logf_("[admin] relay list invalid: %s", rl_err.c_str());
                }
            }
        } else {
            logf_("[admin] db open failed: %s (%s)", cfg_.db_path.c_str(), db_err.c_str());
        }
    }

    if (!cfg_.cert_dir.empty()) {
        relay_->cert_dir = cfg_.cert_dir;
        mkdir(cfg_.cert_dir.c_str(), 0700);
        relay_->loadRevoked();
        audit_.init(cfg_.cert_dir + "/audit.log");
    }

    spawnWorker([this]() {
        if (waitStop(200)) return;
        refreshGroupAcl();
        refreshTokenCache();
    });

    db_.purgeExpiredSessions(AdminDb::nowMs());
    bool want_tls = !cfg_.tls_cert.empty() || cfg_.tls_selfsigned;
    if (!want_tls && cfg_.addr.rfind("0.0.0.0", 0) == 0) {
        logf_("[admin] WARNING: 明文监听 %s，请使用 --admin-tls-selfsigned 或反向代理 TLS",
              cfg_.addr.c_str());
    }
    spawnWorker([this]() { distLoop(); });
    spawnWorker([this]() { relaySyncLoop(); });
    // Token 用量：relay 只保留内存计数，这里按周期拉快照落库；
    // 同时取走用量事件（流量增量/会话/在线时长）写入流水与日聚合。
    // 自愈 worker：与告警线程解耦，串行执行规则生成的作业
    spawnWorker([this]() { playbookWorker(); });

    // 事件流与分钟级指标：500ms 从 relay 取一次，事件进总线广播，指标写库。
    // 采样本身是 60s 一条，这里只是把 relay 线程的队列搬进 SQLite/SSE。
    spawnWorker([this]() {
        long long last_prune = 0;
        std::string metric_backlog;   // 写库失败时留着下一轮补写
        while (!stopping_.load()) {
            if (waitStop(500)) break;
            if (relay_ != nullptr && relay_->loop != NULL) {
                std::string ev = callRelay([this]() { return relay_->takeEvents(); });
                if (ev.size() > 2 && ev[0] == '[') {
                    nlohmann::json arr = nlohmann::json::parse(ev, nullptr, false);
                    if (!arr.is_discarded() && arr.is_array()) {
                        for (auto& e : arr) {
                            if (!e.is_object()) continue;
                            std::string type = e.value("type", std::string());
                            if (type.empty()) continue;
                            e.erase("type");
                            bus_.publish(type, e.dump());
                        }
                    }
                }
                std::string rows = callRelay([this]() { return relay_->takeMetricSamples(); });
                if (rows.size() > 2 && rows[0] == '[') {
                    if (!metric_backlog.empty()) {
                        // 上一批没写进去：合并后一起写，避免丢点
                        nlohmann::json a = nlohmann::json::parse(metric_backlog, nullptr, false);
                        nlohmann::json b = nlohmann::json::parse(rows, nullptr, false);
                        if (a.is_array() && b.is_array()) {
                            for (auto& x : b) a.push_back(x);
                            rows = a.dump();
                        }
                        metric_backlog.clear();
                    }
                    if (db_.ok()) {
                        int n = 0;
                        std::string err;
                        if (!db_.metricsAppend(rows, n, err)) metric_backlog = rows;
                    } else {
                        metric_backlog = rows;
                    }
                }
            }
            long long now = AdminDb::nowMs();
            if (last_prune == 0) last_prune = now;
            if (now - last_prune > 3600LL * 1000) {
                last_prune = now;
                int days = settingInt("metrics.retention_days", 7);
                if (days < 1) days = 1;
                if (days > 90) days = 90;
                int n = 0;
                db_.metricsPrune(now - (long long)days * 86400LL * 1000, n);
            }
        }
    });

    spawnWorker([this]() {
        std::string backlog;   // 上次写库失败的事件，下一轮先补写，避免丢账
        while (!stopping_.load()) {
            int ms = settingInt("token.usage_flush_ms", 5000);
            if (ms < 1000) ms = 1000;
            if (waitStop(ms)) break;
            if (!db_.ok()) continue;
            if (!backlog.empty()) {
                std::string err;
                if (!db_.usageAppend(backlog, err)) {
                    logf_("[usage] append retry failed: %s", err.c_str());
                    continue;
                }
                backlog.clear();
            }
            std::string snap = callRelay([this]() { return relay_->tokenUsageSnapshot(); });
            if (!snap.empty() && snap[0] == '[') {
                std::string err;
                db_.clientTokenUsageApply(snap, err);
            }
            std::string ev = callRelay([this]() { return relay_->takeUsageEvents(); });
            if (ev.size() > 2) {
                std::string err;
                if (!db_.usageAppend(ev, err)) {
                    backlog = ev;
                    logf_("[usage] append failed: %s", err.c_str());
                }
            }
        }
    });
    spawnWorker([this]() {
        while (!waitStop(10000)) {
            std::vector<std::string> devs;
            db_.ttyDevids(devs);
            for (auto& d : devs) {
                if (!relay_->hasAgent(d.c_str())) continue;
                std::string out;
                if (db_.ttyList(d, out)) relay_->pushTtyCfg(d.c_str(), out);
            }
            std::vector<std::string> mdevs;
            db_.metricSeriesDevids(mdevs);
            for (auto& d : mdevs) {
                if (!relay_->hasAgent(d.c_str())) continue;
                pushMetricSeriesConfig(d);
                std::string st = relay_->takeMetricStatus(d);
                auto sj = json::parse(st, nullptr, false);
                if (!sj.is_discarded() && sj.value("applied", false)) {
                    db_.metricSeriesAck(d, sj.value("revision", 0LL));
                }
            }
        }
    });
    spawnWorker([this]() {
        while (!waitStop(3600 * 1000)) {
            cleanupAudit(settingInt("audit.retention_days", 30));
            cleanupUploads();
            cleanupTtyLogs();
            audit_.rotate(100 * 1024 * 1024);
            // 流水按天保留（默认 180 天）；日聚合不清理，出账/报表依赖它
            int raw_days = settingInt("usage.raw_retention_days", 180);
            if (raw_days < 7) raw_days = 7;
            int pruned = 0;
            long long before = (long long)time(NULL) * 1000 - (long long)raw_days * 86400000;
            if (db_.usagePrune(before, pruned) && pruned > 0) {
                logf_("[usage] pruned %d raw events older than %d days", pruned, raw_days);
            }
        }
    });
    spawnWorker([this]() { alertLoop(); });

    char host[64] = "127.0.0.1";
    int port = 9443;
    const char* colon = strrchr(cfg_.addr.c_str(), ':');
    if (colon) {
        snprintf(host, sizeof(host), "%.*s", (int)(colon - cfg_.addr.c_str()), cfg_.addr.c_str());
        port = atoi(colon + 1);
    } else {
        port = atoi(cfg_.addr.c_str());
    }
    // 安全护栏：非回环监听必须配置凭据
    bool loopback = strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0 ||
                    strcmp(host, "::1") == 0;
    if (!loopback && cfg_.token.empty()) {
        logf_("[admin] refuse to listen on %s without credentials", host);
        stop();
        return -1;
    }

    registerRoutes();
    initTerminal();
    server_.setHost(host);
    // 管理面用多个 HTTP worker：单线程时一个大文件下载/慢 SQL 会挡住所有管理操作。
    // 数据面的 /device 代理另有独立线程（api_proxy.cpp），终端/文件 WS 也有各自的读线程。
    int nthreads = cfg_.threads;
    if (nthreads < 1) nthreads = 1;
    if (nthreads > 32) nthreads = 32;
    server_.setThreadNum(nthreads);

    // 管理台 TLS：显式 cert/key 或自签；未配置时维持原有明文行为
    std::string tls_crt = cfg_.tls_cert;
    std::string tls_key = cfg_.tls_key;
    if (want_tls) {
        if (!admin_tls_supported()) {
            logf_("[admin] 请求了 TLS，但当前构建没有可用的 TLS 后端（WITH_OPENSSL=OFF）");
            stop();
            return -1;
        }
        if (cfg_.tls_selfsigned && cfg_.tls_cert.empty()) {
            std::string dir = cfg_.tls_dir;
            if (dir.empty()) {
                dir = cfg_.cert_dir.empty() ? "admin-tls" : cfg_.cert_dir + "/admin-tls";
            }
            std::string err;
            if (!admin_tls_ensure_selfsigned(dir, host, cfg_.tls_days, 30,
                                             tls_crt, tls_key, err)) {
                logf_("[admin] 自签证书生成失败: %s", err.c_str());
                stop();
                return -1;
            }
        }
        if (tls_key.empty()) {
            logf_("[admin] --admin-tls-cert 需要同时指定 --admin-tls-key");
            stop();
            return -1;
        }
        if (access(tls_crt.c_str(), R_OK) != 0 || access(tls_key.c_str(), R_OK) != 0) {
            logf_("[admin] 证书不可读: cert=%s key=%s", tls_crt.c_str(), tls_key.c_str());
            stop();
            return -1;
        }
        hssl_ctx_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.crt_file = tls_crt.c_str();
        opt.key_file = tls_key.c_str();
        opt.endpoint = HSSL_SERVER;
        opt.verify_peer = 0;
        if (server_.newSslCtx(&opt) != 0) {
            logf_("[admin] SSL_CTX 初始化失败（cert=%s key=%s）", tls_crt.c_str(), tls_key.c_str());
            stop();
            return -1;
        }
        server_.https_port = port;   // 只开 HTTPS，不开明文 HTTP
        admin_cert_path_ = tls_crt;  // 证书到期检查也会盯它
    } else {
        server_.setPort(port);
    }
    int rc = server_.start();
    if (rc != 0) {
        logf_("[admin] listen %s:%d failed rc=%d", host, port, rc);
        stop();
        return rc;
    }
    if (want_tls) {
        logf_("[admin] listening on https://%s:%d/ (%s, cert=%s)", host, port,
              cfg_.token.empty() ? "no auth (loopback)" : "bearer token", tls_crt.c_str());
        if (cfg_.tls_selfsigned && cfg_.tls_cert.empty()) {
            logf_("[admin] 自签证书：浏览器首次访问需手动信任，或把 %s 加入系统信任库",
                  tls_crt.c_str());
        }
    } else {
        logf_("[admin] listening on http://%s:%d/ (%s, users=%zu)", host, port,
              cfg_.token.empty() ? "no auth (loopback)" : "bearer token", 0);
    }
    if (cfg_.docroot.empty()) {
        if (adminWebAssetsAvailable()) {
            logf_("[admin] serving embedded frontend (no --admin-docroot)");
        } else {
            logf_("[admin] WARNING: --admin-docroot not set and no embedded frontend, admin UI disabled");
        }
    }
    return 0;
}

void AdminServer::stop() {
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        stopping_.store(true);
    }
    worker_cv_.notify_all();
    // 先在管理面 loop 还活着时收掉 /device 代理线程，避免它们继续往已关闭的连接写
    adminProxyShutdown();
    server_.stop();
    joinWorkers();
}
