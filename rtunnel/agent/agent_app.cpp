#include "agent_app.hpp"
#include "common/gzip_util.hpp"
#include "hversion.h"
#include "cpputil/json.hpp"
#include "common/logging.hpp"
#include "common/shell_spawn.hpp"
#include "fs_session.hpp"
#include "net_service.hpp"
#include "proc_service.hpp"
#include "storage_service.hpp"
#include "ota_pkg.hpp"
#ifdef RTUNNEL_AGENT_WEB
#include "agent_web.hpp"
#endif

#include <stdio.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/sysinfo.h>
#include <sys/reboot.h>
#include <shadow.h>
#include <crypt.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>
#include <fstream>
#include <chrono>
#include <set>
#include <sstream>
#include <thread>
#include <vector>
#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#include "openssl/x509.h"
#include "openssl/pem.h"
#include "openssl/bio.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pty.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

// ---------------------------------------------------------------- 回调薄封装

namespace {

uint64_t wallMs(); // 定义在文件后部的匿名命名空间

struct WebKick {
    hevent_t ev;
    AgentApp* app = nullptr;
};

// loop 线程直接回一帧 JSON（参数错误、busy 等小响应）。
void sendJsonFrame(hio_t* io, uint8_t type, uint32_t sid, const nlohmann::json& j) {
    std::string body = j.dump();
    rtnp::Writer w;
    w.str(body.c_str());
    rtnp::send(io, type, sid, w.data, (uint16_t)w.len);
}

// CONFIG_SET 的参数校验失败统一回 CONFIG_ACK
void ackConfigError(hio_t* io, uint32_t sid, const char* msg) {
    nlohmann::json e;
    e["ok"] = false;
    e["error"] = msg;
    sendJsonFrame(io, rtnp::CONFIG_ACK, sid, e);
}

// 把 FsSession 的输出装进 RTNP DATA：u8 kind + WebSocket 原始消息
class FsTunnelSink : public rtfs::Sink {
public:
    FsTunnelSink(AgentApp* app, uint32_t sid) : app_(app), sid_(sid) {}
    void send(bool binary, const std::string& msg) override {
        if (!app_->ready()) return;
        std::string p;
        p.reserve(msg.size() + 1);
        p.push_back(binary ? (char)1 : (char)0);
        p.append(msg);
        rtnp::send(app_->io, rtnp::DATA, sid_, p.data(), (uint16_t)p.size());
    }

private:
    AgentApp* app_;
    uint32_t sid_;
};

// 服务管理流：文本 JSON 帧，同样加 1 字节 kind=0 复用 DATA 载荷
class SvcTunnelSink : public rtvs::Sink {
public:
    SvcTunnelSink(AgentApp* app, uint32_t sid) : app_(app), sid_(sid) {}
    void send(const std::string& msg) override {
        if (!app_->ready()) return;
        std::string p;
        p.reserve(msg.size() + 1);
        p.push_back((char)0);
        p.append(msg);
        rtnp::send(app_->io, rtnp::DATA, sid_, p.data(), (uint16_t)p.size());
    }

private:
    AgentApp* app_;
    uint32_t sid_;
};

} // namespace

void AgentApp::localReadCb(hio_t* io, void* buf, int n) {
    auto* s = (LocalStream*)hio_context(io);
    if (s) s->app->onLocalRead(s, buf, n);
}

void AgentApp::localWriteCb(hio_t* io, const void* buf, int len) {
    (void)buf;
    (void)len;
    auto* s = (LocalStream*)hio_context(io);
    if (s) s->app->onLocalWrite(s);
}

void AgentApp::localCloseCb(hio_t* io) {
    auto* s = (LocalStream*)hio_context(io);
    if (s) s->app->onLocalClose(s);
}

void AgentApp::localConnectCb(hio_t* io) {
    auto* s = (LocalStream*)hio_context(io);
    if (s) s->app->onLocalConnect(s);
}

void AgentApp::closeTimeoutCb(htimer_t* t) {
    auto* s = (LocalStream*)hevent_userdata(t);
    if (s) s->app->onCloseTimeout(s);
}

// ---------------------------------------------------------------- 会话管理

int AgentApp::portAllowed(int port) {
    if (allow_all) return 1;
    for (int i = 0; i < 32 && allow_ports[i] > 0; ++i) {
        if (allow_ports[i] == port) return 1;
    }
    return 0;
}

LocalStream* AgentApp::streamFind(uint32_t sid) {
    for (LocalStream* s = streams; s; s = s->next) {
        if (s->sid == sid) return s;
    }
    return NULL;
}

void AgentApp::streamFree(LocalStream* s) {
    if (s == NULL || s->freed) return;
    s->freed = 1;
    LocalStream** pp = &streams;
    while (*pp) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
        pp = &(*pp)->next;
    }
    if (s->close_timer) {
        htimer_del(s->close_timer);
        s->close_timer = NULL;
    }
    killShellChild(s);
    if (s->fs_session) {
        s->fs_session->onClose();
        delete s->fs_session;
        s->fs_session = NULL;
    }
    if (s->svc_session) {
        s->svc_session->onClose();
        delete s->svc_session;
        s->svc_session = NULL;
    }
    if (s->local && !hio_is_closed(s->local)) {
        hio_set_context(s->local, NULL);
        hio_close(s->local);
    }
    delete s;
}

// 半关闭 60s 未收到对端 CLOSE：RESET(CLOSE_TIMEOUT) 并回收（设计 §7.5）
void AgentApp::onCloseTimeout(LocalStream* s) {
    s->close_timer = NULL;
    if (io && state == 2 && !hio_is_closed(io)) {
        rtnp::Writer w;
        w.u16(rtnp::CLOSE_TIMEOUT);
        rtnp::send(io, rtnp::RESET, s->sid, w.data, (uint16_t)w.len);
    }
    streamFree(s);
}

void AgentApp::armCloseTimer(LocalStream* s) {
    if (s->close_timer) return;
    s->close_timer = htimer_add(loop, closeTimeoutCb, 60000, 1);
    hevent_set_userdata(s->close_timer, s);
}

void AgentApp::sendOpenFail(uint32_t sid, uint16_t code, const char* msg) {
    rtnp::Writer w;
    w.u16(code);
    w.str(msg ? msg : "");
    rtnp::send(io, rtnp::OPEN_FAIL, sid, w.data, (uint16_t)w.len);
}

static void teeToMonitors(AgentApp* app, LocalStream* s, const char* p, size_t n) {
    for (LocalStream* m = app->streams; m; m = m->next) {
        if (!m->is_monitor || m->monitor_of != s->sid) continue;
        if (m->credit == 0) continue;
        size_t k = n < m->credit ? n : m->credit;
        if (rtnp::send(app->io, rtnp::DATA, m->sid, (const char*)p, (uint16_t)k) == 0) m->credit -= (uint32_t)k;
    }
}

void AgentApp::flushPending(LocalStream* s) {
    // remote_closed 只表示 A→C 方向 EOF，不影响 C→A 的数据回传
    if (!s->connected) return;
    while (s->pending.len > 0 && s->credit > 0) {
        size_t n = s->pending.len;
        if (n > s->credit) n = s->credit;
        if (n > rtnp::MAX_PAYLOAD) n = rtnp::MAX_PAYLOAD;
        if (rtnp::send(io, rtnp::DATA, s->sid, s->pending.data, (uint16_t)n) != 0) {
            return;
        }
        teeToMonitors(this, s, (const char*)s->pending.data, n);
        s->credit -= (uint32_t)n;
        s->pending.consume(n);
    }
    if (s->pending.len > 0) {
        if (!s->paused) {
            hio_read_stop(s->local);
            s->paused = 1;
        }
    }
    else if (s->paused) {
        s->paused = 0;
        hio_read(s->local);
    }
}

void AgentApp::maybeGrantWindow(LocalStream* s) {
    if (s->local == NULL || hio_is_closed(s->local) || s->unacked == 0) return;
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

void AgentApp::maybeShutdownWrite(LocalStream* s) {
    if (!s->want_shutdown || s->local == NULL || hio_is_closed(s->local)) return;
    if (hio_write_is_complete(s->local)) {
        shutdown(hio_fd(s->local), SHUT_WR);
        s->want_shutdown = 0;
        hio_setcb_write(s->local, NULL);
    }
}

void AgentApp::onLocalWrite(LocalStream* s) {
    maybeShutdownWrite(s);
    maybeGrantWindow(s);
}

void AgentApp::onLocalClose(LocalStream* s) {
    if (verbose) logf_("[agent] local close sid=%u", s->sid);
    killShellChild(s);
    if (!s->connected) {
        // 本地 connect 失败
        sendOpenFail(s->sid, rtnp::TARGET_REFUSED, "connect refused");
        s->local = NULL;
        streamFree(s);
        return;
    }
    if (!s->local_eof) {
        s->local_eof = 1;
        if (state == 2 && io && !hio_is_closed(io)) {
            rtnp::send(io, rtnp::CLOSE, s->sid, NULL, 0);
        }
    }
    s->local = NULL;
    if (s->remote_closed) {
        streamFree(s);
    }
    else {
        armCloseTimer(s);
    }
}

void AgentApp::onLocalRead(LocalStream* s, void* buf, int readbytes) {
    if (verbose) logf_("[agent] local read sid=%u len=%d", s->sid, readbytes);
    if (readbytes <= 0) {
        // pty 的 io_type 不是 socket，libhv 不会自动 close；read==0 视为 EOF
        if (s->local && !hio_is_closed(s->local)) hio_close(s->local);
        return;
    }
    // 注意：remote_closed 只表示 A→C 方向已 EOF，C→A 方向仍要把数据发回去。
    if (s->pending.append(buf, (size_t)readbytes) != 0) {
        rtnp::send(io, rtnp::RESET, s->sid, NULL, 0);
        streamFree(s);
        return;
    }
    flushPending(s);
}

void AgentApp::onLocalConnect(LocalStream* s) {
    int err = hio_error(s->local);
    if (err != 0) {
        sendOpenFail(s->sid, rtnp::TARGET_REFUSED, "connect failed");
        s->local = NULL;
        streamFree(s);
        return;
    }
    s->connected = 1;
    rtnp::Writer w;
    w.str("127.0.0.1");
    w.u16(0);
    rtnp::send(io, rtnp::OPEN_OK, s->sid, w.data, (uint16_t)w.len);
    hio_setcb_read(s->local, localReadCb);
    hio_read(s->local);
    if (verbose) logf_("[agent] stream %u -> local fd=%d ok", s->sid, hio_fd(s->local));
}

void AgentApp::handleOpen(const rtnp::Frame* f) {
    if (verbose) logf_("[agent] OPEN sid=%u flags=0x%04x", f->sid, f->flags);
    // OPEN_FS：设备文件管理协议，payload = u8 want_rw + u8 trusted
    if (f->flags & rtnp::OPEN_FS) {
        if (!fs_.ready()) {
            sendOpenFail(f->sid, rtnp::TARGET_NOT_ALLOWED, "file service disabled");
            return;
        }
        if (streamFind(f->sid) != NULL) {
            sendOpenFail(f->sid, rtnp::PROTOCOL_ERROR, "sid in use");
            return;
        }
        bool want_rw = f->len >= 1 && f->payload[0] != 0;
        bool trusted = f->len >= 2 && f->payload[1] != 0;
        auto* s = new LocalStream{};
        s->app = this;
        s->sid = f->sid;
        s->credit = rtnp::DEFAULT_CREDIT;
        s->connected = 1;
        s->is_fs = 1;
        had_stream = 1;
        s->opened_ms = wallMs();
        s->next = streams;
        streams = s;
        auto* sink = new FsTunnelSink(this, f->sid);
        s->fs_session = new rtfs::Session(&fs_, sink, devid, trusted, want_rw);
        s->fs_session->setCredit(rtnp::DEFAULT_CREDIT);
        rtnp::Writer w;
        w.str("fs");
        w.u16(0);
        rtnp::send(io, rtnp::OPEN_OK, f->sid, w.data, (uint16_t)w.len);
        if (verbose) logf_("[agent] fs stream %u rw=%d trusted=%d", f->sid, (int)want_rw, (int)trusted);
        return;
    }
    // OPEN_SVC：设备服务管理协议，payload = u8 trusted
    if (f->flags & rtnp::OPEN_SVC) {
        if (!svc_.ready()) {
            sendOpenFail(f->sid, rtnp::TARGET_NOT_ALLOWED, svc_enabled == 0 ? "service manager disabled" : "systemd not available");
            return;
        }
        if (streamFind(f->sid) != NULL) {
            sendOpenFail(f->sid, rtnp::PROTOCOL_ERROR, "sid in use");
            return;
        }
        bool trusted = f->len >= 1 && f->payload[0] != 0;
        auto* s = new LocalStream{};
        s->app = this;
        s->sid = f->sid;
        s->credit = rtnp::DEFAULT_CREDIT;
        s->connected = 1;
        s->is_svc = 1;
        had_stream = 1;
        s->opened_ms = wallMs();
        s->next = streams;
        streams = s;
        s->svc_session = new rtvs::Session(&svc_, new SvcTunnelSink(this, f->sid), loop, devid, trusted, true);
        rtnp::Writer w;
        w.str("svc");
        w.u16(0);
        rtnp::send(io, rtnp::OPEN_OK, f->sid, w.data, (uint16_t)w.len);
        if (verbose) logf_("[agent] svc stream %u trusted=%d", f->sid, (int)trusted);
        return;
    }
    // OPEN_SHELL：设备侧直接起 shell（forkpty），独立于端口白名单，须 --allow-shell 显式开启
    if (f->flags & rtnp::OPEN_TTY) {
        rtnp::Reader tr(f->payload, f->len);
        char tdev[128] = {0};
        char path[256] = {0};
        tr.str(tdev, sizeof(tdev));
        tr.str(path, sizeof(path));
        uint32_t baud = tr.u32();
        uint8_t db = tr.u8(), par = tr.u8(), stop = tr.u8();
        if (tr.err || path[0] != '/') {
            sendOpenFail(f->sid, rtnp::PROTOCOL_ERROR, "bad tty");
            return;
        }
        if (verbose) logf_("[agent] tty req path=%s allow=%s baud=%u", path, allow_tty, baud);
        bool allowed = false;
        std::string list = allow_tty;
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t comma = list.find(',', pos);
            std::string one = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!one.empty() && one == path) {
                allowed = true;
                break;
            }
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        if (verbose) logf_("[agent] tty check %s static=%d cfgs=%zu", path, (int)allowed, tty_cfgs_.size());
        auto ci = tty_cfgs_.find(path);
        if (!allowed) {
            if (ci != tty_cfgs_.end() && ci->second.enabled) allowed = true;
        }
        if (!allowed) {
            sendOpenFail(f->sid, rtnp::TARGET_NOT_ALLOWED, "tty not allowed");
            return;
        }
        if (streamFind(f->sid) != NULL) {
            sendOpenFail(f->sid, rtnp::PROTOCOL_ERROR, "sid in use");
            return;
        }
        bool want_monitor = (f->flags & rtnp::OPEN_TTY_MONITOR) != 0;
        bool want_takeover = (f->flags & rtnp::OPEN_TTY_TAKEOVER) != 0;
        if (want_monitor) {
            LocalStream* target = nullptr;
            for (LocalStream* it = streams; it; it = it->next) {
                if (!it->is_monitor && it->tty_path == path) {
                    target = it;
                    break;
                }
            }
            if (!target) {
                sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "tty not open");
                return;
            }
            auto* m = new LocalStream{};
            m->app = this;
            m->sid = f->sid;
            m->credit = rtnp::DEFAULT_CREDIT;
            m->connected = 1;
            m->is_monitor = 1;
            m->monitor_of = target->sid;
            m->tty_path = path;
            m->opened_ms = wallMs();
            m->next = streams;
            streams = m;
            rtnp::Writer w;
            w.str("monitor");
            w.u16(0);
            rtnp::send(io, rtnp::OPEN_OK, f->sid, w.data, (uint16_t)w.len);
            return;
        }
        if (want_takeover) {
            LocalStream* it = streams;
            while (it) {
                LocalStream* nx = it->next;
                if (!it->is_monitor && it->tty_path == path) {
                    rtnp::send(io, rtnp::CLOSE, it->sid, NULL, 0);
                    if (it->local) hio_close(it->local);
                    streamFree(it);
                }
                it = nx;
            }
        }
        int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
        if (fd < 0) {
            sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "open tty failed");
            return;
        }
        struct termios tio {};
        if (tcgetattr(fd, &tio) != 0) {
            close(fd);
            sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "termios failed");
            return;
        }
        cfmakeraw(&tio);
        speed_t sp = B115200;
        switch (baud) {
        case 9600: sp = B9600; break;
        case 19200: sp = B19200; break;
        case 38400: sp = B38400; break;
        case 57600: sp = B57600; break;
        case 115200: sp = B115200; break;
        case 230400: sp = B230400; break;
        case 460800: sp = B460800; break;
        case 921600: sp = B921600; break;
        default: sp = B115200; break;
        }
        cfsetispeed(&tio, sp);
        cfsetospeed(&tio, sp);
        tio.c_cflag &= ~CSIZE;
        tio.c_cflag |= (db == 5 ? CS5 : db == 6 ? CS6 : db == 7 ? CS7 : CS8);
        if (par == 1) {
            tio.c_cflag |= PARENB;
            tio.c_cflag &= ~PARODD;
        }
        else if (par == 2) {
            tio.c_cflag |= PARENB | PARODD;
        }
        else
            tio.c_cflag &= ~PARENB;
        if (stop == 2)
            tio.c_cflag |= CSTOPB;
        else
            tio.c_cflag &= ~CSTOPB;
        tio.c_cflag |= CLOCAL | CREAD;
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;
        if (tcsetattr(fd, TCSANOW, &tio) != 0) {
            close(fd);
            sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "termios set failed");
            return;
        }

        auto* s = new LocalStream{};
        s->app = this;
        s->sid = f->sid;
        s->credit = rtnp::DEFAULT_CREDIT;
        had_stream = 1;
        s->opened_ms = wallMs();
        s->next = streams;
        streams = s;
        hio_t* io = hio_get(loop, fd);
        if (!io) {
            close(fd);
            streamFree(s);
            sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "hio failed");
            return;
        }
        s->local = io;
        s->connected = 1;
        s->tty_path = path;
        hio_set_context(io, s);
        hio_setcb_read(io, localReadCb);
        hio_setcb_close(io, localCloseCb);
        hio_set_max_write_bufsize(io, rtnp::CONN_INFLIGHT + rtnp::MAX_PAYLOAD);
        rtnp::Writer w;
        w.str("tty");
        w.u16(0);
        rtnp::send(io == NULL ? NULL : this->io, rtnp::OPEN_OK, f->sid, w.data, (uint16_t)w.len);
        hio_read(io);
        if (verbose) logf_("[agent] tty %s opened fd=%d", path, fd);
        return;
    }
    if (f->flags & rtnp::OPEN_SHELL) {
        if (!allow_shell) {
            sendOpenFail(f->sid, rtnp::TARGET_NOT_ALLOWED, "shell disabled");
            return;
        }
        auto* s = new LocalStream{};
        s->app = this;
        s->sid = f->sid;
        s->credit = rtnp::DEFAULT_CREDIT;
        s->is_shell = 1;
        had_stream = 1;
        s->opened_ms = wallMs();
        s->next = streams;
        streams = s;
        if (openShellStream(s) != 0) {
            sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "shell spawn failed");
            streamFree(s);
        }
        return;
    }

    rtnp::Reader r(f->payload, f->len);
    uint16_t port = r.u16();
    if (r.err || port == 0) {
        sendOpenFail(f->sid, rtnp::PROTOCOL_ERROR, "bad open");
        return;
    }
    if (!portAllowed(port)) {
        sendOpenFail(f->sid, rtnp::TARGET_NOT_ALLOWED, "port not allowed");
        return;
    }
    if (streamFind(f->sid) != NULL) {
        sendOpenFail(f->sid, rtnp::PROTOCOL_ERROR, "sid in use");
        return;
    }

    auto* s = new LocalStream{};
    s->app = this;
    s->sid = f->sid;
    s->target_port = port;
    had_stream = 1;
    s->credit = rtnp::DEFAULT_CREDIT;
    s->opened_ms = wallMs();
    s->next = streams;
    streams = s;

    hio_t* io = hio_create_socket(loop, "127.0.0.1", port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
    if (io == NULL) {
        sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "socket failed");
        streamFree(s);
        return;
    }
    s->local = io;
    hio_set_context(io, s);
    hio_setcb_connect(io, localConnectCb);
    hio_setcb_close(io, localCloseCb);
    hio_set_max_write_bufsize(io, rtnp::CONN_INFLIGHT + rtnp::MAX_PAYLOAD);
    if (hio_connect(io) != 0) {
        sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "connect failed");
        s->local = NULL;
        streamFree(s);
    }
}

