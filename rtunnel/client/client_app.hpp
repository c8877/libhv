#pragma once
// A：开发者侧客户端。连 B，并在本地起 SOCKS5 入口；CONNECT devid:port -> OPEN。
//
// 用法：rtunnel-client --server HOST --port 8443 --user alice --socks5 127.0.0.1:1080 [--verbose]
// 长连接（HELLO/AUTH/心跳/重连/收帧）由 common/TunnelLink 承担。

#include "common/tunnel_link.hpp"

enum ClientStreamState {
    CS_HANDSHAKE = 0,
    CS_REQUEST,
    CS_OPENING,
    CS_OPEN,
    CS_CLOSED,
};

class ClientApp;

struct ClientStream {
    ClientApp*    app = nullptr;
    uint32_t      sid = 0;
    hio_t*        local = nullptr;
    int           state = CS_HANDSHAKE;
    int           is_forward = 0;   // 端口映射会话（跳过 SOCKS5 应答）
    int           local_eof = 0;
    int           remote_closed = 0;
    int           paused = 0;
    int           want_shutdown = 0;
    uint32_t      credit = 0;
    uint32_t      unacked = 0;      // 已写入 local 但尚未回 WINDOW 的字节数
    rtnp::Buffer  hs;               // SOCKS5 握手缓冲 / 未发送数据
    rtnp::Buffer  pending;          // 受 credit 限制的待发数据
    htimer_t*     open_timer = nullptr;
    htimer_t*     close_timer = nullptr;  // 半关闭 60s 超时
    ClientStream* next = nullptr;
};

class ClientApp : public TunnelLink {
public:
    int run(int argc, char** argv);

    char        user[128] = {0};
    char        socks_host[128] = "127.0.0.1";
    char        socks_uds[256] = {0};
    struct Forward {
        char    listen_host[64] = {0};
        int     listen_port = 0;
        char    devid[128] = {0};
        int     target_port = 0;
        hio_t*  listen_io = nullptr;
    } fwd[8];
    int         nfwd = 0;
    int         socks_port = 1080;
    int         allow_shell = 0;            // 允许 B 通知本机按需拉起 agent（默认关）
    char        agent_path[512] = {0};      // rtunnel-agent 路径（默认同目录）
    int         agent_idle = 60;            // 子 agent 空闲多少秒自退
    struct AgentChild {
        char  devid[128] = {0};
        pid_t pid = -1;
    } children[8];
    ClientStream* streams = nullptr;
    uint32_t    next_sid = 0;

protected:
    uint8_t     role() const override { return rtnp::ROLE_CLIENT; }
    const char* identity() const override { return user; }
    const char* logTag() const override { return "client"; }
    void        onFrame(rtnp::Frame* f) override;
    void        onLinkDown() override;

private:
    ClientStream* streamFind(uint32_t sid);
    void          streamFree(ClientStream* s);
    void          onCloseTimeout(ClientStream* s);
    void          armCloseTimer(ClientStream* s);
    void          socksReply(hio_t* io, uint8_t rep);
    int           validDevid(const char* s);
    void          flushPending(ClientStream* s);
    void          onOpenTimeout(ClientStream* s);
    void          armOpenTimer(ClientStream* s);
    void          trySocks(ClientStream* s);
    void          onLocalRead(ClientStream* s, void* buf, int readbytes);
    void          maybeShutdownWrite(ClientStream* s);
    void          maybeGrantWindow(ClientStream* s);
    void          onLocalWrite(ClientStream* s);
    void          onLocalClose(ClientStream* s);
    ClientStream* streamNew(hio_t* io);
    void          onLocalAccept(hio_t* io);
    void          onFwdAccept(hio_t* io);
    void          onSpawnAgent(const rtnp::Frame* f);
    void          spawnAgent(const char* devid);

    // libhv C 回调的薄封装（对象经 hio_context / hevent_userdata 取回）
    static void localReadCb(hio_t* io, void* buf, int n);
    static void localWriteCb(hio_t* io, const void* buf, int len);
    static void localCloseCb(hio_t* io);
    static void acceptCb(hio_t* io);
    static void fwdAcceptCb(hio_t* io);
    static void closeTimeoutCb(htimer_t* t);
    static void openTimeoutCb(htimer_t* t);
};
