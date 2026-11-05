// B：中继服务器。设备注册表 + ACL + c_sid 分配 + 帧转发。
//
// 用法：rtunnel-relay --port 8443 [--host 0.0.0.0] [--acl user=devid1,devid2] [--allow-all]

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

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

static acl_entry_t* g_acl;

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
    for (acl_entry_t* e = g_acl; e; e = e->next) {
        if (strcmp(e->user, user) != 0) continue;
        if (pattern_match(e->pattern, devid)) return 1;
    }
    return 0;
}

static void acl_add(const char* user, const char* patterns) {
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", patterns);
    char* save = NULL;
    for (char* p = strtok_r(buf, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
        acl_entry_t* e = calloc(1, sizeof(*e));
        snprintf(e->user, sizeof(e->user), "%s", user);
        snprintf(e->pattern, sizeof(e->pattern), "%s", p);
        e->next = g_acl;
        g_acl = e;
    }
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

static void forward(hio_t* to, uint8_t type, uint32_t sid, const uint8_t* payload, uint16_t len) {
    if (to) {
        rtnp_send(to, type, sid, payload, len);
    }
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

// ------------------------------------------------------------------ 读循环

static void on_read(hio_t* io, void* buf, int readbytes);

static void handle_frame(bconn_t* c, rtnp_frame_t* f) {
    c->bytes_in += f->len;
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
            // 顶替旧连接
            if (g_require_mtls) {
#ifdef WITH_OPENSSL
                char san[256] = {0};
                if (peer_cert_san(hio_get_ssl(c->io), san, sizeof(san)) != 0 ||
                    strcmp(san, c->devid) != 0) {
                    logf_("[relay] device cert SAN mismatch: devid=%s san=%s", c->devid, san);
                    send_err(c->io, RTNP_AUTH_FAIL, 0, RTNP_UNAUTHORIZED, "cert/devid mismatch");
                    hio_close(c->io);
                    return;
                }
#else
                logf_("[relay] mTLS required but built without OpenSSL");
                send_err(c->io, RTNP_AUTH_FAIL, 0, RTNP_UNAUTHORIZED, "mtls unsupported");
                hio_close(c->io);
                return;
#endif
            }
            bconn_t* old = find_agent(c->devid);
            if (old && old != c) {
                conn_close_all_streams(old, RTNP_DEVICE_REPLACED);
                registry_remove(old);
                hio_close(old->io);
            }
            c->authed = 1;
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
            c->authed = 1;
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
        if (!r.err && c->authed && c->role == RTNP_ROLE_CLIENT && acl_check(c->user, devid)) {
            online = find_agent(devid) != NULL;
        }
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_str(&w, devid);
        rtnp_wr_u8(&w, (uint8_t)online);
        rtnp_wr_u32(&w, 0);
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
            forward(s->c->io, f->type, s->c_sid, f->payload, f->len);
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
        forward(s->a->io, f->type, s->a_sid, f->payload, f->len);
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
        forward(s->a->io, f->type, s->a_sid, f->payload, f->len);
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

    conn_close_all_streams(c, RTNP_DEVICE_OFFLINE);
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
    if (g_verbose) logf_("[relay] accept fd=%d", hio_fd(io));
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
                acl_add(buf, eq + 1);
            }
        } else {
            fprintf(stderr,
                "Usage: %s [--host H] [--port P] [--acl user=devid[,devid|*]] [--allow-all]\n"
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
    logf_("[relay] listening on %s:%d (%s, acl=%s)", host, port,
          (tls_cert && tls_key) ? "tls" : "tcp",
          g_allow_all ? "allow-all" : (g_acl ? "on" : "deny-all"));
    hloop_run(g_loop);
    hloop_free(&g_loop);
    return 0;
}
