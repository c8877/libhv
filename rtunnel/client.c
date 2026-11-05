// A：开发者侧客户端。连 B，并在本地起 SOCKS5 入口；CONNECT devid:port -> OPEN。
//
// 用法：rtunnel-client --server HOST --port 8443 --user alice --socks5 127.0.0.1:1080 [--verbose]

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rtnp.h"

enum {
    CS_HANDSHAKE = 0,
    CS_REQUEST,
    CS_OPENING,
    CS_OPEN,
    CS_CLOSED,
};

typedef struct cstream_s {
    uint32_t          sid;
    hio_t*            local;
    int               state;
    int               local_eof;
    int               remote_closed;
    int               paused;
    int               want_shutdown;
    uint32_t          credit;
    uint32_t          unacked;      // 已写入 local 但尚未回 WINDOW 的字节数
    rtnp_buf_t        hs;        // SOCKS5 握手缓冲 / 未发送数据
    rtnp_buf_t        pending;   // 受 credit 限制的待发数据
    htimer_t*         open_timer;
    struct cstream_s* next;
} cstream_t;

typedef struct {
    hloop_t*    loop;
    const char* host;
    int         port;
    char        user[128];
    char        tls_ca[256];
    char        tls_cert[256];
    char        tls_key[256];
    char        tls_sni[256];
    int         socks_host_port;
    char        socks_host[128];
    char        socks_uds[256];
    int         socks_port;
    int         verbose;
    hio_t*      io;
    int         state;
    rtnp_buf_t  in;
    cstream_t*  streams;
    uint32_t    next_sid;
    int         backoff_ms;
} client_t;

static client_t g_cli;

