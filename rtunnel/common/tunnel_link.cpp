#include "tunnel_link.hpp"
#include "logging.hpp"
#include "token_util.hpp"
#include "hsocket.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// 连不上/握手失败的地址在这段时间内跳过，避免每次重连都在故障地址上超时；
// 冷却结束后会重新从高优先级开始尝试。
static const long long kEndpointFailCooldownMs = 30000;

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

void TunnelLink::switchCb(htimer_t* t) {
    auto* self = (TunnelLink*)hevent_userdata(t);
    if (self && self->io) hio_close(self->io);
}

// ---------------------------------------------------------------- 帧处理

void TunnelLink::handleFrame(rtnp::Frame* f) {
    switch (f->type) {
    case rtnp::HELLO_ACK: {
        // 可选尾部：u8 proto + u8 proto_min（旧 relay 不发）。
        // 这里只记录，具体用哪个能力由业务帧自己按对端版本决定。
        {
            rtnp::Reader hr(f->payload, f->len);
            hr.u16();   // max_payload
            hr.u8();    // auth flags
            hr.u32();   // heartbeat_ms
            hr.u32();
            if (!hr.err && hr.remaining() >= 2) {
                int rv = hr.u8();   // relay 自身版本
                int rm = hr.u8();   // relay 要求的最低版本
                if (rm > rtnp::PROTO_VERSION) {
                    logf_("[%s] relay(v%d) requires protocol >= %d, local %d", logTag(), rv, rm,
                          rtnp::PROTO_VERSION);
                }
            }
        }
        rtnp::Writer w;
        w.str(identity());
        // AUTH：identity + auth_type(0=none,1=接入令牌 rtk-) + token
        // client 的身份完全由令牌决定，identity 只对 agent（devid）有意义。
        bool has_token = auth_token[0] != '\0';
        w.u8(has_token ? 1 : 0);
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
        if (ep_index_ < ep_fail_until_.size()) ep_fail_until_[ep_index_] = 0;
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
        // 凭证问题不会自愈：该地址标记失败，继续试其它 relay；
        // 全部地址都拒绝才彻底停止重连（运维改完配置后重启进程即可）。
        if (ep_index_ < ep_auth_bad_.size()) ep_auth_bad_[ep_index_] = 1;
        bool all_bad = !ep_auth_bad_.empty();
        for (char bad : ep_auth_bad_) {
            if (!bad) { all_bad = false; break; }
        }
        if (all_bad) auth_fatal_ = 1;
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
    bool was_ready = (state == 2);
    onLinkDown();
    if (hs_timer_) { htimer_del(hs_timer_); hs_timer_ = NULL; }
    io = NULL;
    state = 0;
    ping_unacked_ = 0;
    in_.reset();

    if (auth_fatal_) {
        logf_("[%s] credential rejected, stop reconnecting", logTag());
        return;
    }
    if (!autoReconnect()) return;

    if (pin_next_) {
        // 按 relay 下发的新列表迁移：已经设好 ep_index_，不再重新选
        pin_next_ = false;
    } else {
        // 连不上/握手失败：给该地址一个冷却期，然后从最高优先级重新选；
        // 已建立过连接再断开的（relay 重启等）不计冷却，下次仍优先试它。
        if (!was_ready && ep_index_ < ep_fail_until_.size())
            ep_fail_until_[ep_index_] = (long long)hloop_now_ms(loop) + kEndpointFailCooldownMs;
        pickPreferred(was_ready ? "session lost" : "connect/auth failed");
    }
    logf_("[%s] tunnel closed, reconnect in %d ms", logTag(), backoff_ms);
    scheduleReconnect();
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
    w.u8(rtnp::PROTO_VERSION);
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
    if (io != NULL || endpoints.empty()) return;
    const TunnelEndpoint& ep = endpoints[ep_index_];
    hio_t* nio = hloop_create_tcp_client(loop, ep.host.c_str(), ep.port, connectCb, tunnelCloseCb);
    if (nio == NULL) { scheduleReconnect(); return; }
    // RTNP 帧按「16 字节头 + payload」两次写，开 Nagle 时小帧要等对端延时 ACK（40ms）
    tcp_nodelay(hio_fd(nio), 1);
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
    // 多地址时加 ±20% 抖动，避免所有设备同时重连打爆 relay
    if (endpoints.size() > 1) {
        jitter_seed_ = jitter_seed_ * 1103515245u + 12345u;
        int span = delay / 5;
        if (span > 0) delay += (int)(jitter_seed_ % (uint32_t)(2 * span + 1)) - span;
        if (delay < 100) delay = 100;
    }
    backoff_ms *= 2;
    if (backoff_ms > 30000) backoff_ms = 30000;
    htimer_t* t = htimer_add(loop, reconnectCb, (uint32_t)delay, 1);
    hevent_set_userdata(t, this);
}

// 按优先级选地址：从下标 0 开始，跳过认证失败与冷却中的地址。
// 全部不可用时保持当前地址，交给退避重试；已建立的连接不会被更高优先级抢占。
void TunnelLink::pickPreferred(const char* why) {
    if (endpoints.size() <= 1) return;
    size_t next = tunnel_pick_endpoint(ep_auth_bad_, ep_fail_until_, (long long)hloop_now_ms(loop));
    if (next >= endpoints.size() || next == ep_index_) return;
    logf_("[%s] prefer %s:%d -> %s:%d (%s)", logTag(),
          endpoints[ep_index_].host.c_str(), endpoints[ep_index_].port,
          endpoints[next].host.c_str(), endpoints[next].port, why);
    size_t from = ep_index_;
    ep_index_ = next;
    onEndpointChange(from, next);
}

bool TunnelLink::replaceEndpoints(const std::vector<TunnelEndpoint>& eps, bool allow_leave, bool& switching) {
    switching = false;
    if (eps.empty()) return false;
    size_t cur = eps.size();
    if (!endpoints.empty() && ep_index_ < endpoints.size()) {
        cur = tunnel_endpoint_index(eps, endpoints[ep_index_]);
    }
    if (cur == eps.size() && !allow_leave) return false;

    // 列表常常在每次重连时整份下发，故障地址的冷却要按 host:port 继承下来，
    // 否则每次重连都会把已知的故障地址重新超时一遍。
    std::vector<long long> fail_until(eps.size(), 0);
    for (size_t i = 0; i < eps.size(); ++i) {
        size_t old = tunnel_endpoint_index(endpoints, eps[i]);
        if (old < endpoints.size() && old < ep_fail_until_.size()) fail_until[i] = ep_fail_until_[old];
    }

    // 保留当前连接，只换后续故障切换用的列表；当前地址不在列表里才迁移。
    endpoints = eps;
    ep_index_ = (cur == eps.size()) ? 0 : cur;
    ep_auth_bad_.assign(endpoints.size(), 0);
    ep_fail_until_ = fail_until;
    auth_fatal_ = 0;
    if (cur == eps.size()) {
        switching = true;
        pin_next_ = true;
        backoff_ms = 1000;
        if (io != NULL) {
            // 调用方还要回 RELAY_LIST_ACK，200ms 后再断，避免 ACK 被丢掉
            htimer_t* t = htimer_add(loop, switchCb, 200, 1);
            hevent_set_userdata(t, this);
        } else {
            pin_next_ = false;
            onReconnect();
        }
    }
    return true;
}

void TunnelLink::appendEndpoints(const std::vector<TunnelEndpoint>& extra) {
    for (const auto& e : extra) {
        if (tunnel_endpoint_index(endpoints, e) != endpoints.size()) continue;
        endpoints.push_back(e);
        ep_auth_bad_.push_back(0);
        ep_fail_until_.push_back(0);
    }
}

void TunnelLink::init(hloop_t* loop) {
    this->loop = loop;
    if (endpoints.empty()) {
        TunnelEndpoint ep;
        ep.host = (host && host[0]) ? host : "127.0.0.1";
        ep.port = port > 0 ? port : 8443;
        endpoints.push_back(ep);
    }
    if (ep_index_ >= endpoints.size()) ep_index_ = 0;
    ep_auth_bad_.assign(endpoints.size(), 0);
    ep_fail_until_.assign(endpoints.size(), 0);
    if (jitter_seed_ == 0) {
        jitter_seed_ = (uint32_t)(hloop_now_ms(loop) ^ (uint64_t)(uintptr_t)this);
        if (jitter_seed_ == 0) jitter_seed_ = 1;
    }
    onReconnect();
    htimer_t* t = htimer_add(loop, pingCb, 15000, INFINITE);
    hevent_set_userdata(t, this);
}
