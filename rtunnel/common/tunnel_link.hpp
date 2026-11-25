#pragma once
// agent/client 到 B（relay）的一条长连接：
//   HELLO/AUTH 状态机、15s 心跳（连续 3 个 PING 无 PONG 判死）、
//   握手 5s / 认证 10s 超时、指数退避重连、增量收帧。
// 子类实现 role/identity/logTag，并处理 READY 后的会话级帧与断线清理。

#include "rtnp/codec.hpp"
#include "common/endpoint.hpp"

#include <string>
#include <vector>

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
    std::vector<TunnelEndpoint> endpoints;   // 非空时优先于 host/port；下标即优先级（0 最高）
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
    // relay 在 HELLO_ACK 尾部报告的语义版本；0 = 旧 relay 未上报
    int relayProto() const { return relay_proto_; }
    size_t endpointIndex() const { return ep_index_; }
    const TunnelEndpoint& currentEndpoint() const { return endpoints[ep_index_]; }

    // 用 relay 下发的列表替换端点（只在 loop 线程调用）。
    // 当前地址不在新列表且 allow_leave=false 时返回 false，调用方应拒绝本次下发；
    // switching=true 表示需要断开并重连到新列表的第 0 个地址（延迟 200ms 断开，
    // 给调用方留出回 ACK 的时间）。
    bool replaceEndpoints(const std::vector<TunnelEndpoint>& eps, bool allow_leave, bool& switching);
    // 在列表末尾追加兜底地址（不改变当前下标，不触发切换），已存在的跳过
    void appendEndpoints(const std::vector<TunnelEndpoint>& extra);

protected:
    virtual uint8_t     role() const = 0;      // rtnp::ROLE_AGENT / ROLE_CLIENT
    virtual const char* identity() const = 0;  // AUTH 载荷：devid / user
    virtual const char* logTag() const = 0;    // 日志前缀："agent" / "client"
    virtual void        onReady() {}           // AUTH_OK 之后
    // AUTH_OK 携带扩展信息（账号 Token）。默认忽略；带 token 的客户端用它回填用户名
    virtual void        onAuthInfo(const char* username) { (void)username; }
    virtual void        onFrame(rtnp::Frame* f) = 0;  // READY 后的会话级帧
    virtual void        onLinkDown() = 0;      // 断线：子类清理自己的流表
    virtual bool        autoReconnect() const { return true; }  // shell 等一次性会话返回 false
    // 已切换到新 relay（from -> to）。client 用它处理随需 agent 的绑定关系
    virtual void        onEndpointChange(size_t from, size_t to) { (void)from; (void)to; }

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
    // 按优先级选下一个地址（下标 0 最高）：跳过认证失败与冷却中的地址，
    // 已建立的连接不主动抢占，只在断线/重连时重新选择。
    void pickPreferred(const char* why);

    rtnp::Decoder in_;
    htimer_t*     hs_timer_ = nullptr;   // 握手/认证阶段超时
    int           ping_unacked_ = 0;     // 连续未获 PONG 的 PING 数
    int           auth_fatal_ = 0;       // 认证被永久拒绝：不再重连
    int           relay_proto_ = 0;      // HELLO_ACK 上报的 relay 语义版本
    size_t        ep_index_ = 0;         // 当前 relay 下标
    std::vector<char> ep_auth_bad_;      // 各地址是否被凭证拒绝
    std::vector<long long> ep_fail_until_;  // 各地址的失败冷却截止（ms）
    bool          pin_next_ = false;     // 下次断开不重新选（按新列表迁移）
    uint32_t      jitter_seed_ = 0;

    // libhv C 回调的薄封装（对象经 hio_context / hevent_userdata 取回）
    static void tunnelReadCb(hio_t* io, void* buf, int n);
    static void tunnelCloseCb(hio_t* io);
    static void connectCb(hio_t* io);
    static void pingCb(htimer_t* t);
    static void reconnectCb(htimer_t* t);
    static void hsTimeoutCb(htimer_t* t);
    static void switchCb(htimer_t* t);
};
