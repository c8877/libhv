// B：中继服务器。设备注册表 + ACL + c_sid 分配 + 帧转发。
//
// 用法：rtunnel-relay --port 8443 [--host 0.0.0.0] [--acl user=devid1,devid2] [--allow-all]

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>

#include "rtnp.h"

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

typedef struct bconn_s   bconn_t;
typedef struct bstream_s bstream_t;

struct bstream_s {
    uint32_t  a_sid;
    uint32_t  c_sid;
    bconn_t*  a;
    bconn_t*  c;
    int       a_closed;
    int       c_closed;
    bstream_t* next_a;   // a->streams
    bstream_t* next_c;   // c->streams
};

struct bconn_s {
    hio_t*     io;
    int        role;         // RTNP_ROLE_*
    int        authed;
    char       devid[128];
    char       user[128];
    rtnp_buf_t in;
    bstream_t* streams;
    uint32_t   next_c_sid;
    bconn_t*   next_all;     // 全局链表
    bconn_t*   reg_next;     // 注册表链表（仅 agent）
    uint64_t   bytes_in;
    uint64_t   bytes_out;
    htimer_t*  hs_timer;     // 握手+认证超时（AUTH_OK 后摘除）
};

static hloop_t* g_loop;
static bconn_t* g_conns;      // 全局
static bconn_t* g_agents;     // devid 注册表
static int      g_allow_all = 0;
static int      g_require_mtls = 0;
static int      g_verbose = 0;

typedef struct acl_entry_s {
    char user[128];
    char pattern[128];
    struct acl_entry_s* next;
} acl_entry_t;

static acl_entry_t* g_acl_cli;    // --acl 命令行条目（固定）
static acl_entry_t* g_acl_file;   // --acl-file 文件条目（SIGHUP 热加载）
static char       g_acl_file_path[256];
static hio_t*     g_listenio;
static int        g_draining = 0;
static uint64_t   g_drain_deadline = 0;
static uint32_t   g_drain_timeout_ms = 10000;