// OPEN_SHELL：forkpty 起交互 shell，pty 挂进事件循环，复用现有 DATA/流控逻辑
int AgentApp::openShellStream(LocalStream* s) {
    pid_t pid = -1;
    hio_t* pty = shellSpawnPty(loop, s, localReadCb, localCloseCb, &pid);
    if (pty == NULL) return -1;
    s->child_pid = pid;
    s->local = pty;
    s->connected = 1;
    rtnp::Writer w;
    w.str("127.0.0.1");
    w.u16(0);
    rtnp::send(io, rtnp::OPEN_OK, s->sid, w.data, (uint16_t)w.len);
    if (verbose) logf_("[agent] shell stream %u spawned pid=%d", s->sid, (int)pid);
    return 0;
}

void AgentApp::killShellChild(LocalStream* s) {
    if (s->child_pid > 0) {
        kill(s->child_pid, SIGHUP);
        waitpid(s->child_pid, NULL, WNOHANG);
        s->child_pid = -1;
    }
}

// READY 后的会话级帧（连接级 HELLO/AUTH/PING 由 TunnelLink 处理）
static uint64_t agentMonotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void agentSampleCpu(AgentApp* app) {
    std::ifstream f("/proc/stat");
    if (!f) return;
    std::string line;
    std::vector<unsigned long long> tot, idl;
    while (std::getline(f, line)) {
        if (line.rfind("cpu", 0) != 0) break;
        unsigned long long v[8] = {0};
        int n = sscanf(line.c_str(), "%*s %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        if (n < 4) continue;
        unsigned long long t = 0;
        for (int i = 0; i < 8; ++i) t += v[i];
        tot.push_back(t);
        idl.push_back(v[3] + v[4]);
    }
    if (tot.empty()) return;
    if (app->cpu_last_tot.size() == tot.size()) {
        app->cpu_usage = 0;
        app->cpu_cores.clear();
        for (size_t i = 0; i < tot.size(); ++i) {
            unsigned long long dt = tot[i] - app->cpu_last_tot[i];
            unsigned long long di = idl[i] - app->cpu_last_idle[i];
            double u = dt > 0 ? 100.0 * (double)(dt - di) / (double)dt : 0.0;
            if (i == 0)
                app->cpu_usage = u;
            else
                app->cpu_cores.push_back(u);
        }
    }
    app->cpu_last_tot = tot;
    app->cpu_last_idle = idl;
}

void AgentApp::loadConfig() {
    if (config_file.empty()) return;
    FILE* fp = fopen(config_file.c_str(), "r");
    if (!fp) {
        logf_("[agent] config file %s not found, using command line options", config_file.c_str());
        return;
    }
    std::string js;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) js.append(buf, n);
    fclose(fp);
    auto j = nlohmann::json::parse(js, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        logf_("[agent] config file %s is not valid json, using command line options", config_file.c_str());
        return;
    }
    config_loaded = true;
    if (j.contains("allow") && j["allow"].is_array()) {
        int idx = 0;
        for (auto& v : j["allow"]) {
            if (!v.is_number_integer()) continue;
            int p = v.get<int>();
            if (p > 0 && p < 65536 && idx < 31) allow_ports[idx++] = p;
        }
        allow_ports[idx] = 0;
        nallow = idx;
    }
    if (j.contains("shell") && j["shell"].is_boolean()) allow_shell = j["shell"].get<bool>() ? 1 : 0;
    if (j.contains("ota") && j["ota"].is_boolean()) allow_ota = j["ota"].get<bool>() ? 1 : 0;
    if (j.contains("reboot") && j["reboot"].is_boolean()) allow_reboot = j["reboot"].get<bool>() ? 1 : 0;
    if (j.contains("poweroff") && j["poweroff"].is_boolean()) allow_poweroff = j["poweroff"].get<bool>() ? 1 : 0;
    if (j.contains("allow_tty") && j["allow_tty"].is_array()) {
        std::string joined;
        for (auto& v : j["allow_tty"]) {
            if (!v.is_string()) continue;
            std::string one = v.get<std::string>();
            if (one.empty() || one[0] != '/' || one.find("..") != std::string::npos) continue;
            if (!joined.empty()) joined += ",";
            joined += one;
            if (joined.size() > sizeof(allow_tty) - 2) break;
        }
        snprintf(allow_tty, sizeof(allow_tty), "%s", joined.c_str());
    }
    if (j.contains("metrics") && j["metrics"].is_object()) {
        auto& m = j["metrics"];
        if (m.contains("series") && m["series"].is_array()) {
            for (auto& r : m["series"]) {
                if (!r.is_object()) continue;
                std::string name = r.value("series", "");
                if (name.empty()) continue;
                SeriesCfg c;
                c.enabled = r.value("enabled", true) ? 1 : 0;
                c.interval = r.value("interval_sec", 5);
                c.points = r.value("points", 720);
                series_cfgs[name] = c;
            }
        }
    }
    if (j.contains("file_dir") && j["file_dir"].is_string()) {
        std::string v = j["file_dir"].get<std::string>();
        if (!v.empty() && v[0] == '/' && v.find("..") == std::string::npos) file_dir = v;
    }
    if (j.contains("fs_enabled") && j["fs_enabled"].is_boolean()) file_enabled = j["fs_enabled"].get<bool>() ? 1 : 0;
    if (j.contains("fs_root") && j["fs_root"].is_string()) {
        std::string v = j["fs_root"].get<std::string>();
        if (!v.empty() && v[0] == '/' && v.find("..") == std::string::npos && v.size() <= 512) file_root = v;
    }
    if (j.contains("fs_rw") && j["fs_rw"].is_boolean()) file_rw = j["fs_rw"].get<bool>() ? 1 : 0;
    if (j.contains("svc_enabled") && j["svc_enabled"].is_boolean()) svc_enabled = j["svc_enabled"].get<bool>() ? 1 : 0;
    if (j.contains("svc_rw") && j["svc_rw"].is_boolean()) svc_rw = j["svc_rw"].get<bool>() ? 1 : 0;
    if (j.contains("fs_max_mb") && j["fs_max_mb"].is_number_integer()) {
        int v = j["fs_max_mb"].get<int>();
        if (v > 0 && v <= 10240) file_max_mb = v;
    }
    if (j.contains("local_web")) {
        if (j["local_web"].is_boolean() && !j["local_web"].get<bool>()) {
#ifdef RTUNNEL_AGENT_WEB
            web_enabled = 0;
            web_listen.clear();
#endif
        }
        else if (j["local_web"].is_string()) {
            std::string v = j["local_web"].get<std::string>();
#ifdef RTUNNEL_AGENT_WEB
            if (!v.empty()) {
                web_listen = v;
                web_enabled = 1;
            }
#else
            logf_("[agent] config local_web=%s ignored: binary built without local web", v.c_str());
#endif
        }
    }
    logf_("[agent] config loaded from %s (allow=%d shell=%d)", config_file.c_str(), allow_ports[0], allow_shell);
}

bool AgentApp::saveConfig() {
    if (config_file.empty()) return false;
    std::string dir = config_file.substr(0, config_file.find_last_of('/'));
    if (!dir.empty()) {
        std::string cmd = "mkdir -p '" + dir + "'";
        if (system(cmd.c_str()) != 0) { /* 目录可能已存在 */
        }
    }
    // 以现有文件为底稿，保留本版本不认识的键（例如手工写入的 local_web）
    nlohmann::json j = nlohmann::json::object();
    {
        FILE* fp = fopen(config_file.c_str(), "r");
        if (fp) {
            std::string js;
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) js.append(buf, n);
            fclose(fp);
            auto old = nlohmann::json::parse(js, nullptr, false);
            if (!old.is_discarded() && old.is_object()) j = old;
        }
    }
    j["allow"] = nlohmann::json::array();
    for (int i = 0; i < 32 && allow_ports[i] > 0; ++i) j["allow"].push_back(allow_ports[i]);
    j["shell"] = allow_shell != 0;
    j["file_dir"] = file_dir;
    j["fs_enabled"] = file_enabled != 0;
    j["fs_root"] = file_root.empty() ? file_dir : file_root;
    j["fs_rw"] = file_rw != 0;
    j["fs_max_mb"] = file_max_mb;
    j["svc_enabled"] = svc_enabled != 0;
    j["svc_rw"] = svc_rw != 0;
    j["ota"] = allow_ota != 0;
    j["reboot"] = allow_reboot != 0;
    j["poweroff"] = allow_poweroff != 0;
    // local_web 不在这里写：它来自部署配置或命令行，运行期端口占用导致的失败
    // 不能固化成 "local_web": false，否则下次启动本地页会被永久关掉。
    j["metrics"] = nlohmann::json::object();
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& kv : series_cfgs) {
            nlohmann::json r;
            r["series"] = kv.first;
            r["enabled"] = kv.second.enabled != 0;
            r["interval_sec"] = kv.second.interval;
            r["points"] = kv.second.points;
            arr.push_back(r);
        }
        j["metrics"]["series"] = arr;
    }
    j["allow_tty"] = nlohmann::json::array();
    {
        std::string list = allow_tty;
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t comma = list.find(',', pos);
            std::string one = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!one.empty()) j["allow_tty"].push_back(one);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    std::string tmp = config_file + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "w");
    if (!fp) return false;
    std::string body = j.dump(2);
    bool ok = fwrite(body.data(), 1, body.size(), fp) == body.size();
    fclose(fp);
    if (!ok) {
        unlink(tmp.c_str());
        return false;
    }
    if (rename(tmp.c_str(), config_file.c_str()) != 0) return false;
    config_loaded = true;
    return true;
}

int AgentApp::applyFileConfig(const std::string& root, int rw, int max_mb, int enabled, std::string& err) {
    std::string effective = root.empty() ? file_dir : root;
    if (max_mb <= 0) max_mb = 512;
    if (!enabled) {
        fs_.close();
        file_root = root;
        file_rw = rw ? 1 : 0;
        file_max_mb = max_mb;
        file_enabled = 0;
        return 0;
    }
    rtfs::Config c;
    c.root = effective;
    c.rw = rw != 0;
    c.max_upload = (uint64_t)max_mb * 1024 * 1024;
    c.token = file_token;
    if (fs_.open(c, err) != 0) return -1; // 失败时保留原服务与配置
    file_root = root;
    file_rw = rw ? 1 : 0;
    file_max_mb = max_mb;
    file_enabled = 1;
    return 0;
}

