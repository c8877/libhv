#pragma once
// agent/client 到 B（relay）的一条长连接：
//   HELLO/AUTH 状态机、15s 心跳（连续 3 个 PING 无 PONG 判死）、
//   握手 5s / 认证 10s 超时、指数退避重连、增量收帧。
// 子类实现 role/identity/logTag，并处理 READY 后的会话级帧与断线清理。

#include "rtnp/codec.hpp"

class TunnelLink {
public:
    TunnelLink() = default;
    virtual ~TunnelLink() = default;
    TunnelLink(const TunnelLink&) = delete;
    TunnelLink& operator=(const TunnelLink&) = delete;

    // 建 loop 后调用：启动首次连接 + 心跳定时器（不跑 hloop_run）
    void init(hloop_t* loop);

    // 配置（init 前填）
    const char* host = "127.0.0.1";
    int         port = 8443;
    char        tls_ca[256] = {0};
    char        tls_cert[256] = {0};
    char        tls_key[256] = {0};
    char        tls_sni[256] = {0};
    char        auth_token[256] = {0};   // client 侧凭证（agent 留空）
    int         verbose = 0;

    // 状态（子类直接读写，保持与原 C 代码一致的访问方式）
    hloop_t*    loop = nullptr;
    hio_t*      io = nullptr;
    int         state = 0;      // 0=connecting 1=hello 2=authed(READY)
    int         backoff_ms = 1000;

    bool ready() const { return state == 2 && io && !hio_is_closed(io); }

protected:
    virtual uint8_t     role() const = 0;      // rtnp::ROLE_AGENT / ROLE_CLIENT
    virtual const char* identity() const = 0;  // AUTH 载荷：devid / user
    virtual const char* logTag() const = 0;    // 日志前缀："agent" / "client"
    virtual void        onReady() {}           // AUTH_OK 之后
    virtual void        onFrame(rtnp::Frame* f) = 0;  // READY 后的会话级帧
    virtual void        onLinkDown() = 0;      // 断线：子类清理自己的流表
    virtual bool        autoReconnect() const { return true; }  // shell 等一次性会话返回 false

private:
    void handleFrame(rtnp::Frame* f);
    void onTunnelRead(void* buf, int readbytes);
    void onTunnelClose();
    void onHsTimeout();
    void armHsTimer(uint32_t ms);
    void onConnect(hio_t* io);
    void onPing();
    void onReconnect();
    void scheduleReconnect();

    rtnp::Decoder in_;
    htimer_t*     hs_timer_ = nullptr;   // 握手/认证阶段超时
    int           ping_unacked_ = 0;     // 连续未获 PONG 的 PING 数

    // libhv C 回调的薄封装（对象经 hio_context / hevent_userdata 取回）
    static void tunnelReadCb(hio_t* io, void* buf, int n);
    static void tunnelCloseCb(hio_t* io);
    static void connectCb(hio_t* io);
    static void pingCb(htimer_t* t);
    static void reconnectCb(htimer_t* t);
    static void hsTimeoutCb(htimer_t* t);
};
