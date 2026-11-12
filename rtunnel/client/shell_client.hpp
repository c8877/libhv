#pragma once
// A 端 CLI 免密 shell：rtunnel-client --server B --user U shell <devid>
// 复用 TunnelLink 连 relay，OPEN_SHELL 后把设备 shell 桥到本地终端
// （raw mode stdin/stdout + SIGWINCH → RESIZE 帧）。Ctrl-] 退出。

#include "common/tunnel_link.hpp"
#include <termios.h>
#include <string>

class ShellClient : public TunnelLink {
public:
    int run(const char* devid);

    char user[128] = {0};   // 登录 relay 的用户（identity() 用它）

protected:
    uint8_t     role() const override { return rtnp::ROLE_CLIENT; }
    const char* identity() const override { return user; }
    const char* logTag() const override { return "shell"; }
    void        onReady() override;
    void        onFrame(rtnp::Frame* f) override;
    void        onLinkDown() override;
    bool        autoReconnect() const override { return !ready_ && attempts_ < (int)endpoints.size(); }

private:
    std::string devid_;
    uint32_t    sid_ = 0;
    bool        ready_ = false;
    int         attempts_ = 0;   // 已尝试的 relay 数
    rtnp::Buffer pending_input_;
    struct termios saved_tio_;
    bool        raw_active_ = false;

    void        enterRawMode();
    void        leaveRawMode();
    void        onStdin(hio_t* io, void* buf, int n);
    void        onWinch();
    void        sendInput(const void* data, size_t len);

    static void stdinCb(hio_t* io, void* buf, int n);
    static void winchCb(hsignal_t* sig);
};
