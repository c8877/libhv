// C：设备侧 agent。连 B、认证、执行 OPEN、把字节透传给本地端口。
//
// 用法：rtunnel-agent --server HOST --port 8443 --devid PILE-000123 --allow 22 [--verbose]

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>

#include "rtnp.h"

typedef struct astream_s {
    uint32_t         sid;
    hio_t*           local;
    int              connected;
    int              local_eof;
    int              remote_closed;
    int              paused;
    int              want_shutdown;
    uint32_t         credit;
    uint32_t         unacked;
    rtnp_buf_t       pending;
    htimer_t*        close_timer;   // 半关闭 60s 超时
    struct astream_s* next;
} astream_t;

typedef struct {
    hloop_t*     loop;
    const char*  host;
    int          port;
    char         devid[128];
    char         tls_ca[256];
    char         tls_cert[256];
    char         tls_key[256];
    char         tls_sni[256];
    int          allow_ports[32];
    int          nallow;
    int          allow_all;
    int          verbose;
    hio_t*       io;
    int          state;     // 0=connecting 1=hello 2=authed
    rtnp_buf_t   in;
    astream_t*   streams;
    int          backoff_ms;
    htimer_t*    hs_timer;      // 握手/认证阶段超时
    int          ping_unacked;  // 连续未获 PONG 的 PING 数
} agent_t;

static agent_t g_agent;

static void on_local_write(hio_t* io, const void* buf, int len);
static void maybe_grant_window(astream_t* s);
static void schedule_reconnect(void);
static void arm_hs_timer(uint32_t ms);

