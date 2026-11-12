#include "shell_client.hpp"
#include "common/logging.hpp"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

void ShellClient::stdinCb(hio_t* io, void* buf, int n) {
    auto* self = (ShellClient*)hio_context(io);
    if (self) self->onStdin(io, buf, n);
}

void ShellClient::winchCb(hsignal_t* sig) {
    auto* self = (ShellClient*)hevent_userdata(sig);
    if (self) self->onWinch();
}

void ShellClient::enterRawMode() {
    if (tcgetattr(STDIN_FILENO, &saved_tio_) != 0) return;
    struct termios tio = saved_tio_;
    tio.c_lflag &= ~(ICANON | ECHO);
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &tio) == 0) raw_active_ = true;
}

void ShellClient::leaveRawMode() {
    if (raw_active_) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_tio_);
        raw_active_ = false;
    }
}

void ShellClient::onWinch() {
    if (sid_ == 0 || io == NULL || hio_is_closed(io)) return;
    struct winsize wsz;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &wsz) != 0) return;
    rtnp::Writer w;
    w.u16(wsz.ws_col);
    w.u16(wsz.ws_row);
    rtnp::send(io, rtnp::RESIZE, sid_, w.data, (uint16_t)w.len);
}

void ShellClient::sendInput(const void* data, size_t len) {
    if (io == NULL || hio_is_closed(io)) return;   // 连接已关闭，禁止再写（防 UAF）
    if (sid_ == 0) {
        pending_input_.append(data, len);
        return;
    }
    rtnp::send(io, rtnp::DATA, sid_, data, (uint16_t)len);
}

void ShellClient::onStdin(hio_t* io, void* buf, int n) {
    if (n <= 0) {
        // stdin EOF：停止监听（防止 EOF 反复触发空 DATA），但不退出——
        // 让对端输出继续回来，直到对端关闭或用户 Ctrl-]
        hio_read_stop(io);
        return;
    }
    const char* p = (const char*)buf;
    for (int i = 0; i < n; ++i) {
        if (p[i] == 0x1d) {   // Ctrl-]：退出并恢复终端
            hloop_stop(loop);
            return;
        }
    }
    sendInput(buf, (size_t)n);
}

void ShellClient::onReady() {
    if (io == NULL || hio_is_closed(io)) return;
    rtnp::Writer w;
    w.str(devid_.c_str());
    w.u16(0);
    rtnp::send(io, rtnp::OPEN, 1, w.data, (uint16_t)w.len, rtnp::OPEN_SHELL);
    sid_ = 1;
}

void ShellClient::onFrame(rtnp::Frame* f) {
    if (io == NULL || hio_is_closed(io)) return;
    switch (f->type) {
    case rtnp::OPEN_OK:
        ready_ = true;
        if (pending_input_.len > 0) {
            rtnp::send(io, rtnp::DATA, sid_, pending_input_.data, (uint16_t)pending_input_.len);
            pending_input_.clear();
        }
        onWinch();   // 建链后同步一次窗口尺寸
        return;
    case rtnp::OPEN_FAIL: {
        std::string msg = "open failed";
        if (f->payload && f->len > 4) msg.assign((const char*)f->payload + 4, f->len - 4);
        fprintf(stderr, "\r\nOPEN_FAIL: %s\r\n", msg.c_str());
        hloop_stop(loop);
        return;
    }
    case rtnp::DATA:
        if (f->len > 0) {
            fwrite(f->payload, 1, f->len, stdout);
            fflush(stdout);   // pipe 模式下 stdio 全缓冲，必须立即刷出
        }
        return;
    case rtnp::CLOSE:
    case rtnp::RESET:
        hloop_stop(loop);
        return;
    case rtnp::PING:
        rtnp::send(io, rtnp::PONG, 0, f->payload, f->len);
        return;
    default:
        return;
    }
}

void ShellClient::onLinkDown() {
    attempts_++;
    // 没拿到会话前可以在地址列表里换一个再试；已建立过会话则直接退出
    bool retry = !ready_ && attempts_ < (int)endpoints.size();
    if (!retry) hloop_stop(loop);
}

int ShellClient::run(const char* devid) {
    devid_ = devid;

    hloop_t* lp = hloop_new(0);
    init(lp);

    enterRawMode();
    hio_t* stdin_io = hio_get(lp, STDIN_FILENO);
    if (stdin_io) {
        hio_set_context(stdin_io, this);
        hio_setcb_read(stdin_io, stdinCb);
        hio_read(stdin_io);
    }
    hsignal_t* sig = hsignal_add(lp, winchCb, SIGWINCH);
    hevent_set_userdata(sig, this);

    hloop_run(lp);
    leaveRawMode();
    hloop_free(&lp);
    return 0;
}
