#include "relay_server.hpp"
#include "cpputil/json.hpp"
#include "common/logging.hpp"
#include "common/sha256.hpp"
#include "common/token_util.hpp"
#include "relay/admin/backend/admin_server.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#if defined(WITH_OPENSSL)
// mTLS 身份校验在 rtunnel 内完成，不改 libhv：
// 用公开的 hio_get_ssl() 取到 SSL*，直接读对端证书 SAN。
#include "openssl/ssl.h"
#include "openssl/x509v3.h"

static int peer_cert_san(hssl_t ssl, char* buf, int bufsize) {
    if (ssl == NULL || buf == NULL || bufsize <= 0) return -1;
    SSL* ssl_ = (SSL*)ssl;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    X509* cert = SSL_get1_peer_certificate(ssl_);
#else
    X509* cert = SSL_get_peer_certificate(ssl_);
#endif
    if (cert == NULL) return -1;
    int ret = -1;
    GENERAL_NAMES* names = (GENERAL_NAMES*)X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    if (names) {
        int n = sk_GENERAL_NAME_num(names);
        for (int i = 0; i < n; ++i) {
            GENERAL_NAME* nm = sk_GENERAL_NAME_value(names, i);
            if (nm->type != GEN_DNS) continue;
            const unsigned char* d = ASN1_STRING_get0_data(nm->d.dNSName);
            int len = ASN1_STRING_length(nm->d.dNSName);
            if (d == NULL || len <= 0 || len >= bufsize) continue;
            memcpy(buf, d, (size_t)len);
            buf[len] = '\0';
            ret = 0;
            break;
        }
        GENERAL_NAMES_free(names);
    }
    X509_free(cert);
    return ret;
}
#endif

// accept 回调拿不到 hio_context（新连接尚未挂对象），用进程内单例指针
static RelayServer* g_self = nullptr;

// ---------------------------------------------------------------- 回调薄封装

void RelayServer::readCb(hio_t* io, void* buf, int n) {
    auto* c = (RelayConn*)hio_context(io);
    if (c) c->server->onRead(c, buf, n);
}

void RelayServer::closeCb(hio_t* io) {
    auto* c = (RelayConn*)hio_context(io);
    if (c) c->server->onClose(c);
}

void RelayServer::acceptCb(hio_t* io) {
    if (g_self) g_self->onAccept(io);
}

void RelayServer::hsTimeoutCb(htimer_t* t) {
    auto* c = (RelayConn*)hevent_userdata(t);
    if (c) c->server->onHsTimeout(c);
}

void RelayServer::statsCb(htimer_t* t) {
    auto* self = (RelayServer*)hevent_userdata(t);
    if (self) self->onStats();
}

void RelayServer::tokenCutCb(htimer_t* t) {
    auto* self = (RelayServer*)hevent_userdata(t);
    if (self) self->tokenApplyCuts();
}

void RelayServer::tokenCb(htimer_t* t) {
    auto* self = (RelayServer*)hevent_userdata(t);
    if (self) self->tokenSweep();
}

void RelayServer::drainCb(htimer_t* t) {
    auto* self = (RelayServer*)hevent_userdata(t);
    if (self) self->onDrain(t);
}

void RelayServer::sighupCb(hsignal_t* sig) {
    auto* self = (RelayServer*)hevent_userdata(sig);
    if (self) self->onSighup();
}

void RelayServer::shutdownCb(hsignal_t* sig) {
    auto* self = (RelayServer*)hevent_userdata(sig);
    if (self) self->onShutdown((int)hevent_id(sig));
}

// ---------------------------------------------------------------- ACL

static int pattern_match(const char* pat, const char* s) {
    size_t n = strlen(pat);
    if (n > 0 && pat[n - 1] == '*') {
        return strncmp(pat, s, n - 1) == 0;
    }
    return strcmp(pat, s) == 0;
}

bool RelayServer::shellAllowed(const char* user) const {
    for (const auto& u : shell_users) {
        if (u == "*" || u == user) return true;
    }
    return false;
}

RelayConn* RelayServer::findClientByUser(const char* user) {
    for (RelayConn* c = conns; c; c = c->next_all) {
        if (c->role == rtnp::ROLE_CLIENT && c->authed && strcmp(c->user, user) == 0) return c;
    }
    return NULL;
}

void RelayServer::addPending(RelayConn* a, uint32_t a_sid, const char* devid, uint16_t port, uint16_t flags) {
    auto* p = new PendingOpen{};
    p->a = a;
    p->a_sid = a_sid;
    snprintf(p->devid, sizeof(p->devid), "%s", devid);
    p->port = port;
    p->flags = flags;
    p->deadline_ms = hloop_now_ms(loop) + 10000;
    p->next = pending;
    pending = p;
}

void RelayServer::completePending(const char* devid) {
    RelayConn* agent = findAgent(devid);
    if (agent == NULL) return;
    PendingOpen** pp = &pending;
    while (*pp) {
        PendingOpen* p = *pp;
        if (strcmp(p->devid, devid) != 0) { pp = &p->next; continue; }
        *pp = p->next;
        if (p->a == NULL || p->a->io == NULL || hio_is_closed(p->a->io)) { delete p; continue; }

        auto* s = new RelayStream{};
        s->a = p->a; s->a_sid = p->a_sid;
        s->c = agent; s->c_sid = ++agent->next_c_sid;
        s->next_a = p->a->streams; p->a->streams = s;
        s->next_c = agent->streams; agent->streams = s;

        rtnp::Writer w;
        w.u16(p->port);
        rtnp::send(agent->io, rtnp::OPEN, s->c_sid, w.data, (uint16_t)w.len, p->flags);
        logf_("[relay] OPEN_SHELL %s resumed (a_sid=%u c_sid=%u)", devid, s->a_sid, s->c_sid);
        delete p;
    }
}

void RelayServer::sweepPending() {
    uint64_t now = hloop_now_ms(loop);
    PendingOpen** pp = &pending;
    while (*pp) {
        PendingOpen* p = *pp;
        if (now < p->deadline_ms) { pp = &p->next; continue; }
        *pp = p->next;
        if (p->a && p->a->io && !hio_is_closed(p->a->io)) {
            sendErr(p->a->io, rtnp::OPEN_FAIL, p->a_sid, rtnp::TIMEOUT, "agent spawn timeout");
        }
        logf_("[relay] OPEN_SHELL %s: spawn timeout", p->devid);
        delete p;
    }
}

void RelayServer::dropPendingFor(RelayConn* a) {
    PendingOpen** pp = &pending;
    while (*pp) {
        PendingOpen* p = *pp;
        if (p->a == a) { *pp = p->next; delete p; }
        else pp = &p->next;
    }
}

void RelayServer::setGroupAcl(std::vector<GroupAclRule> rules, std::set<std::string> users) {
    acl_groups = std::move(rules);
    acl_grouped_users = std::move(users);
    if (verbose) logf_("[relay] group acl: %zu rules, %zu grouped users",
                       acl_groups.size(), acl_grouped_users.size());
}

// ---------------------------------------------------------------- Token 运行时

void RelayServer::setAuthPolicies(const std::string& json) {
    auto doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object() || !doc.contains("tokens")) {
        logf_("[relay] auth policies: bad payload");
        return;
    }
    account_active.clear();
    if (doc.contains("accounts") && doc["accounts"].is_object()) {
        for (auto it = doc["accounts"].begin(); it != doc["accounts"].end(); ++it) {
            account_active[it.key()] = it.value() == "active" ? 1 : 0;
        }
    }
    std::unordered_map<std::string, TokenPolicy> next;
    for (auto& item : doc["tokens"]) {
        if (!item.is_object()) continue;
        TokenPolicy p;
        p.id = item.value("id", 0LL);
        p.account_id = item.value("account_id", 0LL);
        p.username = item.value("username", std::string());
        p.hash = item.value("hash", std::string());
        p.prefix = item.value("prefix", std::string());
        p.name = item.value("name", std::string());
        p.status = item.value("status", std::string()) == "active" ? 0 : 1;
        p.account_active = item.value("account_status", std::string()) == "active" ? 1 : 0;
        p.expires_at_ms = item.value("expires_at_ms", 0LL);
        p.quota_bytes = item.value("quota_bytes", 0LL);
        p.quota_period = (int)rtutil::tokenPeriodFromName(item.value("quota_period", std::string("total")));
        p.period_start_ms = item.value("period_start_ms", 0LL);
        p.used_base = item.value("used_bytes", 0LL);
        p.rate_per_min = item.value("rate_per_min", 0);
        p.burst = item.value("burst", 0);
        p.max_sessions = item.value("max_sessions", 0);
        p.quota_action = item.value("quota_action", std::string()) == "block" ? 1 : 0;
        if (p.hash.empty()) continue;

        auto it = token_usage.find(p.id);
        if (it == token_usage.end()) {
            TokenUsage u;
            u.used_bytes = p.used_base;
            u.period_start_ms = p.period_start_ms;
            token_usage[p.id] = u;
        } else if (p.period_start_ms > 0 && p.period_start_ms > it->second.period_start_ms) {
            // 周期已重置（或管理员改了周期），以 DB 为准
            it->second.used_bytes = p.used_base;
            it->second.period_start_ms = p.period_start_ms;
            it->second.bucket = 0;
        } else if (p.period_start_ms == it->second.period_start_ms && p.used_base > it->second.used_bytes) {
            // 落库值比本地大（例如重启后），取大者
            it->second.used_bytes = p.used_base;
        }
        next[p.hash] = p;
    }
    token_cache = std::move(next);

    // 丢掉已删除 Token 的运行状态
    for (auto it = token_usage.begin(); it != token_usage.end();) {
        bool keep = false;
        for (auto& kv : token_cache) {
            if (kv.second.id == it->first) { keep = true; break; }
        }
        if (keep) ++it; else it = token_usage.erase(it);
    }

    // 停用/过期/账号停用的 Token：踢掉在线连接（先收集，close 会同步 delete）
    uint64_t now = (uint64_t)time(NULL) * 1000;
    std::vector<RelayConn*> dead_conns;
    for (RelayConn* c = conns; c; c = c->next_all) {
        if (c->token_id == 0) continue;
        const TokenPolicy* p = tokenById(c->token_id);
        bool dead = (p == nullptr) || p->status != 0 || !p->account_active ||
                    (p->expires_at_ms > 0 && (int64_t)now >= p->expires_at_ms);
        if (dead) dead_conns.push_back(c);
    }
    for (RelayConn* c : dead_conns) {
        sendErr(c->io, rtnp::ERROR, 0, rtnp::TOKEN_DISABLED, "token disabled");
        connCloseAllStreams(c, rtnp::TOKEN_DISABLED);
        hio_close(c->io);
    }
    if (verbose) logf_("[relay] auth policies: %zu tokens, %zu accounts",
                       token_cache.size(), account_active.size());
}