static void logf_(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int pattern_match(const char* pat, const char* s) {
    size_t n = strlen(pat);
    if (n > 0 && pat[n - 1] == '*') {
        return strncmp(pat, s, n - 1) == 0;
    }
    return strcmp(pat, s) == 0;
}

static int acl_check(const char* user, const char* devid) {
    if (g_allow_all) return 1;
    if (user[0] == '\0') return 0;
    for (int i = 0; i < 2; ++i) {
        acl_entry_t* head = i == 0 ? g_acl_cli : g_acl_file;
        for (acl_entry_t* e = head; e; e = e->next) {
            if (strcmp(e->user, user) != 0) continue;
            if (pattern_match(e->pattern, devid)) return 1;
        }
    }
    return 0;
}

static void acl_add_to(acl_entry_t** head, const char* user, const char* patterns) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", patterns);
    char* save = NULL;
    for (char* p = strtok_r(buf, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        acl_entry_t* e = calloc(1, sizeof(*e));
        snprintf(e->user, sizeof(e->user), "%s", user);
        snprintf(e->pattern, sizeof(e->pattern), "%s", p);
        e->next = *head;
        *head = e;
    }
}

static void acl_free(acl_entry_t** head) {
    acl_entry_t* e = *head;
    while (e) {
        acl_entry_t* next = e->next;
        free(e);
        e = next;
    }
    *head = NULL;
}

// 加载 ACL 文件：每行 user=devid[,devid|*]，# 注释、空行跳过。
// 只有整文件解析成功才替换旧表；任何错误保留旧授权（fail-closed 由默认拒绝保证）。
static int acl_file_load(void) {
    if (g_acl_file_path[0] == '\0') return -1;
    FILE* fp = fopen(g_acl_file_path, "r");
    if (fp == NULL) {
        logf_("[relay] acl reload: cannot open %s, keep old acl", g_acl_file_path);
        return -1;
    }
    acl_entry_t* newlist = NULL;
    char line[512];
    int lineno = 0;
    while (fgets(line, sizeof(line), fp)) {
        lineno++;
        char* p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char* eq = strchr(p, '=');
        if (eq == NULL) {
            logf_("[relay] acl reload: %s:%d bad line, keep old acl", g_acl_file_path, lineno);
            fclose(fp);
            acl_free(&newlist);
            return -1;
        }
        *eq = '\0';
        char* pats = eq + 1;
        char* end = pats + strlen(pats);
        while (end > pats && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        acl_add_to(&newlist, p, pats);
    }
    fclose(fp);
    acl_free(&g_acl_file);
    g_acl_file = newlist;
    int n = 0;
    for (acl_entry_t* e = g_acl_file; e; e = e->next) n++;
    logf_("[relay] acl reloaded: %d entries from %s", n, g_acl_file_path);
    return 0;
}

static bconn_t* find_agent(const char* devid) {
    for (bconn_t* c = g_agents; c; c = c->reg_next) {
        if (strcmp(c->devid, devid) == 0) return c;
    }
    return NULL;
}

static bstream_t* find_stream_by_a(bconn_t* a, uint32_t sid, bstream_t** prev) {
    bstream_t* p = NULL;
    for (bstream_t* s = a->streams; s; p = s, s = s->next_a) {
        if (s->a_sid == sid) { if (prev) *prev = p; return s; }
    }
    return NULL;
}

static bstream_t* find_stream_by_c(bconn_t* c, uint32_t sid, bstream_t** prev) {
    bstream_t* p = NULL;
    for (bstream_t* s = c->streams; s; p = s, s = s->next_c) {
        if (s->c_sid == sid) { if (prev) *prev = p; return s; }
    }
    return NULL;
}

static void stream_remove(bstream_t* s) {
    bstream_t* prev = NULL;
    if (find_stream_by_a(s->a, s->a_sid, &prev)) {
        if (prev) prev->next_a = s->next_a; else s->a->streams = s->next_a;
    }
    if (s->c) {
        prev = NULL;
        if (find_stream_by_c(s->c, s->c_sid, &prev)) {
            if (prev) prev->next_c = s->next_c; else s->c->streams = s->next_c;
        }
    }
    free(s);
}

static void send_err(hio_t* io, uint8_t type, uint32_t sid, uint16_t code, const char* msg) {
    rtnp_wr_t w; rtnp_wr_init(&w);
    rtnp_wr_u16(&w, code);
    rtnp_wr_str(&w, msg ? msg : "");
    rtnp_send(io, type, sid, w.data, (uint16_t)w.len);
    rtnp_wr_free(&w);
}

static void forward(bconn_t* to, uint8_t type, uint32_t sid, const uint8_t* payload, uint16_t len) {
    if (to && to->io && !hio_is_closed(to->io)) {
        if (rtnp_send(to->io, type, sid, payload, len) == 0) {
            to->bytes_out += (uint64_t)len + RTNP_HEADER_SIZE;
        }
    }
}

// mTLS 身份校验：对端证书 SAN 必须与 AUTH 声称的身份一致（agent 的 devid、
// client 的 user）——身份取自证书，不信任自报。返回 1 = 通过，0 = 已拒绝并关闭。
static int check_peer_san(bconn_t* c, const char* claimed, const char* what, const char* id_name) {
    if (!g_require_mtls) return 1;
#ifdef WITH_OPENSSL
    char san[256] = {0};
    if (peer_cert_san(hio_get_ssl(c->io), san, sizeof(san)) != 0 ||
        strcmp(san, claimed) != 0) {
        logf_("[relay] %s cert SAN mismatch: %s=%s san=%s", what, id_name, claimed, san);
        send_err(c->io, RTNP_AUTH_FAIL, 0, RTNP_UNAUTHORIZED, "cert identity mismatch");
        hio_close(c->io);
        return 0;
    }
    return 1;
#else
    logf_("[relay] mTLS required but built without OpenSSL");
    send_err(c->io, RTNP_AUTH_FAIL, 0, RTNP_UNAUTHORIZED, "mtls unsupported");
    hio_close(c->io);
    return 0;
#endif
}

// ------------------------------------------------------------------ 注册表

static void registry_remove(bconn_t* c) {
    bconn_t** pp = &g_agents;
    while (*pp) {
        if (*pp == c) { *pp = c->reg_next; break; }
        pp = &(*pp)->reg_next;
    }
}

static void conn_close_all_streams(bconn_t* c, uint16_t code) {
    bstream_t* s = c->streams;
    while (s) {
        bstream_t* next = (c->role == RTNP_ROLE_AGENT) ? s->next_c : s->next_a;
        bconn_t* peer = (c->role == RTNP_ROLE_AGENT) ? s->a : s->c;
        if (peer && peer->io && !hio_is_closed(peer->io)) {
            send_err(peer->io, RTNP_RESET,
                     c->role == RTNP_ROLE_AGENT ? s->a_sid : s->c_sid, code, "peer gone");
        }
        // 解链另一侧，避免悬垂
        s->a_closed = s->c_closed = 1;
        stream_remove(s);
        s = next;
    }
    c->streams = NULL;
}

// 未认证连接超时：10s 内未走完 HELLO + AUTH 即回收，防半开连接占 fd
static void on_hs_timeout(htimer_t* timer) {
    bconn_t* c = (bconn_t*)hevent_userdata(timer);
    if (c == NULL) return;
    c->hs_timer = NULL;
    if (!c->authed) {
        logf_("[relay] handshake/auth timeout fd=%d", hio_fd(c->io));
        hio_close(c->io);
    }
}

// ------------------------------------------------------------------ 读循环

static void on_read(hio_t* io, void* buf, int readbytes);

static void handle_frame(bconn_t* c, rtnp_frame_t* f) {
    if (g_verbose) logf_("[relay] frame role=%d type=0x%02x sid=%u len=%u", c->role, f->type, f->sid, f->len);

    switch (f->type) {
    case RTNP_HELLO: {
        rtnp_rd_t r; rtnp_rd_init(&r, f->payload, f->len);
        uint8_t role = rtnp_rd_u8(&r);
        uint8_t ver  = rtnp_rd_u8(&r);
        if (r.err || ver != RTNP_VERSION || (role != RTNP_ROLE_AGENT && role != RTNP_ROLE_CLIENT)) {
            send_err(c->io, RTNP_AUTH_FAIL, 0, RTNP_PROTOCOL_ERROR, "bad hello");
            hio_close(c->io);
            return;
        }
        c->role = role;
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_u16(&w, RTNP_MAX_PAYLOAD);
        rtnp_wr_u8(&w, RTNP_AUTH_MTLS);
        rtnp_wr_u32(&w, 15000);
        rtnp_wr_u32(&w, 0);
        rtnp_send(c->io, RTNP_HELLO_ACK, 0, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
        if (g_verbose) logf_("[relay] HELLO role=%d fd=%d", role, hio_fd(c->io));
        return;
    }
    case RTNP_AUTH: {
        rtnp_rd_t r; rtnp_rd_init(&r, f->payload, f->len);
        if (c->role == RTNP_ROLE_AGENT) {
            rtnp_rd_str(&r, c->devid, sizeof(c->devid));
            if (r.err || c->devid[0] == '\0') {
                send_err(c->io, RTNP_AUTH_FAIL, 0, RTNP_UNAUTHORIZED, "bad devid");
                hio_close(c->io);
                return;
            }
            if (!check_peer_san(c, c->devid, "device", "devid")) return;
            // 顶替旧连接
            bconn_t* old = find_agent(c->devid);
            if (old && old != c) {
                conn_close_all_streams(old, RTNP_DEVICE_REPLACED);
                registry_remove(old);
                hio_close(old->io);
            }
            c->authed = 1;
            if (c->hs_timer) { htimer_del(c->hs_timer); c->hs_timer = NULL; }
            c->reg_next = g_agents;
            g_agents = c;
            rtnp_wr_t w; rtnp_wr_init(&w);
            rtnp_wr_str(&w, c->devid);
            rtnp_wr_u32(&w, 15000);
            rtnp_send(c->io, RTNP_AUTH_OK, 0, w.data, (uint16_t)w.len);
            rtnp_wr_free(&w);
            logf_("[relay] agent online: %s (fd=%d)", c->devid, hio_fd(c->io));
        } else {
            rtnp_rd_str(&r, c->user, sizeof(c->user));
            if (r.err || c->user[0] == '\0') {
                send_err(c->io, RTNP_AUTH_FAIL, 0, RTNP_UNAUTHORIZED, "bad user");
                hio_close(c->io);
                return;
            }
            if (!check_peer_san(c, c->user, "client", "user")) return;
            c->authed = 1;
            if (c->hs_timer) { htimer_del(c->hs_timer); c->hs_timer = NULL; }
            rtnp_wr_t w; rtnp_wr_init(&w);
            rtnp_wr_str(&w, c->user);
            rtnp_wr_u32(&w, 15000);
            rtnp_send(c->io, RTNP_AUTH_OK, 0, w.data, (uint16_t)w.len);
            rtnp_wr_free(&w);
            logf_("[relay] client online: %s (fd=%d)", c->user, hio_fd(c->io));
        }
        return;
    }
    case RTNP_PING:
        rtnp_send(c->io, RTNP_PONG, 0, f->payload, f->len);
        return;
    case RTNP_DEVICE_QUERY: {
        rtnp_rd_t r; rtnp_rd_init(&r, f->payload, f->len);
        char devid[128] = {0};
        rtnp_rd_str(&r, devid, sizeof(devid));
        int online = 0;
        uint32_t active = 0;
        if (!r.err && c->authed && c->role == RTNP_ROLE_CLIENT && acl_check(c->user, devid)) {
            bconn_t* agent = find_agent(devid);
            if (agent) {
                online = 1;
                for (bstream_t* st = agent->streams; st; st = st->next_c) active++;
            }
        }
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_str(&w, devid);
        rtnp_wr_u8(&w, (uint8_t)online);
        rtnp_wr_u32(&w, active);
        rtnp_send(c->io, RTNP_DEVICE_STATUS, 0, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
        return;
    }
    default:
        break;
    }

    if (!c->authed) {
        send_err(c->io, RTNP_AUTH_FAIL, 0, RTNP_UNAUTHORIZED, "not authed");
        hio_close(c->io);
        return;
    }

    if (c->role == RTNP_ROLE_CLIENT) {
        switch (f->type) {
        case RTNP_OPEN: {
            rtnp_rd_t r; rtnp_rd_init(&r, f->payload, f->len);
            char devid[128] = {0};
            rtnp_rd_str(&r, devid, sizeof(devid));
            uint16_t port = rtnp_rd_u16(&r);
            if (r.err || devid[0] == '\0' || port == 0) {
                send_err(c->io, RTNP_OPEN_FAIL, f->sid, RTNP_PROTOCOL_ERROR, "bad open");
                return;
            }
            if (find_stream_by_a(c, f->sid, NULL) != NULL) {
                send_err(c->io, RTNP_OPEN_FAIL, f->sid, RTNP_PROTOCOL_ERROR, "sid in use");
                return;
            }
            if (!acl_check(c->user, devid)) {
                send_err(c->io, RTNP_OPEN_FAIL, f->sid, RTNP_ACL_DENIED, "acl denied");
                return;
            }
            bconn_t* agent = find_agent(devid);
            if (agent == NULL) {
                send_err(c->io, RTNP_OPEN_FAIL, f->sid, RTNP_DEVICE_OFFLINE, "device offline");
                return;
            }
            bstream_t* s = calloc(1, sizeof(*s));
            s->a = c; s->a_sid = f->sid;
            s->c = agent; s->c_sid = ++agent->next_c_sid;
            s->next_a = c->streams; c->streams = s;
            s->next_c = agent->streams; agent->streams = s;

            rtnp_wr_t w; rtnp_wr_init(&w);
            rtnp_wr_u16(&w, port);
            rtnp_send(agent->io, RTNP_OPEN, s->c_sid, w.data, (uint16_t)w.len);
            rtnp_wr_free(&w);
            if (g_verbose) logf_("[relay] OPEN %s:%u a_sid=%u c_sid=%u", devid, port, s->a_sid, s->c_sid);
            return;
        }
        case RTNP_DATA:
        case RTNP_WINDOW:
        case RTNP_CLOSE:
        case RTNP_RESET: {
            bstream_t* s = find_stream_by_a(c, f->sid, NULL);
            if (s == NULL || s->c == NULL) {
                send_err(c->io, RTNP_RESET, f->sid, RTNP_PROTOCOL_ERROR, "unknown sid");
                return;
            }
            forward(s->c, f->type, s->c_sid, f->payload, f->len);
            if (f->type == RTNP_CLOSE) s->a_closed = 1;
            if (f->type == RTNP_RESET) { s->a_closed = s->c_closed = 1; }
            if (s->a_closed && s->c_closed) stream_remove(s);
            return;
        }
        default:
            send_err(c->io, RTNP_ERROR, 0, RTNP_PROTOCOL_ERROR, "unexpected type");
            return;
        }
    }

    // 来自 agent 的会话级帧
    switch (f->type) {
    case RTNP_OPEN_OK:
    case RTNP_OPEN_FAIL: {
        bstream_t* s = find_stream_by_c(c, f->sid, NULL);
        if (s == NULL) return;
        forward(s->a, f->type, s->a_sid, f->payload, f->len);
        if (f->type == RTNP_OPEN_FAIL) { s->a_closed = s->c_closed = 1; stream_remove(s); }
        return;
    }
    case RTNP_DATA:
    case RTNP_WINDOW:
    case RTNP_CLOSE:
    case RTNP_RESET: {
        bstream_t* s = find_stream_by_c(c, f->sid, NULL);
        if (s == NULL) {
            send_err(c->io, RTNP_RESET, f->sid, RTNP_PROTOCOL_ERROR, "unknown sid");
            return;
        }
        forward(s->a, f->type, s->a_sid, f->payload, f->len);
        if (f->type == RTNP_CLOSE) s->c_closed = 1;
        if (f->type == RTNP_RESET) { s->a_closed = s->c_closed = 1; }
        if (s->a_closed && s->c_closed) stream_remove(s);
        return;
    }
    default:
        send_err(c->io, RTNP_ERROR, 0, RTNP_PROTOCOL_ERROR, "unexpected type");
        return;
    }
}

static void on_read(hio_t* io, void* buf, int readbytes) {
    bconn_t* c = (bconn_t*)hio_context(io);
    if (c == NULL) return;
    c->bytes_in += (uint64_t)readbytes;
    if (rtnp_buf_append(&c->in, buf, (size_t)readbytes) != 0) {
        hio_close(io);
        return;
    }
    for (;;) {
        rtnp_frame_t f;
        int rc = rtnp_buf_parse(&c->in, &f);
        if (rc == 0) return;
        if (rc < 0) {
            send_err(io, RTNP_ERROR, 0, RTNP_PROTOCOL_ERROR, "bad frame");
            hio_close(io);
            return;
        }
        handle_frame(c, &f);
        if (hio_is_closed(io)) return;
    }
}

static void on_close(hio_t* io) {
    bconn_t* c = (bconn_t*)hio_context(io);
    if (c == NULL) return;

    if (c->hs_timer) { htimer_del(c->hs_timer); c->hs_timer = NULL; }
    // agent 掉线 = 设备离线；client 掉线 = 会话取消（设备并未离线）
    conn_close_all_streams(c, c->role == RTNP_ROLE_AGENT ? RTNP_DEVICE_OFFLINE : RTNP_CANCELLED);
    if (c->role == RTNP_ROLE_AGENT) {
        registry_remove(c);
        if (c->devid[0]) logf_("[relay] agent offline: %s", c->devid);
    }

    // 从全局链表摘除
    bconn_t** pp = &g_conns;
    while (*pp) {
        if (*pp == c) { *pp = c->next_all; break; }
        pp = &(*pp)->next_all;
    }
    rtnp_buf_free(&c->in);
    hio_set_context(io, NULL);
    free(c);
}

static void on_accept(hio_t* io) {
    bconn_t* c = calloc(1, sizeof(*c));
    c->io = io;
    rtnp_buf_init(&c->in);
    c->next_all = g_conns;
    g_conns = c;

    hio_set_context(io, c);
    hio_setcb_read(io, on_read);
    hio_setcb_close(io, on_close);
    hio_set_max_write_bufsize(io, RTNP_CONN_INFLIGHT + RTNP_MAX_PAYLOAD);
    hio_read(io);
    // 未认证连接 10s 未走完 HELLO+AUTH 即回收
    c->hs_timer = htimer_add(g_loop, on_hs_timeout, 10000, 1);
    hevent_set_userdata(c->hs_timer, c);
    if (g_verbose) logf_("[relay] accept fd=%d", hio_fd(io));
}

// ------------------------------------------------------------------ 运维面

// SIGHUP：热加载 --acl-file；命令行 --acl 条目不受影响
static void on_sighup(hsignal_t* sig) {
    (void)sig;
    if (g_acl_file_path[0]) {
        acl_file_load();
    } else {
        logf_("[relay] SIGHUP ignored: no --acl-file configured");
    }
}

// 周期统计：连接数 / 活跃流（按 agent 侧计，避免双算）/ 累计字节
static void on_stats(htimer_t* timer) {
    (void)timer;
    int nclient = 0, nagent = 0, nstream = 0;
    uint64_t in = 0, out = 0;
    for (bconn_t* c = g_conns; c; c = c->next_all) {
        if (c->role == RTNP_ROLE_AGENT) {
            nagent++;
            for (bstream_t* s = c->streams; s; s = s->next_c) nstream++;
        } else if (c->role == RTNP_ROLE_CLIENT) {
            nclient++;
        }
        in += c->bytes_in;
        out += c->bytes_out;
    }
    logf_("[relay] stats: agents=%d clients=%d streams=%d bytes_in=%llu bytes_out=%llu",
          nagent, nclient, nstream, (unsigned long long)in, (unsigned long long)out);
}

static void on_drain(htimer_t* timer) {
    if (g_conns && hloop_now_ms(g_loop) < g_drain_deadline) return;
    int left = 0;
    for (bconn_t* c = g_conns; c; c = c->next_all) left++;
    logf_("[relay] drain done (remaining=%d), exiting", left);
    htimer_del(timer);
    hloop_stop(g_loop);
}

// SIGTERM/SIGINT：停止 accept → 等现有连接收尾（--drain-timeout）→ 退出。
// 第二次信号立即退出。A/C 侧断线后按既有退避重连，无需额外通知。
static void on_shutdown(hsignal_t* sig) {
    int signo = (int)hevent_id(sig);
    if (g_draining) {
        logf_("[relay] signal %d again, force exit", signo);
        hloop_stop(g_loop);
        return;
    }
    g_draining = 1;
    logf_("[relay] signal %d: draining (stop accept, timeout %u ms)", signo, g_drain_timeout_ms);
    if (g_listenio) hio_close(g_listenio);
    if (g_conns == NULL) { hloop_stop(g_loop); return; }
    g_drain_deadline = hloop_now_ms(g_loop) + g_drain_timeout_ms;
    htimer_add(g_loop, on_drain, 100, INFINITE);
}

int main(int argc, char** argv) {
    const char* host = "0.0.0.0";
    int port = 8443;
    const char* tls_cert = NULL;
    const char* tls_key  = NULL;
    const char* tls_ca   = NULL;

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
            g_allow_all = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_verbose = 1;
        } else if (strcmp(argv[i], "--acl") == 0 && i + 1 < argc) {
            char buf[512];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char* eq = strchr(buf, '=');
            if (eq) {
                *eq = '\0';
                acl_add_to(&g_acl_cli, buf, eq + 1);
            }
        } else if (strcmp(argv[i], "--acl-file") == 0 && i + 1 < argc) {
            snprintf(g_acl_file_path, sizeof(g_acl_file_path), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--drain-timeout") == 0 && i + 1 < argc) {
            g_drain_timeout_ms = (uint32_t)atoi(argv[++i]);
        } else {
            fprintf(stderr,
                "Usage: %s [--host H] [--port P] [--acl user=devid[,devid|*]] [--allow-all]\n"
                "          [--acl-file PATH] [--drain-timeout MS]\n"
                "          [--tls-cert crt --tls-key key [--tls-ca ca]] [--verbose]\n",
                argv[0]);
            return 1;
        }
    }

    if (tls_ca) g_require_mtls = 1;
    if (g_require_mtls && !(tls_cert && tls_key)) {
        fprintf(stderr, "--tls-ca requires --tls-cert/--tls-key\n");
        return 1;
    }

    g_loop = hloop_new(0);
    hio_t* listenio = NULL;
    if (tls_cert && tls_key) {
        listenio = hloop_create_ssl_server(g_loop, host, port, on_accept);
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
        listenio = hloop_create_tcp_server(g_loop, host, port, on_accept);
    }
    if (listenio == NULL) {
        fprintf(stderr, "listen %s:%d failed\n", host, port);
        return 2;
    }
    g_listenio = listenio;
    if (g_acl_file_path[0]) acl_file_load();
    logf_("[relay] listening on %s:%d (%s, acl=%s)", host, port,
          (tls_cert && tls_key) ? "tls" : "tcp",
          g_allow_all ? "allow-all" : ((g_acl_cli || g_acl_file) ? "on" : "deny-all"));
    if (!g_require_mtls) {
        logf_("[relay] WARNING: no peer identity verification (--tls-ca off); "
              "peers can claim any devid/user");
    }
    hsignal_add(g_loop, on_shutdown, SIGTERM);
    hsignal_add(g_loop, on_shutdown, SIGINT);
    hsignal_add(g_loop, on_sighup, SIGHUP);
    htimer_add(g_loop, on_stats, 60000, INFINITE);
    hloop_run(g_loop);
    hloop_free(&g_loop);
    return 0;
}
