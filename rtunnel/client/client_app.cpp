#include "client_app.hpp"
#include "common/logging.hpp"
#include "common/shell_spawn.hpp"

#include <sys/ioctl.h>
#include "shell_client.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hsocket.h"

// accept 回调拿不到 hio_context（新连接尚未挂对象），用进程内单例指针
static ClientApp* g_self = nullptr;

// ---------------------------------------------------------------- 回调薄封装

void ClientApp::localReadCb(hio_t* io, void* buf, int n) {
    auto* s = (ClientStream*)hio_context(io);
    if (s) s->app->onLocalRead(s, buf, n);
}

void ClientApp::localWriteCb(hio_t* io, const void* buf, int len) {
    (void)buf; (void)len;
    auto* s = (ClientStream*)hio_context(io);
    if (s) s->app->onLocalWrite(s);
}

void ClientApp::localCloseCb(hio_t* io) {
    auto* s = (ClientStream*)hio_context(io);
    if (s) s->app->onLocalClose(s);
}

void ClientApp::acceptCb(hio_t* io) {
    if (g_self) g_self->onLocalAccept(io);
}

void ClientApp::fwdAcceptCb(hio_t* io) {
    if (g_self) g_self->onFwdAccept(io);
}

void ClientApp::closeTimeoutCb(htimer_t* t) {
    auto* s = (ClientStream*)hevent_userdata(t);
    if (s) s->app->onCloseTimeout(s);
}

void ClientApp::openTimeoutCb(htimer_t* t) {
    auto* s = (ClientStream*)hevent_userdata(t);
    if (s) s->app->onOpenTimeout(s);
}

// ---------------------------------------------------------------- 会话管理

ClientStream* ClientApp::streamFind(uint32_t sid) {
    for (ClientStream* s = streams; s; s = s->next) {
        if (s->sid == sid) return s;
    }
    return NULL;
}

void ClientApp::streamFree(ClientStream* s) {
    ClientStream** pp = &streams;
    while (*pp) {
        if (*pp == s) { *pp = s->next; break; }
        pp = &(*pp)->next;
    }
    if (s->close_timer) { htimer_del(s->close_timer); s->close_timer = NULL; }
    if (s->open_timer) { htimer_del(s->open_timer); s->open_timer = NULL; }
    if (s->local && !hio_is_closed(s->local)) {
        // 先摘掉 context，避免 close_cb 再次回到已释放的 stream
        hio_set_context(s->local, NULL);
        hio_close(s->local);
    }
    delete s;
}

// 半关闭 60s 未收到对端 CLOSE：RESET(CLOSE_TIMEOUT) 并回收（设计 §7.5）
void ClientApp::onCloseTimeout(ClientStream* s) {
    s->close_timer = NULL;
    if (io && state == 2 && !hio_is_closed(io)) {
        rtnp::Writer w;
        w.u16(rtnp::CLOSE_TIMEOUT);
        rtnp::send(io, rtnp::RESET, s->sid, w.data, (uint16_t)w.len);
    }
    streamFree(s);
}

void ClientApp::armCloseTimer(ClientStream* s) {
    if (s->close_timer) return;
    s->close_timer = htimer_add(loop, closeTimeoutCb, 60000, 1);
    hevent_set_userdata(s->close_timer, s);
}