std::string RelayServer::tokenUsageSnapshot() {
    nlohmann::json arr = nlohmann::json::array();
    for (auto& kv : token_usage) {
        nlohmann::json j;
        j["id"] = kv.first;
        j["used_bytes"] = kv.second.used_bytes;
        j["period_start_ms"] = kv.second.period_start_ms;
        j["last_used_ms"] = (long long)kv.second.last_used_ms;
        j["last_used_ip"] = kv.second.last_ip;
        arr.push_back(j);
    }
    return arr.dump();
}

const TokenPolicy* RelayServer::tokenById(long long id) const {
    for (auto& kv : token_cache) {
        if (kv.second.id == id) return &kv.second;
    }
    return nullptr;
}

TokenPolicy* RelayServer::tokenById(long long id) {
    for (auto& kv : token_cache) {
        if (kv.second.id == id) return &kv.second;
    }
    return nullptr;
}

void RelayServer::tokenSweep() {
    uint64_t now = (uint64_t)time(NULL) * 1000;
    for (auto it = token_cache.begin(); it != token_cache.end();) {
        TokenPolicy& p = it->second;
        bool expired = p.status != 0 || !p.account_active ||
                       (p.expires_at_ms > 0 && (int64_t)now >= p.expires_at_ms);
        if (expired) {
            tokenDropConnections(p.id, rtnp::TOKEN_DISABLED, "token disabled");
            it = token_cache.erase(it);
            continue;
        }
        // 周期重置
        TokenUsage& u = token_usage[p.id];
        if (p.quota_period != rtutil::PERIOD_TOTAL && u.period_start_ms > 0) {
            int64_t next = rtutil::tokenPeriodNextFrom((rtutil::TokenPeriod)p.quota_period, u.period_start_ms);
            if (next > 0 && (int64_t)now >= next) {
                u.used_bytes = 0;
                u.period_start_ms = rtutil::tokenPeriodStart((rtutil::TokenPeriod)p.quota_period, (int64_t)now);
            }
        }
        ++it;
    }
}

void RelayServer::tokenDropConnections(long long token_id, uint16_t code, const char* msg) {
    // 先收集再关闭：hio_close 会同步触发 onClose 并 delete 连接，不能边走链表边关
    std::vector<RelayConn*> targets;
    for (RelayConn* c = conns; c; c = c->next_all) {
        if (c->token_id == token_id) targets.push_back(c);
    }
    for (RelayConn* c : targets) {
        sendErr(c->io, rtnp::ERROR, 0, code, msg);
        connCloseAllStreams(c, code);
        hio_close(c->io);
    }
}

void RelayServer::tokenApplyCuts() {
    if (pending_token_cuts.empty()) return;
    std::set<long long> ids;
    ids.swap(pending_token_cuts);
    for (long long id : ids) tokenDropConnections(id, rtnp::QUOTA_EXCEEDED, "token quota exceeded");
}

void RelayServer::tokenAddBytes(RelayConn* c, uint32_t n) {
    if (c == NULL || c->token_id == 0 || n == 0) return;
    auto uit = token_usage.find(c->token_id);
    if (uit == token_usage.end()) return;
    TokenUsage& u = uit->second;
    u.used_bytes += n;
    u.last_used_ms = (uint64_t)time(NULL) * 1000;
    snprintf(u.last_ip, sizeof(u.last_ip), "%s", c->remote_addr);

    const TokenPolicy* p = tokenById(c->token_id);
    if (p == nullptr || p->quota_bytes <= 0) return;
    if (u.used_bytes < p->quota_bytes) return;
    if (p->quota_action == 1) return;   // block：已有会话继续，只拒绝新 OPEN
    // 这里在帧处理路径内，直接 close 会同步 delete 连接并造成悬垂，改为事件循环末尾统一处理
    logf_("[relay] token %lld quota exceeded (%lld bytes)", (long long)c->token_id,
          (long long)u.used_bytes);
    if (pending_token_cuts.insert(c->token_id).second) {
        htimer_t* t = htimer_add(loop, tokenCutCb, 1, 1);
        hevent_set_userdata(t, this);
    }
}

bool RelayServer::tokenRateAllow(RelayConn* c, uint32_t* retry_after_ms) {
    if (c == NULL || c->token_id == 0) return true;
    const TokenPolicy* p = tokenById(c->token_id);
    if (p == nullptr || p->rate_per_min <= 0) return true;
    TokenUsage& u = token_usage[c->token_id];
    int burst = p->burst > 0 ? p->burst : p->rate_per_min;
    double rate = p->rate_per_min / 60.0;   // 每秒补充
    uint64_t now = (uint64_t)time(NULL) * 1000;
    if (u.refill_ms == 0) {
        u.bucket = burst;
        u.refill_ms = now;
    } else if (now > u.refill_ms) {
        u.bucket += (now - u.refill_ms) * rate / 1000.0;
        if (u.bucket > burst) u.bucket = burst;
        u.refill_ms = now;
    }
    if (u.bucket >= 1.0) {
        u.bucket -= 1.0;
        return true;
    }
    if (retry_after_ms) {
        double need = 1.0 - u.bucket;
        *retry_after_ms = (uint32_t)(need / rate * 1000.0) + 1;
    }
    return false;
}

void RelayServer::tokenSessionAdd(RelayConn* c) {
    if (c == NULL || c->token_id == 0) return;
    token_usage[c->token_id].active_sessions++;
}

void RelayServer::tokenSessionRemove(RelayConn* c) {
    if (c == NULL || c->token_id == 0) return;
    auto it = token_usage.find(c->token_id);
    if (it != token_usage.end() && it->second.active_sessions > 0) it->second.active_sessions--;
}