void AgentApp::handleConfigSet(const rtnp::Frame* f) {
    rtnp::Reader rr(f->payload, f->len);
    static char cfg[8192];
    rr.str(cfg, sizeof(cfg));
    auto r = nlohmann::json::parse(rr.err ? std::string() : std::string(cfg), nullptr, false);
    if (r.is_discarded() || !r.is_object()) {
        ackConfigError(io, f->sid, "bad json");
        return;
    }
    int new_ports[32] = {0};
    bool ports_set = false, shell_set = false;
    int new_shell = allow_shell;
    if (r.contains("ports")) {
        if (!r["ports"].is_array()) {
            ackConfigError(io, f->sid, "ports must be array");
            return;
        }
        int idx = 0;
        for (auto& v : r["ports"]) {
            if (!v.is_number_integer()) {
                ackConfigError(io, f->sid, "bad port");
                return;
            }
            int p = v.get<int>();
            if (p <= 0 || p > 65535 || idx >= 32) {
                ackConfigError(io, f->sid, "port out of range or too many");
                return;
            }
            bool dup = false;
            for (int k = 0; k < idx; ++k)
                if (new_ports[k] == p) dup = true;
            if (!dup) new_ports[idx++] = p;
        }
        ports_set = true;
        new_ports[idx] = 0;
    }
    if (r.contains("shell")) {
        if (!r["shell"].is_boolean()) {
            ackConfigError(io, f->sid, "shell must be bool");
            return;
        }
        new_shell = r["shell"].get<bool>() ? 1 : 0;
        shell_set = true;
    }
    std::string new_file_dir = file_dir;
    if (r.contains("file_dir")) {
        if (!r["file_dir"].is_string()) {
            ackConfigError(io, f->sid, "file_dir must be string");
            return;
        }
        std::string v = r["file_dir"].get<std::string>();
        if (v.empty() || v[0] != '/' || v.find("..") != std::string::npos || v.size() > 200) {
            ackConfigError(io, f->sid, "bad file_dir");
            return;
        }
        new_file_dir = v;
    }
    bool fs_set = false;
    int new_fs_enabled = file_enabled;
    std::string new_fs_root = file_root;
    int new_fs_rw = file_rw;
    int new_fs_max = file_max_mb;
    if (r.contains("fs")) {
        if (!r["fs"].is_boolean()) {
            ackConfigError(io, f->sid, "fs must be bool");
            return;
        }
        new_fs_enabled = r["fs"].get<bool>() ? 1 : 0;
        fs_set = true;
    }
    if (r.contains("fs_root")) {
        if (!r["fs_root"].is_string()) {
            ackConfigError(io, f->sid, "fs_root must be string");
            return;
        }
        std::string v = r["fs_root"].get<std::string>();
        if (v.empty() || v[0] != '/' || v.find("..") != std::string::npos || v.size() > 512) {
            ackConfigError(io, f->sid, "bad fs_root");
            return;
        }
        new_fs_root = v;
        fs_set = true;
    }
    if (r.contains("fs_rw")) {
        if (!r["fs_rw"].is_boolean()) {
            ackConfigError(io, f->sid, "fs_rw must be bool");
            return;
        }
        new_fs_rw = r["fs_rw"].get<bool>() ? 1 : 0;
        fs_set = true;
    }
    if (r.contains("fs_max_mb")) {
        if (!r["fs_max_mb"].is_number_integer()) {
            ackConfigError(io, f->sid, "fs_max_mb must be integer");
            return;
        }
        int v = r["fs_max_mb"].get<int>();
        if (v <= 0 || v > 10240) {
            ackConfigError(io, f->sid, "fs_max_mb out of range");
            return;
        }
        new_fs_max = v;
        fs_set = true;
    }
    bool svc_set = false;
    int new_svc_enabled = svc_enabled;
    int new_svc_rw = svc_rw;
    if (r.contains("svc")) {
        if (!r["svc"].is_boolean()) {
            ackConfigError(io, f->sid, "svc must be bool");
            return;
        }
        new_svc_enabled = r["svc"].get<bool>() ? 1 : 0;
        svc_set = true;
    }
    if (r.contains("svc_rw")) {
        if (!r["svc_rw"].is_boolean()) {
            ackConfigError(io, f->sid, "svc_rw must be bool");
            return;
        }
        new_svc_rw = r["svc_rw"].get<bool>() ? 1 : 0;
        svc_set = true;
    }
    bool ota_set = false;
    int new_ota = allow_ota;
    if (r.contains("ota")) {
        if (!r["ota"].is_boolean()) {
            ackConfigError(io, f->sid, "ota must be bool");
            return;
        }
        new_ota = r["ota"].get<bool>() ? 1 : 0;
        ota_set = true;
    }
    bool reboot_set = false;
    int new_reboot = allow_reboot;
    if (r.contains("reboot")) {
        if (!r["reboot"].is_boolean()) {
            ackConfigError(io, f->sid, "reboot must be bool");
            return;
        }
        new_reboot = r["reboot"].get<bool>() ? 1 : 0;
        reboot_set = true;
    }
    bool poweroff_set = false;
    int new_poweroff = allow_poweroff;
    if (r.contains("poweroff")) {
        if (!r["poweroff"].is_boolean()) {
            ackConfigError(io, f->sid, "poweroff must be bool");
            return;
        }
        new_poweroff = r["poweroff"].get<bool>() ? 1 : 0;
        poweroff_set = true;
    }
    bool tty_set = false;
    std::string new_allow_tty = allow_tty;
    if (r.contains("allow_tty")) {
        if (!r["allow_tty"].is_array()) {
            ackConfigError(io, f->sid, "allow_tty must be array");
            return;
        }
        std::string joined;
        for (auto& v : r["allow_tty"]) {
            if (!v.is_string()) {
                ackConfigError(io, f->sid, "bad tty path");
                return;
            }
            std::string one = v.get<std::string>();
            if (one.empty() || one[0] != '/' || one.size() > 200 || one.find("..") != std::string::npos) {
                ackConfigError(io, f->sid, "bad tty path");
                return;
            }
            if (!joined.empty()) joined += ",";
            joined += one;
            if (joined.size() > sizeof(allow_tty) - 2) {
                ackConfigError(io, f->sid, "too many tty");
                return;
            }
        }
        new_allow_tty = joined;
        tty_set = true;
    }
    if (fs_set) {
        std::string ferr;
        if (applyFileConfig(new_fs_root, new_fs_rw, new_fs_max, new_fs_enabled, ferr) != 0) {
            nlohmann::json e;
            e["ok"] = false;
            e["error"] = ferr.empty() ? std::string("file service unavailable") : ferr;
            sendJsonFrame(io, rtnp::CONFIG_ACK, f->sid, e);
            return;
        }
    }
    if (ports_set) {
        memcpy(allow_ports, new_ports, sizeof(allow_ports));
        nallow = 0;
        while (nallow < 32 && allow_ports[nallow] > 0) nallow++;
    }
    if (shell_set) allow_shell = new_shell;
    if (ota_set) allow_ota = new_ota;
    if (reboot_set) allow_reboot = new_reboot;
    if (poweroff_set) allow_poweroff = new_poweroff;
    if (tty_set) snprintf(allow_tty, sizeof(allow_tty), "%s", new_allow_tty.c_str());
    if (svc_set) {
        svc_enabled = new_svc_enabled;
        svc_rw = new_svc_rw;
        svc_.setEnabled(svc_enabled != 0);
        svc_.setWritable(svc_rw != 0);
    }
    file_dir = new_file_dir;
    bool persisted = saveConfig();
    nlohmann::json ack;
    ack["ok"] = true;
    ack["persisted"] = persisted;
    ack["ports"] = nlohmann::json::array();
    for (int i = 0; i < 32 && allow_ports[i] > 0; ++i) ack["ports"].push_back(allow_ports[i]);
    ack["shell"] = allow_shell != 0;
    ack["file_dir"] = file_dir;
    ack["fs"] = file_enabled && fs_.ready();
    ack["fs_root"] = file_root.empty() ? file_dir : file_root;
    ack["fs_rw"] = file_rw != 0;
    ack["fs_max_mb"] = file_max_mb;
    ack["svc"] = svc_.ready();
    ack["svc_rw"] = svc_.ready() && svc_rw != 0;
    ack["ota"] = allow_ota != 0;
    ack["reboot"] = allow_reboot != 0;
    ack["poweroff"] = allow_poweroff != 0;
    ack["allow_tty"] = nlohmann::json::array();
    {
        std::string list = allow_tty;
        size_t pos = 0;
        while (pos <= list.size()) {
            size_t comma = list.find(',', pos);
            std::string one = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            if (!one.empty()) ack["allow_tty"].push_back(one);
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
    }
    logf_("[agent] config set: ports=%d shell=%d fs=%d svc=%d reboot=%d poweroff=%d persisted=%d", allow_ports[0],
          allow_shell, file_enabled && fs_.ready(), svc_.ready(), allow_reboot, allow_poweroff, (int)persisted);
    sendJsonFrame(io, rtnp::CONFIG_ACK, f->sid, ack);
}

struct FileRx {
    FILE* fp = nullptr;
    uint64_t expect = 0;
    uint64_t written = 0;
    std::string name;
    std::string dest;
};

static std::map<uint32_t, FileRx> g_file_rx;

// 自带 SHA-256（不依赖 OpenSSL，板子可能按无 TLS 构建）
struct Sha256 {
    uint32_t h[8];
    uint64_t total;
    unsigned char buf[64];
    size_t n;
    void init() {
        static const uint32_t H[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        for (int i = 0; i < 8; ++i) h[i] = H[i];
        total = 0;
        n = 0;
    }
    static uint32_t rotr(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const unsigned char* p) {
        static const uint32_t K[64] = {0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
                                       0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
                                       0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
                                       0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                                       0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
                                       0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
                                       0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) | ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }
    void update(const void* data, size_t len) {
        const unsigned char* p = (const unsigned char*)data;
        total += len;
        while (len > 0) {
            size_t take = 64 - n;
            if (take > len) take = len;
            memcpy(buf + n, p, take);
            n += take;
            p += take;
            len -= take;
            if (n == 64) {
                block(buf);
                n = 0;
            }
        }
    }
    std::string hex() {
        uint64_t bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (n != 56) update(&zero, 1);
        unsigned char lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = (unsigned char)(bits >> (56 - i * 8));
        update(lenb, 8);
        static const char* hx = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; ++i) {
            for (int j = 3; j >= 0; --j) {
                unsigned char c = (h[i] >> (j * 8)) & 0xFF;
                out += hx[c >> 4];
                out += hx[c & 0xF];
            }
        }
        return out;
    }
};

std::string AgentApp::sha256File(const std::string& path) {
    FILE* fp0 = fopen(path.c_str(), "rb");
    if (!fp0) return std::string();
    Sha256 ctx;
    ctx.init();
    char b[64 * 1024];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), fp0)) > 0) ctx.update(b, n);
    fclose(fp0);
    return ctx.hex();
}

void AgentApp::handleFileFrame(const rtnp::Frame* f) {
    auto sendAck = [&](bool ok, uint64_t received, const std::string& err) {
        nlohmann::json j;
        j["ok"] = ok;
        j["received"] = received;
        if (!err.empty()) j["error"] = err;
        std::string s = j.dump();
        rtnp::Writer w;
        w.str(s.c_str());
        rtnp::send(io, rtnp::FILE_ACK, f->sid, w.data, (uint16_t)w.len);
    };
    if (f->type == rtnp::FILE_BEGIN) {
        rtnp::Reader r(f->payload, f->len);
        char dir[256] = {0};
        r.str(dir, sizeof(dir));
        char name[256] = {0};
        r.str(name, sizeof(name));
        uint64_t size = r.u64();
        char sha[80] = {0};
        r.str(sha, sizeof(sha));
        bool bad_dir = false;
        for (const char* p = dir; *p; ++p) {
            if (!(isalnum((unsigned char)*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-')) {
                bad_dir = true;
                break;
            }
        }
        if (strstr(dir, "..") || dir[0] == '/') bad_dir = true;
        if (r.err || name[0] == '\0' || strstr(name, "..") || strchr(name, '/') || size == 0 || bad_dir) {
            sendAck(false, 0, "bad begin");
            return;
        }
        std::string target = file_dir;
        if (dir[0]) target += "/" + std::string(dir);
        if (!target.empty()) {
            std::string cmd = "mkdir -p '" + target + "'";
            system(cmd.c_str());
        }
        std::string dest = target + "/" + name;
        std::string part = dest + ".part";
        uint64_t have = 0;
        struct stat st {};
        if (stat(part.c_str(), &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= size) {
            have = (uint64_t)st.st_size;
        }
        FILE* fp = fopen(part.c_str(), have ? "ab" : "wb");
        if (!fp) {
            sendAck(false, 0, "open failed");
            return;
        }
        FileRx rx;
        rx.fp = fp;
        rx.expect = size;
        rx.written = have;
        rx.name = name;
        rx.dest = dest;
        g_file_rx[f->sid] = rx;
        if (have) logf_("[agent] resume %s at %llu", name, (unsigned long long)have);
        sendAck(true, have, "");
        return;
    }
    auto it = g_file_rx.find(f->sid);
    if (it == g_file_rx.end()) {
        sendAck(false, 0, "no transfer");
        return;
    }
    FileRx& rx = it->second;
    if (f->type == rtnp::FILE_DATA) {
        if (f->len > 0 && fwrite(f->payload, 1, f->len, rx.fp) != f->len) {
            sendAck(false, rx.written, "write failed");
            return;
        }
        rx.written += f->len;
        sendAck(true, rx.written, "");
        return;
    }
    // FILE_END
    fflush(rx.fp);
    fsync(fileno(rx.fp));
    fclose(rx.fp);
    rx.fp = nullptr;
    std::string part = rx.dest + ".part";
    std::string want = sha256File(part);
    rtnp::Reader r(f->payload, f->len);
    char expect[80] = {0};
    r.str(expect, sizeof(expect));
    if (want.empty() || strcmp(want.c_str(), expect) != 0) {
        unlink(part.c_str());
        g_file_rx.erase(it);
        sendAck(false, rx.written, "sha256 mismatch");
        return;
    }
    if (rename(part.c_str(), rx.dest.c_str()) != 0) {
        unlink(part.c_str());
        g_file_rx.erase(it);
        sendAck(false, rx.written, "rename failed");
        return;
    }
    logf_("[agent] file received: %s (%llu bytes)", rx.dest.c_str(), (unsigned long long)rx.written);
    g_file_rx.erase(it);
    sendAck(true, rx.written, "");
}

void AgentApp::cleanupParts(const std::string& dir, time_t cutoff) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st {};
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            cleanupParts(p, cutoff);
        }
        else if (st.st_mtime < cutoff) {
            size_t n = strlen(e->d_name);
            if (n > 5 && strcmp(e->d_name + n - 5, ".part") == 0) unlink(p.c_str());
        }
    }
    closedir(d);
}

// ---------------------------------------------------------------- OTA 安装
// 部署包：gzip(tar)，内含 manifest.json + install.sh + bin + service + etc。

static bool otaVersionOk(const std::string& v) {
    if (v.empty() || v.size() > 40) return false;
    for (char c : v) {
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+')) return false;
    }
    return true;
}

// 跑 <new-binary> --version，2s 超时；输出需与目标版本完全一致
bool AgentApp::otaProbe(const std::string& path, const std::string& expect, std::string& out) {
    int pfd[2];
    if (pipe(pfd) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {
        close(pfd[0]);
        dup2(pfd[1], 1);
        dup2(pfd[1], 2);
        close(pfd[1]);
        execl(path.c_str(), path.c_str(), "--version", (char*)NULL);
        _exit(127);
    }
    close(pfd[1]);
    char buf[256] = {0};
    size_t n = 0;
    int status = 0;
    bool exited = false;
    for (int i = 0; i < 200; ++i) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            exited = true;
            break;
        }
        if (r < 0) break;
        usleep(10000);
    }
    if (!exited) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    // 先杀再读：不合作的子进程会一直占着写端，read 会阻塞
    ssize_t got = read(pfd[0], buf, sizeof(buf) - 1);
    close(pfd[0]);
    if (!exited) return false;
    if (got > 0) {
        n = (size_t)got;
        while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' ')) buf[--n] = '\0';
        out.assign(buf, n);
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    return out == expect;
}

// 读入整个升级包（上限 64MB，与 relay 侧一致）
static bool otaReadFile(const std::string& path, std::string& out) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > 64LL * 1024 * 1024) {
        close(fd);
        return false;
    }
    out.resize((size_t)st.st_size);
    size_t off = 0;
    while (off < out.size()) {
        ssize_t n = read(fd, &out[off], out.size() - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            return false;
        }
        off += (size_t)n;
    }
    close(fd);
    return true;
}

static std::string otaBaseName(const std::string& p) {
    size_t s = p.find_last_of('/');
    return s == std::string::npos ? p : p.substr(s + 1);
}

static bool otaUnderSystemd() {
    const char* inv = getenv("INVOCATION_ID");
    return inv && inv[0] && access("/run/systemd/system", F_OK) == 0;
}

static std::string otaHostArch() {
    struct utsname u {};
    if (uname(&u) == 0 && u.machine[0]) return u.machine;
    return std::string();
}

static std::string otaTail(const std::string& path, size_t max) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::string();
    off_t sz = lseek(fd, 0, SEEK_END);
    if (sz < 0) {
        close(fd);
        return std::string();
    }
    off_t start = sz > (off_t)max ? sz - (off_t)max : 0;
    if (lseek(fd, start, SEEK_SET) < 0) {
        close(fd);
        return std::string();
    }
    std::string s;
    char buf[1024];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) s.append(buf, (size_t)n);
    close(fd);
    for (auto& c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    }
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

// 运行 install.sh；stdout/stderr 落日志，60s 超时。
// 返回进程退出码；-1 启动失败，-2 超时
static int otaRunInstaller(const std::string& prefix, const std::string& pkgdir,
                           const std::string& variant, const std::string& version, bool units,
                           std::string& out) {
    std::string script = pkgdir + "/install.sh";
    std::string log = pkgdir + "/install.log";
    int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fd);
        return -1;
    }
    if (pid == 0) {
        dup2(fd, 1);
        dup2(fd, 2);
        close(fd);
        if (units) {
            execl("/bin/sh", "sh", script.c_str(), "--pkg-dir", pkgdir.c_str(), "--variant", variant.c_str(),
                  "--version", version.c_str(), "--prefix", prefix.c_str(), (char*)NULL);
        }
        else {
            execl("/bin/sh", "sh", script.c_str(), "--pkg-dir", pkgdir.c_str(), "--variant", variant.c_str(),
                  "--version", version.c_str(), "--prefix", prefix.c_str(), "--no-units", (char*)NULL);
        }
        _exit(127);
    }
    close(fd);
    int status = 0;
    bool done = false;
    for (int i = 0; i < 600; ++i) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            done = true;
            break;
        }
        if (r < 0) break;
        usleep(100000);
    }
    if (!done) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    out = otaTail(log, 1200);
    if (!done) return -2;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

bool AgentApp::hasOtaPending() {
    if (app_prefix_.empty()) return false;
    return access((app_prefix_ + "/state/pending").c_str(), F_OK) == 0;
}

void AgentApp::clearOtaPending() {
    if (app_prefix_.empty()) return;
    std::string p = app_prefix_ + "/state/pending";
    FILE* fp = fopen(p.c_str(), "r");
    if (!fp) return;
    char line[512] = {0};
    bool got = fgets(line, sizeof(line), fp) != NULL;
    fclose(fp);
    if (!got) return;
    // prev\tnew\ttries\tlast-start
    std::string s(line);
    size_t t1 = s.find('\t');
    size_t t2 = (t1 == std::string::npos) ? std::string::npos : s.find('\t', t1 + 1);
    std::string target = (t1 != std::string::npos && t2 != std::string::npos) ? s.substr(t1 + 1, t2 - t1 - 1) : std::string();
    // 只有当前进程就是 pending 里的新版本才清；回滚后的旧进程要留给 boot-check 判断
    if (target.empty() || otaBaseName(self_path) != target) return;
    if (unlink(p.c_str()) == 0) logf_("[agent] ota pending cleared (%s)", target.c_str());
}

void AgentApp::onReady() {
    ota_connected_ = true;
    clearOtaPending();
    sendSysinfo();
}

void AgentApp::scheduleAgentRestart(uint32_t sid, const std::string& installed) {
    ota_restart_sid_ = sid;
    ota_installed_path_ = installed;
    ota_flush_ticks_ = 0;
    htimer_t* t = htimer_add(
        loop,
        [](htimer_t* tm) {
            auto* app = (AgentApp*)hevent_userdata(tm);
            // ACK 还在用户态缓冲就退出会丢帧；最多等 2s。
            // 连接已断开时 io 为空，直接重启（relay 会按超时处理）。
            if (app->io != NULL && !hio_write_is_complete(app->io) && ++app->ota_flush_ticks_ < 40) return;
            app->restartAgentNow();
        },
        50, INFINITE);
    hevent_set_userdata(t, this);
}

