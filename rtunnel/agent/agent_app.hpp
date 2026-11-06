#pragma once
// C：设备侧 agent。连 B、认证、执行 OPEN、把字节透传给本地端口。
//
// 用法：rtunnel-agent --server HOST --port 8443 --devid PILE-000123 --allow 22 [--verbose]
// 长连接（HELLO/AUTH/心跳/重连/收帧）由 common/TunnelLink 承担。

#include "common/tunnel_link.hpp"

class AgentApp;

struct LocalStream {
    AgentApp*    app = nullptr;
    uint32_t     sid = 0;
    hio_t*       local = nullptr;
    int          connected = 0;
    int          local_eof = 0;
    int          remote_closed = 0;
    int          paused = 0;
    int          want_shutdown = 0;
    uint32_t     credit = 0;
    uint32_t     unacked = 0;
    rtnp::Buffer pending;
    htimer_t*    close_timer = nullptr;   // 半关闭 60s 超时
    LocalStream* next = nullptr;
};

class AgentApp : public TunnelLink {
public:
    int run(int argc, char** argv);

    char         devid[128] = {0};
    int          allow_ports[32] = {0};
    int          nallow = 0;
    int          allow_all = 0;
    LocalStream* streams = nullptr;

protected:
    uint8_t     role() const override { return rtnp::ROLE_AGENT; }
    const char* identity() const override { return devid; }
    const char* logTag() const override { return "agent"; }
    void        onFrame(rtnp::Frame* f) override;
    void        onLinkDown() override;

private:
    int          portAllowed(int port);
    LocalStream* streamFind(uint32_t sid);
    void         streamFree(LocalStream* s);
    void         onCloseTimeout(LocalStream* s);
    void         armCloseTimer(LocalStream* s);
    void         sendOpenFail(uint32_t sid, uint16_t code, const char* msg);
    void         flushPending(LocalStream* s);
    void         maybeGrantWindow(LocalStream* s);
    void         maybeShutdownWrite(LocalStream* s);
    void         onLocalWrite(LocalStream* s);
    void         onLocalClose(LocalStream* s);
    void         onLocalRead(LocalStream* s, void* buf, int readbytes);
    void         onLocalConnect(LocalStream* s);
    void         handleOpen(const rtnp::Frame* f);

    // libhv C 回调的薄封装（对象经 hio_context / hevent_userdata 取回）
    static void localReadCb(hio_t* io, void* buf, int n);
    static void localWriteCb(hio_t* io, const void* buf, int len);
    static void localCloseCb(hio_t* io);
    static void localConnectCb(hio_t* io);
    static void closeTimeoutCb(htimer_t* t);
};