static void logf_(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void schedule_reconnect(void);

static cstream_t* stream_find(uint32_t sid) {
    for (cstream_t* s = g_cli.streams; s; s = s->next) {
        if (s->sid == sid) return s;
    }
    return NULL;
}

static void stream_free(cstream_t* s) {
    cstream_t** pp = &g_cli.streams;
    while (*pp) {
        if (*pp == s) { *pp = s->next; break; }
        pp = &(*pp)->next;
    }
    if (s->open_timer) { htimer_del(s->open_timer); s->open_timer = NULL; }
    if (s->local && !hio_is_closed(s->local)) {
        // 先摘掉 context，避免 close_cb 再次回到已释放的 stream
        hio_set_context(s->local, NULL);
        hio_close(s->local);
    }
    rtnp_buf_free(&s->hs);
    rtnp_buf_free(&s->pending);
    free(s);
}

static void buf_consume(rtnp_buf_t* b, size_t n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

static void socks_reply(hio_t* io, uint8_t rep) {
    uint8_t r[10] = { 0x05, rep, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    if (g_cli.verbose) logf_("[client] socks5 reply rep=0x%02x", rep);
    hio_write(io, r, sizeof(r));
}

static int valid_devid(const char* s) {
    if (s == NULL || s[0] == '\0') return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return 0;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

static void flush_pending(cstream_t* s) {
    if (s->state != CS_OPEN || g_cli.state != 2) return;
    while (s->pending.len > 0 && s->credit > 0) {
        size_t n = s->pending.len;
        if (n > s->credit) n = s->credit;
        if (n > RTNP_MAX_PAYLOAD) n = RTNP_MAX_PAYLOAD;
        if (rtnp_send(g_cli.io, RTNP_DATA, s->sid, s->pending.data, (uint16_t)n) != 0) return;
        s->credit -= (uint32_t)n;
        buf_consume(&s->pending, n);
    }
    if (s->pending.len > 0) {
        if (!s->paused) { hio_read_stop(s->local); s->paused = 1; }
    } else if (s->paused) {
        s->paused = 0;
        hio_read(s->local);
    }
}

static void on_open_timeout(htimer_t* timer) {
    cstream_t* s = (cstream_t*)hevent_userdata(timer);
    if (s && s->state == CS_OPENING) {
        rtnp_send(g_cli.io, RTNP_RESET, s->sid, NULL, 0);
        if (s->local) socks_reply(s->local, 0x04);
    }
    if (s) s->open_timer = NULL;
}

// ---------------------------------------------------------------- SOCKS5 握手

static void try_socks(cstream_t* s) {
    for (;;) {
        if (s->state == CS_HANDSHAKE) {
            if (s->hs.len < 2) return;
            uint8_t* p = s->hs.data;
            if (p[0] != 0x05) { hio_close(s->local); return; }
            size_t need = 2 + p[1];
            if (s->hs.len < need) return;
            uint8_t reply[2] = { 0x05, 0x00 };
            hio_write(s->local, reply, 2);
            buf_consume(&s->hs, need);
            s->state = CS_REQUEST;
            continue;
        }
        if (s->state == CS_REQUEST) {
            if (s->hs.len < 4) return;
            uint8_t* p = s->hs.data;
            if (p[0] != 0x05) { hio_close(s->local); return; }
            if (p[1] != 0x01) { socks_reply(s->local, 0x07); hio_close(s->local); return; }
            if (p[3] != 0x03) { socks_reply(s->local, 0x08); hio_close(s->local); return; }
            if (s->hs.len < 5) return;
            size_t dlen = p[4];
            size_t need = 5 + dlen + 2;
            if (s->hs.len < need) return;

            char devid[128] = {0};
            if (dlen >= sizeof(devid)) { socks_reply(s->local, 0x01); hio_close(s->local); return; }
            memcpy(devid, p + 5, dlen);
            uint16_t port = (uint16_t)((p[5 + dlen] << 8) | p[6 + dlen]);
            if (!valid_devid(devid) || port == 0) { socks_reply(s->local, 0x01); hio_close(s->local); return; }
            buf_consume(&s->hs, need);

            if (g_cli.state != 2 || g_cli.io == NULL) {
                socks_reply(s->local, 0x01);
                hio_close(s->local);
                return;
            }

            s->sid = ++g_cli.next_sid;
            s->state = CS_OPENING;
            s->open_timer = htimer_add(g_cli.loop, on_open_timeout, 15000, 1);
            hevent_set_userdata(s->open_timer, s);

            rtnp_wr_t w; rtnp_wr_init(&w);
            rtnp_wr_str(&w, devid);
            rtnp_wr_u16(&w, port);
            rtnp_send(g_cli.io, RTNP_OPEN, s->sid, w.data, (uint16_t)w.len);
            rtnp_wr_free(&w);
            if (g_cli.verbose) logf_("[client] OPEN %s:%u sid=%u", devid, port, s->sid);
            return;
        }
        return;
    }
}

static void on_local_read(hio_t* io, void* buf, int readbytes) {
    cstream_t* s = (cstream_t*)hio_context(io);
    if (s == NULL || readbytes <= 0) return;

    if (s->state < CS_OPEN) {
        if (rtnp_buf_append(&s->hs, buf, (size_t)readbytes) != 0) { hio_close(io); return; }
        if (s->state == CS_HANDSHAKE || s->state == CS_REQUEST) {
            try_socks(s);
        }
        return;
    }
    if (s->state != CS_OPEN) return;
    // 注意：remote_closed 只表示 C→A 方向已 EOF，A→C 方向仍要转发客户端数据。
    if (rtnp_buf_append(&s->pending, buf, (size_t)readbytes) != 0) {
        if (g_cli.io) rtnp_send(g_cli.io, RTNP_RESET, s->sid, NULL, 0);
        stream_free(s);
        return;
    }
    flush_pending(s);
}

static void on_local_write(hio_t* io, const void* buf, int len);
static void maybe_finish(cstream_t* s);
static void maybe_grant_window(cstream_t* s);

static void maybe_shutdown_write(cstream_t* s) {
    if (!s->want_shutdown || s->local == NULL || hio_is_closed(s->local)) return;
    if (hio_write_is_complete(s->local)) {
        shutdown(hio_fd(s->local), SHUT_WR);
        s->want_shutdown = 0;
        hio_setcb_write(s->local, NULL);
    }
}

static void maybe_grant_window(cstream_t* s) {
    if (s->local == NULL || hio_is_closed(s->local) || s->unacked == 0) return;
    // 只有本地写队列降到低水位才补窗口（hio_write 入队不代表对端已消费）
    if (hio_write_bufsize(s->local) > RTNP_LOW_WATER) {
        hio_setcb_write(s->local, (hwrite_cb)on_local_write);
        return;
    }
    if (g_cli.io) {
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_u32(&w, s->unacked);
        rtnp_send(g_cli.io, RTNP_WINDOW, s->sid, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
    }
    s->unacked = 0;
    if (!s->want_shutdown) hio_setcb_write(s->local, NULL);
}

static void on_local_write(hio_t* io, const void* buf, int len) {
    (void)buf; (void)len;
    cstream_t* s = (cstream_t*)hio_context(io);
    if (s == NULL) return;
    maybe_shutdown_write(s);
    maybe_grant_window(s);
}

static void maybe_finish(cstream_t* s) {
    if (!s->local_eof || !s->remote_closed) return;
    if (s->local == NULL || hio_is_closed(s->local)) { s->local = NULL; stream_free(s); return; }
    if (hio_write_is_complete(s->local)) {
        shutdown(hio_fd(s->local), SHUT_WR);
        hio_close(s->local);   // 由 on_local_close 统一回收
    } else {
        s->want_shutdown = 1;
        hio_setcb_write(s->local, (hwrite_cb)on_local_write);
    }
}

static void on_local_eof(hio_t* io) {
    cstream_t* s = (cstream_t*)hio_context(io);
    if (g_cli.verbose) logf_("[client] local eof sid=%u", s ? s->sid : 0);
    if (s == NULL) return;
    if (!s->local_eof) {
        s->local_eof = 1;
        if (g_cli.state == 2 && g_cli.io) {
            rtnp_send(g_cli.io, RTNP_CLOSE, s->sid, NULL, 0);
        }
    }
    maybe_finish(s);
}

static void on_local_close(hio_t* io) {
    cstream_t* s = (cstream_t*)hio_context(io);
    if (s == NULL) return;
    s->local = NULL;
    if (s->state == CS_OPEN && !s->local_eof && g_cli.state == 2 && g_cli.io) {
        s->local_eof = 1;
        rtnp_send(g_cli.io, RTNP_CLOSE, s->sid, NULL, 0);
    }
    if (s->state != CS_OPEN || s->remote_closed) {
        stream_free(s);
    }
}

static void on_local_accept(hio_t* io) {
    cstream_t* s = calloc(1, sizeof(*s));
    s->local = io;
    s->state = CS_HANDSHAKE;
    s->credit = RTNP_DEFAULT_CREDIT;
    rtnp_buf_init(&s->hs);
    rtnp_buf_init(&s->pending);
    s->next = g_cli.streams;
    g_cli.streams = s;

    hio_set_context(io, s);
    hio_setcb_read(io, on_local_read);
    hio_setcb_close(io, on_local_close);
    hio_setcb_eof(io, on_local_eof);
    hio_set_keep_write_on_eof(io, 1);
    hio_set_max_write_bufsize(io, RTNP_CONN_INFLIGHT + RTNP_MAX_PAYLOAD);
    hio_read(io);
}

// ---------------------------------------------------------------- 隧道帧

static void handle_frame(rtnp_frame_t* f) {
    switch (f->type) {
    case RTNP_HELLO_ACK: {
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_str(&w, g_cli.user);
        rtnp_send(g_cli.io, RTNP_AUTH, 0, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
        g_cli.state = 1;
        return;
    }
    case RTNP_AUTH_OK:
        g_cli.state = 2;
        g_cli.backoff_ms = 1000;
        logf_("[client] authenticated as %s", g_cli.user);
        return;
    case RTNP_AUTH_FAIL:
    case RTNP_ERROR:
        logf_("[client] rejected by relay");
        hio_close(g_cli.io);
        return;
    case RTNP_PING:
        rtnp_send(g_cli.io, RTNP_PONG, 0, f->payload, f->len);
        return;
    default:
        break;
    }

    cstream_t* s = stream_find(f->sid);
    if (s == NULL) return;

    switch (f->type) {
    case RTNP_OPEN_OK:
        if (s->open_timer) { htimer_del(s->open_timer); s->open_timer = NULL; }
        if (s->state != CS_OPENING) return;
        s->state = CS_OPEN;
        socks_reply(s->local, 0x00);
        // 客户端可能在收到 SOCKS5 应答前就发了数据（pipelining）
        if (s->hs.len > 0) {
            rtnp_buf_append(&s->pending, s->hs.data, s->hs.len);
            s->hs.len = 0;
        }
        flush_pending(s);
        return;
    case RTNP_OPEN_FAIL: {
        if (s->open_timer) { htimer_del(s->open_timer); s->open_timer = NULL; }
        rtnp_rd_t r; rtnp_rd_init(&r, f->payload, f->len);
        uint16_t code = rtnp_rd_u16(&r);
        if (g_cli.verbose) logf_("[client] OPEN_FAIL sid=%u code=%u", s->sid, code);
        uint8_t rep = 0x01;
        if (code == RTNP_DEVICE_OFFLINE || code == RTNP_TIMEOUT) rep = 0x04;
        else if (code == RTNP_TARGET_REFUSED) rep = 0x05;
        else if (code == RTNP_ACL_DENIED || code == RTNP_TARGET_NOT_ALLOWED) rep = 0x02;
        socks_reply(s->local, rep);
        hio_close(s->local);
        return;
    }
    case RTNP_DATA:
        if (s->state != CS_OPEN || s->local == NULL) return;
        if (f->len > 0) {
            hio_write(s->local, f->payload, f->len);
            s->unacked += f->len;
        }
        maybe_grant_window(s);
        return;
    case RTNP_WINDOW: {
        rtnp_rd_t r; rtnp_rd_init(&r, f->payload, f->len);
        uint32_t inc = rtnp_rd_u32(&r);
        if (r.err) return;
        s->credit += inc;
        if (s->credit > RTNP_DEFAULT_CREDIT) s->credit = RTNP_DEFAULT_CREDIT;
        flush_pending(s);
        return;
    }
    case RTNP_CLOSE:
        s->remote_closed = 1;
        if (s->local == NULL) { stream_free(s); return; }
        if (hio_write_is_complete(s->local)) {
            shutdown(hio_fd(s->local), SHUT_WR);
        } else {
            s->want_shutdown = 1;
            hio_setcb_write(s->local, (hwrite_cb)on_local_write);
        }
        maybe_finish(s);
        return;
    case RTNP_RESET:
        stream_free(s);
        return;
    default:
        return;
    }
}

static void on_read(hio_t* io, void* buf, int readbytes) {
    (void)io;
    if (rtnp_buf_append(&g_cli.in, buf, (size_t)readbytes) != 0) {
        hio_close(g_cli.io);
        return;
    }
    for (;;) {
        rtnp_frame_t f;
        int rc = rtnp_buf_parse(&g_cli.in, &f);
        if (rc == 0) return;
        if (rc < 0) { hio_close(g_cli.io); return; }
        if (g_cli.verbose) logf_("[client] frame type=0x%02x sid=%u len=%u", f.type, f.sid, f.len);
        handle_frame(&f);
        if (g_cli.io == NULL || hio_is_closed(g_cli.io)) return;
    }
}

static void on_tunnel_close(hio_t* io) {
    (void)io;
    logf_("[client] tunnel closed, reconnect in %d ms", g_cli.backoff_ms);
    while (g_cli.streams) {
        cstream_t* s = g_cli.streams;
        g_cli.streams = s->next;
        if (s->local && !hio_is_closed(s->local)) hio_close(s->local);
        if (s->open_timer) htimer_del(s->open_timer);
        rtnp_buf_free(&s->hs);
        rtnp_buf_free(&s->pending);
        free(s);
    }
    g_cli.io = NULL;
    g_cli.state = 0;
    g_cli.in.len = 0;
    schedule_reconnect();
}

static void on_connect(hio_t* io) {
    if (hio_error(io) != 0) {
        logf_("[client] connect to relay failed: %d", hio_error(io));
        hio_close(io);
        return;
    }
    g_cli.io = io;
    g_cli.state = 1;
    if (g_cli.verbose) logf_("[client] connected to relay");
    hio_read(io);
    rtnp_wr_t w; rtnp_wr_init(&w);
    rtnp_wr_u8(&w, RTNP_ROLE_CLIENT);
    rtnp_wr_u8(&w, RTNP_VERSION);
    rtnp_wr_str(&w, "rtunnel-client/1");
    rtnp_wr_u32(&w, 0);
    rtnp_send(io, RTNP_HELLO, 0, w.data, (uint16_t)w.len);
    rtnp_wr_free(&w);
}

static void on_ping(htimer_t* timer) {
    (void)timer;
    if (g_cli.state == 2 && g_cli.io) {
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_u64(&w, hloop_now_ms(g_cli.loop));
        rtnp_send(g_cli.io, RTNP_PING, 0, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
    }
}

static void on_reconnect(htimer_t* timer) {
    (void)timer;
    if (g_cli.io != NULL) return;
    hio_t* io = hloop_create_tcp_client(g_cli.loop, g_cli.host, g_cli.port, on_connect, on_tunnel_close);
    if (io == NULL) { schedule_reconnect(); return; }
    if (g_cli.tls_ca[0]) {
        hssl_ctx_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.ca_file = g_cli.tls_ca;
        opt.crt_file = g_cli.tls_cert[0] ? g_cli.tls_cert : NULL;
        opt.key_file = g_cli.tls_key[0] ? g_cli.tls_key : NULL;
        opt.verify_peer = 1;
        opt.endpoint = HSSL_CLIENT;
        if (hio_new_ssl_ctx(io, &opt) != 0) {
            logf_("[client] ssl ctx init failed");
            hio_close(io);
            return;
        }
        hio_enable_ssl(io);
        if (g_cli.tls_sni[0]) hio_set_hostname(io, g_cli.tls_sni);
    }
    hio_setcb_read(io, on_read);
    hio_set_max_write_bufsize(io, RTNP_CONN_INFLIGHT + RTNP_MAX_PAYLOAD);
    g_cli.io = io;
}

static void schedule_reconnect(void) {
    int delay = g_cli.backoff_ms;
    g_cli.backoff_ms *= 2;
    if (g_cli.backoff_ms > 30000) g_cli.backoff_ms = 30000;
    htimer_add(g_cli.loop, on_reconnect, (uint32_t)delay, 1);
}

int main(int argc, char** argv) {
    memset(&g_cli, 0, sizeof(g_cli));
    g_cli.host = "127.0.0.1";
    g_cli.port = 8443;
    snprintf(g_cli.socks_host, sizeof(g_cli.socks_host), "%s", "127.0.0.1");
    g_cli.socks_port = 1080;
    g_cli.backoff_ms = 1000;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            g_cli.host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_cli.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            snprintf(g_cli.user, sizeof(g_cli.user), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--socks5") == 0 && i + 1 < argc) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char* colon = strrchr(buf, ':');
            if (strncmp(buf, "unix:", 5) == 0) {
                snprintf(g_cli.socks_uds, sizeof(g_cli.socks_uds), "%s", buf + 5);
            } else if (colon) {
                *colon = '\0';
                snprintf(g_cli.socks_host, sizeof(g_cli.socks_host), "%s", buf);
                g_cli.socks_port = atoi(colon + 1);
                g_cli.socks_uds[0] = '\0';
            }
        } else if (strcmp(argv[i], "--tls-ca") == 0 && i + 1 < argc) {
            snprintf(g_cli.tls_ca, sizeof(g_cli.tls_ca), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            snprintf(g_cli.tls_cert, sizeof(g_cli.tls_cert), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            snprintf(g_cli.tls_key, sizeof(g_cli.tls_key), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-sni") == 0 && i + 1 < argc) {
            snprintf(g_cli.tls_sni, sizeof(g_cli.tls_sni), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_cli.verbose = 1;
        } else {
            fprintf(stderr,
                "Usage: %s --user U [--server H] [--port P] [--socks5 HOST:PORT] [--verbose]\n",
                argv[0]);
            return 1;
        }
    }
    if (g_cli.user[0] == '\0') {
        fprintf(stderr, "--user is required\n");
        return 1;
    }

    g_cli.loop = hloop_new(0);
    rtnp_buf_init(&g_cli.in);

    hio_t* listenio = NULL;
#ifdef ENABLE_UDS
    if (g_cli.socks_uds[0]) {
        unlink(g_cli.socks_uds);
        listenio = hio_create_socket(g_cli.loop, g_cli.socks_uds, -1, HIO_TYPE_TCP, HIO_SERVER_SIDE);
        if (listenio) {
            hio_setcb_accept(listenio, on_local_accept);
            if (hio_accept(listenio) != 0) listenio = NULL;
        }
        if (listenio == NULL) {
            fprintf(stderr, "socks5 listen unix:%s failed\n", g_cli.socks_uds);
            return 2;
        }
        chmod(g_cli.socks_uds, 0600);
        logf_("[client] socks5 listening on unix:%s", g_cli.socks_uds);
    } else
#endif
    {
        listenio = hloop_create_tcp_server(g_cli.loop, g_cli.socks_host, g_cli.socks_port, on_local_accept);
        if (listenio == NULL) {
            fprintf(stderr, "socks5 listen %s:%d failed\n", g_cli.socks_host, g_cli.socks_port);
            return 2;
        }
        logf_("[client] socks5 listening on %s:%d", g_cli.socks_host, g_cli.socks_port);
    }

    on_reconnect(NULL);
    htimer_add(g_cli.loop, on_ping, 15000, INFINITE);
    hloop_run(g_cli.loop);
    hloop_free(&g_cli.loop);
    return 0;
}