void AgentApp::restartAgentNow() {
    // 新进程要重新建立 relay 连接，不能继承旧 socket
    for (int fd = 3; fd < 1024; ++fd) fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (otaUnderSystemd()) {
        logf_("[agent] ota restart via systemd");
        int rc = system("systemctl --no-block restart rtunnel-agent.service");
        if (rc != 0) logf_("[agent] systemctl restart rc=%d", rc);
        _exit(0);
    }
    std::vector<char*> av;
    for (auto& s : saved_argv) av.push_back(const_cast<char*>(s.c_str()));
    av.push_back(NULL);
    execv(ota_installed_path_.c_str(), av.data());
    int e = errno;
    logf_("[agent] ota restart failed: %s", strerror(e));
    // 非 systemd 布局：把软链切回上一个版本，避免下次启动继续跑坏文件
    std::string prev;
    {
        FILE* fp = fopen((app_prefix_ + "/state/previous").c_str(), "r");
        if (fp) {
            char line[512] = {0};
            if (fgets(line, sizeof(line), fp)) {
                std::string s(line);
                size_t t = s.find('\t');
                prev = s.substr(0, t == std::string::npos ? s.size() : t);
                while (!prev.empty() && (prev.back() == '\n' || prev.back() == '\r')) prev.pop_back();
            }
            fclose(fp);
        }
    }
    std::string link = app_prefix_ + "/bin/rtunnel-agent";
    if (!prev.empty() && access((app_prefix_ + "/bin/" + prev).c_str(), F_OK) == 0) {
        std::string tmp = link + ".tmp." + std::to_string((long)getpid());
        unlink(tmp.c_str());
        if (symlink(prev.c_str(), tmp.c_str()) == 0) rename(tmp.c_str(), link.c_str());
        logf_("[agent] ota rolled back to %s", prev.c_str());
    }
    nlohmann::json rj;
    rj["ok"] = false;
    rj["phase"] = "installed";
    rj["error"] = std::string("restart failed: ") + strerror(e);
    std::string s = rj.dump();
    rtnp::Writer w;
    w.str(s.c_str());
    if (io != NULL) rtnp::send(io, rtnp::OTA_APPLY_ACK, ota_restart_sid_, w.data, (uint16_t)w.len);
}

void AgentApp::handleOtaApply(const rtnp::Frame* f) {
    auto ack = [&](bool ok, const char* phase, const std::string& err) {
        nlohmann::json j;
        j["ok"] = ok;
        if (phase && phase[0]) j["phase"] = phase;
        if (!err.empty()) j["error"] = err.size() > 600 ? err.substr(0, 600) + "..." : err;
        if (!ok) logf_("[agent] ota: %s", err.c_str());
        std::string s = j.dump();
        rtnp::Writer w;
        w.str(s.c_str());
        rtnp::send(io, rtnp::OTA_APPLY_ACK, f->sid, w.data, (uint16_t)w.len);
    };

    if (f->type == rtnp::OTA_CANCEL) return; // 安装在同步流程里完成，收到时已无法中止
    if (!allow_ota) {
        ack(false, NULL, "ota disabled");
        return;
    }
    if (strncmp(devid, "a:", 2) == 0) {
        ack(false, NULL, "on-demand agent");
        return;
    }

    rtnp::Reader r(f->payload, f->len);
    static char body[4096];
    r.str(body, sizeof(body));
    auto j = nlohmann::json::parse(r.err ? std::string() : std::string(body), nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        ack(false, NULL, "bad json");
        return;
    }
    std::string path = j.value("path", "");
    std::string version = j.value("version", "");
    uint64_t size = j.value("size", (uint64_t)0);
    std::string sha = j.value("sha256", "");
    if (!otaVersionOk(version) || size == 0 || size > 64ULL * 1024 * 1024 || sha.size() != 64) {
        ack(false, NULL, "bad params");
        return;
    }
    std::string base = file_dir;
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    if (path.size() <= base.size() || path.compare(0, base.size(), base) != 0 || path[base.size()] != '/' || path.find("..") != std::string::npos) {
        ack(false, NULL, "path outside file_dir");
        return;
    }
    struct stat st {};
    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || (uint64_t)st.st_size != size) {
        ack(false, NULL, "file missing or size mismatch");
        return;
    }
    if (self_path.empty()) {
        ack(false, NULL, "self path unknown");
        return;
    }
    // 分发通道落盘是 0644，自检前先补可执行位
    if (chmod(path.c_str(), 0755) != 0) {
        ack(false, NULL, "chmod failed");
        return;
    }

    std::string sha_got = sha256File(path);
    if (sha_got.empty() || sha_got != sha) {
        ack(false, NULL, "sha256 mismatch");
        return;
    }

    std::string raw;
    if (path.size() < 8 || path.compare(path.size() - 7, 7, ".tar.gz") != 0) {
        ack(false, NULL, "not a .tar.gz package");
        return;
    }
    {
        std::string buf;
        if (!otaReadFile(path, buf)) {
            ack(false, NULL, "read package failed");
            return;
        }
        if (!gzipDecompress(buf.data(), buf.size(), raw, 128ULL * 1024 * 1024) || raw.empty()) {
            ack(false, NULL, "gunzip failed");
            return;
        }
        logf_("[agent] ota: gunzip %s -> %zu bytes", path.c_str(), raw.size());
    }
    if (!rtopkg::isTar(raw)) {
        ack(false, NULL, "package is not a tar archive");
        return;
    }

    {
        if (app_prefix_.empty()) {
            ack(false, NULL, "unexpected install layout");
            return;
        }
        std::string staging = app_prefix_ + "/.ota/" + std::to_string((long)getpid());
        rtopkg::removeTree(staging);
        std::string err;
        if (!rtopkg::extract(raw, staging, err)) {
            ack(false, NULL, "unpack: " + err);
            return;
        }
        struct StageGuard {
            std::string p;
            ~StageGuard() { if (!p.empty()) rtopkg::removeTree(p); }
        } stage_guard{staging};

        std::string pkgroot;
        if (!rtopkg::resolveRoot(staging, pkgroot, err)) {
            ack(false, NULL, "package: " + err);
            return;
        }
        rtopkg::Manifest m;
        if (!rtopkg::readManifest(pkgroot, m, err)) {
            ack(false, NULL, "manifest: " + err);
            return;
        }
        if (m.version != version) {
            ack(false, NULL, "manifest version mismatch");
            return;
        }
        if (m.arch != otaHostArch()) {
            ack(false, NULL, "arch mismatch: package " + m.arch + ", host " + otaHostArch());
            return;
        }
        std::string pkgbin = pkgroot + "/" + m.binary;
        if (stat(pkgbin.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            ack(false, NULL, "package binary missing");
            return;
        }
        chmod(pkgbin.c_str(), 0755);

        std::string probe;
        if (!otaProbe(pkgbin, version, probe)) {
            std::string e = "version probe failed";
            if (!probe.empty()) e += ": " + probe;
            ack(false, NULL, e);
            return;
        }
        ack(true, "verified", "");

        std::string log;
        int rc = otaRunInstaller(app_prefix_, pkgroot, m.variant, version, otaUnderSystemd(), log);
        if (rc != 0) {
            std::string e = (rc == -2) ? "install timeout" : "install failed";
            if (rc > 0) e += " (" + std::to_string(rc) + ")";
            if (!log.empty()) e += ": " + log;
            ack(false, NULL, e);
            return;
        }
        std::string installed = app_prefix_ + "/bin/rtunnel-agent-" + m.variant + "-" + version;
        size_t ip = log.find("INSTALLED=");
        if (ip != std::string::npos) {
            size_t start = ip + 10;
            size_t end = log.find(' ', start);
            std::string v = log.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!v.empty() && v[0] == '/') installed = v;
        }
        logf_("[agent] ota installed %s -> %s", version.c_str(), installed.c_str());
        ack(true, "installed", "");
        scheduleAgentRestart(f->sid, installed);
        return;
    }
}

static bool readTtyCounters(const std::string& want, unsigned long long& rx, unsigned long long& tx) {
    // /dev/ttyS0 对应 /proc/tty/driver 中的 "0:" 端口行
    int port = -1;
    if (want.size() > 6 && want.compare(0, 6, "ttySTM") == 0) {
        port = atoi(want.c_str() + 6);
    }
    else if (want.size() > 4 && want.compare(0, 4, "ttyS") == 0) {
        port = atoi(want.c_str() + 4);
    }
    DIR* d = opendir("/proc/tty/driver");
    if (!d) return false;
    bool found = false;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        std::ifstream in(std::string("/proc/tty/driver/") + e->d_name);
        std::string line;
        while (std::getline(in, line)) {
            bool match = false;
            if (port >= 0) {
                int n = -1;
                if (sscanf(line.c_str(), "%d:", &n) == 1 && n == port) match = true;
            }
            else if (line.find(want) != std::string::npos) {
                match = true;
            }
            if (!match) continue;
            size_t p1 = line.find("tx:"), p2 = line.find("rx:");
            if (p1 != std::string::npos) sscanf(line.c_str() + p1, "tx:%llu", &tx);
            if (p2 != std::string::npos) sscanf(line.c_str() + p2, "rx:%llu", &rx);
            found = true;
        }
    }
    closedir(d);
    return found;
}

void AgentApp::sampleSeries(const std::string& name, const SeriesCfg& cfg) {
    nlohmann::json j;
    j["ts"] = (long long)time(NULL) * 1000;
    if (name == "cpu") {
        j["v"] = cpu_usage;
    }
    else if (name == "mem") {
        std::ifstream mf("/proc/meminfo");
        std::string line;
        long long total = 0, avail = 0, free_kb = 0;
        while (std::getline(mf, line)) {
            long long v = 0;
            if (sscanf(line.c_str(), "MemTotal: %lld", &v) == 1)
                total = v;
            else if (sscanf(line.c_str(), "MemAvailable: %lld", &v) == 1)
                avail = v;
            else if (sscanf(line.c_str(), "MemFree: %lld", &v) == 1)
                free_kb = v;
        }
        if (avail <= 0) avail = free_kb;
        j["v"] = total > 0 ? (double)(total - avail) * 100.0 / total : 0.0;
    }
    else if (name == "load") {
        double la[3] = {0, 0, 0};
        getloadavg(la, 3);
        j["v"] = la[0];
        j["load5"] = la[1];
        j["load15"] = la[2];
    }
    else if (name == "temp") {
        // 取所有热区的最高温度，单个 zone 过热也能在曲线上看到
        double max_c = 0;
        DIR* d = opendir("/sys/class/thermal");
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                std::string n = e->d_name;
                if (n.rfind("thermal_zone", 0) != 0) continue;
                std::ifstream tf("/sys/class/thermal/" + n + "/temp");
                long long mc = 0;
                if (!(tf >> mc)) continue;
                double c = mc > 1000 ? (double)mc / 1000.0 : (double)mc;
                if (c > max_c) max_c = c;
            }
            closedir(d);
        }
        j["v"] = (double)((long long)(max_c * 10)) / 10.0;
    }
    else if (name.rfind("net:", 0) == 0) {
        std::string ifn = name.substr(4);
        auto rd = [&](const char* f) -> unsigned long long {
            std::ifstream in(std::string("/sys/class/net/") + ifn + "/statistics/" + f);
            unsigned long long v = 0;
            in >> v;
            return v;
        };
        j["rx"] = rd("rx_bytes");
        j["tx"] = rd("tx_bytes");
        j["v"] = 0;
    }
    else if (name.rfind("serial:", 0) == 0) {
        unsigned long long rx = 0, tx = 0;
        readTtyCounters(name.substr(7), rx, tx);
        j["rx"] = rx;
        j["tx"] = tx;
        j["v"] = 0;
    }
    else {
        return;
    }
    std::string line = j.dump();
    std::lock_guard<std::mutex> lk(series_mu_);
    auto& ring = series_rings[name];
    ring.push_back(line);
    while ((int)ring.size() > cfg.points) ring.pop_front();
}

void AgentApp::seriesTick() {
    agentSampleCpu(this);
    long long now = (long long)time(NULL) * 1000;
    for (auto& kv : series_cfgs) {
        if (!kv.second.enabled) continue;
        auto& last = series_last_ms[kv.first];
        if (now - last < (long long)kv.second.interval * 1000) continue;
        last = now;
        sampleSeries(kv.first, kv.second);
    }
}

// 存储统计只采集本地文件系统：伪文件系统没有意义，
// 网络/FUSE 挂载上的 statvfs 可能长时间阻塞事件循环。
static bool mountIgnored(const std::string& fstype, const std::string& mnt) {
    static const char* kPseudo[] = {"proc", "sysfs", "devtmpfs", "devpts", "cgroup", "cgroup2", "pstore",
        "debugfs", "securityfs", "tracefs", "configfs", "mqueue", "hugetlbfs", "fusectl",
        "binfmt_misc", "autofs", "efivarfs", "bpf", "squashfs"};
    static const char* kRemote[] = {"nfs", "cifs", "smb", "fuse", "9p", "ceph",
        "glusterfs", "afs", "davfs", "sshfs", "ncpfs", "lustre"};
    for (const char* t : kPseudo) {
        if (fstype == t) return true;
    }
    for (const char* t : kRemote) {
        if (fstype.rfind(t, 0) == 0) return true;
    }
    if (fstype == "overlay" && mnt != "/") return true;
    if (mnt == "/run" || mnt.rfind("/run/", 0) == 0) return true;
    if (mnt == "/dev/shm" || mnt.rfind("/dev/shm/", 0) == 0) return true;
    if (mnt == "/sys" || mnt.rfind("/sys/", 0) == 0) return true;
    return false;
}

// 读 /sys 下的单行小文件（网卡 MAC/MTU/速率/统计）
static bool readSysLine(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[256];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    out.assign(buf, n);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return !out.empty();
}

static unsigned long long readSysULL(const std::string& path) {
    std::string s;
    if (!readSysLine(path, s)) return 0;
    return strtoull(s.c_str(), nullptr, 10);
}

// 网卡的静态属性与累计收发量，供详情页「网卡」tab 展示
static void fillNicDetail(const std::string& name, nlohmann::json& n) {
    std::string base = "/sys/class/net/" + name;
    std::string mac, mtu, speed, oper, carrier;
    if (readSysLine(base + "/address", mac)) n["mac"] = mac;
    if (readSysLine(base + "/mtu", mtu)) n["mtu"] = atoi(mtu.c_str());
    if (readSysLine(base + "/speed", speed)) n["speed_mbps"] = atoi(speed.c_str()) > 0 ? atoi(speed.c_str()) : 0;
    if (readSysLine(base + "/operstate", oper)) n["operstate"] = oper;
    if (readSysLine(base + "/carrier", carrier)) n["carrier"] = carrier == "1";
    n["rx_bytes"] = readSysULL(base + "/statistics/rx_bytes");
    n["tx_bytes"] = readSysULL(base + "/statistics/tx_bytes");
    n["rx_errors"] = readSysULL(base + "/statistics/rx_errors");
    n["tx_errors"] = readSysULL(base + "/statistics/tx_errors");
    n["rx_dropped"] = readSysULL(base + "/statistics/rx_dropped");
    n["tx_dropped"] = readSysULL(base + "/statistics/tx_dropped");
}

// 证书主体/签发者/序列号只解析一次；有效期每次快照按当前时间判断。
void AgentApp::parseCert() {
#if defined(WITH_OPENSSL)
    if (!tls_cert[0]) return;
    FILE* fp = fopen(tls_cert, "r");
    if (!fp) return;
    X509* x = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);
    if (!x) return;
    char subj[256] = {0}, iss[256] = {0};
    X509_NAME_oneline(X509_get_subject_name(x), subj, sizeof(subj));
    X509_NAME_oneline(X509_get_issuer_name(x), iss, sizeof(iss));
    sys_static_.cert_subject = subj;
    sys_static_.cert_issuer = iss;
    sys_static_.cert_serial = std::to_string((long long)ASN1_INTEGER_get(X509_get_serialNumber(x)));
    {
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        if (ASN1_TIME_to_tm(X509_get0_notAfter(x), &tm)) {
            sys_static_.cert_not_after = (long long)timegm(&tm);
        }
    }
    X509_free(x);
#endif
}

// 内核、板卡、CPU 型号、证书这些不会随运行变化，只读一次。
void AgentApp::fillSysinfoStatic() {
    if (sys_static_.ready) return;
    sys_static_.ready = true;
    struct utsname u;
    if (uname(&u) == 0) {
        sys_static_.kernel = u.release;
        sys_static_.machine = u.machine;
    }
    {
        std::ifstream f("/proc/device-tree/model");
        if (f) std::getline(f, sys_static_.board);
    }
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("processor", 0) == 0) sys_static_.cpu_count++;
            if (sys_static_.cpu_model.empty() &&
                (line.rfind("model name", 0) == 0 || line.rfind("Processor", 0) == 0)) {
                size_t p = line.find(':');
                if (p != std::string::npos) sys_static_.cpu_model = line.substr(p + 1);
            }
        }
        while (!sys_static_.cpu_model.empty() &&
               (sys_static_.cpu_model[0] == ' ' || sys_static_.cpu_model[0] == '\t'))
            sys_static_.cpu_model.erase(0, 1);
    }
    parseCert();
}

namespace {

// 读 /sys/class/thermal 的所有热区，返回 [{zone,type,temp_c}]。
// 温度统一按摄氏度输出；内核给的是毫摄氏度时除以 1000。
nlohmann::json readTemps() {
    using nlohmann::json;
    json arr = json::array();
    DIR* d = opendir("/sys/class/thermal");
    if (!d) return arr;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string name = e->d_name;
        if (name.rfind("thermal_zone", 0) != 0) continue;
        std::string base = "/sys/class/thermal/" + name;
        std::ifstream tf(base + "/temp");
        long long mc = 0;
        if (!(tf >> mc)) continue;
        std::string type;
        {
            std::ifstream yf(base + "/type");
            if (yf) std::getline(yf, type);
        }
        json o;
        o["zone"] = name;
        o["type"] = type;
        double c = mc > 1000 ? (double)mc / 1000.0 : (double)mc;
        o["temp_c"] = (double)((long long)(c * 10)) / 10.0;
        arr.push_back(o);
    }
    closedir(d);
    return arr;
}