void ClientApp::socksReply(hio_t* io, uint8_t rep) {
    uint8_t r[10] = { 0x05, rep, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
    if (verbose) logf_("[client] socks5 reply rep=0x%02x", rep);
    hio_write(io, r, sizeof(r));
}

int ClientApp::validDevid(const char* s) {
    if (s == NULL || s[0] == '\0') return 0;
    if (!((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return 0;
    for (const char* p = s; *p; ++p) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) return 0;
    }
    return 1;
}

void ClientApp::flushPending(ClientStream* s) {
    if (s->state != CS_OPEN || state != 2) return;
    while (s->pending.len > 0 && s->credit > 0) {
        size_t n = s->pending.len;
        if (n > s->credit) n = s->credit;
        if (n > rtnp::MAX_PAYLOAD) n = rtnp::MAX_PAYLOAD;
        if (rtnp::send(io, rtnp::DATA, s->sid, s->pending.data, (uint16_t)n) != 0) return;
        s->credit -= (uint32_t)n;
        s->pending.consume(n);
    }
    if (s->pending.len > 0) {
        if (!s->paused) { hio_read_stop(s->local); s->paused = 1; }
    } else if (s->paused) {
        s->paused = 0;
        hio_read(s->local);
    }
}

void ClientApp::onOpenTimeout(ClientStream* s) {
    s->open_timer = NULL;
    if (s->state != CS_OPENING) return;
    // OPEN 15s 未决：RESET 通知 B/C 取消，释放本地，避免映射悬挂
    if (state == 2 && io && !hio_is_closed(io)) {
        rtnp::send(io, rtnp::RESET, s->sid, NULL, 0);
    }
    if (s->local && !s->is_forward) socksReply(s->local, 0x04);
    if (s->local) {
        s->state = CS_CLOSED;   // 让 onLocalClose 走 streamFree 分支
        hio_close(s->local);
    } else {
        streamFree(s);
    }
}

void ClientApp::armOpenTimer(ClientStream* s) {
    s->open_timer = htimer_add(loop, openTimeoutCb, 15000, 1);
    hevent_set_userdata(s->open_timer, s);
}

// ---------------------------------------------------------------- SOCKS5 握手

void ClientApp::trySocks(ClientStream* s) {
    for (;;) {
        if (s->state == CS_HANDSHAKE) {
            if (s->hs.len < 2) return;
            uint8_t* p = s->hs.data;
            if (p[0] != 0x05) { hio_close(s->local); return; }
            size_t need = 2 + p[1];
            if (s->hs.len < need) return;
            uint8_t reply[2] = { 0x05, 0x00 };
            hio_write(s->local, reply, 2);
            s->hs.consume(need);
            s->state = CS_REQUEST;
            continue;
        }
        if (s->state == CS_REQUEST) {
            if (s->hs.len < 4) return;
            uint8_t* p = s->hs.data;
            if (p[0] != 0x05) { hio_close(s->local); return; }
            if (p[1] != 0x01) { socksReply(s->local, 0x07); hio_close(s->local); return; }
            if (p[3] != 0x03) { socksReply(s->local, 0x08); hio_close(s->local); return; }
            if (s->hs.len < 5) return;
            size_t dlen = p[4];
            size_t need = 5 + dlen + 2;
            if (s->hs.len < need) return;

            char devid[128] = {0};
            if (dlen >= sizeof(devid)) { socksReply(s->local, 0x01); hio_close(s->local); return; }
            memcpy(devid, p + 5, dlen);
            uint16_t port = (uint16_t)((p[5 + dlen] << 8) | p[6 + dlen]);
            if (!validDevid(devid) || port == 0) { socksReply(s->local, 0x01); hio_close(s->local); return; }
            s->hs.consume(need);

            if (state != 2 || io == NULL) {
                socksReply(s->local, 0x01);
                hio_close(s->local);
                return;
            }

            s->sid = ++next_sid;
            s->state = CS_OPENING;
            armOpenTimer(s);

            rtnp::Writer w;
            w.str(devid);
            w.u16(port);
            rtnp::send(io, rtnp::OPEN, s->sid, w.data, (uint16_t)w.len);
            if (verbose) logf_("[client] OPEN %s:%u sid=%u", devid, port, s->sid);
            return;
        }
        return;
    }
}

void ClientApp::onLocalRead(ClientStream* s, void* buf, int readbytes) {
    if (readbytes <= 0) return;

    if (s->state < CS_OPEN) {
        if (s->hs.append(buf, (size_t)readbytes) != 0) { hio_close(s->local); return; }
        if (s->state == CS_HANDSHAKE || s->state == CS_REQUEST) {
            trySocks(s);
        }
        return;
    }
    if (s->state != CS_OPEN) return;
    // 注意：remote_closed 只表示 C→A 方向已 EOF，A→C 方向仍要转发客户端数据。
    if (s->pending.append(buf, (size_t)readbytes) != 0) {
        if (io) rtnp::send(io, rtnp::RESET, s->sid, NULL, 0);
        streamFree(s);
        return;
    }
    flushPending(s);
}

void ClientApp::maybeShutdownWrite(ClientStream* s) {
    if (!s->want_shutdown || s->local == NULL || hio_is_closed(s->local)) return;
    if (hio_write_is_complete(s->local)) {
        shutdown(hio_fd(s->local), SHUT_WR);
        s->want_shutdown = 0;
        hio_setcb_write(s->local, NULL);
    }
}

void ClientApp::maybeGrantWindow(ClientStream* s) {
    if (s->local == NULL || hio_is_closed(s->local) || s->unacked == 0) return;
    // 只有本地写队列降到低水位才补窗口（hio_write 入队不代表对端已消费）
    if (hio_write_bufsize(s->local) > rtnp::LOW_WATER) {
        hio_setcb_write(s->local, localWriteCb);
        return;
    }
    if (io) {
        rtnp::Writer w;
        w.u32(s->unacked);
        rtnp::send(io, rtnp::WINDOW, s->sid, w.data, (uint16_t)w.len);
    }
    s->unacked = 0;
    if (!s->want_shutdown) hio_setcb_write(s->local, NULL);
}

void ClientApp::onLocalWrite(ClientStream* s) {
    maybeShutdownWrite(s);
    maybeGrantWindow(s);
}

void ClientApp::onLocalClose(ClientStream* s) {
    s->local = NULL;
    if (s->open_timer) { htimer_del(s->open_timer); s->open_timer = NULL; }
    if (state == 2 && io && !hio_is_closed(io)) {
        if (s->state == CS_OPENING) {
            // OPEN 未决时本地断开：通知 B 取消（CANCELLED），避免 relay 侧映射悬挂
            rtnp::send(io, rtnp::RESET, s->sid, NULL, 0);
        } else if (s->state == CS_OPEN && !s->local_eof) {
            s->local_eof = 1;
            rtnp::send(io, rtnp::CLOSE, s->sid, NULL, 0);
        }
    }
    if (s->state != CS_OPEN || s->remote_closed) {
        streamFree(s);
    } else {
        armCloseTimer(s);
    }
}

ClientStream* ClientApp::streamNew(hio_t* io) {
    auto* s = new ClientStream{};
    s->app = this;
    s->local = io;
    s->credit = rtnp::DEFAULT_CREDIT;
    s->next = streams;
    streams = s;

    hio_set_context(io, s);
    hio_setcb_read(io, localReadCb);
    hio_setcb_close(io, localCloseCb);
    hio_set_max_write_bufsize(io, rtnp::CONN_INFLIGHT + rtnp::MAX_PAYLOAD);
    hio_read(io);
    return s;
}

void ClientApp::onLocalAccept(hio_t* io) {
    streamNew(io);   // 初始状态 CS_HANDSHAKE
}

// 端口映射模式：本地端口直接对应 devid:target_port，跳过 SOCKS5 握手
void ClientApp::onFwdAccept(hio_t* io) {
    struct sockaddr* la = hio_localaddr(io);
    int lport = la ? sockaddr_port((sockaddr_u*)la) : 0;
    int idx = -1;
    for (int i = 0; i < nfwd; ++i) {
        if (fwd[i].listen_port == lport) { idx = i; break; }
    }
    if (idx < 0 || state != 2 || io == NULL) {
        hio_close(io);
        return;
    }
    ClientStream* s = streamNew(io);
    s->is_forward = 1;
    s->state = CS_OPENING;
    s->sid = ++next_sid;
    armOpenTimer(s);

    rtnp::Writer w;
    w.str(fwd[idx].devid);
    w.u16((uint16_t)fwd[idx].target_port);
    rtnp::send(this->io, rtnp::OPEN, s->sid, w.data, (uint16_t)w.len);
    if (verbose) logf_("[client] forward %s:%d -> %s:%d sid=%u",
                       fwd[idx].listen_host, lport,
                       fwd[idx].devid, fwd[idx].target_port, s->sid);
}

// READY 后的会话级帧（连接级 HELLO/AUTH/PING 由 TunnelLink 处理）
void ClientApp::onFrame(rtnp::Frame* f) {
    ClientStream* s = streamFind(f->sid);
    if (s == NULL) return;

    switch (f->type) {
    case rtnp::OPEN_OK:
        if (s->open_timer) { htimer_del(s->open_timer); s->open_timer = NULL; }
        if (s->state != CS_OPENING) return;
        s->state = CS_OPEN;
        if (!s->is_forward) socksReply(s->local, 0x00);
        // 客户端可能在收到 SOCKS5 应答前就发了数据（pipelining）
        if (s->hs.len > 0) {
            s->pending.append(s->hs.data, s->hs.len);
            s->hs.clear();
        }
        flushPending(s);
        return;
    case rtnp::OPEN_FAIL: {
        if (s->open_timer) { htimer_del(s->open_timer); s->open_timer = NULL; }
        rtnp::Reader r(f->payload, f->len);
        uint16_t code = r.u16();
        if (verbose) logf_("[client] OPEN_FAIL sid=%u code=%u", s->sid, code);
        uint8_t rep = 0x01;
        if (code == rtnp::DEVICE_OFFLINE || code == rtnp::TIMEOUT) rep = 0x04;
        else if (code == rtnp::TARGET_REFUSED) rep = 0x05;
        else if (code == rtnp::ACL_DENIED || code == rtnp::TARGET_NOT_ALLOWED) rep = 0x02;
        if (!s->is_forward) socksReply(s->local, rep);
        s->state = CS_CLOSED;   // relay 侧已删映射，防止 onLocalClose 再发 RESET
        hio_close(s->local);
        return;
    }
    case rtnp::DATA:
        if (s->state != CS_OPEN || s->local == NULL) return;
        if (f->len > 0) {
            hio_write(s->local, f->payload, f->len);
            s->unacked += f->len;
        }
        maybeGrantWindow(s);
        return;
    case rtnp::WINDOW: {
        rtnp::Reader r(f->payload, f->len);
        uint32_t inc = r.u32();
        if (r.err) return;
        s->credit += inc;
        if (s->credit > rtnp::DEFAULT_CREDIT) s->credit = rtnp::DEFAULT_CREDIT;
        flushPending(s);
        return;
    }
    case rtnp::CLOSE:
        s->remote_closed = 1;
        if (s->local == NULL) { streamFree(s); return; }
        if (hio_write_is_complete(s->local)) {
            shutdown(hio_fd(s->local), SHUT_WR);
            s->want_shutdown = 0;
        } else {
            s->want_shutdown = 1;
            hio_setcb_write(s->local, localWriteCb);
        }
        if (s->local_eof) streamFree(s);
        else armCloseTimer(s);
        return;
    case rtnp::RESET:
        streamFree(s);
        return;
    default:
        return;
    }
}

void ClientApp::onLinkDown() {
    while (streams) {
        ClientStream* s = streams;
        streams = s->next;
        if (s->close_timer) htimer_del(s->close_timer);
        if (s->local && !hio_is_closed(s->local)) hio_close(s->local);
        if (s->open_timer) htimer_del(s->open_timer);
        delete s;
    }
}

// ---------------------------------------------------------------- 入口

int ClientApp::run(int argc, char** argv) {
    g_self = this;

    const char* shell_devid = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "shell") == 0 && i + 1 < argc) {
            shell_devid = argv[++i];
        } else if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc) {
            snprintf(user, sizeof(user), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--socks5") == 0 && i + 1 < argc) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char* colon = strrchr(buf, ':');
            if (strncmp(buf, "unix:", 5) == 0) {
                snprintf(socks_uds, sizeof(socks_uds), "%s", buf + 5);
            } else if (colon) {
                *colon = '\0';
                snprintf(socks_host, sizeof(socks_host), "%s", buf);
                socks_port = atoi(colon + 1);
                socks_uds[0] = '\0';
            }
        } else if (strcmp(argv[i], "--forward") == 0 && i + 1 < argc && nfwd < 8) {
            char buf[384];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char* eq = strchr(buf, '=');
            char* colon = eq ? strchr(eq + 1, ':') : NULL;
            char* lcolon = NULL;
            if (eq && colon) {
                *eq = '\0';
                *colon = '\0';
                lcolon = strrchr(buf, ':');   // 只在 "listen_host:port" 里找
            }
            if (lcolon) {
                *lcolon = '\0';
                snprintf(fwd[nfwd].listen_host, 64, "%s", buf);
                fwd[nfwd].listen_port = atoi(lcolon + 1);
                snprintf(fwd[nfwd].devid, 128, "%s", eq + 1);
                fwd[nfwd].target_port = atoi(colon + 1);
                nfwd++;
            } else {
                fprintf(stderr, "--forward 格式: 127.0.0.1:16022=devid:22\n");
                return 1;
            }
        } else if (strcmp(argv[i], "--tls-ca") == 0 && i + 1 < argc) {
            snprintf(tls_ca, sizeof(tls_ca), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            snprintf(tls_cert, sizeof(tls_cert), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            snprintf(tls_key, sizeof(tls_key), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-sni") == 0 && i + 1 < argc) {
            snprintf(tls_sni, sizeof(tls_sni), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else {
            fprintf(stderr,
                "Usage: %s --user U [--server H] [--port P] [--socks5 HOST:PORT]\n"
                "          [--forward 127.0.0.1:16022=devid:22 ...] [--verbose]\n",
                argv[0]);
            return 1;
        }
    }
    if (user[0] == '\0') {
        fprintf(stderr, "--user is required\n");
        return 1;
    }

    // shell 子命令：本机终端直连设备免密 shell（不启动 SOCKS5/forward 入口）
    if (shell_devid) {
        ShellClient sc;
        sc.host = host;
        sc.port = port;
        sc.verbose = verbose;
        snprintf(sc.user, sizeof(sc.user), "%s", user);
        snprintf(sc.tls_ca, sizeof(sc.tls_ca), "%s", tls_ca);
        snprintf(sc.tls_cert, sizeof(sc.tls_cert), "%s", tls_cert);
        snprintf(sc.tls_key, sizeof(sc.tls_key), "%s", tls_key);
        snprintf(sc.tls_sni, sizeof(sc.tls_sni), "%s", tls_sni);
        return sc.run(shell_devid);
    }

    hloop_t* lp = hloop_new(0);

    hio_t* listenio = NULL;
#ifdef ENABLE_UDS
    if (socks_uds[0]) {
        unlink(socks_uds);
        listenio = hio_create_socket(lp, socks_uds, -1, HIO_TYPE_TCP, HIO_SERVER_SIDE);
        if (listenio) {
            hio_setcb_accept(listenio, acceptCb);
            if (hio_accept(listenio) != 0) listenio = NULL;
        }
        if (listenio == NULL) {
            fprintf(stderr, "socks5 listen unix:%s failed\n", socks_uds);
            return 2;
        }
        chmod(socks_uds, 0600);
        logf_("[client] socks5 listening on unix:%s", socks_uds);
    } else
#endif
    {
        listenio = hloop_create_tcp_server(lp, socks_host, socks_port, acceptCb);
        if (listenio == NULL) {
            fprintf(stderr, "socks5 listen %s:%d failed\n", socks_host, socks_port);
            return 2;
        }
        logf_("[client] socks5 listening on %s:%d", socks_host, socks_port);
    }

    for (int i = 0; i < nfwd; ++i) {
        hio_t* lio = hloop_create_tcp_server(lp, fwd[i].listen_host,
                                             fwd[i].listen_port, fwdAcceptCb);
        if (lio == NULL) {
            fprintf(stderr, "forward listen %s:%d failed\n",
                    fwd[i].listen_host, fwd[i].listen_port);
            return 2;
        }
        fwd[i].listen_io = lio;
        logf_("[client] forward listening on %s:%d -> %s:%d",
              fwd[i].listen_host, fwd[i].listen_port,
              fwd[i].devid, fwd[i].target_port);
    }

    init(lp);
    hloop_run(lp);
    hloop_free(&lp);
    return 0;
}