static bool aclPortAllowed(const std::string& ports, uint16_t port) {
    if (ports.empty()) return false;
    size_t pos = 0;
    while (pos <= ports.size()) {
        size_t comma = ports.find(',', pos);
        std::string one = ports.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        while (!one.empty() && (one[0] == ' ' || one[0] == '\t')) one.erase(0, 1);
        if (one == "*") return true;
        size_t dash = one.find('-');
        if (dash == std::string::npos) {
            if (atoi(one.c_str()) == port) return true;
        } else {
            int lo = atoi(one.substr(0, dash).c_str());
            int hi = atoi(one.substr(dash + 1).c_str());
            if (lo <= port && port <= hi) return true;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

static bool aclTtyAllowed(const std::string& ttys, const char* path) {
    if (ttys.empty() || !path || !path[0]) return false;
    size_t pos = 0;
    while (pos <= ttys.size()) {
        size_t comma = ttys.find(',', pos);
        std::string one = ttys.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        while (!one.empty() && (one[0] == ' ' || one[0] == '\t')) one.erase(0, 1);
        if (!one.empty() && pattern_match(one.c_str(), path)) return true;
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

bool RelayServer::groupAclDevice(const char* user, const char* devid) {
    for (auto& r : acl_groups) {
        if (r.user != user) continue;
        if (pattern_match(r.devid.c_str(), devid)) return true;
    }
    return false;
}

bool RelayServer::groupAclCheck(const char* user, const char* devid, uint16_t port,
                                int is_shell, const char* tty_path) {
    for (auto& r : acl_groups) {
        if (r.user != user) continue;
        if (!pattern_match(r.devid.c_str(), devid)) continue;
        if (is_shell) {
            if (r.shell) return true;
        } else if (tty_path && tty_path[0]) {
            if (aclTtyAllowed(r.tty, tty_path)) return true;
        } else if (aclPortAllowed(r.ports, port)) {
            return true;
        }
    }
    return false;
}

int RelayServer::aclCheck(const char* user, const char* devid) {
    if (allow_all) return 1;
    if (user[0] == '\0') return 0;
    for (int i = 0; i < 2; ++i) {
        AclEntry* head = i == 0 ? acl_cli : acl_file;
        for (AclEntry* e = head; e; e = e->next) {
            if (strcmp(e->user, user) != 0) continue;
            if (pattern_match(e->pattern, devid)) return 1;
        }
    }
    return 0;
}

void RelayServer::aclAddTo(AclEntry** head, const char* user, const char* patterns) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", patterns);
    char* save = NULL;
    for (char* p = strtok_r(buf, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        auto* e = new AclEntry{};
        snprintf(e->user, sizeof(e->user), "%s", user);
        snprintf(e->pattern, sizeof(e->pattern), "%s", p);
        e->next = *head;
        *head = e;
    }
}

void RelayServer::aclFree(AclEntry** head) {
    AclEntry* e = *head;
    while (e) {
        AclEntry* next = e->next;
        delete e;
        e = next;
    }
    *head = NULL;
}

// 加载 ACL 文件：每行 user=devid[,devid|*]，# 注释、空行跳过。
// 只有整文件解析成功才替换旧表；任何错误保留旧授权（fail-closed 由默认拒绝保证）。
int RelayServer::aclFileLoad() {
    if (acl_file_path[0] == '\0') return -1;
    FILE* fp = fopen(acl_file_path, "r");
    if (fp == NULL) {
        logf_("[relay] acl reload: cannot open %s, keep old acl", acl_file_path);
        return -1;
    }
    AclEntry* newlist = NULL;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char* eq = strchr(p, '=');
        if (eq == NULL) {
            logf_("[relay] acl reload: %s:%d bad line, keep old acl", acl_file_path, lineno);
            fclose(fp);
            aclFree(&newlist);
            return -1;
        }
        *eq = '\0';
        char* pats = eq + 1;
        char* end = pats + strlen(pats);
        while (end > pats && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        aclAddTo(&newlist, p, pats);
    }
    fclose(fp);
    aclFree(&acl_file);
    acl_file = newlist;
    int n = 0;
    for (AclEntry* e = acl_file; e; e = e->next) n++;
    logf_("[relay] acl reloaded: %d entries from %s", n, acl_file_path);
    return 0;
}

// ---------------------------------------------------------------- 注册表与流

RelayConn* RelayServer::findAgent(const char* devid) {
    for (RelayConn* c = agents; c; c = c->reg_next) {
        if (strcmp(c->devid, devid) == 0) return c;
    }
    return NULL;
}

RelayStream* RelayServer::findStreamByA(RelayConn* a, uint32_t sid, RelayStream** prev) {
    RelayStream* p = NULL;
    for (RelayStream* s = a->streams; s; p = s, s = s->next_a) {
        if (s->a_sid == sid) { if (prev) *prev = p; return s; }
    }
    return NULL;
}

RelayStream* RelayServer::findStreamByC(RelayConn* c, uint32_t sid, RelayStream** prev) {
    RelayStream* p = NULL;
    for (RelayStream* s = c->streams; s; p = s, s = s->next_c) {
        if (s->c_sid == sid) { if (prev) *prev = p; return s; }
    }
    return NULL;
}

void RelayServer::streamRemove(RelayStream* s) {
    RelayStream* prev = NULL;
    if (findStreamByA(s->a, s->a_sid, &prev)) {
        if (prev) prev->next_a = s->next_a; else s->a->streams = s->next_a;
    }
    if (s->c) {
        prev = NULL;
        if (findStreamByC(s->c, s->c_sid, &prev)) {
            if (prev) prev->next_c = s->next_c; else s->c->streams = s->next_c;
        }
    }
    tokenSessionRemove(s->a);
    delete s;
}

void RelayServer::sendErr(hio_t* io, uint8_t type, uint32_t sid, uint16_t code, const char* msg) {
    rtnp::Writer w;
    w.u16(code);
    w.str(msg ? msg : "");
    rtnp::send(io, type, sid, w.data, (uint16_t)w.len);
}

void RelayServer::forward(RelayConn* to, uint8_t type, uint32_t sid, const uint8_t* payload, uint16_t len, uint16_t flags) {
    if (to && to->io && !hio_is_closed(to->io)) {
        if (rtnp::send(to->io, type, sid, payload, len, flags) == 0) {
            to->bytes_out += (uint64_t)len + rtnp::HEADER_SIZE;
        }
    }
}

// mTLS 身份校验：对端证书 SAN 必须与 AUTH 声称的身份一致（agent 的 devid、
// client 的 user）——身份取自证书，不信任自报。返回 1 = 通过，0 = 已拒绝并关闭。
int RelayServer::checkPeerSan(RelayConn* c, const char* claimed, const char* what, const char* id_name) {
    // 吊销黑名单优先于一切：身份被吊销即拒（与是否 mTLS 无关）
    if (isRevoked(claimed)) {
        logf_("[relay] %s identity revoked: %s=%s", what, id_name, claimed);
        sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "identity revoked");
        hio_close(c->io);
        return 0;
    }
    if (!require_mtls) return 1;
#ifdef WITH_OPENSSL
    char san[256] = {0};
    if (peer_cert_san(hio_get_ssl(c->io), san, sizeof(san)) != 0 ||
        strcmp(san, claimed) != 0) {
        logf_("[relay] %s cert SAN mismatch: %s=%s san=%s", what, id_name, claimed, san);
        sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "cert identity mismatch");
        hio_close(c->io);
        return 0;
    }
    return 1;
#else
    logf_("[relay] mTLS required but built without OpenSSL");
    sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "mtls unsupported");
    hio_close(c->io);
    return 0;
#endif
}

void RelayServer::registryRemove(RelayConn* c) {
    RelayConn** pp = &agents;
    while (*pp) {
        if (*pp == c) { *pp = c->reg_next; break; }
        pp = &(*pp)->reg_next;
    }
}

void RelayServer::connCloseAllStreams(RelayConn* c, uint16_t code) {
    RelayStream* s = c->streams;
    while (s) {
        RelayStream* next = (c->role == rtnp::ROLE_AGENT) ? s->next_c : s->next_a;
        RelayConn* peer = (c->role == rtnp::ROLE_AGENT) ? s->a : s->c;
        if (peer && peer->io && !hio_is_closed(peer->io)) {
            sendErr(peer->io, rtnp::RESET,
                    c->role == rtnp::ROLE_AGENT ? s->a_sid : s->c_sid, code, "peer gone");
        }
        // 解链另一侧，避免悬垂
        s->a_closed = s->c_closed = 1;
        streamRemove(s);
        s = next;
    }
    c->streams = NULL;
}

// ---------------------------------------------------------------- 管理台写操作

bool RelayServer::kickStream(uint32_t a_sid) {
    for (RelayConn* c = conns; c; c = c->next_all) {
        if (c->role != rtnp::ROLE_CLIENT) continue;
        RelayStream* s = findStreamByA(c, a_sid, NULL);
        if (!s) continue;
        if (s->a && !hio_is_closed(s->a->io)) {
            sendErr(s->a->io, rtnp::RESET, s->a_sid, rtnp::CANCELLED, "kicked by admin");
        }
        if (s->c && !hio_is_closed(s->c->io)) {
            sendErr(s->c->io, rtnp::RESET, s->c_sid, rtnp::CANCELLED, "kicked by admin");
        }
        s->a_closed = s->c_closed = 1;
        streamRemove(s);
        logf_("[relay] stream kicked: a_sid=%u", a_sid);
        return true;
    }
    return false;
}

bool RelayServer::kickAgent(const char* devid) {
    RelayConn* c = findAgent(devid);
    if (c == NULL) return false;
    connCloseAllStreams(c, rtnp::CANCELLED);
    registryRemove(c);
    hio_close(c->io);
    logf_("[relay] agent kicked: %s", devid);
    return true;
}

bool RelayServer::isRevoked(const std::string& san) const {
    for (const auto& r : revoked_sans) {
        if (r == san) return true;
    }
    return false;
}

bool RelayServer::revokeSan(const std::string& san) {
    if (!isRevoked(san)) {
        revoked_sans.push_back(san);
        if (!cert_dir.empty()) {
            FILE* fp = fopen((cert_dir + "/revoked.txt").c_str(), "a");
            if (fp) {
                fprintf(fp, "%s\n", san.c_str());
                fclose(fp);
            }
        }
    }
    // 先收集再关闭：hio_close 可能同步触发 onClose → delete c，不能边遍历边关
    std::vector<RelayConn*> victims;
    for (RelayConn* c = conns; c; c = c->next_all) {
        const char* id = (c->role == rtnp::ROLE_AGENT) ? c->devid : c->user;
        if (id[0] && san == id) victims.push_back(c);
    }
    for (RelayConn* c : victims) {
        connCloseAllStreams(c, rtnp::UNAUTHORIZED);
        if (c->role == rtnp::ROLE_AGENT) registryRemove(c);
        hio_close(c->io);
    }
    logf_("[relay] revoked: %s (kicked %zu connections)", san.c_str(), victims.size());
    return true;
}

static const uint64_t FILE_WINDOW = 256 * 1024;
static const size_t   FILE_CHUNK  = rtnp::MAX_PAYLOAD;

static bool sendFileChunk(RelayConn* a, uint32_t tid, FilePush& p) {
    if (!p.fp || p.sent >= p.size) return false;
    size_t want = (size_t)((p.size - p.sent) > FILE_CHUNK ? FILE_CHUNK : (p.size - p.sent));
    std::vector<char> buf(want);
    size_t got = fread(buf.data(), 1, want, p.fp);
    if (got == 0) return false;
    rtnp::send(a->io, rtnp::FILE_DATA, tid, buf.data(), (uint16_t)got);
    p.sent += got;
    return true;
}

static bool pumpFile(RelayConn* a, uint32_t tid, FilePush& p) {
    while (p.sent < p.size && p.sent - p.received < FILE_WINDOW) {
        if (!sendFileChunk(a, tid, p)) return false;
    }
    return true;
}

bool RelayServer::startFilePush(const char* devid, const char* path, const char* dir,
                                const char* name, uint64_t size, const std::string& sha,
                                std::string& err, uint32_t& tid) {
    RelayConn* a = findAgent(devid);
    if (!a || !a->io) { err = "device offline"; return false; }
    FILE* fp = fopen(path, "rb");
    if (!fp) { err = "open failed"; return false; }
    tid = ++push_seq;
    FilePush p;
    p.fp = fp;
    p.size = size;
    p.devid = devid;
    p.name = name;
    p.sha = sha;
    p.phase = 0;
    pushes[tid] = p;
    rtnp::Writer w;
    w.str(dir && dir[0] ? dir : "");
    w.str(name);
    w.u64(size);
    w.str(sha.c_str());
    rtnp::send(a->io, rtnp::FILE_BEGIN, tid, w.data, (uint16_t)w.len);
    return true;
}

std::string RelayServer::fileDirOf(const char* devid) {
    auto it = caps_file_dir.find(devid);
    return it == caps_file_dir.end() ? std::string() : it->second;
}

bool RelayServer::startTtyList(const char* devid, uint32_t& rid, std::string& err) {
    RelayConn* a = findAgent(devid);
    if (!a || !a->io) { err = "device offline"; return false; }
    rid = ++tty_list_seq;
    rtnp::send(a->io, rtnp::TTY_LIST_REQ, rid, NULL, 0);
    return true;
}

std::string RelayServer::takeTtyListResult(uint32_t rid) {
    auto it = tty_list_results.find(rid);
    if (it == tty_list_results.end()) return std::string();
    std::string s = it->second;
    tty_list_results.erase(it);
    return s;
}

bool RelayServer::hasAgent(const char* devid) { return findAgent(devid) != NULL; }

bool RelayServer::startMetricQuery(const char* devid, const std::string& json, uint32_t& rid, std::string& err) {
    RelayConn* a = findAgent(devid);
    if (!a || !a->io) { err = "device offline"; return false; }
    rid = ++metric_query_seq;
    rtnp::Writer w;
    w.str(json.c_str());
    rtnp::send(a->io, rtnp::METRICS_QUERY, rid, w.data, (uint16_t)w.len);
    return true;
}

std::string RelayServer::takeMetricData(uint32_t rid) {
    auto it = metric_data_results.find(rid);
    if (it == metric_data_results.end()) return std::string();
    std::string s = it->second;
    metric_data_results.erase(it);
    return s;
}

std::string RelayServer::takeMetricStatus(const std::string& devid) {
    auto it = metric_status.find(devid);
    if (it == metric_status.end()) return std::string();
    std::string s = it->second;
    metric_status.erase(it);
    return s;
}

bool RelayServer::pushMetricCfg(const char* devid, const std::string& json) {
    RelayConn* a = findAgent(devid);
    if (!a || !a->io) return false;
    rtnp::Writer w;
    w.str(json.c_str());
    rtnp::send(a->io, rtnp::METRICS_CFG, 0, w.data, (uint16_t)w.len);
    return true;
}

bool RelayServer::pushTtyCfg(const char* devid, const std::string& json) {
    RelayConn* a = findAgent(devid);
    if (!a || !a->io) return false;
    rtnp::Writer w;
    w.str(json.c_str());
    rtnp::send(a->io, rtnp::TTY_CFG_SET, 0, w.data, (uint16_t)w.len);
    return true;
}

bool RelayServer::startFileCheck(const char* devid, const char* dir, const char* name,
                                 uint32_t& cid, std::string& err) {
    RelayConn* a = findAgent(devid);
    if (!a || !a->io) { err = "device offline"; return false; }
    cid = ++check_seq;
    rtnp::Writer w;
    w.str(dir && dir[0] ? dir : "");
    w.str(name);
    rtnp::send(a->io, rtnp::FILE_CHECK_REQ, cid, w.data, (uint16_t)w.len);
    return true;
}

std::string RelayServer::takeCheckResult(uint32_t cid) {
    auto it = check_results.find(cid);
    if (it == check_results.end()) return std::string();
    std::string s = it->second;
    check_results.erase(it);
    return s;
}

bool RelayServer::cancelFilePush(uint32_t tid) {
    auto it = pushes.find(tid);
    if (it == pushes.end()) return false;
    if (it->second.fp) fclose(it->second.fp);
    nlohmann::json r;
    r["ok"] = false;
    r["error"] = "canceled";
    push_results[tid] = r.dump();
    pushes.erase(it);
    return true;
}

std::string RelayServer::pushProgress(uint32_t tid) {
    nlohmann::json j;
    auto pit = pushes.find(tid);
    if (pit != pushes.end()) {
        j["found"] = true;
        j["done"] = false;
        j["sent"] = pit->second.sent;
        j["size"] = pit->second.size;
        return j.dump();
    }
    auto rit = push_results.find(tid);
    if (rit != push_results.end()) {
        auto r = nlohmann::json::parse(rit->second, nullptr, false);
        j["found"] = true;
        j["done"] = true;
        j["ok"] = r.is_discarded() ? false : r.value("ok", false);
        j["received"] = r.is_discarded() ? 0 : r.value("received", (uint64_t)0);
        j["size"] = j["received"];
        if (!r.is_discarded() && r.contains("error")) j["error"] = r["error"];
        return j.dump();
    }
    j["found"] = false;
    return j.dump();
}

void RelayServer::onFileAck(RelayConn* c, const rtnp::Frame* f) {
    auto it = pushes.find(f->sid);
    if (it == pushes.end()) return;
    FilePush& p = it->second;
    rtnp::Reader rr(f->payload, f->len);
    static char ackbuf[1024];
    rr.str(ackbuf, sizeof ackbuf);
    auto j = nlohmann::json::parse(rr.err ? std::string() : std::string(ackbuf), nullptr, false);
    auto fail = [&](const std::string& e) {
        if (p.fp) fclose(p.fp);
        nlohmann::json r;
        r["ok"] = false;
        r["error"] = e;
        push_results[f->sid] = r.dump();
        pushes.erase(it);
    };
    if (j.is_discarded() || !j.value("ok", false)) {
        fail(j.is_discarded() ? "bad ack" : j.value("error", std::string("agent error")));
        return;
    }
    uint64_t received = j.value("received", (uint64_t)0);
    if (p.phase == 0) {
        if (received > p.size) { fail("received > size"); return; }
        p.received = received;
        p.sent = received;
        if (p.fp) fseek(p.fp, (long)received, SEEK_SET);
        p.phase = 1;
        if (!pumpFile(c, f->sid, p)) fail("read failed");
        return;
    }
    if (p.phase == 1) {
        if (received > p.sent) { fail("received mismatch"); return; }
        p.received = received;
        if (p.sent >= p.size && p.received >= p.size) {
            p.phase = 2;
            rtnp::Writer w;
            w.str(p.sha.c_str());
            rtnp::send(c->io, rtnp::FILE_END, f->sid, w.data, (uint16_t)w.len);
            return;
        }
        if (!pumpFile(c, f->sid, p)) fail("read failed");
        return;
    }
    // phase 2：完成
    if (p.fp) fclose(p.fp);
    nlohmann::json r;
    r["ok"] = true;
    r["received"] = received;
    push_results[f->sid] = r.dump();
    pushes.erase(it);
}

bool RelayServer::sendConfig(const char* devid, uint32_t sid, const std::string& json) {
    RelayConn* a = findAgent(devid);
    if (!a || !a->io) return false;
    rtnp::Writer w;
    w.str(json.c_str());
    rtnp::send(a->io, rtnp::CONFIG_SET, sid, w.data, (uint16_t)w.len);
    return true;
}

std::string RelayServer::configAck(const char* devid, uint32_t sid, uint64_t* ts) {
    auto it = config_acks.find(devid);
    if (it == config_acks.end() || it->second.sid != sid) { if (ts) *ts = 0; return std::string(); }
    if (ts) *ts = it->second.ts;
    return it->second.json;
}

bool RelayServer::sendSysinfoReq(const char* devid, int mode) {
    RelayConn* a = findAgent(devid);
    if (!a || !a->io) return false;
    rtnp::Writer w;
    w.u8((uint8_t)mode);
    rtnp::send(a->io, rtnp::SYSINFO_REQ, 0, w.data, (uint16_t)w.len);
    return true;
}

std::string RelayServer::sysinfoLastOf(const char* devid, bool* full, uint64_t* ts) {
    auto it = sysinfo_last.find(devid);
    if (it == sysinfo_last.end()) { if (full) *full = true; if (ts) *ts = 0; return std::string(); }
    if (full) *full = it->second.full;
    if (ts) *ts = it->second.ts;
    return it->second.json;
}

std::string RelayServer::sysinfoOf(const char* devid, uint64_t* ts) {
    auto it = sysinfo_cache.find(devid);
    if (it == sysinfo_cache.end()) { if (ts) *ts = 0; return std::string(); }
    if (ts) *ts = it->second.ts;
    return it->second.json;
}

void RelayServer::loadRevoked() {
    if (cert_dir.empty()) return;
    FILE* fp = fopen((cert_dir + "/revoked.txt").c_str(), "r");
    if (fp == NULL) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        std::string san(line);
        while (!san.empty() && (san.back() == '\n' || san.back() == '\r' || san.back() == ' ')) san.pop_back();
        if (!san.empty() && san[0] != '#' && !isRevoked(san)) revoked_sans.push_back(san);
    }
    fclose(fp);
    logf_("[relay] loaded %zu revoked identities", revoked_sans.size());
}

// 未认证连接超时：10s 内未走完 HELLO + AUTH 即回收，防半开连接占 fd
void RelayServer::onHsTimeout(RelayConn* c) {
    c->hs_timer = NULL;
    if (!c->authed) {
        logf_("[relay] handshake/auth timeout fd=%d", hio_fd(c->io));
        hio_close(c->io);
    }
}

// ---------------------------------------------------------------- 读循环

void RelayServer::handleFrame(RelayConn* c, rtnp::Frame* f) {
    if (verbose) logf_("[relay] frame role=%d type=0x%02x sid=%u len=%u", c->role, f->type, f->sid, f->len);

    switch (f->type) {
    case rtnp::HELLO: {
        rtnp::Reader r(f->payload, f->len);
        uint8_t role = r.u8();
        uint8_t ver  = r.u8();
        char aver[64] = {0};
        r.str(aver, sizeof(aver));
        c->internal = (strstr(aver, "admin") != NULL || strstr(aver, "terminal") != NULL);
        if (r.err || ver != rtnp::VERSION || (role != rtnp::ROLE_AGENT && role != rtnp::ROLE_CLIENT)) {
            sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::PROTOCOL_ERROR, "bad hello");
            hio_close(c->io);
            return;
        }
        c->role = role;
        rtnp::Writer w;
        w.u16(rtnp::MAX_PAYLOAD);
        w.u8(rtnp::AUTH_MTLS);
        w.u32(15000);
        w.u32(0);
        rtnp::send(c->io, rtnp::HELLO_ACK, 0, w.data, (uint16_t)w.len);
        if (verbose) logf_("[relay] HELLO role=%d fd=%d", role, hio_fd(c->io));
        return;
    }
    case rtnp::FILE_ACK:
        onFileAck(c, f);
        return;
    case rtnp::METRICS_DATA: {
        rtnp::Reader rr(f->payload, f->len);
        std::string md = rr.strBytes();
        if (!rr.err) metric_data_results[f->sid] = md;
        return;
    }
    case rtnp::METRICS_STATUS: {
        rtnp::Reader rr(f->payload, f->len);
        static char ms[4096];
        rr.str(ms, sizeof ms);
        if (!rr.err && c->devid) metric_status[c->devid] = std::string(ms);
        return;
    }
    case rtnp::TTY_LIST_ACK: {
        rtnp::Reader rr(f->payload, f->len);
        static char tb[64 * 1024];
        rr.str(tb, sizeof tb);
        if (!rr.err) tty_list_results[f->sid] = std::string(tb);
        return;
    }
    case rtnp::FILE_CHECK_ACK: {
        rtnp::Reader rr(f->payload, f->len);
        static char buf[1024];
        rr.str(buf, sizeof buf);
        if (!rr.err) check_results[f->sid] = std::string(buf);
        return;
    }
    case rtnp::CONFIG_ACK: {
        if (c->role != rtnp::ROLE_AGENT) return;
        rtnp::Reader r(f->payload, f->len);
        static char buf[4096];
        r.str(buf, sizeof(buf));
        if (!r.err) {
            ConfigAck a;
            a.sid = f->sid;
            a.json = std::string(buf);
            a.ts = (uint64_t)time(NULL) * 1000;
            config_acks[c->devid] = a;
            auto aj = nlohmann::json::parse(a.json, nullptr, false);
            if (!aj.is_discarded() && aj.contains("shell")) {
                caps_shell[c->devid] = aj["shell"].get<bool>() ? 1 : 0;
            }
            if (!aj.is_discarded() && aj.contains("file_dir")) {
                caps_file_dir[c->devid] = aj["file_dir"].get<std::string>();
            }
        }
        return;
    }
    case rtnp::SYSINFO: {
        if (c->role != rtnp::ROLE_AGENT) return;
        rtnp::Reader r(f->payload, f->len);
        static char buf[65600];
        r.str(buf, sizeof(buf));
        if (!r.err) {
            SysinfoEntry e;
            auto part = nlohmann::json::parse(std::string(buf), nullptr, false);
            bool full = part.is_discarded() || part.value("full", true);
            e.ts = hloop_now_ms(loop);
            if (full) {
                e.json = std::string(buf);
                sysinfo_cache[c->devid] = e;
            } else {
                auto it = sysinfo_cache.find(c->devid);
                if (it == sysinfo_cache.end()) {
                    e.json = std::string(buf);
                    sysinfo_cache[c->devid] = e;
                } else {
                    auto base = nlohmann::json::parse(it->second.json, nullptr, false);
                    if (base.is_discarded()) base = nlohmann::json::object();
                    if (part.contains("swap_free_kb")) base["swap_free_kb"] = part["swap_free_kb"];
                    if (part.contains("mounts") && base.contains("mounts")) {
                        std::map<std::string, uint64_t> free_kb;
                        for (auto& m : part["mounts"]) free_kb[m.value("mount", "")] = m.value("free_kb", (uint64_t)0);
                        for (auto& m : base["mounts"]) {
                            auto fi = free_kb.find(m.value("mount", ""));
                            if (fi != free_kb.end()) m["free_kb"] = fi->second;
                        }
                    }
                    if (part.contains("serial")) base["serial"] = part["serial"];
                    if (part.contains("nics") && base.contains("nics")) {
                        std::map<std::string, nlohmann::json> nm;
                        for (auto& n : part["nics"]) nm[n.value("name", "")] = n;
                        for (auto& n : base["nics"]) {
                            auto ni = nm.find(n.value("name", ""));
                            if (ni == nm.end()) continue;
                            if (ni->second.contains("ip")) n["ip"] = ni->second["ip"];
                            if (ni->second.contains("up")) n["up"] = ni->second["up"];
                        }
                    }
                    e.json = base.dump();
                    sysinfo_cache[c->devid] = e;
                }
            }
            SysinfoLast sl;
            sl.json = std::string(buf);
            sl.full = full;
            sl.ts = e.ts;
            sysinfo_last[c->devid] = sl;
            auto sj = nlohmann::json::parse(e.json, nullptr, false);
            if (!sj.is_discarded() && sj.contains("caps") && sj["caps"].contains("shell")) {
                caps_shell[c->devid] = sj["caps"]["shell"].get<bool>() ? 1 : 0;
            }
            if (!sj.is_discarded() && sj.contains("caps") && sj["caps"].contains("file_dir")) {
                caps_file_dir[c->devid] = sj["caps"]["file_dir"].get<std::string>();
            }
        }
        return;
    }
    case rtnp::AUTH: {
        rtnp::Reader r(f->payload, f->len);
        if (c->role == rtnp::ROLE_AGENT) {
            r.str(c->devid, sizeof(c->devid));
            if (r.err || c->devid[0] == '\0') {
                sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "bad devid");
                hio_close(c->io);
                return;
            }
            // A 侧按需 agent 用 a:<user> 注册，证书 SAN 仍是 user（复用客户端证书）
            const char* san_expect = c->devid;
            if (strncmp(san_expect, "a:", 2) == 0) san_expect += 2;
            if (!checkPeerSan(c, san_expect, "device", "devid")) return;
            // 顶替旧连接
            RelayConn* old = findAgent(c->devid);
            if (old && old != c) {
                connCloseAllStreams(old, rtnp::DEVICE_REPLACED);
                registryRemove(old);
                hio_close(old->io);
            }
            c->authed = 1;
            if (c->hs_timer) { htimer_del(c->hs_timer); c->hs_timer = NULL; }
            c->reg_next = agents;
            agents = c;
            rtnp::Writer w;
            w.str(c->devid);
            w.u32(15000);
            rtnp::send(c->io, rtnp::AUTH_OK, 0, w.data, (uint16_t)w.len);
            logf_("[relay] agent online: %s (fd=%d)", c->devid, hio_fd(c->io));
            completePending(c->devid);
        } else {
            r.str(c->user, sizeof(c->user));
            // 凭证：u8 auth_type(0=none,1=静态 token,2=账号 token) + str token
            uint8_t auth_type = r.err ? 0 : r.u8();
            char tok[256] = {0};
            if (!r.err && (auth_type == 1 || auth_type == 2)) r.str(tok, sizeof(tok));

            const TokenPolicy* pol = NULL;
            TokenUsage* usage = NULL;
            uint64_t now = (uint64_t)time(NULL) * 1000;

            if (auth_type == 2) {
                std::string hash = rtutil::sha256Hex(std::string(tok));
                auto it = token_cache.find(hash);
                if (it == token_cache.end()) {
                    logf_("[relay] token auth rejected: unknown token prefix=%.12s", tok);
                    sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::TOKEN_DISABLED, "invalid token");
                    hio_close(c->io);
                    return;
                }
                pol = &it->second;
                if (pol->status != 0 || !pol->account_active) {
                    sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::TOKEN_DISABLED, "token disabled");
                    hio_close(c->io);
                    return;
                }
                if (pol->expires_at_ms > 0 && (int64_t)now >= pol->expires_at_ms) {
                    sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::TOKEN_EXPIRED, "token expired");
                    hio_close(c->io);
                    return;
                }
                if (c->user[0] == '\0') {
                    snprintf(c->user, sizeof(c->user), "%s", pol->username.c_str());
                } else if (c->user != pol->username) {
                    sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "user mismatch");
                    hio_close(c->io);
                    return;
                }
                usage = &token_usage[pol->id];
                if (pol->quota_bytes > 0 && usage->used_bytes >= pol->quota_bytes) {
                    sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::QUOTA_EXCEEDED, "token quota exceeded");
                    hio_close(c->io);
                    return;
                }
                c->token_id = pol->id;
                snprintf(c->token_prefix, sizeof(c->token_prefix), "%s", pol->prefix.c_str());
                usage->last_used_ms = now;
                snprintf(usage->last_ip, sizeof(usage->last_ip), "%s", c->remote_addr);
            }
            // 静态凭据：auth_type=2 之外（含未带凭证的 0）都必须通过静态校验
            if (auth_type != 2 && clientAuthEnabled()) {
                std::string expect;
                for (const auto& cc : client_creds) {
                    if (cc.user == c->user) { expect = cc.token; break; }
                }
                if (expect.empty()) expect = client_token;
                if (expect.empty() || tok != expect) {
                    logf_("[relay] client auth rejected: user=%s", c->user);
                    sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "bad token");
                    hio_close(c->io);
                    return;
                }
            }
            // 静态凭据也要服从账号状态：账号停用后旧 token 一并失效
            if (auth_type == 1 && c->user[0] != '\0') {
                auto ait = account_active.find(c->user);
                if (ait != account_active.end() && ait->second == 0) {
                    sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::TOKEN_DISABLED, "account disabled");
                    hio_close(c->io);
                    return;
                }
            }

            if (r.err || c->user[0] == '\0') {
                sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "bad user");
                hio_close(c->io);
                return;
            }
            if (!checkPeerSan(c, c->user, "client", "user")) return;
            c->authed = 1;
            if (c->hs_timer) { htimer_del(c->hs_timer); c->hs_timer = NULL; }
            rtnp::Writer w;
            w.str(c->user);
            w.u32(15000);
            if (pol != NULL) {
                // 可选尾部：旧客户端读到 username+heartbeat 即停止解析，多出的字节被忽略
                w.u8(1);
                w.u64((uint64_t)pol->expires_at_ms);
                w.u64((uint64_t)pol->quota_bytes);
                w.u64((uint64_t)usage->used_bytes);
                w.u32((uint32_t)pol->rate_per_min);
                w.u32((uint32_t)pol->max_sessions);
            }
            rtnp::send(c->io, rtnp::AUTH_OK, 0, w.data, (uint16_t)w.len);
            if (pol != NULL) {
                logf_("[relay] client online: %s token=%s (fd=%d)", c->user,
                      c->token_prefix, hio_fd(c->io));
            } else {
                logf_("[relay] client online: %s (fd=%d)", c->user, hio_fd(c->io));
            }
        }
        return;
    }
    case rtnp::PING:
        rtnp::send(c->io, rtnp::PONG, 0, f->payload, f->len);
        return;
    case rtnp::DEVICE_QUERY: {
        rtnp::Reader r(f->payload, f->len);
        char devid[128] = {0};
        r.str(devid, sizeof(devid));
        int online = 0;
        uint32_t active = 0;
        bool allowed = c->authed && c->role == rtnp::ROLE_CLIENT &&
            (acl_grouped_users.count(c->user) ? groupAclDevice(c->user, devid) : aclCheck(c->user, devid));
        // 查询限速：超限按离线处理，不返回 ERROR（客户端会把 ERROR 当作认证失败断链）
        if (!r.err && allowed && !tokenRateAllow(c, NULL)) allowed = false;
        if (!r.err && allowed) {
            RelayConn* agent = findAgent(devid);
            if (agent) {
                online = 1;
                for (RelayStream* st = agent->streams; st; st = st->next_c) active++;
            }
        }
        rtnp::Writer w;
        w.str(devid);
        w.u8((uint8_t)online);
        w.u32(active);
        rtnp::send(c->io, rtnp::DEVICE_STATUS, 0, w.data, (uint16_t)w.len);
        return;
    }
    default:
        break;
    }

    if (!c->authed) {
        sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "not authed");
        hio_close(c->io);
        return;
    }

    if (c->role == rtnp::ROLE_CLIENT) {
        switch (f->type) {
        case rtnp::OPEN: {
            rtnp::Reader r(f->payload, f->len);
            char devid[128] = {0};
            r.str(devid, sizeof(devid));
            uint16_t port = r.u16();
            // OPEN_SHELL 无目标端口（设备侧 forkpty），port=0 合法；普通 OPEN 必须非 0
            bool is_shell = (f->flags & rtnp::OPEN_SHELL) != 0;
            bool is_tty = (f->flags & rtnp::OPEN_TTY) != 0;
            char tty_path[256] = {0};
            if (is_tty && !r.err) {
                char tdev[128] = {0};
                r.str(tdev, sizeof(tdev));
                r.str(tty_path, sizeof(tty_path));
            }
            if (r.err || devid[0] == '\0' || (!is_shell && !is_tty && port == 0)) {
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::PROTOCOL_ERROR, "bad open");
                return;
            }
            if (findStreamByA(c, f->sid, NULL) != NULL) {
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::PROTOCOL_ERROR, "sid in use");
                return;
            }
            if (c->token_id != 0) {
                uint32_t retry = 0;
                if (!tokenRateAllow(c, &retry)) {
                    char msg[64];
                    snprintf(msg, sizeof(msg), "rate limited, retry after %u ms", retry);
                    sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::RATE_LIMITED, msg);
                    return;
                }
                const TokenPolicy* tp = tokenById(c->token_id);
                TokenUsage& tu = token_usage[c->token_id];
                if (tp != NULL) {
                    if (tp->max_sessions > 0 && tu.active_sessions >= tp->max_sessions) {
                        sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::SESSION_LIMIT, "session limit reached");
                        return;
                    }
                    if (tp->quota_action == 1 && tp->quota_bytes > 0 && tu.used_bytes >= tp->quota_bytes) {
                        sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::QUOTA_EXCEEDED, "token quota exceeded");
                        return;
                    }
                }
            }
            bool grouped = acl_grouped_users.count(c->user) != 0;
            bool acl_ok = grouped
                ? groupAclCheck(c->user, devid, port, is_shell ? 1 : 0, tty_path)
                : aclCheck(c->user, devid);
            if (!acl_ok) {
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::ACL_DENIED, "acl denied");
                return;
            }
            if (is_shell && !shellAllowed(c->user)) {
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::ACL_DENIED, "shell not allowed");
                return;
            }
            RelayConn* agent = findAgent(devid);
            if (agent == NULL) {
                // A 侧按需 agent：a:<user> 未注册时通知该 client 拉起本地 agent，先挂起 OPEN
                if (is_shell && strncmp(devid, "a:", 2) == 0) {
                    RelayConn* target = findClientByUser(devid + 2);
                    if (target != NULL) {
                        rtnp::Writer w;
                        w.str(devid);
                        rtnp::send(target->io, rtnp::SPAWN_AGENT, 0, w.data, (uint16_t)w.len);
                        addPending(c, f->sid, devid, port, f->flags);
                        logf_("[relay] OPEN_SHELL %s: ask client %s to spawn agent", devid, target->user);
                        return;
                    }
                }
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::DEVICE_OFFLINE, "device offline");
                return;
            }
            auto* s = new RelayStream{};
            s->a = c; s->a_sid = f->sid;
            s->c = agent; s->c_sid = ++agent->next_c_sid;
            s->next_a = c->streams; c->streams = s;
            s->next_c = agent->streams; agent->streams = s;
            tokenSessionAdd(c);

            if (is_tty) {
                rtnp::send(agent->io, rtnp::OPEN, s->c_sid, f->payload, f->len, f->flags);
            } else {
                rtnp::Writer w;
                w.u16(port);
                rtnp::send(agent->io, rtnp::OPEN, s->c_sid, w.data, (uint16_t)w.len, f->flags);
            }
            if (verbose) logf_("[relay] OPEN %s:%u a_sid=%u c_sid=%u flags=0x%04x", devid, port, s->a_sid, s->c_sid, f->flags);
            return;
        }
        case rtnp::DATA:
        case rtnp::WINDOW:
        case rtnp::CLOSE:
        case rtnp::RESET:
        case rtnp::RESIZE: {
            RelayStream* s = findStreamByA(c, f->sid, NULL);
            if (s == NULL || s->c == NULL) {
                sendErr(c->io, rtnp::RESET, f->sid, rtnp::PROTOCOL_ERROR, "unknown sid");
                return;
            }
            forward(s->c, f->type, s->c_sid, f->payload, f->len, f->flags);
            if (f->type == rtnp::DATA) tokenAddBytes(c, f->len);
            if (f->type == rtnp::CLOSE) s->a_closed = 1;
            if (f->type == rtnp::RESET) { s->a_closed = s->c_closed = 1; }
            if (s->a_closed && s->c_closed) streamRemove(s);
            return;
        }
        default:
            sendErr(c->io, rtnp::ERROR, 0, rtnp::PROTOCOL_ERROR, "unexpected type");
            return;
        }
    }

    // 来自 agent 的会话级帧（含 A 的 client 作为 agent 端点：按 c_sid 路由）
    switch (f->type) {
    case rtnp::OPEN_OK:
    case rtnp::OPEN_FAIL: {
        RelayStream* s = findStreamByC(c, f->sid, NULL);
        if (s == NULL) return;
        if (f->type == rtnp::OPEN_OK && !s->opened) {
            s->opened = 1;
            s->opened_at_ms = hloop_now_ms(loop);
        }
        forward(s->a, f->type, s->a_sid, f->payload, f->len, f->flags);
        if (f->type == rtnp::OPEN_FAIL) { s->a_closed = s->c_closed = 1; streamRemove(s); }
        return;
    }
    case rtnp::DATA:
    case rtnp::WINDOW:
    case rtnp::CLOSE:
    case rtnp::RESET:
    case rtnp::RESIZE: {
        RelayStream* s = findStreamByC(c, f->sid, NULL);
        if (s == NULL) {
            sendErr(c->io, rtnp::RESET, f->sid, rtnp::PROTOCOL_ERROR, "unknown sid");
            return;
        }
        forward(s->a, f->type, s->a_sid, f->payload, f->len, f->flags);
        if (f->type == rtnp::DATA) tokenAddBytes(s->a, f->len);
        if (f->type == rtnp::CLOSE) s->c_closed = 1;
        if (f->type == rtnp::RESET) { s->a_closed = s->c_closed = 1; }
        if (s->a_closed && s->c_closed) streamRemove(s);
        return;
    }
    default:
        sendErr(c->io, rtnp::ERROR, 0, rtnp::PROTOCOL_ERROR, "unexpected type");
        return;
    }
}