// eMMC/SD 寿命：life_time 两个十六进制字节（SLC/MLC 已用寿命 10%~100%），
// pre_eol_info 0x01 正常 / 0x02 警告 / 0x03 紧急。
nlohmann::json readDiskHealth() {
    using nlohmann::json;
    json arr = json::array();
    DIR* d = opendir("/sys/block");
    if (!d) return arr;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string name = e->d_name;
        if (name == "." || name == ".." || name.rfind("loop", 0) == 0 ||
            name.rfind("ram", 0) == 0 || name.rfind("zram", 0) == 0)
            continue;
        std::string base = "/sys/block/" + name + "/device";
        std::ifstream lf(base + "/life_time");
        std::ifstream pf(base + "/pre_eol_info");
        if (!lf && !pf) continue;
        json o;
        o["device"] = name;
        if (lf) {
            std::string a, b;
            if (lf >> a >> b) {
                auto pct = [](const std::string& h) -> int {
                    char* end = nullptr;
                    long v = strtol(h.c_str(), &end, 16);
                    return (int)((v & 0x0f) * 10);
                };
                o["life_slc"] = pct(a);
                o["life_mlc"] = pct(b);
            }
        }
        if (pf) {
            std::string v;
            if (pf >> v) {
                int n = (int)strtol(v.c_str(), nullptr, 16);
                o["pre_eol"] = n;
                o["pre_eol_text"] = n == 1 ? "正常" : n == 2 ? "警告" : n == 3 ? "紧急" : "未知";
            }
        }
        arr.push_back(o);
    }
    closedir(d);
    return arr;
}

} // namespace

nlohmann::json AgentApp::buildSysinfo() {
    using nlohmann::json;
    fillSysinfoStatic();
    json j;
    j["version"] = hv_version();
    j["args"] = args_line;
    j["kernel"] = sys_static_.kernel;
    j["machine"] = sys_static_.machine;
    j["board"] = sys_static_.board;
    j["cpu_model"] = sys_static_.cpu_model;
    j["cpu_count"] = sys_static_.cpu_count;
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        unsigned long long total = 0, avail = 0, free_kb = 0, swap_total = 0, swap_free = 0;
        while (std::getline(f, line)) {
            unsigned long long v = 0;
            if (sscanf(line.c_str(), "MemTotal: %llu kB", &v) == 1)
                total = v;
            else if (sscanf(line.c_str(), "MemAvailable: %llu kB", &v) == 1)
                avail = v;
            else if (sscanf(line.c_str(), "MemFree: %llu kB", &v) == 1)
                free_kb = v;
            else if (sscanf(line.c_str(), "SwapTotal: %llu kB", &v) == 1)
                swap_total = v;
            else if (sscanf(line.c_str(), "SwapFree: %llu kB", &v) == 1)
                swap_free = v;
        }
        j["mem_total_kb"] = total;
        j["mem_free_kb"] = free_kb;
        j["mem_available_kb"] = avail ? avail : free_kb;
        j["swap_total_kb"] = swap_total;
        j["swap_free_kb"] = swap_free;
    }
    {
        nlohmann::json mounts = nlohmann::json::array();
        std::ifstream mf("/proc/mounts");
        std::string line;
        std::set<std::string> seen;
        std::map<std::string, size_t> by_source; // 同一设备的 bind mount 只保留最短路径
        while (std::getline(mf, line)) {
            std::istringstream is(line);
            std::string dev, mnt, fstype, opts;
            if (!(is >> dev >> mnt >> fstype)) continue;
            is >> opts;
            if (mountIgnored(fstype, mnt) || !seen.insert(mnt).second) continue;
            // bind mount 与源挂载是同一个文件系统（st_dev 相同），只保留最短路径；
            // btrfs 子卷共用 superblock，但属于不同挂载，按 subvol 区分
            struct stat mst;
            std::string key = stat(mnt.c_str(), &mst) == 0 ? "dev:" + std::to_string((unsigned long long)mst.st_dev)
                                                           : "src:" + dev;
            size_t sv = opts.find("subvol=");
            if (sv != std::string::npos) {
                size_t end = opts.find(',', sv);
                key += "|" + opts.substr(sv, end == std::string::npos ? std::string::npos : end - sv);
            }
            auto dup = by_source.find(key);
            if (dup != by_source.end()) {
                if (mnt.size() < mounts[dup->second]["mount"].get<std::string>().size()) {
                    mounts[dup->second]["mount"] = mnt;
                }
                continue;
            }
            struct statvfs vfs;
            if (statvfs(mnt.c_str(), &vfs) != 0) continue;
            unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize / 1024;
            if (total == 0) continue;
            nlohmann::json m;
            m["mount"] = mnt;
            m["fstype"] = fstype;
            m["total_kb"] = total;
            // used 用 f_bfree（真实已用），avail 用 f_bavail（非特权可用）；
            // 使用率按 used/(used+avail) 计算，与 df 的 Use% 一致。
            m["used_kb"] = (unsigned long long)(vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize / 1024;
            m["avail_kb"] = (unsigned long long)vfs.f_bavail * vfs.f_frsize / 1024;
            by_source[key] = mounts.size();
            mounts.push_back(m);
        }
        j["mounts"] = mounts;
    }
    {
        json nics = json::array();
        struct ifaddrs* ifs = NULL;
        if (getifaddrs(&ifs) == 0) {
            for (struct ifaddrs* p = ifs; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                if (strcmp(p->ifa_name, "lo") == 0) continue;
                char ip[64] = {0}, mask[64] = {0};
                struct sockaddr_in* sin = (struct sockaddr_in*)p->ifa_addr;
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                if (p->ifa_netmask) {
                    struct sockaddr_in* m = (struct sockaddr_in*)p->ifa_netmask;
                    inet_ntop(AF_INET, &m->sin_addr, mask, sizeof(mask));
                }
                json n;
                n["name"] = p->ifa_name;
                n["ip"] = ip;
                n["netmask"] = mask;
                n["up"] = (p->ifa_flags & IFF_UP) ? true : false;
                fillNicDetail(p->ifa_name, n);
                nics.push_back(n);
            }
            freeifaddrs(ifs);
        }
        j["nics"] = nics;
    }
    j["temps"] = readTemps();
    j["disk_health"] = readDiskHealth();
    {
        // 通过 relay 的 /device/<devid>/10080/ 打开本页时，代理自身会占一条
        // 到本地 Web 端口的 TCP 流；它不是用户会话，不能出现在卡片里。
        int self_web_port = 0;
        {
            size_t p = web_listen.rfind(':');
            if (p != std::string::npos) self_web_port = atoi(web_listen.c_str() + p + 1);
        }
        json arr = json::array();
        for (LocalStream* s = streams; s; s = s->next) {
            if (s->freed) continue;
            if (self_web_port && s->target_port == self_web_port) continue;
            const char* kind = s->is_fs ? "fs"
                             : s->is_svc ? "svc"
                             : s->is_shell ? "shell"
                             : (!s->tty_path.empty() ? "tty" : "tcp");
            json o;
            o["sid"] = s->sid;
            o["kind"] = kind;
            o["opened_ms"] = (long long)s->opened_ms;
            if (!s->tty_path.empty()) o["path"] = s->tty_path;
            arr.push_back(o);
        }
        j["streams"] = arr;
    }
    {
        // /dev 扫描用于发现新插入的串口，30s 一次即可；配置里的路径每次都并进来
        uint64_t now_ms = wallMs();
        if (tty_dev_paths_.empty() || now_ms - tty_dev_ms_ > 30000) {
            std::vector<std::string> devs;
            DIR* d = opendir("/dev");
            if (d) {
                struct dirent* e;
                while ((e = readdir(d)) != NULL) {
                    std::string n = e->d_name;
                    if (n.rfind("ttyS", 0) != 0 && n.rfind("ttyUSB", 0) != 0 &&
                        n.rfind("ttyACM", 0) != 0 && n.rfind("ttySTM", 0) != 0)
                        continue;
                    devs.push_back("/dev/" + n);
                }
                closedir(d);
            }
            tty_dev_paths_ = std::move(devs);
            tty_dev_ms_ = now_ms;
        }
        std::set<std::string> paths(tty_dev_paths_.begin(), tty_dev_paths_.end());
        for (auto& kv : tty_cfgs_) paths.insert(kv.first);
        nlohmann::json serial = nlohmann::json::array();
        for (auto& path : paths) {
            unsigned long long rx = 0, tx = 0;
            char rp[PATH_MAX];
            std::string base = path;
            if (realpath(path.c_str(), rp)) {
                std::string r = rp;
                size_t p = r.find_last_of('/');
                if (p != std::string::npos) base = r.substr(p + 1);
            }
            else {
                size_t p = base.find_last_of('/');
                if (p != std::string::npos) base = base.substr(p + 1);
            }
            bool found = readTtyCounters(base, rx, tx);
            auto cfg = tty_cfgs_.find(path);
            bool configured = cfg != tty_cfgs_.end();
            if (!found && !configured) continue;
            nlohmann::json sj;
            sj["path"] = path;
            sj["rx"] = rx;
            sj["tx"] = tx;
            sj["configured"] = configured;
            if (configured) {
                sj["enabled"] = cfg->second.enabled != 0;
                sj["baud"] = cfg->second.baud;
                sj["data_bits"] = cfg->second.data_bits;
                sj["parity"] = std::string(1, cfg->second.parity);
                sj["stop_bits"] = cfg->second.stop_bits;
            }
            serial.push_back(sj);
        }
        j["serial"] = serial;
    }
    j["mtls"] = (tls_cert[0] != '\0');
    {
        nlohmann::json caps;
        caps["ports"] = nlohmann::json::array();
        for (int i = 0; i < 32 && allow_ports[i] > 0; ++i) caps["ports"].push_back(allow_ports[i]);
        caps["shell"] = allow_shell != 0;
        caps["sysinfo"] = true;
        caps["mtls"] = (tls_cert[0] != '\0');
        caps["file_dir"] = file_dir;
        caps["fs"] = file_enabled && fs_.ready();
        caps["fs_rw"] = file_rw != 0;
        caps["fs_root"] = file_root.empty() ? file_dir : file_root;
        caps["fs_token"] = fs_.tokenRequired();
        caps["fs_max_mb"] = file_max_mb;
        caps["svc"] = svc_.ready();
        caps["svc_rw"] = svc_.ready() && svc_rw != 0;
        caps["svc_token"] = svc_.tokenRequired();
        caps["ota"] = allow_ota != 0;
        caps["ota_pkg"] = "tar.gz"; // 支持 install.sh 部署包
        caps["reboot"] = allow_reboot != 0;
        caps["poweroff"] = allow_poweroff != 0;
        caps["diag"] = true;        // 支持 DIAG_REQ 诊断包
        caps["net"] = true;         // 支持 NET_REQ 网络/IPC 快照
        caps["proc"] = true;        // 支持 PROC_REQ 进程列表与信号
        caps["warm"] = true;        // 支持 WARM_REQ 详情页预采样
        caps["allow_tty"] = nlohmann::json::array();
        {
            std::string list = allow_tty;
            size_t pos = 0;
            while (pos <= list.size()) {
                size_t comma = list.find(',', pos);
                std::string one = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!one.empty()) caps["allow_tty"].push_back(one);
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
        }
        caps["version"] = hv_version();
        caps["arch"] = otaHostArch();
#ifdef RTUNNEL_AGENT_WEB
        caps["variant"] = "web";
        caps["local_web"] = web_enabled ? web_listen : std::string();
#else
        caps["variant"] = "lite";
        caps["local_web"] = std::string();
#endif
        caps["config_file"] = config_file;
        caps["config_loaded"] = config_loaded;
        caps["metrics"] = nlohmann::json::object();
        {
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& kv : series_cfgs) {
                nlohmann::json r;
                r["series"] = kv.first;
                r["enabled"] = kv.second.enabled != 0;
                r["interval_sec"] = kv.second.interval;
                r["points"] = kv.second.points;
                arr.push_back(r);
            }
            caps["metrics"]["series"] = arr;
        }
        j["caps"] = caps;
    }

    if (!sys_static_.cert_subject.empty()) {
        j["cert_subject"] = sys_static_.cert_subject;
        j["cert_issuer"] = sys_static_.cert_issuer;
        j["cert_serial"] = sys_static_.cert_serial;
        j["cert_valid"] = (sys_static_.cert_not_after > (long long)time(NULL)) ? 1 : 0;
    }

    j["devid"] = devid;
    j["online"] = ready();
    if (!endpoints.empty()) {
        const TunnelEndpoint& ep = currentEndpoint();
        j["relay"] = ep.host + ":" + std::to_string(ep.port);
        j["relay_index"] = (int)endpointIndex();
        j["relay_count"] = (int)endpoints.size();
    }
    j["cpu_usage"] = cpu_usage;
    j["cpu_cores"] = cpu_cores;
    {
        double la[3] = {0, 0, 0};
        if (getloadavg(la, 3) == 3) j["loadavg"] = json::array({la[0], la[1], la[2]});
    }
    {
        std::ifstream uf("/proc/uptime");
        double up = 0;
        if (uf >> up) j["uptime_sec"] = (long long)up;
    }
    j["agent_uptime_sec"] = (long long)((agentMonotonicMs() - started_ms_) / 1000);
    return j;
}

void AgentApp::refreshWebSnapshot() {
    uint64_t now = wallMs();
    // 无人看页面时降到 10s：mounts 的 statvfs 与 /proc 读取没必要每 2s 跑一遍；
    // 页面请求会通过 noteWebRequest 立即补齐一次，再恢复 2s 节奏。
    bool active = web_req_ms_.load(std::memory_order_relaxed) + 15000 > now;
    if (!active && web_snapshot_ms_.load(std::memory_order_relaxed) + 10000 > now) return;
    nlohmann::json j = buildSysinfo();
    j["ok"] = true;
    j["ts"] = (long long)time(NULL) * 1000;
    j["web"] = {{"listen", web_listen}, {"interval_ms", 2000}};
    std::string body = j.dump();
    {
        std::lock_guard<std::mutex> lk(web_mu_);
        web_body_.swap(body);
    }
    web_snapshot_ms_.store(now, std::memory_order_relaxed);
}

// HTTP 线程调用：记录活跃时间，快照不新鲜时投递一次重建，首屏不用等定时器。
void AgentApp::noteWebRequest() {
    uint64_t now = wallMs();
    web_req_ms_.store(now, std::memory_order_relaxed);
    if (web_snapshot_ms_.load(std::memory_order_relaxed) + 1500 > now) return;
    if (!loop) return;
    if (web_kick_pending_.exchange(1) != 0) return;
    WebKick* k = new WebKick;
    memset(&k->ev, 0, sizeof k->ev);
    k->ev.cb = webKickCb;
    k->ev.userdata = k;
    k->app = this;
    hloop_post_event(loop, &k->ev);
}

void AgentApp::webKickCb(hevent_t* ev) {
    WebKick* k = static_cast<WebKick*>(hevent_userdata(ev));
    AgentApp* app = k->app;
    delete k;
    if (!app) return;
    app->web_kick_pending_.store(0, std::memory_order_relaxed);
    app->refreshWebSnapshot();
}

std::string AgentApp::webSnapshot() {
    std::lock_guard<std::mutex> lk(web_mu_);
    if (web_body_.empty()) return "{\"ok\":false,\"error\":\"snapshot not ready\"}";
    return web_body_;
}

void AgentApp::sendSysinfo(int mode) {
    using nlohmann::json;
    json j = buildSysinfo();

    nlohmann::json sig;
    for (const char* k : {"version", "machine", "kernel", "cpu_model", "cpu_count", "board", "mem_total_kb", "swap_total_kb", "caps", "mtls", "cert_serial"}) {
        if (j.contains(k)) sig[k] = j[k];
    }
    sig["mounts"] = nlohmann::json::array();
    for (auto& m : j["mounts"]) sig["mounts"].push_back({{"mount", m["mount"]}, {"fstype", m["fstype"]}, {"total_kb", m["total_kb"]}});
    sig["nics"] = nlohmann::json::array();
    for (auto& n : j["nics"]) sig["nics"].push_back(n["name"]);
    std::string sigs = sig.dump();

    nlohmann::json out;
    if (mode == 1 && !sysinfo_sig.empty() && sigs == sysinfo_sig) {
        out["full"] = false;
        out["swap_free_kb"] = j["swap_free_kb"];
        // 概览页要实时 CPU/负载/内存，不能只在全量帧里带
        out["cpu_usage"] = j["cpu_usage"];
        out["cpu_cores"] = j["cpu_cores"];
        if (j.contains("loadavg")) out["loadavg"] = j["loadavg"];
        out["mem_free_kb"] = j["mem_free_kb"];
        out["mem_available_kb"] = j["mem_available_kb"];
        out["mounts"] = nlohmann::json::array();
        for (auto& m : j["mounts"]) out["mounts"].push_back({{"mount", m["mount"]}, {"used_kb", m["used_kb"]}, {"avail_kb", m["avail_kb"]}});
        out["nics"] = nlohmann::json::array();
        // 增量帧带上网卡全部字段：IP/状态会变，收发计数每次都在增长
        for (auto& n : j["nics"]) out["nics"].push_back(n);
        out["serial"] = j["serial"];
    }
    else {
        sysinfo_sig = sigs;
        j["full"] = true;
        out = j;
    }
    std::string body = out.dump();
    rtnp::Writer w;
    w.str(body.c_str());
    rtnp::send(io, rtnp::SYSINFO, 0, w.data, (uint16_t)w.len);
}

