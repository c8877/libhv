#include "tunnel_link.hpp"
#include "logging.hpp"
#include "token_util.hpp"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- 回调薄封装

void TunnelLink::tunnelReadCb(hio_t* io, void* buf, int n) {
    auto* self = (TunnelLink*)hio_context(io);
    if (self) self->onTunnelRead(buf, n);
}

void TunnelLink::tunnelCloseCb(hio_t* io) {
    auto* self = (TunnelLink*)hio_context(io);
    if (self) self->onTunnelClose();
}

void TunnelLink::connectCb(hio_t* io) {
    auto* self = (TunnelLink*)hio_context(io);
    if (self) self->onConnect(io);
}

void TunnelLink::pingCb(htimer_t* t) {
    auto* self = (TunnelLink*)hevent_userdata(t);
    if (self) self->onPing();
}

void TunnelLink::reconnectCb(htimer_t* t) {
    auto* self = (TunnelLink*)hevent_userdata(t);
    if (self) self->onReconnect();
}

void TunnelLink::hsTimeoutCb(htimer_t* t) {
    auto* self = (TunnelLink*)hevent_userdata(t);
    if (self) self->onHsTimeout();
}

// ---------------------------------------------------------------- 帧处理

void TunnelLink::handleFrame(rtnp::Frame* f) {
    switch (f->type) {
    case rtnp::HELLO_ACK: {
        rtnp::Writer w;
        w.str(identity());
        // AUTH：identity + auth_type(0=none,1=静态 token,2=账号 token) + token
        bool has_token = auth_token[0] != '\0';
        uint8_t auth_type = 0;
        if (has_token) auth_type = rtutil::isClientToken(auth_token) ? 2 : 1;
        w.u8(auth_type);
        w.str(has_token ? auth_token : "");
        rtnp::send(io, rtnp::AUTH, 0, w.data, (uint16_t)w.len);
        state = 1;
        armHsTimer(10000);   // 认证超时
        return;
    }
    case rtnp::AUTH_OK: {
        state = 2;
        backoff_ms = 1000;
        ping_unacked_ = 0;
        if (hs_timer_) { htimer_del(hs_timer_); hs_timer_ = NULL; }
        // 可选尾部：账号 Token 会带回来用户名与限额，旧 relay 不发送
        rtnp::Reader r(f->payload, f->len);
        char name[128] = {0};
        r.str(name, sizeof(name));
        r.u32();   // heartbeat_ms
        if (!r.err && r.remaining() >= 1) {
            uint8_t flags = r.u8();
            if ((flags & 1) && !r.err) {
                r.u64(); r.u64(); r.u64(); r.u32(); r.u32();
                if (!r.err) onAuthInfo(name);
            }
        }
        logf_("[%s] authenticated as %s", logTag(), identity());
        onReady();
        return;
    }
    case rtnp::AUTH_FAIL: {
        rtnp::Reader r(f->payload, f->len);
        uint16_t code = r.u16();
        std::string msg = r.strBytes();
        logf_("[%s] auth failed: code=%u %s", logTag(), code, msg.c_str());
        if (code == rtnp::TOKEN_EXPIRED || code == rtnp::TOKEN_DISABLED ||
            code == rtnp::QUOTA_EXCEEDED) {
            auth_fatal_ = 1;
        }
        hio_close(io);
        return;
    }
    case rtnp::ERROR:
        logf_("[%s] rejected by relay", logTag());
        hio_close(io);
        return;
    case rtnp::PING:
        rtnp::send(io, rtnp::PONG, 0, f->payload, f->len);
        return;
    case rtnp::PONG:
        ping_unacked_ = 0;
        return;
    default:
        break;
    }

    if (state != 2) return;
    onFrame(f);
}

void TunnelLink::onTunnelRead(void* buf, int readbytes) {
    if (in_.append(buf, (size_t)readbytes) != 0) {
        hio_close(io);
        return;
    }
    for (;;) {
        rtnp::Frame f;
        int rc = in_.next(f);
        if (rc == 0) return;
        if (rc < 0) { hio_close(io); return; }
        if (verbose) logf_("[%s] frame type=0x%02x sid=%u len=%u", logTag(), f.type, f.sid, f.len);
        handleFrame(&f);
        if (io == NULL || hio_is_closed(io)) return;
    }
}

