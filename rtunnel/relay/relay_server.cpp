#include "relay_server.hpp"
#include "common/logging.hpp"
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
    case rtnp::AUTH: {
        rtnp::Reader r(f->payload, f->len);
        if (c->role == rtnp::ROLE_AGENT) {
            r.str(c->devid, sizeof(c->devid));
            if (r.err || c->devid[0] == '\0') {
                sendErr(c->io, rtnp::AUTH_FAIL, 0, rtnp::UNAUTHORIZED, "bad devid");
                hio_close(c->io);
                return;
            }
            if (!checkPeerSan(c, c->devid, "device", "devid")) return;
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
        } else {
            r.str(c->user, sizeof(c->user));
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
            rtnp::send(c->io, rtnp::AUTH_OK, 0, w.data, (uint16_t)w.len);
            logf_("[relay] client online: %s (fd=%d)", c->user, hio_fd(c->io));
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
        if (!r.err && c->authed && c->role == rtnp::ROLE_CLIENT && aclCheck(c->user, devid)) {
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
            if (r.err || devid[0] == '\0' || (!is_shell && port == 0)) {
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::PROTOCOL_ERROR, "bad open");
                return;
            }
            if (findStreamByA(c, f->sid, NULL) != NULL) {
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::PROTOCOL_ERROR, "sid in use");
                return;
            }
            if (!aclCheck(c->user, devid)) {
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::ACL_DENIED, "acl denied");
                return;
            }
            RelayConn* agent = findAgent(devid);
            if (agent == NULL) {
                sendErr(c->io, rtnp::OPEN_FAIL, f->sid, rtnp::DEVICE_OFFLINE, "device offline");
                return;
            }
            auto* s = new RelayStream{};
            s->a = c; s->a_sid = f->sid;
            s->c = agent; s->c_sid = ++agent->next_c_sid;
            s->next_a = c->streams; c->streams = s;
            s->next_c = agent->streams; agent->streams = s;

            rtnp::Writer w;
            w.u16(port);
            rtnp::send(agent->io, rtnp::OPEN, s->c_sid, w.data, (uint16_t)w.len, f->flags);
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
    const char* admin_users = NULL;
    const char* admin_ca_cert = NULL;
    const char* admin_ca_key = NULL;
    const char* admin_cert_dir = NULL;
    const char* admin_file_dir = NULL;
    const char* admin_shell_user = "devuser";

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
        } else if (strcmp(argv[i], "--admin-users") == 0 && i + 1 < argc) {
            admin_users = argv[++i];
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
                "          [--admin 127.0.0.1:9443 [--admin-token T] [--admin-users F] [--admin-docroot DIR]\n"
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
    started_at_ms = hloop_now_ms(loop);
    if (admin_addr) {
        auto* adm = new AdminServer(this);
        admin = adm;
        AdminConfig cfg;
        cfg.addr = admin_addr;
        cfg.token = admin_token ? admin_token : "";
        cfg.users = admin_users ? admin_users : "";
        cfg.docroot = admin_docroot ? admin_docroot : "";
        cfg.ca_cert = admin_ca_cert ? admin_ca_cert : "";
        cfg.ca_key = admin_ca_key ? admin_ca_key : "";
        cfg.cert_dir = admin_cert_dir ? admin_cert_dir : "";
        cfg.file_dir = admin_file_dir ? admin_file_dir : "";
        cfg.shell_user = admin_shell_user;
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
    hsignal_t* sig;
    sig = hsignal_add(loop, shutdownCb, SIGTERM);
    hevent_set_userdata(sig, this);
    sig = hsignal_add(loop, shutdownCb, SIGINT);
    hevent_set_userdata(sig, this);
    sig = hsignal_add(loop, sighupCb, SIGHUP);
    hevent_set_userdata(sig, this);
    htimer_t* t = htimer_add(loop, statsCb, 60000, INFINITE);
    hevent_set_userdata(t, this);
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