static void logf_(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static int port_allowed(int port) {
    if (g_agent.allow_all) return 1;
    for (int i = 0; i < g_agent.nallow; ++i) {
        if (g_agent.allow_ports[i] == port) return 1;
    }
    return 0;
}

static astream_t* stream_find(uint32_t sid) {
    for (astream_t* s = g_agent.streams; s; s = s->next) {
        if (s->sid == sid) return s;
    }
    return NULL;
}

static void stream_free(astream_t* s) {
    astream_t** pp = &g_agent.streams;
    while (*pp) {
        if (*pp == s) { *pp = s->next; break; }
        pp = &(*pp)->next;
    }
    if (s->close_timer) { htimer_del(s->close_timer); s->close_timer = NULL; }
    if (s->local && !hio_is_closed(s->local)) {
        hio_set_context(s->local, NULL);
        hio_close(s->local);
    }
    rtnp_buf_free(&s->pending);
    free(s);
}

// 半关闭 60s 未收到对端 CLOSE：RESET(CLOSE_TIMEOUT) 并回收（设计 §7.5）
static void on_close_timeout(htimer_t* timer) {
    astream_t* s = (astream_t*)hevent_userdata(timer);
    if (s == NULL) return;
    s->close_timer = NULL;
    if (g_agent.io && g_agent.state == 2 && !hio_is_closed(g_agent.io)) {
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_u16(&w, RTNP_CLOSE_TIMEOUT);
        rtnp_send(g_agent.io, RTNP_RESET, s->sid, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
    }
    stream_free(s);
}

static void arm_close_timer(astream_t* s) {
    if (s->close_timer) return;
    s->close_timer = htimer_add(g_agent.loop, on_close_timeout, 60000, 1);
    hevent_set_userdata(s->close_timer, s);
}

static void buf_consume(rtnp_buf_t* b, size_t n) {
    if (n >= b->len) { b->len = 0; return; }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

static void send_open_fail(uint32_t sid, uint16_t code, const char* msg) {
    rtnp_wr_t w; rtnp_wr_init(&w);
    rtnp_wr_u16(&w, code);
    rtnp_wr_str(&w, msg ? msg : "");
    rtnp_send(g_agent.io, RTNP_OPEN_FAIL, sid, w.data, (uint16_t)w.len);
    rtnp_wr_free(&w);
}

static void flush_pending(astream_t* s) {
    // remote_closed 只表示 A→C 方向 EOF，不影响 C→A 的数据回传
    if (!s->connected) return;
    while (s->pending.len > 0 && s->credit > 0) {
        size_t n = s->pending.len;
        if (n > s->credit) n = s->credit;
        if (n > RTNP_MAX_PAYLOAD) n = RTNP_MAX_PAYLOAD;
        if (rtnp_send(g_agent.io, RTNP_DATA, s->sid, s->pending.data, (uint16_t)n) != 0) {
            return;
        }
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

static void maybe_grant_window(astream_t* s) {
    if (s->local == NULL || hio_is_closed(s->local) || s->unacked == 0) return;
    if (hio_write_bufsize(s->local) > RTNP_LOW_WATER) {
        hio_setcb_write(s->local, on_local_write);
        return;
    }
    if (g_agent.io) {
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_u32(&w, s->unacked);
        rtnp_send(g_agent.io, RTNP_WINDOW, s->sid, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
    }
    s->unacked = 0;
    if (!s->want_shutdown) hio_setcb_write(s->local, NULL);
}

static void maybe_shutdown_write(astream_t* s) {
    if (!s->want_shutdown || s->local == NULL || hio_is_closed(s->local)) return;
    if (hio_write_is_complete(s->local)) {
        shutdown(hio_fd(s->local), SHUT_WR);
        s->want_shutdown = 0;
        hio_setcb_write(s->local, NULL);
    }
}

static void on_local_write(hio_t* io, const void* buf, int len) {
    (void)buf; (void)len;
    astream_t* s = (astream_t*)hio_context(io);
    if (s == NULL) return;
    maybe_shutdown_write(s);
    maybe_grant_window(s);
}

static void on_local_close(hio_t* io) {
    astream_t* s = (astream_t*)hio_context(io);
    if (g_agent.verbose) logf_("[agent] local close sid=%u", s ? s->sid : 0);
    if (s == NULL) return;
    if (!s->connected) {
        // 本地 connect 失败
        send_open_fail(s->sid, RTNP_TARGET_REFUSED, "connect refused");
        s->local = NULL;
        stream_free(s);
        return;
    }
    if (!s->local_eof) {
        s->local_eof = 1;
        if (g_agent.state == 2 && g_agent.io && !hio_is_closed(g_agent.io)) {
            rtnp_send(g_agent.io, RTNP_CLOSE, s->sid, NULL, 0);
        }
    }
    s->local = NULL;
    if (s->remote_closed) {
        stream_free(s);
    } else {
        arm_close_timer(s);
    }
}

static void on_local_read(hio_t* io, void* buf, int readbytes) {
    astream_t* s = (astream_t*)hio_context(io);
    if (g_agent.verbose) logf_("[agent] local read sid=%u len=%d", s ? s->sid : 0, readbytes);
    if (s == NULL || readbytes <= 0) return;
    // 注意：remote_closed 只表示 A→C 方向已 EOF，C→A 方向仍要把数据发回去。
    if (rtnp_buf_append(&s->pending, buf, (size_t)readbytes) != 0) {
        rtnp_send(g_agent.io, RTNP_RESET, s->sid, NULL, 0);
        stream_free(s);
        return;
    }
    flush_pending(s);
}

static void on_local_connect(hio_t* io) {
    astream_t* s = (astream_t*)hio_context(io);
    if (s == NULL) return;
    int err = hio_error(io);
    if (err != 0) {
        send_open_fail(s->sid, RTNP_TARGET_REFUSED, "connect failed");
        s->local = NULL;
        stream_free(s);
        return;
    }
    s->connected = 1;
    rtnp_wr_t w; rtnp_wr_init(&w);
    rtnp_wr_str(&w, "127.0.0.1");
    rtnp_wr_u16(&w, 0);
    rtnp_send(g_agent.io, RTNP_OPEN_OK, s->sid, w.data, (uint16_t)w.len);
    rtnp_wr_free(&w);
    hio_setcb_read(io, on_local_read);
    hio_read(io);
    if (g_agent.verbose) logf_("[agent] stream %u -> local fd=%d ok", s->sid, hio_fd(io));
}

static void handle_open(const rtnp_frame_t* f) {
    rtnp_rd_t r; rtnp_rd_init(&r, f->payload, f->len);
    uint16_t port = rtnp_rd_u16(&r);
    if (r.err || port == 0) {
        send_open_fail(f->sid, RTNP_PROTOCOL_ERROR, "bad open");
        return;
    }
    if (!port_allowed(port)) {
        send_open_fail(f->sid, RTNP_TARGET_NOT_ALLOWED, "port not allowed");
        return;
    }
    if (stream_find(f->sid) != NULL) {
        send_open_fail(f->sid, RTNP_PROTOCOL_ERROR, "sid in use");
        return;
    }

    astream_t* s = calloc(1, sizeof(*s));
    s->sid = f->sid;
    s->credit = RTNP_DEFAULT_CREDIT;
    rtnp_buf_init(&s->pending);
    s->next = g_agent.streams;
    g_agent.streams = s;

    hio_t* io = hio_create_socket(g_agent.loop, "127.0.0.1", port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
    if (io == NULL) {
        send_open_fail(f->sid, RTNP_TARGET_REFUSED, "socket failed");
        stream_free(s);
        return;
    }
    s->local = io;
    hio_set_context(io, s);
    hio_setcb_connect(io, on_local_connect);
    hio_setcb_close(io, on_local_close);
    hio_set_max_write_bufsize(io, RTNP_CONN_INFLIGHT + RTNP_MAX_PAYLOAD);
    if (hio_connect(io) != 0) {
        send_open_fail(f->sid, RTNP_TARGET_REFUSED, "connect failed");
        s->local = NULL;
        stream_free(s);
    }
}

static void handle_frame(rtnp_frame_t* f) {
    switch (f->type) {
    case RTNP_HELLO_ACK: {
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_str(&w, g_agent.devid);
        rtnp_send(g_agent.io, RTNP_AUTH, 0, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
        g_agent.state = 1;
        arm_hs_timer(10000);   // 认证超时
        return;
    }
    case RTNP_AUTH_OK:
        g_agent.state = 2;
        g_agent.backoff_ms = 1000;
        g_agent.ping_unacked = 0;
        if (g_agent.hs_timer) { htimer_del(g_agent.hs_timer); g_agent.hs_timer = NULL; }
        logf_("[agent] authenticated as %s", g_agent.devid);
        return;
    case RTNP_AUTH_FAIL:
    case RTNP_ERROR:
        logf_("[agent] rejected by relay, closing");
        hio_close(g_agent.io);
        return;
    case RTNP_PING:
        rtnp_send(g_agent.io, RTNP_PONG, 0, f->payload, f->len);
        return;
    case RTNP_PONG:
        g_agent.ping_unacked = 0;
        return;
    default:
        break;
    }

    if (g_agent.state != 2) return;

    switch (f->type) {
    case RTNP_OPEN:
        handle_open(f);
        return;
    case RTNP_DATA: {
        astream_t* s = stream_find(f->sid);
        if (s == NULL || s->local == NULL) return;
        if (f->len > 0) {
            hio_write(s->local, f->payload, f->len);
            s->unacked += f->len;
        }
        maybe_grant_window(s);
        return;
    }
    case RTNP_WINDOW: {
        astream_t* s = stream_find(f->sid);
        if (s == NULL) return;
        rtnp_rd_t r; rtnp_rd_init(&r, f->payload, f->len);
        uint32_t inc = rtnp_rd_u32(&r);
        if (r.err) return;
        s->credit += inc;
        if (s->credit > RTNP_DEFAULT_CREDIT) s->credit = RTNP_DEFAULT_CREDIT;
        flush_pending(s);
        return;
    }
    case RTNP_CLOSE: {
        astream_t* s = stream_find(f->sid);
        if (s == NULL) return;
        s->remote_closed = 1;
        if (s->local && hio_write_is_complete(s->local)) {
            shutdown(hio_fd(s->local), SHUT_WR);
            s->want_shutdown = 0;
        } else {
            s->want_shutdown = 1;
            if (s->local) hio_setcb_write(s->local, on_local_write);
        }
        if (s->local_eof) stream_free(s);
        else arm_close_timer(s);
        return;
    }
    case RTNP_RESET: {
        astream_t* s = stream_find(f->sid);
        if (s) stream_free(s);
        return;
    }
    default:
        return;
    }
}

static void on_read(hio_t* io, void* buf, int readbytes) {
    (void)io;
    if (rtnp_buf_append(&g_agent.in, buf, (size_t)readbytes) != 0) {
        hio_close(g_agent.io);
        return;
    }
    for (;;) {
        rtnp_frame_t f;
        int rc = rtnp_buf_parse(&g_agent.in, &f);
        if (rc == 0) return;
        if (rc < 0) { hio_close(g_agent.io); return; }
        handle_frame(&f);
        if (g_agent.io == NULL || hio_is_closed(g_agent.io)) return;
    }
}

static void on_close(hio_t* io) {
    (void)io;
    logf_("[agent] link to relay closed, reconnect in %d ms", g_agent.backoff_ms);
    // 关闭所有本地连接
    while (g_agent.streams) {
        astream_t* s = g_agent.streams;
        g_agent.streams = s->next;
        if (s->close_timer) htimer_del(s->close_timer);
        if (s->local && !hio_is_closed(s->local)) hio_close(s->local);
        rtnp_buf_free(&s->pending);
        free(s);
    }
    if (g_agent.hs_timer) { htimer_del(g_agent.hs_timer); g_agent.hs_timer = NULL; }
    g_agent.io = NULL;
    g_agent.state = 0;
    g_agent.in.len = 0;
    schedule_reconnect();
}

// 握手/认证阶段超时：HELLO_ACK 5s、AUTH_OK 10s（设计 §8.8）
static void on_hs_timeout(htimer_t* timer) {
    (void)timer;
    g_agent.hs_timer = NULL;
    if (g_agent.state == 2) return;
    logf_("[agent] handshake/auth timeout");
    if (g_agent.io) hio_close(g_agent.io);
}

static void arm_hs_timer(uint32_t ms) {
    if (g_agent.hs_timer) htimer_del(g_agent.hs_timer);
    g_agent.hs_timer = htimer_add(g_agent.loop, on_hs_timeout, ms, 1);
}

static void on_connect(hio_t* io) {
    if (hio_error(io) != 0) {
        logf_("[agent] connect to relay failed: %d", hio_error(io));
        hio_close(io);
        return;
    }
    g_agent.io = io;
    g_agent.state = 1;
    if (g_agent.verbose) logf_("[agent] connected to relay");
    hio_read(io);
    rtnp_wr_t w; rtnp_wr_init(&w);
    rtnp_wr_u8(&w, RTNP_ROLE_AGENT);
    rtnp_wr_u8(&w, RTNP_VERSION);
    rtnp_wr_str(&w, "rtunnel-agent/1");
    rtnp_wr_u32(&w, 0);
    rtnp_send(io, RTNP_HELLO, 0, w.data, (uint16_t)w.len);
    rtnp_wr_free(&w);
    arm_hs_timer(5000);   // 握手超时
}

static void on_ping(htimer_t* timer) {
    (void)timer;
    if (g_agent.state == 2 && g_agent.io) {
        if (g_agent.ping_unacked >= 3) {
            // 连续 3 个 PING 无 PONG：连接半开，断开走重连（设计 §5.3）
            logf_("[agent] heartbeat lost, reconnecting");
            hio_close(g_agent.io);
            return;
        }
        rtnp_wr_t w; rtnp_wr_init(&w);
        rtnp_wr_u64(&w, hloop_now_ms(g_agent.loop));
        rtnp_send(g_agent.io, RTNP_PING, 0, w.data, (uint16_t)w.len);
        rtnp_wr_free(&w);
        g_agent.ping_unacked++;
    }
}

static void on_reconnect(htimer_t* timer) {
    (void)timer;
    if (g_agent.io != NULL) return;
    hio_t* io = hloop_create_tcp_client(g_agent.loop, g_agent.host, g_agent.port, on_connect, on_close);
    if (io == NULL) {
        schedule_reconnect();
        return;
    }
    if (g_agent.tls_ca[0]) {
        hssl_ctx_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.ca_file = g_agent.tls_ca;
        opt.crt_file = g_agent.tls_cert[0] ? g_agent.tls_cert : NULL;
        opt.key_file = g_agent.tls_key[0] ? g_agent.tls_key : NULL;
        opt.verify_peer = 1;
        opt.endpoint = HSSL_CLIENT;
        if (hio_new_ssl_ctx(io, &opt) != 0) {
            logf_("[agent] ssl ctx init failed");
            hio_close(io);
            return;
        }
        hio_enable_ssl(io);
        if (g_agent.tls_sni[0]) hio_set_hostname(io, g_agent.tls_sni);
    }
    hio_setcb_read(io, on_read);
    hio_set_max_write_bufsize(io, RTNP_CONN_INFLIGHT + RTNP_MAX_PAYLOAD);
    g_agent.io = io;
}

static void schedule_reconnect(void) {
    int delay = g_agent.backoff_ms;
    g_agent.backoff_ms *= 2;
    if (g_agent.backoff_ms > 30000) g_agent.backoff_ms = 30000;
    htimer_add(g_agent.loop, on_reconnect, (uint32_t)delay, 1);
}

int main(int argc, char** argv) {
    memset(&g_agent, 0, sizeof(g_agent));
    g_agent.host = "127.0.0.1";
    g_agent.port = 8443;
    g_agent.backoff_ms = 1000;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            g_agent.host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            g_agent.port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--devid") == 0 && i + 1 < argc) {
            snprintf(g_agent.devid, sizeof(g_agent.devid), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--allow") == 0 && i + 1 < argc) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char* save = NULL;
            for (char* p = strtok_r(buf, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
                if (g_agent.nallow < (int)(sizeof(g_agent.allow_ports) / sizeof(int))) {
                    g_agent.allow_ports[g_agent.nallow++] = atoi(p);
                }
            }
        } else if (strcmp(argv[i], "--allow-all") == 0) {
            g_agent.allow_all = 1;
        } else if (strcmp(argv[i], "--tls-ca") == 0 && i + 1 < argc) {
            snprintf(g_agent.tls_ca, sizeof(g_agent.tls_ca), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            snprintf(g_agent.tls_cert, sizeof(g_agent.tls_cert), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            snprintf(g_agent.tls_key, sizeof(g_agent.tls_key), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-sni") == 0 && i + 1 < argc) {
            snprintf(g_agent.tls_sni, sizeof(g_agent.tls_sni), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            g_agent.verbose = 1;
        } else {
            fprintf(stderr,
                "Usage: %s --devid ID [--server H] [--port P] [--allow 22,80] [--allow-all] [--verbose]\n",
                argv[0]);
            return 1;
        }
    }
    if (g_agent.devid[0] == '\0') {
        fprintf(stderr, "--devid is required\n");
        return 1;
    }

    g_agent.loop = hloop_new(0);
    rtnp_buf_init(&g_agent.in);
    on_reconnect(NULL);
    htimer_add(g_agent.loop, on_ping, 15000, INFINITE);
    hloop_run(g_agent.loop);
    hloop_free(&g_agent.loop);
    return 0;
}