void RelayServer::onRead(RelayConn* c, void* buf, int readbytes) {
    // handleFrame 可能经 hio_close 同步触发 onClose 并 delete c，
    // 此后只允许使用 io 的局部副本，不得再解引用 c（与原 C 版一致）。
    hio_t* io = c->io;
    c->bytes_in += (uint64_t)readbytes;
    c->last_seen_ms = hloop_now_ms(loop);
    if (c->in.append(buf, (size_t)readbytes) != 0) {
        hio_close(io);
        return;
    }
    for (;;) {
        rtnp::Frame f;
        int rc = c->in.next(f);
        if (rc == 0) return;
        if (rc < 0) {
            sendErr(io, rtnp::ERROR, 0, rtnp::PROTOCOL_ERROR, "bad frame");
            hio_close(io);
            return;
        }
        handleFrame(c, &f);
        if (hio_is_closed(io)) return;
    }
}

void RelayServer::onClose(RelayConn* c) {
    if (c->hs_timer) { htimer_del(c->hs_timer); c->hs_timer = NULL; }
    // agent 掉线 = 设备离线；client 掉线 = 会话取消（设备并未离线）
    connCloseAllStreams(c, c->role == rtnp::ROLE_AGENT ? rtnp::DEVICE_OFFLINE : rtnp::CANCELLED);
    dropPendingFor(c);
    if (c->role == rtnp::ROLE_AGENT) {
        registryRemove(c);
        if (c->devid[0]) logf_("[relay] agent offline: %s", c->devid);
    } else if (c->role == rtnp::ROLE_CLIENT && c->user[0]) {
        logf_("[relay] client offline: %s", c->user);
    }

    // 从全局链表摘除
    RelayConn** pp = &conns;
    while (*pp) {
        if (*pp == c) { *pp = c->next_all; break; }
        pp = &(*pp)->next_all;
    }
    hio_set_context(c->io, NULL);
    delete c;
}