void TunnelLink::onTunnelClose() {
    if (auth_fatal_) {
        logf_("[%s] credential rejected, stop reconnecting", logTag());
        onLinkDown();
        io = NULL;
        state = 0;
        in_.reset();
        return;
    }
    if (autoReconnect()) {
        logf_("[%s] tunnel closed, reconnect in %d ms", logTag(), backoff_ms);
    }
    onLinkDown();
    if (hs_timer_) { htimer_del(hs_timer_); hs_timer_ = NULL; }
    io = NULL;
    state = 0;
    in_.reset();
    if (autoReconnect()) scheduleReconnect();
}

// 握手/认证阶段超时：HELLO_ACK 5s、AUTH_OK 10s（设计 §8.8）
void TunnelLink::onHsTimeout() {
    hs_timer_ = NULL;
    if (state == 2) return;
    logf_("[%s] handshake/auth timeout", logTag());
    if (io) hio_close(io);
}

void TunnelLink::armHsTimer(uint32_t ms) {
    if (hs_timer_) htimer_del(hs_timer_);
    hs_timer_ = htimer_add(loop, hsTimeoutCb, ms, 1);
    hevent_set_userdata(hs_timer_, this);
}

void TunnelLink::onConnect(hio_t* io) {
    if (hio_error(io) != 0) {
        logf_("[%s] connect to relay failed: %d", logTag(), hio_error(io));
        hio_close(io);
        return;
    }
    this->io = io;
    state = 1;
    if (verbose) logf_("[%s] connected to relay", logTag());
    hio_read(io);
    char agent_ver[64];
    snprintf(agent_ver, sizeof(agent_ver), "rtunnel-%s/1", logTag());
    rtnp::Writer w;
    w.u8(role());
    w.u8(rtnp::VERSION);
    w.str(agent_ver);
    w.u32(0);
    rtnp::send(io, rtnp::HELLO, 0, w.data, (uint16_t)w.len);
    armHsTimer(5000);   // 握手超时
}

void TunnelLink::onPing() {
    if (state == 2 && io) {
        if (ping_unacked_ >= 3) {
            // 连续 3 个 PING 无 PONG：连接半开，断开走重连（设计 §5.3）
            logf_("[%s] heartbeat lost, reconnecting", logTag());
            hio_close(io);
            return;
        }
        rtnp::Writer w;
        w.u64(hloop_now_ms(loop));
        rtnp::send(io, rtnp::PING, 0, w.data, (uint16_t)w.len);
        ping_unacked_++;
    }
}

void TunnelLink::onReconnect() {
    if (io != NULL) return;
    hio_t* nio = hloop_create_tcp_client(loop, host, port, connectCb, tunnelCloseCb);
    if (nio == NULL) { scheduleReconnect(); return; }
    if (tls_ca[0]) {
        hssl_ctx_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.ca_file = tls_ca;
        opt.crt_file = tls_cert[0] ? tls_cert : NULL;
        opt.key_file = tls_key[0] ? tls_key : NULL;
        opt.verify_peer = 1;
        opt.endpoint = HSSL_CLIENT;
        if (hio_new_ssl_ctx(nio, &opt) != 0) {
            logf_("[%s] ssl ctx init failed", logTag());
            hio_close(nio);
            return;
        }
        hio_enable_ssl(nio);
        if (tls_sni[0]) hio_set_hostname(nio, tls_sni);
    }
    hio_set_context(nio, this);
    hio_setcb_read(nio, tunnelReadCb);
    hio_set_max_write_bufsize(nio, rtnp::CONN_INFLIGHT + rtnp::MAX_PAYLOAD);
    io = nio;
}

void TunnelLink::scheduleReconnect() {
    int delay = backoff_ms;
    backoff_ms *= 2;
    if (backoff_ms > 30000) backoff_ms = 30000;
    htimer_t* t = htimer_add(loop, reconnectCb, (uint32_t)delay, 1);
    hevent_set_userdata(t, this);
}

void TunnelLink::init(hloop_t* loop) {
    this->loop = loop;
    onReconnect();
    htimer_t* t = htimer_add(loop, pingCb, 15000, INFINITE);
    hevent_set_userdata(t, this);
}