namespace {

uint64_t wallMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// 异步采集结果必须回到 loop 线程再写，避免连接已重连时往旧 io 发送
struct AsyncPost {
    hevent_t ev;
    AgentApp* app = nullptr;
    uint16_t type = rtnp::NET_INFO;
    uint32_t sid = 0;
    std::string body;
};

void asyncPostCb(hevent_t* ev) {
    AsyncPost* p = static_cast<AsyncPost*>(hevent_userdata(ev));
    if (p->app && p->app->io) {
        rtnp::Writer w;
        w.str(p->body.c_str());
        rtnp::send(p->app->io, p->type, p->sid, w.data, (uint16_t)w.len);
    }
    delete p;
}

// 工作线程把结果投递回 loop 线程发送：重连后 io 已换，不能直接写。
void postFrame(AgentApp* app, hloop_t* lp, uint8_t type, uint32_t sid, std::string body) {
    AsyncPost* t = new AsyncPost;
    memset(&t->ev, 0, sizeof t->ev);
    t->ev.cb = asyncPostCb;
    t->ev.userdata = t;
    t->app = app;
    t->type = type;
    t->sid = sid;
    t->body = std::move(body);
    hloop_post_event(lp, &t->ev);
}

// 大块结果（诊断包、目录树）：INFO 带元信息，DATA 按 32KB 顺序分块，END 收尾。
// 在工作线程里生成，回到 loop 线程发送，避免连接重连后写到旧 io。
struct ChunkPost {
    hevent_t ev;
    AgentApp* app = nullptr;
    uint32_t sid = 0;
    uint8_t info_type = 0;
    uint8_t data_type = 0;
    uint8_t end_type = 0;
    bool ok = false;
    std::string meta;
    std::string end;
    std::string body;
};

void chunkPostCb(hevent_t* ev) {
    ChunkPost* p = static_cast<ChunkPost*>(hevent_userdata(ev));
    hio_t* io = p->app ? p->app->io : nullptr;
    if (io) {
        rtnp::Writer w;
        w.str(p->meta.c_str());
        rtnp::send(io, p->info_type, p->sid, w.data, (uint16_t)w.len);
        if (p->ok && !p->body.empty()) {
            const size_t kChunk = rtnp::MAX_PAYLOAD;
            for (size_t off = 0; off < p->body.size(); off += kChunk) {
                size_t n = p->body.size() - off;
                if (n > kChunk) n = kChunk;
                rtnp::send(io, p->data_type, p->sid, p->body.data() + off, (uint16_t)n);
            }
        }
        rtnp::Writer e;
        e.str(p->end.c_str());
        rtnp::send(io, p->end_type, p->sid, e.data, (uint16_t)e.len);
    }
    delete p;
}

// 执行命令并抓 stdout；截断到 cap，命令本身用 timeout 兜底。
std::string diagRun(const std::string& cmd, size_t cap) {
    std::string out;
    FILE* fp = popen(cmd.c_str(), "r");
    if (!fp) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (out.size() >= cap) continue;
        size_t left = cap - out.size();
        out.append(buf, n < left ? n : left);
    }
    pclose(fp);
    return out;
}

bool diagWrite(const std::string& path, const std::string& data) {
    FILE* fp = fopen(path.c_str(), "w");
    if (!fp) return false;
    size_t n = fwrite(data.data(), 1, data.size(), fp);
    fclose(fp);
    return n == data.size();
}

} // namespace

// 指标点查询：relay 的 METRICS_QUERY 与本地页 /api/v1/metrics 共用。
// ring 在 loop 线程写、HTTP 线程读，用 series_mu_ 保护。
std::string AgentApp::metricPointsJson(const nlohmann::json& q) {
    using nlohmann::json;
    std::string name = q.value("series", "");
    long long since = q.value("since_ts", (long long)0);
    long long oldest = 0, newest = 0;
    json pts = json::array();
    bool truncated = false;
    {
        std::lock_guard<std::mutex> lk(series_mu_);
        auto it = series_rings.find(name);
        if (it != series_rings.end()) {
            for (auto& line : it->second) {
                auto p = json::parse(line, nullptr, false);
                if (p.is_discarded()) continue;
                long long ts = p.value("ts", (long long)0);
                if (!oldest) oldest = ts;
                newest = ts;
                if (since > 0 && ts <= since) continue;
                if (since > 0 && oldest > since && ts == oldest) truncated = true;
                pts.push_back(p);
            }
        }
    }
    json out;
    out["series"] = name;
    out["oldest_ts"] = oldest;
    out["newest_ts"] = newest;
    out["truncated"] = truncated;
    out["points"] = std::move(pts);
    return out.dump();
}

// 同步采集一帧网络快照。relay 的 NET_REQ 与本地页 /api/v1/net 共用，
// 会 fork lsipc，调用方必须放在工作线程。
std::string AgentApp::netSnapshotJson(const nlohmann::json& rj) {
    using nlohmann::json;
    rtnet::Options opt;
    opt.scope = rj.value("scope", std::string("all"));
    if (!rtnet::scopeValid(opt.scope)) opt.scope = "all";
    opt.with_proc = rj.value("proc", true);
    opt.page = rj.value("page", 1);
    opt.page_size = rj.value("page_size", 10);
    opt.q = rj.value("q", std::string());
    opt.listen_only = rj.value("listen", false);
    opt.utype = rj.value("utype", std::string("all"));
    opt.ipc_kind = rj.value("ipc_kind", std::string("shm"));
    bool refresh = rj.value("refresh", false);
    bool want_ipc = (opt.scope == "ipc" || opt.scope == "all");

    // IPC 采集要 fork 三次，30s 内复用上次结果；tcp/udp/unix 每次都实时读
    json cached;
    bool fresh = false;
    if (want_ipc && !refresh) {
        std::lock_guard<std::mutex> lk(net_mu_);
        if (!net_ipc_cache_.is_null() && wallMs() - net_ipc_ts_ < 30000) {
            cached = net_ipc_cache_;
            fresh = true;
        }
    }
    opt.with_ipc = want_ipc && !fresh;

    std::string err;
    json out = rtnet::collect(opt, err);
    if (!err.empty()) out["error"] = err;
    if (out.contains("ipc")) {
        json full = out["ipc"];
        {
            std::lock_guard<std::mutex> lk(net_mu_);
            net_ipc_cache_ = full;
            net_ipc_ts_ = wallMs();
        }
        out["ipc"] = rtnet::pageIpc(full, opt);
    }
    else if (fresh) {
        out["ipc"] = rtnet::pageIpc(cached, opt);
        if (out.contains("summary")) out["summary"]["ipc"] = cached.value("total", 0);
    }
    return out.dump();
}

void AgentApp::handleNetReq(const rtnp::Frame* f) {
    using nlohmann::json;
    uint32_t sid = f->sid;
    std::string req;
    if (f->len) {
        rtnp::Reader r(f->payload, f->len);
        req = r.strBytes();
    }
    json rj = json::parse(req, nullptr, false);
    if (rj.is_discarded() || !rj.is_object()) rj = json::object();

    // 采集要扫 /proc 与 fork lsipc，交给查询 worker 串行执行；队列满时直接拒绝
    if (!querySubmit([this, sid, rj]() {
            postFrame(this, loop, rtnp::NET_INFO, sid, netSnapshotJson(rj));
        })) {
        json e;
        e["error"] = "busy";
        e["scope"] = rj.value("scope", std::string("all"));
        e["summary"] = {{"tcp", -1}, {"udp", -1}, {"unix", -1}, {"ipc", -1}};
        sendJsonFrame(io, rtnp::NET_INFO, sid, e);
    }
}

// 同步取一页进程列表。relay 的 PROC_REQ 与本地页 /api/v1/procs 共用，
// 缓存未命中时要双采样 300ms，调用方必须放在工作线程。
std::string AgentApp::procsListJson(const nlohmann::json& rj) {
    using nlohmann::json;
    rtproc::Options opt;
    opt.sort = rj.value("sort", std::string("cpu"));
    opt.q = rj.value("q", std::string());
    opt.kernel = rj.value("kernel", false);
    opt.page = rj.value("page", 1);
    opt.page_size = rj.value("page_size", 10);
    bool force = rj.value("refresh", false); // 自动刷新/手动刷新要求重新采样

    // 预热缓存命中：用已有采样渲染，省掉 300ms 双采样
    uint64_t now_ms = wallMs();
    if (!force) {
        rtproc::Snapshot snap;
        bool hit = false;
        {
            std::lock_guard<std::mutex> lk(proc_cache_mu_);
            if (proc_cache_.ts && now_ms >= proc_cache_.ts && now_ms - proc_cache_.ts <= 35000) {
                snap = proc_cache_;
                hit = true;
            }
        }
        if (hit) {
            json out = rtproc::render(snap, opt, now_ms - snap.ts);
            out["op"] = "list";
            return out.dump();
        }
    }

    // 有 1s 以上的旧快照时，用两次快照的间隔算 CPU%，只采一次 /proc，
    // 既不把本次扫描开销当成 agent 的高 CPU，也省掉 300ms 等待。
    rtproc::Snapshot prev;
    bool diff = false;
    {
        std::lock_guard<std::mutex> lk(proc_cache_mu_);
        if (proc_cache_.ts && now_ms > proc_cache_.ts &&
            now_ms - proc_cache_.ts >= 1000 && now_ms - proc_cache_.ts <= 35000) {
            prev = proc_cache_;
            diff = true;
        }
    }

    rtproc::Snapshot snap;
    std::string err;
    json out;
    bool ok = diff ? rtproc::sample(snap, err, 0) : rtproc::sample(snap, err);
    if (ok) {
        if (diff) rtproc::diffCpu(snap, prev);
        out = rtproc::render(snap, opt, 0);
        {
            std::lock_guard<std::mutex> lk(proc_cache_mu_);
            proc_cache_ = std::move(snap);
        }
        warm_next_ms_.store(wallMs() + 30000);
    }
    else {
        out = json::object();
        out["error"] = err;
    }
    out["op"] = "list";
    return out.dump();
}

void AgentApp::handleProcReq(const rtnp::Frame* f) {
    using nlohmann::json;
    uint32_t sid = f->sid;
    std::string req;
    if (f->len) {
        rtnp::Reader r(f->payload, f->len);
        req = r.strBytes();
    }
    json rj = json::parse(req, nullptr, false);
    if (rj.is_discarded() || !rj.is_object()) rj = json::object();
    std::string op = rj.value("op", std::string("list"));

    // 信号与详情很快，直接在 loop 里做；列表要两次采样，放后台线程
    if (op == "signal") {
        int pid = rj.value("pid", 0);
        int sig = rj.value("signal", SIGTERM);
        std::string err;
        bool ok = rtproc::sendSignal(pid, sig, err);
        json out;
        out["op"] = "signal";
        out["ok"] = ok;
        out["pid"] = pid;
        out["signal"] = sig;
        if (!ok) out["error"] = err;
        sendJsonFrame(io, rtnp::PROC_INFO, sid, out);
        return;
    }

    if (op == "detail") {
        int pid = rj.value("pid", 0);
        std::string err;
        json out = rtproc::detail(pid, err);
        out["op"] = "detail";
        if (!err.empty()) out["error"] = err;
        sendJsonFrame(io, rtnp::PROC_INFO, sid, out);
        return;
    }

    if (!querySubmit([this, sid, rj]() {
            postFrame(this, loop, rtnp::PROC_INFO, sid, procsListJson(rj));
        })) {
        json e;
        e["op"] = "list";
        e["error"] = "busy";
        sendJsonFrame(io, rtnp::PROC_INFO, sid, e);
    }
}

void AgentApp::ensureWarmTimer() {
    if (warm_timer_started_) return;
    warm_timer_started_ = true;
    htimer_t* t = htimer_add(loop, warmTickCb, 5000, INFINITE);
    hevent_set_userdata(t, this);
}

void AgentApp::warmTickCb(htimer_t* t) {
    ((AgentApp*)hevent_userdata(t))->onWarmTick();
}

// 页面打开期间每 30s 预采样一次，proc 结果进缓存，服务列表刷新 Service 内部缓存。
void AgentApp::onWarmTick() {
    uint64_t now = wallMs();
    uint64_t until = warm_until_ms_.load(std::memory_order_relaxed);
    if (!until || now > until) return;
    if (now < warm_next_ms_.load(std::memory_order_relaxed)) return;
    int expect = 0;
    if (!warm_inflight_.compare_exchange_strong(expect, 1)) return;
    warm_next_ms_.store(now + 30000);

    // 服务列表排到 Service 自己的 worker；进程采样排到查询 worker，
    // 和 relay/本地页的请求共用同一个串行队列。
    if (svc_.ready()) {
        // 结果丢弃，只为把 Service 的列表缓存刷新到最新
        svc_.listAsync(rtvs::ListOptions(), [](const nlohmann::json&) {});
    }
    if (!querySubmit([this]() {
            rtproc::Snapshot snap;
            std::string err;
            if (rtproc::sample(snap, err)) {
                std::lock_guard<std::mutex> lk(proc_cache_mu_);
                proc_cache_ = std::move(snap);
            }
            warm_inflight_.store(0);
        })) {
        warm_inflight_.store(0);
    }
}

namespace {
struct WarmKick {
    hevent_t ev;
    AgentApp* app = nullptr;
};
} // namespace

// 本地页打开/轮询时续期预热。可从 HTTP 工作线程调用：
// 续期是原子写，预热与 timer 初始化投递回 loop 线程。
void AgentApp::requestWarm(uint64_t ttl_ms) {
    uint64_t now = wallMs();
    uint64_t until = now + ttl_ms;
    uint64_t old = warm_until_ms_.load(std::memory_order_relaxed);
    while (old < until &&
           !warm_until_ms_.compare_exchange_weak(old, until, std::memory_order_relaxed)) {
    }
    if (!loop || now < warm_next_ms_.load(std::memory_order_relaxed)) return;
    WarmKick* k = new WarmKick;
    memset(&k->ev, 0, sizeof k->ev);
    k->ev.cb = warmKickCb;
    k->ev.userdata = k;
    k->app = this;
    hloop_post_event(loop, &k->ev);
}

void AgentApp::warmKickCb(hevent_t* ev) {
    WarmKick* k = static_cast<WarmKick*>(hevent_userdata(ev));
    AgentApp* app = k->app;
    delete k;
    if (!app) return;
    app->ensureWarmTimer();
    app->onWarmTick();
}

// ---------------------------------------------------------------- 按需查询 worker
// 进程扫描（两次读 /proc）与网络快照（fork lsipc）都会阻塞，放在单个线程串行执行：
// 单核设备上不会出现多份扫描互相抢 CPU，线程数也固定为一个。
void AgentApp::queryStart() {
    if (query_thread_.joinable()) return;
    query_stop_.store(false);
    query_thread_ = std::thread([this]() { queryLoop(); });
}

void AgentApp::queryStop() {
    query_stop_.store(true);
    {
        std::lock_guard<std::mutex> lk(query_mu_);
        query_q_.clear();
    }
    query_cv_.notify_all();
    if (query_thread_.joinable()) query_thread_.join();
}

void AgentApp::queryLoop() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(query_mu_);
            query_cv_.wait(lk, [this]() { return query_stop_.load() || !query_q_.empty(); });
            if (query_stop_.load()) return;
            fn = std::move(query_q_.front());
            query_q_.pop_front();
        }
        fn();
    }
}

bool AgentApp::querySubmit(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(query_mu_);
        if (query_stop_.load() || query_q_.size() >= kQueryMaxQueue) return false;
        query_q_.push_back(std::move(fn));
    }
    query_cv_.notify_one();
    return true;
}

// 同步入口给本地 HTTP 用：任务仍在查询线程执行，调用线程只等结果，
// 这样 /api/v1/procs 与 relay 的 PROC_REQ 共用同一个并发上限。
std::string AgentApp::querySync(const std::function<std::string()>& fn) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string out;
    if (!querySubmit([&]() {
            std::string r = fn();
            {
                std::lock_guard<std::mutex> lk(m);
                out.swap(r);
                done = true;
            }
            cv.notify_one();
        })) {
        return std::string();
    }
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&]() { return done || query_stop_.load(); });
    return out;
}

// ---------------------------------------------------------------- 目录扫描 worker
// 整盘遍历在单核设备上要 1-2s，放查询队列会让同时打开的进程/网络页一起等，
// 因此单独一个线程，队列只保留少量请求。
void AgentApp::storageStart() {
    if (storage_thread_.joinable()) return;
    storage_stop_.store(false);
    storage_cancel_.store(false);
    storage_thread_ = std::thread([this]() { storageLoop(); });
}

void AgentApp::storageStop() {
    storage_stop_.store(true);
    storage_cancel_.store(true);
    {
        std::lock_guard<std::mutex> lk(storage_mu_);
        storage_q_.clear();
    }
    storage_cv_.notify_all();
    if (storage_thread_.joinable()) storage_thread_.join();
}

void AgentApp::storageLoop() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(storage_mu_);
            storage_cv_.wait(lk, [this]() { return storage_stop_.load() || !storage_q_.empty(); });
            if (storage_stop_.load()) return;
            fn = std::move(storage_q_.front());
            storage_q_.pop_front();
        }
        fn();
    }
}

bool AgentApp::storageSubmit(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(storage_mu_);
        if (storage_stop_.load() || storage_q_.size() >= kStorageMaxQueue) return false;
        storage_q_.push_back(std::move(fn));
    }
    storage_cv_.notify_one();
    return true;
}

std::string AgentApp::storageSync(const std::function<std::string()>& fn) {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    std::string out;
    if (!storageSubmit([&]() {
            std::string r = fn();
            {
                std::lock_guard<std::mutex> lk(m);
                out.swap(r);
                done = true;
            }
            cv.notify_one();
        })) {
        return std::string();
    }
    std::unique_lock<std::mutex> lk(m);
    cv.wait(lk, [&]() { return done || storage_stop_.load(); });
    return out;
}