void RelayServer::onAccept(hio_t* io) {
    auto* c = new RelayConn{};
    c->server = this;
    c->io = io;
    c->connected_at_ms = c->last_seen_ms = hloop_now_ms(loop);
    SOCKADDR_STR(hio_peeraddr(io), c->remote_addr);
    c->next_all = conns;
    conns = c;

    hio_set_context(io, c);
    hio_setcb_read(io, readCb);
    hio_setcb_close(io, closeCb);
    hio_set_max_write_bufsize(io, rtnp::CONN_INFLIGHT + rtnp::MAX_PAYLOAD);
    hio_read(io);
    // 未认证连接 10s 未走完 HELLO+AUTH 即回收
    c->hs_timer = htimer_add(loop, hsTimeoutCb, 10000, 1);
    hevent_set_userdata(c->hs_timer, c);
    if (verbose) logf_("[relay] accept fd=%d", hio_fd(io));
}

// ---------------------------------------------------------------- 运维面

// SIGHUP：热加载 --acl-file；命令行 --acl 条目不受影响
void RelayServer::onSighup() {
    if (acl_file_path[0]) {
        aclFileLoad();
    } else {
        logf_("[relay] SIGHUP ignored: no --acl-file configured");
    }
}

// 周期统计：连接数 / 活跃流（按 agent 侧计，避免双算）/ 累计字节
void RelayServer::onStats() {
    int nclient = 0, nagent = 0, nstream = 0;
    uint64_t in = 0, out = 0;
    for (RelayConn* c = conns; c; c = c->next_all) {
        if (c->role == rtnp::ROLE_AGENT) {
            nagent++;
            for (RelayStream* s = c->streams; s; s = s->next_c) nstream++;
        } else if (c->role == rtnp::ROLE_CLIENT) {
            nclient++;
        }
        in += c->bytes_in;
        out += c->bytes_out;
    }
    logf_("[relay] stats: agents=%d clients=%d streams=%d bytes_in=%llu bytes_out=%llu",
          nagent, nclient, nstream, (unsigned long long)in, (unsigned long long)out);
}