// 扫描一个挂载点，返回 rtstorage 的 JSON。relay 的 STORAGE_REQ 与本地页共用。
std::string AgentApp::storageTreeJson(const nlohmann::json& req) {
    rtstorage::Options opt;
    opt.mount = req.value("mount", std::string("/"));
    opt.path = req.value("path", std::string());
    // depth 是相对 path 的回传层级：1 = 只回直接子项
    int depth = req.value("depth", 1);
    if (depth < 0) depth = 0;
    if (depth > 64) depth = 64;
    opt.emit_depth = depth;
    rtstorage::Result r;
    rtstorage::scan(opt, storage_cancel_, r);
    return rtstorage::toJson(r);
}

void AgentApp::handleStorageReq(const rtnp::Frame* f) {
    using nlohmann::json;
    uint32_t sid = f->sid;
    json rj = json::object();
    if (f->len) {
        rtnp::Reader r(f->payload, f->len);
        std::string req = r.strBytes();
        auto j = json::parse(req, nullptr, false);
        if (!j.is_discarded() && j.is_object()) rj = j;
    }
    if (!storageSubmit([this, sid, rj]() {
            std::string body = storageTreeJson(rj);
            json meta;
            json info = json::parse(body, nullptr, false);
            if (info.is_discarded() || !info.value("ok", false)) {
                meta["ok"] = false;
                meta["mount"] = rj.value("mount", std::string("/"));
                meta["path"] = rj.value("path", std::string());
                meta["error"] = info.is_discarded() ? std::string("bad scan result")
                                                    : info.value("error", std::string("scan failed"));
            }
            else {
                meta["ok"] = true;
                meta["mount"] = info.value("mount", rj.value("mount", std::string("/")));
                meta["path"] = info.value("path", rj.value("path", std::string()));
            }
            ChunkPost* p = new ChunkPost;
            memset(&p->ev, 0, sizeof p->ev);
            p->ev.cb = chunkPostCb;
            p->ev.userdata = p;
            p->app = this;
            p->sid = sid;
            p->info_type = rtnp::STORAGE_INFO;
            p->data_type = rtnp::STORAGE_DATA;
            p->end_type = rtnp::STORAGE_END;
            p->ok = meta.value("ok", false);
            if (p->ok) {
                // 整份响应（元信息 + nodes）压成 gzip 上传，relay 直接原样透传给浏览器。
                // 首字节标记：'\1' 压缩、'\0' 明文，和 metrics 帧保持一致。
                std::string plain = info.dump();
                std::string gz;
                if (gzipCompress(plain, gz, 6)) {
                    p->body.assign(1, '\1');
                    p->body += gz;
                    meta["gz"] = 1;
                }
                else {
                    p->body.assign(1, '\0');
                    p->body += plain;
                    meta["gz"] = 0;
                }
            }
            p->meta = meta.dump();
            json end;
            end["ok"] = p->ok;
            end["size"] = (long long)p->body.size();
            if (!p->ok) end["error"] = meta["error"];
            p->end = end.dump();
            hloop_post_event(loop, &p->ev);
        })) {
        json e;
        e["ok"] = false;
        e["error"] = "busy";
        e["mount"] = rj.value("mount", std::string("/"));
        e["path"] = rj.value("path", std::string());
        sendJsonFrame(io, rtnp::STORAGE_INFO, sid, e);
        sendJsonFrame(io, rtnp::STORAGE_END, sid, e);
    }
}

// B→C：设备详情页打开后按 ttl 续期预热（默认 120s，每 30s 采样一次）。
void AgentApp::handleWarmReq(const rtnp::Frame* f) {
    using nlohmann::json;
    uint64_t ttl = 120000;
    if (f->len) {
        rtnp::Reader r(f->payload, f->len);
        std::string req = r.strBytes();
        json j = json::parse(req, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            long long t = j.value("ttl_ms", 120000LL);
            if (t >= 30000 && t <= 600000) ttl = (uint64_t)t;
        }
    }
    uint64_t now = wallMs();
    uint64_t old = warm_until_ms_.exchange(now + ttl);
    ensureWarmTimer();
    if (!old || now > old) {
        // 首次或已过期：立即预热一轮，页面随后进来的请求就能命中
        warm_next_ms_.store(now);
        onWarmTick();
    }
}

namespace {
struct PowerAction {
    hevent_t ev;
    AgentApp* app = nullptr;
    std::string action;
};
} // namespace

// 电源动作延迟执行：先把 ACK 发出去，再重启/关机，避免调用方只看到断连。
void AgentApp::powerTimerCb(htimer_t* t) {
    PowerAction* p = static_cast<PowerAction*>(hevent_userdata(t));
    std::string action = p ? p->action : std::string();
    delete p;
    if (action == "poweroff")
        logf_("[agent] poweroff requested via relay");
    else
        logf_("[agent] reboot requested via relay");
    // systemctl 会正常停服务；没有 systemd 时退回 syscall
    int rc = system(action == "poweroff" ? "systemctl --no-block poweroff" : "systemctl --no-block reboot");
    if (rc != 0) {
        sync();
        reboot(action == "poweroff" ? RB_POWER_OFF : RB_AUTOBOOT);
    }
}

// 校验本机 root 密码。用 /etc/shadow 的哈希 + crypt_r，不依赖 PAM，
// 这样静态/裁剪镜像也能用；密码只在内存里存在，不写日志。
bool AgentApp::verifyRootPassword(const std::string& password, std::string& err) {
    struct spwd spbuf;
    struct spwd* sp = nullptr;
    char buf[4096];
    if (getspnam_r("root", &spbuf, buf, sizeof buf, &sp) != 0 || !sp) {
        err = "无法读取 root 账户";
        return false;
    }
    std::string hash = sp->sp_pwdp ? sp->sp_pwdp : "";
    if (hash.empty()) {
        err = "设备未设置 root 密码，无法远程重启/关机";
        return false;
    }
    if (hash[0] == '!' || hash[0] == '*') {
        err = "root 账户已锁定";
        return false;
    }
    struct crypt_data data;
    memset(&data, 0, sizeof data);
    char* enc = crypt_r(password.c_str(), hash.c_str(), &data);
    if (!enc) {
        err = "密码校验失败";
        return false;
    }
    if (hash != enc) {
        err = "root 密码错误";
        return false;
    }
    return true;
}

void AgentApp::handlePowerReq(const rtnp::Frame* f) {
    using nlohmann::json;
    json rj = json::object();
    if (f->len) {
        rtnp::Reader r(f->payload, f->len);
        std::string req = r.strBytes();
        if (!r.err) {
            auto j = json::parse(req, nullptr, false);
            if (!j.is_discarded() && j.is_object()) rj = j;
        }
    }
    std::string action = rj.value("action", std::string());
    std::string password = rj.value("password", std::string());
    long long delay = rj.value("delay_ms", 1000LL);
    if (delay < 0) delay = 0;
    if (delay > 600000) delay = 600000;

    json ack;
    ack["action"] = action;
    ack["delay_ms"] = delay;
    uint64_t now = wallMs();
    if (action != "reboot" && action != "poweroff") {
        ack["ok"] = false;
        ack["error"] = "action must be reboot or poweroff";
    }
    else if (action == "reboot" ? !allow_reboot : !allow_poweroff) {
        ack["ok"] = false;
        ack["error"] = action == "reboot" ? "reboot disabled" : "poweroff disabled";
    }
    else if (now < power_lock_until_ms_) {
        ack["ok"] = false;
        ack["error"] = "too many password failures, retry later";
        ack["retry_ms"] = (long long)(power_lock_until_ms_ - now);
    }
    else if (password.empty() || password.size() > 256) {
        ack["ok"] = false;
        ack["error"] = "root password required";
    }
    else {
        std::string verr;
        if (!verifyRootPassword(password, verr)) {
            // 连续 5 次失败冷却 5 分钟，避免把接口当成在线爆破入口
            if (++power_fail_ >= 5) {
                power_fail_ = 0;
                power_lock_until_ms_ = now + 300000;
            }
            ack["ok"] = false;
            ack["error"] = verr;
        }
        else {
            power_fail_ = 0;
            ack["ok"] = true;
            PowerAction* p = new PowerAction;
            memset(&p->ev, 0, sizeof p->ev);
            p->app = this;
            p->action = action;
            htimer_t* t = htimer_add(loop, powerTimerCb, (uint32_t)delay, 1);
            hevent_set_userdata(t, p);
            logf_("[agent] power %s scheduled in %lldms", action.c_str(), delay);
        }
    }
    sendJsonFrame(io, rtnp::POWER_ACK, f->sid, ack);
}

// 诊断包：系统信息 + 内核日志 + journal + 网络 + 服务 + 进程 + 配置。
// 输出 tar.gz，大小上限 8MB；整个过程在工作线程执行，不阻塞 loop。
std::string AgentApp::buildDiagBundle(const nlohmann::json& req, std::string& name, std::string& sha,
                                      std::string& err) {
    int max_mb = req.value("max_mb", 8);
    if (max_mb <= 0 || max_mb > 32) max_mb = 8;
    char tmpl[] = "/tmp/rtunnel-diag.XXXXXX";
    char* dir = mkdtemp(tmpl);
    if (!dir) {
        err = "mkdtemp failed";
        return std::string();
    }
    std::string d(dir);
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char stamp[32];
    snprintf(stamp, sizeof stamp, "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1,
             tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    name = std::string("diag-") + devid + "-" + stamp + ".tar.gz";

    diagWrite(d + "/version.txt",
              std::string("version: ") + hv_version() + "\nargs: " + args_line + "\ndevid: " + devid + "\n");
    diagWrite(d + "/sysinfo.json", webSnapshot());
    diagWrite(d + "/dmesg.txt", diagRun("timeout 5 dmesg 2>/dev/null | tail -n 300", 1 << 20));
    diagWrite(d + "/journal.txt",
              diagRun("timeout 10 journalctl -n 500 --no-pager 2>/dev/null", 3 << 20));
    diagWrite(d + "/network.txt",
              diagRun("timeout 5 sh -c 'ip addr 2>&1; echo; ip route 2>&1; echo; ss -s 2>&1'", 1 << 20));
    diagWrite(d + "/services.txt",
              diagRun("timeout 10 systemctl list-units --type=service --all --no-pager 2>/dev/null", 1 << 20));
    if (!config_file.empty()) {
        std::ifstream cf(config_file);
        if (cf) {
            std::string cfg((std::istreambuf_iterator<char>(cf)), std::istreambuf_iterator<char>());
            diagWrite(d + "/agent.json", cfg);
        }
    }
    {
        nlohmann::json q;
        q["sort"] = "cpu";
        q["page"] = 1;
        q["page_size"] = 30;
        q["kernel"] = false;
        q["refresh"] = true;
        diagWrite(d + "/procs.json", procsListJson(q));
    }
    {
        nlohmann::json q;
        q["scope"] = "all";
        q["proc"] = true;
        q["page"] = 1;
        q["page_size"] = 50;
        q["refresh"] = true;
        diagWrite(d + "/net.json", netSnapshotJson(q));
    }

    std::string out = d + "/../" + name;
    std::string cmd = "timeout 30 tar -czf '" + out + "' -C '" + d + "' . 2>/dev/null";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        err = "tar failed";
        cmd = "rm -rf '" + d + "'";
        if (system(cmd.c_str()) != 0) { /* 清理失败不影响结果 */ }
        return std::string();
    }

    std::string body;
    {
        std::ifstream f(out, std::ios::binary);
        if (!f) {
            err = "open bundle failed";
        }
        else {
            f.seekg(0, std::ios::end);
            std::streamoff sz = f.tellg();
            f.seekg(0, std::ios::beg);
            if (sz <= 0) err = "empty bundle";
            else if (sz > (std::streamoff)max_mb * 1024 * 1024) err = "bundle too large";
            else {
                body.resize((size_t)sz);
                f.read(&body[0], sz);
                if (!f) {
                    body.clear();
                    err = "read bundle failed";
                }
            }
        }
    }
    if (!body.empty()) sha = sha256File(out);
    unlink(out.c_str());
    cmd = "rm -rf '" + d + "'";
    if (system(cmd.c_str()) != 0) { /* 清理失败不影响结果 */ }
    return body;
}

void AgentApp::handleDiagReq(const rtnp::Frame* f) {
    using nlohmann::json;
    uint32_t sid = f->sid;
    json rj = json::object();
    if (f->len) {
        rtnp::Reader r(f->payload, f->len);
        std::string req = r.strBytes();
        auto j = json::parse(req, nullptr, false);
        if (!j.is_discarded() && j.is_object()) rj = j;
    }
    if (!querySubmit([this, sid, rj]() {
            std::string name, sha, err;
            std::string body = buildDiagBundle(rj, name, sha, err);
            ChunkPost* p = new ChunkPost;
            memset(&p->ev, 0, sizeof p->ev);
            p->ev.cb = chunkPostCb;
            p->ev.userdata = p;
            p->app = this;
            p->sid = sid;
            p->info_type = rtnp::DIAG_INFO;
            p->data_type = rtnp::DIAG_DATA;
            p->end_type = rtnp::DIAG_END;
            json meta;
            if (body.empty()) {
                meta["ok"] = false;
                meta["error"] = err.empty() ? std::string("diag failed") : err;
                p->ok = false;
            }
            else {
                meta["ok"] = true;
                meta["name"] = name;
                meta["size"] = (long long)body.size();
                meta["sha256"] = sha;
                p->ok = true;
                p->body = std::move(body);
            }
            p->meta = meta.dump();
            json end;
            end["ok"] = p->ok;
            end["size"] = (long long)p->body.size();
            end["sha256"] = sha;
            if (!p->ok) end["error"] = meta["error"];
            p->end = end.dump();
            hloop_post_event(loop, &p->ev);
        })) {
        json e;
        e["ok"] = false;
        e["error"] = "busy";
        sendJsonFrame(io, rtnp::DIAG_INFO, sid, e);
        sendJsonFrame(io, rtnp::DIAG_END, sid, e);
    }
}

void AgentApp::onFrame(rtnp::Frame* f) {
    switch (f->type) {
    case rtnp::SYSINFO_REQ: {
        int mode = 0;
        if (f->len > 0) {
            rtnp::Reader r(f->payload, f->len);
            mode = r.u8();
        }
        sendSysinfo(mode);
        return;
    }
    case rtnp::CONFIG_SET: handleConfigSet(f); return;
    case rtnp::FILE_BEGIN:
    case rtnp::FILE_DATA:
    case rtnp::FILE_END: handleFileFrame(f); return;
    case rtnp::OTA_APPLY:
    case rtnp::OTA_CANCEL: handleOtaApply(f); return;
    case rtnp::NET_REQ: handleNetReq(f); return;
    case rtnp::PROC_REQ: handleProcReq(f); return;
    case rtnp::WARM_REQ: handleWarmReq(f); return;
    case rtnp::POWER_REQ: handlePowerReq(f); return;
    case rtnp::DIAG_REQ: handleDiagReq(f); return;
    case rtnp::STORAGE_REQ: handleStorageReq(f); return;
    case rtnp::TTY_LIST_REQ: {
        nlohmann::json arr = nlohmann::json::array();
        DIR* d = opendir("/dev");
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) != NULL) {
                std::string n = e->d_name;
                if (n.rfind("ttyUSB", 0) != 0 && n.rfind("ttyACM", 0) != 0 && n.rfind("ttyS", 0) != 0 && n.rfind("ttySTM", 0) != 0) continue;
                nlohmann::json j;
                j["path"] = "/dev/" + n;
                arr.push_back(j);
            }
            closedir(d);
        }
        DIR* b = opendir("/dev/serial/by-id");
        if (b) {
            struct dirent* e;
            while ((e = readdir(b)) != NULL) {
                std::string n = e->d_name;
                if (n == "." || n == "..") continue;
                nlohmann::json j;
                j["path"] = "/dev/serial/by-id/" + n;
                arr.push_back(j);
            }
            closedir(b);
        }
        std::string out = arr.dump();
        rtnp::Writer w;
        w.str(out.c_str());
        rtnp::send(io, rtnp::TTY_LIST_ACK, f->sid, w.data, (uint16_t)w.len);
        return;
    }
    case rtnp::TTY_CFG_SET: {
        rtnp::Reader cr(f->payload, f->len);
        static char cbuf[64 * 1024];
        cr.str(cbuf, sizeof cbuf);
        if (cr.err) return;
        auto arr = nlohmann::json::parse(cbuf, nullptr, false);
        if (arr.is_discarded() || !arr.is_array()) return;
        tty_cfgs_.clear();
        if (verbose) logf_("[agent] TTY_CFG_SET n=%zu", arr.size());
        for (auto& j : arr) {
            if (!j.is_object()) continue;
            std::string path = j.value("path", "");
            if (path.empty()) continue;
            TtyCfg c;
            c.enabled = j.value("enabled", true) ? 1 : 0;
            c.baud = j.value("baud", 115200);
            c.data_bits = j.value("data_bits", 8);
            std::string par = j.value("parity", "N");
            c.parity = par.empty() ? 'N' : par[0];
            c.stop_bits = j.value("stop_bits", 1);
            tty_cfgs_[path] = c;
            if (verbose) logf_("[agent] tty cfg %s enabled=%d", path.c_str(), c.enabled);
        }
        return;
    }
    case rtnp::METRICS_CFG: {
        rtnp::Reader mc(f->payload, f->len);
        static char mb[16384];
        mc.str(mb, sizeof mb);
        if (mc.err) return;
        auto cj = nlohmann::json::parse(mb, nullptr, false);
        if (cj.is_discarded() || !cj.is_object() || !cj.contains("series")) return;
        series_revision = cj.value("revision", 0);
        series_cfgs.clear();
        for (auto& r : cj["series"]) {
            if (!r.is_object()) continue;
            std::string name = r.value("series", "");
            if (name.empty()) continue;
            SeriesCfg c;
            c.enabled = r.value("enabled", true) ? 1 : 0;
            c.interval = r.value("interval_sec", 5);
            c.points = r.value("points", 720);
            if (c.interval < 1) c.interval = 1;
            if (c.interval > 300) c.interval = 300;
            if (c.points < 60) c.points = 60;
            if (c.points > 86400) c.points = 86400;
            series_cfgs[name] = c;
            {
                std::lock_guard<std::mutex> lk(series_mu_);
                auto it = series_rings.find(name);
                if (it != series_rings.end()) {
                    while ((int)it->second.size() > c.points) it->second.pop_front();
                }
            }
        }
        // 采样配置属于能力配置，改完立即落盘，重启后仍生效
        bool persisted = saveConfig();
        nlohmann::json st;
        st["applied"] = true;
        st["revision"] = series_revision;
        st["series"] = (int)series_cfgs.size();
        st["persisted"] = persisted;
        std::string s2 = st.dump();
        rtnp::Writer sw;
        sw.str(s2.c_str());
        rtnp::send(io, rtnp::METRICS_STATUS, 0, sw.data, (uint16_t)sw.len);
        return;
    }
    case rtnp::METRICS_QUERY: {
        rtnp::Reader qr(f->payload, f->len);
        static char qb[4096];
        qr.str(qb, sizeof qb);
        if (qr.err) return;
        auto q = nlohmann::json::parse(qb, nullptr, false);
        if (q.is_discarded() || !q.is_object()) return;
        std::string os = metricPointsJson(q);
        if (q.value("since_ts", (long long)0) == 0) {
            std::string gz;
            if (gzipCompress(os, gz))
                os = std::string(1, '\1') + gz;
            else
                os = std::string(1, '\0') + os;
        }
        else {
            os = std::string(1, '\0') + os;
        }
        rtnp::Writer ow;
        ow.strn(os.data(), os.size());
        rtnp::send(io, rtnp::METRICS_DATA, f->sid, ow.data, (uint16_t)ow.len);
        return;
    }
    case rtnp::FILE_CHECK_REQ: {
        rtnp::Reader r(f->payload, f->len);
        char dir[256] = {0}, name[256] = {0};
        r.str(dir, sizeof(dir));
        r.str(name, sizeof(name));
        nlohmann::json j;
        j["exists"] = false;
        j["size"] = 0;
        j["sha256"] = "";
        std::string target = file_dir;
        if (dir[0]) target += "/" + std::string(dir);
        std::string path = target + "/" + name;
        bool ok = !r.err && name[0] && !strstr(name, "..") && !strchr(name, '/') && !strstr(dir, "..") && dir[0] != '/';
        struct stat st {};
        if (ok && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
            j["exists"] = true;
            j["size"] = (long long)st.st_size;
            j["sha256"] = sha256File(path);
        }
        std::string out = j.dump();
        rtnp::Writer w;
        w.str(out.c_str());
        rtnp::send(io, rtnp::FILE_CHECK_ACK, f->sid, w.data, (uint16_t)w.len);
        return;
    }
    case rtnp::OPEN: handleOpen(f); return;
    case rtnp::DATA: {
        LocalStream* s = streamFind(f->sid);
        if (s == NULL) return;
        if (s->is_svc) {
            if (f->len > 0 && s->svc_session) {
                s->svc_session->onText(std::string((const char*)f->payload + 1, (size_t)f->len - 1));
            }
            return;
        }
        if (s->is_fs) {
            if (f->len > 0 && s->fs_session) {
                bool binary = f->payload[0] != 0;
                std::string msg((const char*)f->payload + 1, (size_t)f->len - 1);
                if (binary)
                    s->fs_session->onBinary(msg);
                else
                    s->fs_session->onText(msg);
            }
            // 请求都很小，收到即回窗口
            if (io && state == 2 && f->len > 0) {
                rtnp::Writer w;
                w.u32(f->len);
                rtnp::send(io, rtnp::WINDOW, s->sid, w.data, (uint16_t)w.len);
            }
            return;
        }
        if (s->local == NULL) return;
        if (f->len > 0) {
            hio_write(s->local, f->payload, f->len);
            s->unacked += f->len;
            teeToMonitors(this, s, (const char*)f->payload, f->len);
        }
        maybeGrantWindow(s);
        return;
    }
    case rtnp::WINDOW: {
        LocalStream* s = streamFind(f->sid);
        if (s == NULL) return;
        rtnp::Reader r(f->payload, f->len);
        uint32_t inc = r.u32();
        if (r.err) return;
        if (s->is_svc) return;   // 服务管理只有小 JSON，不做流控
        if (s->is_fs) {
            if (s->fs_session) s->fs_session->addCredit(inc);
            return;
        }
        s->credit += inc;
        if (s->credit > rtnp::DEFAULT_CREDIT) s->credit = rtnp::DEFAULT_CREDIT;
        flushPending(s);
        return;
    }
    case rtnp::CLOSE: {
        LocalStream* s = streamFind(f->sid);
        if (s == NULL) return;
        if (s->is_fs) {
            // 回 CLOSE，让 relay 把这条会话从 c/a 两侧的流表移除
            if (io && state == 2 && !hio_is_closed(io)) {
                rtnp::send(io, rtnp::CLOSE, s->sid, NULL, 0);
            }
            streamFree(s);
            return;
        }
        s->remote_closed = 1;
        if (s->is_shell) {
            // pty 无 SHUT_WR 语义：对端 EOF = 结束 shell 子进程
            killShellChild(s);
        }
        else if (s->local && hio_write_is_complete(s->local)) {
            shutdown(hio_fd(s->local), SHUT_WR);
            s->want_shutdown = 0;
        }
        else {
            s->want_shutdown = 1;
            if (s->local) hio_setcb_write(s->local, localWriteCb);
        }
        if (s->local_eof)
            streamFree(s);
        else
            armCloseTimer(s);
        return;
    }
    case rtnp::RESIZE: {
        LocalStream* s = streamFind(f->sid);
        if (s == NULL || !s->is_shell || s->local == NULL) return;
        rtnp::Reader r(f->payload, f->len);
        uint16_t cols = r.u16();
        uint16_t rows = r.u16();
        if (r.err || cols == 0 || rows == 0) return;
        struct winsize wsz = {(unsigned short)rows, (unsigned short)cols, 0, 0};
        ioctl(hio_fd(s->local), TIOCSWINSZ, &wsz);
        return;
    }
    case rtnp::RESET: {
        LocalStream* s = streamFind(f->sid);
        if (s) streamFree(s);
        return;
    }
    default: return;
    }
}

void AgentApp::onLinkDown() {
    // 关闭所有本地连接
    while (streams) {
        LocalStream* s = streams;
        streams = s->next;
        if (s->close_timer) htimer_del(s->close_timer);
        if (s->fs_session) {
            s->fs_session->onClose();
            delete s->fs_session;
        }
        if (s->svc_session) {
            s->svc_session->onClose();
            delete s->svc_session;
        }
        if (s->local && !hio_is_closed(s->local)) hio_close(s->local);
        delete s;
    }
}

// ---------------------------------------------------------------- 入口

int AgentApp::run(int argc, char** argv) {
    started_ms_ = agentMonotonicMs();
    for (int i = 0; i < argc; ++i) {
        if (i) args_line += " ";
        args_line += argv[i];
    }
    saved_argv.assign(argv, argv + argc);
#ifdef RTUNNEL_AGENT_WEB
    // 合并 lite/web 为同一个 unit 后，带 Web 的构建默认开启本地页；
    // 配置文件里的 local_web 可以改地址或设为 false 关闭。
    web_listen = "127.0.0.1:8080";
    web_enabled = 1;
#endif
    {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            self_path = buf;
        }
    }
    {
        const char* pfx = getenv("RTUNNEL_AGENT_PREFIX");
        app_prefix_ = (pfx && pfx[0]) ? pfx : "/opt/rtunnel/agent";
    }
    std::vector<std::string> servers;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            servers.push_back(argv[++i]);
        }
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--devid") == 0 && i + 1 < argc) {
            snprintf(devid, sizeof(devid), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--allow") == 0 && i + 1 < argc) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char* save = NULL;
            for (char* p = strtok_r(buf, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
                if (nallow < (int)(sizeof(allow_ports) / sizeof(int))) {
                    allow_ports[nallow++] = atoi(p);
                }
            }
        }
        else if (strcmp(argv[i], "--allow-all") == 0) {
            allow_all = 1;
        }
        else if (strcmp(argv[i], "--config-file") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        }
        else if (strcmp(argv[i], "--file-dir") == 0 && i + 1 < argc) {
            file_dir = argv[++i];
        }
        else if (strcmp(argv[i], "--allow-tty") == 0 && i + 1 < argc) {
            snprintf(allow_tty, sizeof(allow_tty), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--allow-shell") == 0) {
            allow_shell = 1;
        }
        else if (strcmp(argv[i], "--allow-ota") == 0) {
            allow_ota = 1;
        }
        else if (strcmp(argv[i], "--local-web") == 0) {
#ifdef RTUNNEL_AGENT_WEB
            if (i + 1 < argc && argv[i + 1][0] != '-')
                web_listen = argv[++i];
            else
                web_listen = "127.0.0.1:8080";
            web_enabled = 1;
#else
            // 老 web unit 会传 --local-web；lite 包升级到 web 设备时忽略而不是退出，
            // 否则服务会 crash loop，设备再也无法通过 relay 完成下一次 OTA。
            if (i + 1 < argc && argv[i + 1][0] != '-') ++i;
            fprintf(stderr, "[agent] --local-web ignored: binary built without local web\n");
#endif
        }
        else if (strcmp(argv[i], "--file-root") == 0 && i + 1 < argc) {
            file_root = argv[++i];
        }
        else if (strcmp(argv[i], "--file-rw") == 0) {
            file_rw = 1;
        }
        else if (strcmp(argv[i], "--file-ro") == 0) {
            file_rw = 0;
        }
        else if (strcmp(argv[i], "--file-max-mb") == 0 && i + 1 < argc) {
            file_max_mb = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--file-token") == 0 && i + 1 < argc) {
            file_token = argv[++i];
        }
        else if (strcmp(argv[i], "--svc-rw") == 0) {
            svc_rw = 1;
        }
        else if (strcmp(argv[i], "--svc-ro") == 0) {
            svc_rw = 0;
        }
        else if (strcmp(argv[i], "--no-svc") == 0) {
            svc_enabled = 0;
        }
        else if (strcmp(argv[i], "--svc-token") == 0 && i + 1 < argc) {
            svc_token = argv[++i];
        }
        else if (strcmp(argv[i], "--version") == 0) {
            printf("%s\n", hv_version());
            return 0;
        }
        else if (strcmp(argv[i], "--max-idle") == 0 && i + 1 < argc) {
            max_idle = atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--tls-ca") == 0 && i + 1 < argc) {
            snprintf(tls_ca, sizeof(tls_ca), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            snprintf(tls_cert, sizeof(tls_cert), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            snprintf(tls_key, sizeof(tls_key), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--tls-sni") == 0 && i + 1 < argc) {
            snprintf(tls_sni, sizeof(tls_sni), "%s", argv[++i]);
        }
        else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        }
        else {
            fprintf(stderr,
                    "Usage: %s --devid ID [--server H[:P] ...] [--port P] [--allow 22,80] [--allow-all]\n"
                    "          [--config-file FILE] [--allow-shell [--max-idle SEC]] [--allow-ota]\n"
                    "          [--allow-tty /dev/ttyS0,/dev/ttyS1] [--verbose]\n"
#ifdef RTUNNEL_AGENT_WEB
                    "          [--local-web [ADDR:]PORT]\n"
#endif
                    "          [--file-dir DIR] [--file-root DIR] [--file-rw|--file-ro] [--file-max-mb N] [--file-token T]\n"
                    "          [--svc-rw|--svc-ro] [--no-svc] [--svc-token T]\n"
                    "       --server 可重复或逗号分隔，故障时按顺序自动切换（最多 8 个）\n",
                    argv[0]);
            return 1;
        }
    }
    if (devid[0] == '\0') {
        fprintf(stderr, "--devid is required\n");
        return 1;
    }

    {
        std::string spec, err;
        for (auto& s : servers) {
            if (!spec.empty()) spec += ",";
            spec += s;
        }
        if (spec.empty()) spec = "127.0.0.1";
        if (!tunnel_parse_endpoints(spec, port, endpoints, err)) {
            fprintf(stderr, "--server: %s\n", err.c_str());
            return 1;
        }
        host = endpoints[0].host.c_str();
    }

    hloop_t* lp = hloop_new(0);
    last_active_ms = hloop_now_ms(lp);
    if (max_idle > 0) {
        agentSampleCpu(this);
        htimer_t* cpu_timer = htimer_add(
            lp, [](htimer_t* t) { agentSampleCpu((AgentApp*)hevent_userdata(t)); }, 1000, INFINITE);
        hevent_set_userdata(cpu_timer, this);
        htimer_t* idle_timer = htimer_add(
            lp,
            [](htimer_t* t) {
                auto* app = (AgentApp*)hevent_userdata(t);
                uint64_t now = hloop_now_ms(app->loop);
                if (app->streams != NULL) {
                    app->last_active_ms = now;
                    app->had_stream = 1;
                    app->idle_since_ms = 0;
                    return;
                }
                if (app->had_stream) {
                    // 按需 agent：最后一个会话结束后 2s 退出
                    if (!app->idle_since_ms) app->idle_since_ms = now;
                    if (now - app->idle_since_ms >= 2000) {
                        logf_("[agent] no sessions, exiting (on-demand agent)");
                        hloop_stop(app->loop);
                    }
                    return;
                }
                if (now - app->last_active_ms >= (uint64_t)app->max_idle * 1000) {
                    logf_("[agent] idle %ds, exiting (on-demand agent)", app->max_idle);
                    hloop_stop(app->loop);
                }
            },
            1000, INFINITE);
        hevent_set_userdata(idle_timer, this);
    }
    htimer_t* part_timer = htimer_add(
        lp,
        [](htimer_t* t) {
            auto* app = (AgentApp*)hevent_userdata(t);
            app->cleanupParts(app->file_dir, time(NULL) - 24 * 3600);
        },
        3600 * 1000, INFINITE);
    hevent_set_userdata(part_timer, this);
    loadConfig();
    if (otaUnderSystemd() && hasOtaPending()) {
        logf_("[agent] ota pending: waiting for relay before commit");
        htimer_t* ota_wd = htimer_add(
            lp,
            [](htimer_t* t) {
                auto* app = (AgentApp*)hevent_userdata(t);
                if (!app->ota_connected_) {
                    logf_("[agent] ota: relay not reached in 45s, exiting for boot-check rollback");
                    _exit(1);
                }
            },
            45000, 1);
        hevent_set_userdata(ota_wd, this);
    }
    {
        std::string ferr;
        int frc = applyFileConfig(file_root, file_rw, file_max_mb, file_enabled, ferr);
        if (!file_enabled)
            logf_("[agent] file service disabled by config");
        else if (frc != 0)
            logf_("[agent] file service disabled: %s", ferr.c_str());
        else
            logf_("[agent] file root %s (%s)", fs_.root().c_str(), fs_.writable() ? "rw" : "ro");
    }
    svc_.setEnabled(svc_enabled != 0);
    svc_.setWritable(svc_rw != 0);
    svc_.setToken(svc_token);
    if (!svc_enabled)
        logf_("[agent] service manager disabled");
    else if (!svc_.systemd())
        logf_("[agent] service manager disabled: systemd not running");
    else
        logf_("[agent] service manager ready (%s)", svc_.writable() ? "rw" : "ro");
    init(lp);
#ifdef RTUNNEL_AGENT_WEB
    if (web_enabled) {
        // 本地页常驻设备：启动后先预热进程/服务缓存，
        // 首个打开页面的人不必等 systemctl 首次全量扫描
        requestWarm(60000);
        // 先产出一份快照再起 HTTP，避免首屏拿到 "not ready"
        refreshWebSnapshot();
        std::string werr;
        if (agentWebStart(this, web_listen, werr) != 0) {
            // 本地页是辅助能力，端口被占用时不拖垮隧道/OTA
            fprintf(stderr, "[agent] local web disabled: %s\n", werr.c_str());
            web_enabled = 0;
            web_listen.clear();
        }
        else {
            logf_("[agent] local web on %s", web_listen.c_str());
            htimer_t* web_timer = htimer_add(
                lp, [](htimer_t* t) { ((AgentApp*)hevent_userdata(t))->refreshWebSnapshot(); }, 2000, INFINITE);
            hevent_set_userdata(web_timer, this);
        }
    }
#endif
    if (series_cfgs.empty()) {
        SeriesCfg def;
        def.enabled = 1;
        def.interval = 5;
        def.points = 720;
        series_cfgs["cpu"] = def;
        series_cfgs["mem"] = def;
        series_cfgs["load"] = def;
    }
    htimer_t* series_timer = htimer_add(
        lp,
        [](htimer_t* t) {
            auto* app = (AgentApp*)hevent_userdata(t);
            app->seriesTick();
        },
        1000, INFINITE);
    hevent_set_userdata(series_timer, this);
    queryStart();
    storageStart();
    hloop_run(lp);
    storageStop();
    queryStop();
#ifdef RTUNNEL_AGENT_WEB
    if (web_enabled) agentWebStop();
#endif
    fs_.close();
    hloop_free(&lp);
    return 0;
}