void RelayServer::onDrain(htimer_t* timer) {
    if (conns && hloop_now_ms(loop) < drain_deadline) return;
    int left = 0;
    for (RelayConn* c = conns; c; c = c->next_all) left++;
    logf_("[relay] drain done (remaining=%d), exiting", left);
    htimer_del(timer);
    hloop_stop(loop);
}

// SIGTERM/SIGINT：停止 accept → 等现有连接收尾（--drain-timeout）→ 退出。
// 第二次信号立即退出。A/C 侧断线后按既有退避重连，无需额外通知。
void RelayServer::onShutdown(int signo) {
    if (draining) {
        logf_("[relay] signal %d again, force exit", signo);
        hloop_stop(loop);
        return;
    }
    draining = 1;
    logf_("[relay] signal %d: draining (stop accept, timeout %u ms)", signo, drain_timeout_ms);
    if (listenio) hio_close(listenio);
    if (conns == NULL) { hloop_stop(loop); return; }
    drain_deadline = hloop_now_ms(loop) + drain_timeout_ms;
    htimer_t* t = htimer_add(loop, drainCb, 100, INFINITE);
    hevent_set_userdata(t, this);
}

// ---------------------------------------------------------------- 入口

int RelayServer::run(int argc, char** argv) {
    g_self = this;
    const char* host = "0.0.0.0";
    int port = 8443;
    const char* tls_cert = NULL;
    const char* tls_key  = NULL;
    const char* tls_ca   = NULL;
    const char* admin_addr = NULL;
    const char* admin_token = NULL;
    const char* admin_docroot = NULL;
    const char* admin_ca_cert = NULL;
    const char* admin_ca_key = NULL;
    const char* admin_cert_dir = NULL;
    const char* admin_file_dir = NULL;
    const char* admin_shell_user = "devuser";
    const char* admin_shell_token = NULL;
    const char* admin_db = NULL;
    const char* client_users_file = NULL;
    const char* client_token_arg = NULL;
    const char* shell_users_arg = NULL;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            tls_cert = argv[++i];
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            tls_key = argv[++i];
        } else if (strcmp(argv[i], "--tls-ca") == 0 && i + 1 < argc) {
            tls_ca = argv[++i];
        } else if (strcmp(argv[i], "--allow-all") == 0) {
            allow_all = 1;
        } else if (strcmp(argv[i], "--client-users") == 0 && i + 1 < argc) {
            client_users_file = argv[++i];
        } else if (strcmp(argv[i], "--client-token") == 0 && i + 1 < argc) {
            client_token_arg = argv[++i];
        } else if (strcmp(argv[i], "--shell-users") == 0 && i + 1 < argc) {
            shell_users_arg = argv[++i];
        } else if (strcmp(argv[i], "--admin-shell-token") == 0 && i + 1 < argc) {
            admin_shell_token = argv[++i];
        } else if (strcmp(argv[i], "--admin-db") == 0 && i + 1 < argc) {
            admin_db = argv[++i];
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--acl") == 0 && i + 1 < argc) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char* eq = strchr(buf, '=');
            if (eq) {
                *eq = '\0';
                aclAddTo(&acl_cli, buf, eq + 1);
            }
        } else if (strcmp(argv[i], "--acl-file") == 0 && i + 1 < argc) {
            snprintf(acl_file_path, sizeof(acl_file_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--drain-timeout") == 0 && i + 1 < argc) {
            drain_timeout_ms = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--admin") == 0 && i + 1 < argc) {
            admin_addr = argv[++i];
        } else if (strcmp(argv[i], "--admin-token") == 0 && i + 1 < argc) {
            admin_token = argv[++i];
        } else if (strcmp(argv[i], "--admin-docroot") == 0 && i + 1 < argc) {
            admin_docroot = argv[++i];
        } else if (strcmp(argv[i], "--admin-ca-cert") == 0 && i + 1 < argc) {
            admin_ca_cert = argv[++i];
        } else if (strcmp(argv[i], "--admin-ca-key") == 0 && i + 1 < argc) {
            admin_ca_key = argv[++i];
        } else if (strcmp(argv[i], "--admin-cert-dir") == 0 && i + 1 < argc) {
            admin_cert_dir = argv[++i];
        } else if (strcmp(argv[i], "--admin-file-dir") == 0 && i + 1 < argc) {
            admin_file_dir = argv[++i];
        } else if (strcmp(argv[i], "--admin-shell-user") == 0 && i + 1 < argc) {
            admin_shell_user = argv[++i];
        } else {
            fprintf(stderr,
                "Usage: %s [--host H] [--port P] [--acl user=devid[,devid|*]] [--allow-all]\n"
                "          [--acl-file PATH] [--drain-timeout MS]\n"
                "          [--tls-cert crt --tls-key key [--tls-ca ca]] [--verbose]\n"
                "          [--client-users F] [--client-token T] [--shell-users u1,u2]\n"
                "          [--admin 127.0.0.1:9443 [--admin-token T] [--admin-db PATH]\n"
                "            [--admin-docroot DIR]\n"
                "            [--admin-ca-cert C --admin-ca-key K --admin-cert-dir D]]\n",
                argv[0]);
            return 1;
        }
    }

    if (tls_ca) require_mtls = 1;
    if (require_mtls && !(tls_cert && tls_key)) {
        fprintf(stderr, "--tls-ca requires --tls-cert/--tls-key\n");
        return 1;
    }

    loop = hloop_new(0);
    if (tls_cert && tls_key) {
        listenio = hloop_create_ssl_server(loop, host, port, acceptCb);
        if (listenio) {
            hssl_ctx_opt_t opt;
            memset(&opt, 0, sizeof(opt));
            opt.crt_file = tls_cert;
            opt.key_file = tls_key;
            opt.ca_file  = tls_ca;
            opt.verify_peer = tls_ca ? 1 : 0;
            opt.endpoint = HSSL_SERVER;
            if (hio_new_ssl_ctx(listenio, &opt) != 0) {
                fprintf(stderr, "ssl ctx init failed\n");
                return 3;
            }
        }
    } else {
        listenio = hloop_create_tcp_server(loop, host, port, acceptCb);
    }
    if (listenio == NULL) {
        fprintf(stderr, "listen %s:%d failed\n", host, port);
        return 2;
    }
    if (acl_file_path[0]) aclFileLoad();
    if (client_users_file) {
        FILE* fp = fopen(client_users_file, "r");
        if (fp == NULL) {
            fprintf(stderr, "open %s failed\n", client_users_file);
            return 5;
        }
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            char* p = line;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p == '#' || *p == '\n' || *p == '\0') continue;
            char* nl = strpbrk(p, "\r\n");
            if (nl) *nl = '\0';
            char* colon = strchr(p, ':');
            if (colon == NULL) continue;
            *colon = '\0';
            client_creds.push_back({std::string(p), std::string(colon + 1)});
        }
        fclose(fp);
    }
    if (client_token_arg) this->client_token = client_token_arg;
    if (shell_users_arg) {
        std::string all = shell_users_arg;
        size_t pos = 0;
        while (pos <= all.size()) {
            size_t comma = all.find(',', pos);
            std::string u = all.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!u.empty()) shell_users.push_back(u);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    started_at_ms = hloop_now_ms(loop);
    if (admin_addr) {
        auto* adm = new AdminServer(this);
        admin = adm;
        AdminConfig cfg;
        cfg.addr = admin_addr;
        cfg.shell_relay_port = port;   // 终端 direct 模式连本机 relay 的端口，必须跟 --port 一致
        cfg.token = admin_token ? admin_token : "";
        cfg.docroot = admin_docroot ? admin_docroot : "";
        cfg.ca_cert = admin_ca_cert ? admin_ca_cert : "";
        cfg.ca_key = admin_ca_key ? admin_ca_key : "";
        cfg.cert_dir = admin_cert_dir ? admin_cert_dir : "";
        cfg.file_dir = admin_file_dir ? admin_file_dir : "";
        cfg.shell_user = admin_shell_user;
        cfg.shell_token = admin_shell_token ? admin_shell_token : "";
        if (admin_db) cfg.db_path = admin_db;
        if (adm->start(cfg) != 0) {
            delete adm;
            admin = NULL;
            return 4;
        }
    }
    logf_("[relay] listening on %s:%d (%s, acl=%s)", host, port,
          (tls_cert && tls_key) ? "tls" : "tcp",
          allow_all ? "allow-all" : ((acl_cli || acl_file) ? "on" : "deny-all"));
    if (!require_mtls) {
        logf_("[relay] WARNING: no peer identity verification (--tls-ca off); "
              "peers can claim any devid/user");
    }
    if (clientAuthEnabled()) {
        logf_("[relay] client auth: on (%zu users, global token %s)",
              client_creds.size(), client_token.empty() ? "off" : "on");
    } else if (admin != NULL) {
        logf_("[relay] client auth: 仅接受管理台签发的账号 Token");
    } else {
        logf_("[relay] WARNING: client auth disabled (--client-users/--client-token off); "
              "anyone who can reach this port can claim any user");
    }
    logf_("[relay] shell users: %s", shell_users.empty() ? "none (OPEN_SHELL denied)" : "configured");
    hsignal_t* sig;
    sig = hsignal_add(loop, shutdownCb, SIGTERM);
    hevent_set_userdata(sig, this);
    sig = hsignal_add(loop, shutdownCb, SIGINT);
    hevent_set_userdata(sig, this);
    sig = hsignal_add(loop, sighupCb, SIGHUP);
    hevent_set_userdata(sig, this);
    htimer_t* sweeper = htimer_add(loop, [](htimer_t* t) { ((RelayServer*)hevent_userdata(t))->sweepPending(); }, 1000, INFINITE);
    hevent_set_userdata(sweeper, this);
    htimer_t* t = htimer_add(loop, statsCb, 60000, INFINITE);
    hevent_set_userdata(t, this);
    // 过期 / 配额的兜底扫描（限流与计量本身在收发路径上实时完成）
    htimer_t* tk = htimer_add(loop, tokenCb, 5000, INFINITE);
    hevent_set_userdata(tk, this);
    hloop_run(loop);
    if (admin) {
        auto* adm = (AdminServer*)admin;
        adm->stop();
        delete adm;
        admin = NULL;
    }
    hloop_free(&loop);
    return 0;
}
