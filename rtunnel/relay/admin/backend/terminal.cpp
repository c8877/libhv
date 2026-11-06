// Web 终端：浏览器 xterm.js ↔ WebSocket ↔ pty(ssh) ↔ A 子进程(rtunnel-client) ↔ 设备 sshd。
// 密码走 SSH 交互式提示（xterm 内输入），admin 不经手、不落盘。
// 每次终端 = 一个 A 子进程（按需 spawn，关闭回收）+ 一个 ssh 子进程（forkpty）。
#include "admin_server.hpp"
#include "common/logging.hpp"
#include "EventLoop.h"

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <execinfo.h>

#include <pty.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>

struct TerminalSession {
    AdminServer*         admin = nullptr;
    WebSocketChannelPtr  ws;
    hio_t*               pty_io = nullptr;
    pid_t                ssh_pid = -1;
    pid_t                agent_pid = -1;
    std::string          devid;
    std::string          user;
    int                  fwd_port = 0;
    bool                 closed = false;
    // direct（RTNP client 直连 relay，免 ssh/密码，走 agent 的 OPEN_SHELL）
    bool                 direct = false;
    hio_t*               link_io = nullptr;
    rtnp::Decoder        in;
    uint32_t             sid = 0;
    rtnp::Buffer         pending_input;   // OPEN_OK 前的输入先缓冲，避免 DATA sid=0 被 RESET
};

static hloop_t* adminLoop(AdminServer* adm);

// ---------------------------------------------------------------- 清理

static void sessionClose(TerminalSession* s) {
    if (s->closed) return;
    s->closed = true;
    if (s->pty_io) {
        hio_close(s->pty_io);
        s->pty_io = nullptr;
    }
    if (s->link_io) {
        hio_close(s->link_io);
        s->link_io = nullptr;
    }
    if (s->ssh_pid > 0) {
        kill(s->ssh_pid, SIGHUP);
        waitpid(s->ssh_pid, NULL, WNOHANG);
    }
    if (s->agent_pid > 0) {
        kill(s->agent_pid, SIGTERM);
        waitpid(s->agent_pid, NULL, WNOHANG);
    }
    if (s->admin) {
        s->admin->auditPublic("operator", "terminal.close", s->devid);
    }
}

// ---------------------------------------------------------------- pty ↔ ws

static void onPtyRead(hio_t* io, void* buf, int readbytes) {
    auto* s = (TerminalSession*)hio_context(io);
    if (s == NULL || readbytes <= 0) return;
    // 终端输出原样转发（UTF-8 文本帧）
    s->ws->send(std::string((const char*)buf, (size_t)readbytes));
}

static void onPtyClose(hio_t* io) {
    auto* s = (TerminalSession*)hio_context(io);
    if (s == NULL) return;
    // ssh 退出：通知前端并关闭 ws（onWsClose 里做资源清理）
    if (s->ws && s->ws->isConnected()) s->ws->close();
}

// ---------------------------------------------------------------- A 子进程 + ssh

static int allocFreePort() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 16022;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    int port = 16022;
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        socklen_t len = sizeof(addr);
        if (getsockname(fd, (struct sockaddr*)&addr, &len) == 0) port = ntohs(addr.sin_port);
    }
    close(fd);
    return port;
}

static pid_t spawnAgentClient(AdminServer* adm, const std::string& devid, int fwd_port) {
    std::string fwd = "127.0.0.1:" + std::to_string(fwd_port) + "=" + devid + ":22";
    std::string port = std::to_string(adm->config().shell_relay_port);
    pid_t pid = fork();
    if (pid != 0) return pid;   // 父进程

    setsid();
    char logpath[128];
    snprintf(logpath, sizeof(logpath), "/tmp/rtunnel-shell-a-%d.log", (int)getpid());
    int fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
    }
    int nullfd = open("/dev/null", O_RDONLY);
    if (nullfd >= 0) dup2(nullfd, STDIN_FILENO);
    const char* path = adm->config().shell_client_path.c_str();
    execl(path, path, "--server", "127.0.0.1", "--port", port.c_str(),
          "--user", adm->config().shell_user.c_str(), "--forward", fwd.c_str(),
          "--verbose", (char*)NULL);
    _exit(127);
}

static void spawnSsh(htimer_t* timer) {
    auto* s = (TerminalSession*)hevent_userdata(timer);
    if (s == NULL || s->closed) return;
    hloop_t* loop = adminLoop(s->admin);

    struct winsize wsz = {24, 80, 0, 0};
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &wsz);
    if (pid < 0) {
        if (s->ws && s->ws->isConnected()) s->ws->close();
        return;
    }
    if (pid == 0) {
        // 子进程：ssh → A 子进程的 forward 端口 → 设备 sshd
        std::string port = std::to_string(s->fwd_port);
        std::string target = (s->user.empty() ? "root" : s->user) + "@127.0.0.1";
        execlp("ssh", "ssh", "-p", port.c_str(),
               "-o", "StrictHostKeyChecking=no",
               "-o", "UserKnownHostsFile=/dev/null",
               "-o", "LogLevel=ERROR",
               "-o", "ConnectTimeout=8",
               target.c_str(), (char*)NULL);
        _exit(127);
    }

    s->ssh_pid = pid;
    fcntl(master, F_SETFL, O_NONBLOCK);
    s->pty_io = hio_get(loop, master);
    hio_set_context(s->pty_io, s);
    hio_setcb_read(s->pty_io, onPtyRead);
    hio_setcb_close(s->pty_io, onPtyClose);
    hio_read(s->pty_io);
}

// ---------------------------------------------------------------- direct：RTNP client 直连 relay（免密）

static void directConnectCb(hio_t* io) {
    auto* s = (TerminalSession*)hio_context(io);
    if (s == NULL) return;
    if (hio_error(io) != 0) {
        if (s->ws && s->ws->isConnected()) s->ws->close();
        return;
    }
    rtnp::Writer w;
    w.u8(rtnp::ROLE_CLIENT);
    w.u8(rtnp::VERSION);
    w.str("rtunnel-admin/1");
    w.u32(0);
    rtnp::send(io, rtnp::HELLO, 0, w.data, (uint16_t)w.len);
}

static void directCloseCb(hio_t* io) {
    auto* s = (TerminalSession*)hio_context(io);
    if (s) {
        logf_("[terminal] direct link closed devid=%s error=%d", s->devid.c_str(), hio_error(io));
        if (s->ws && s->ws->isConnected()) s->ws->close();
    }
}

static void directFrame(TerminalSession* s, rtnp::Frame* f) {
    switch (f->type) {
    case rtnp::HELLO_ACK: {
        rtnp::Writer w;
        // AUTH 用户来自 WS query 的 user（A 端列表点谁的 Shell 就用谁的身份），
        // 未指定时回退到配置的默认 shell_user
        const char* auth_user = s->user.empty() ? s->admin->config().shell_user.c_str() : s->user.c_str();
        w.str(auth_user);
        rtnp::send(s->link_io, rtnp::AUTH, 0, w.data, (uint16_t)w.len);
        return;
    }
    case rtnp::AUTH_OK: {
        rtnp::Writer w;
        w.str(s->devid.c_str());
        w.u16(0);
        rtnp::send(s->link_io, rtnp::OPEN, 1, w.data, (uint16_t)w.len, rtnp::OPEN_SHELL);
        s->sid = 1;
        return;
    }
    case rtnp::OPEN_OK:
        // shell 就绪：把 OPEN 完成前缓冲的输入补发出去
        if (s->pending_input.len > 0) {
            rtnp::send(s->link_io, rtnp::DATA, s->sid, s->pending_input.data, (uint16_t)s->pending_input.len);
            s->pending_input.clear();
        }
        return;
    case rtnp::OPEN_FAIL: {
        std::string msg = "open failed";
        if (f->payload && f->len > 4) msg.assign((const char*)f->payload + 4, f->len - 4);
        if (s->ws && s->ws->isConnected()) {
            s->ws->send("OPEN_FAIL: " + msg + "\r\n");
            s->ws->close();
        }
        return;
    }
    case rtnp::DATA:
        if (s->ws && f->len > 0) s->ws->send(std::string((const char*)f->payload, f->len));
        return;
    case rtnp::CLOSE:
    case rtnp::RESET:
        if (s->ws && s->ws->isConnected()) s->ws->close();
        return;
    case rtnp::PING:
        rtnp::send(s->link_io, rtnp::PONG, 0, f->payload, f->len);
        return;
    case rtnp::AUTH_FAIL:
    case rtnp::ERROR:
        if (s->ws && s->ws->isConnected()) s->ws->close();
        return;
    default:
        return;
    }
}

static void directReadCb(hio_t* io, void* buf, int readbytes) {
    auto* s = (TerminalSession*)hio_context(io);
    if (s == NULL || readbytes <= 0) return;
    if (s->in.append(buf, (size_t)readbytes) != 0) { hio_close(io); return; }
    for (;;) {
        rtnp::Frame f;
        int rc = s->in.next(f);
        if (rc == 0) return;
        if (rc < 0) { hio_close(io); return; }
        directFrame(s, &f);
        if (s->closed || hio_is_closed(io)) return;
    }
}

static void directOpen(AdminServer* adm, TerminalSession* s) {
    hloop_t* loop = adminLoop(adm);
    s->link_io = hloop_create_tcp_client(loop, "127.0.0.1", adm->config().shell_relay_port,
                                         directConnectCb, directCloseCb);
    if (s->link_io == NULL) {
        if (s->ws && s->ws->isConnected()) s->ws->close();
        return;
    }
    hio_set_context(s->link_io, s);
    hio_setcb_read(s->link_io, directReadCb);
    hio_read(s->link_io);
}

// ---------------------------------------------------------------- ws 回调

void AdminServer::onWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    // 权限：query 里的 token（浏览器 WebSocket 无法自定义 Authorization 头）
    std::string token, devid, user;
    for (const auto& kv : req->query_params) {
        if (kv.first == "token") token = kv.second;
        else if (kv.first == "devid") devid = kv.second;
        else if (kv.first == "user") user = kv.second;
    }
    auto roleOfToken = [this](const std::string& tok) -> std::string {
        if (cfg_.token.empty() && users_.empty()) return "admin";
        if (!cfg_.token.empty() && tok == cfg_.token) return "admin";
        auto it = users_.find(tok);
        return it != users_.end() ? it->second : "";
    };
    std::string role = roleOfToken(token);
    if (role != "admin" && role != "operator") {
        logf_("[terminal] reject: bad token (devid=%s)", devid.c_str());
        channel->close();
        return;
    }
    if (cfg_.shell_client_path.empty() || devid.empty()) {
        logf_("[terminal] reject: shell disabled or bad devid");
        channel->close();
        return;
    }

    auto s = std::make_shared<TerminalSession>();
    s->admin = this;
    s->ws = channel;
    s->devid = devid;
    s->user = user;
    s->fwd_port = allocFreePort();
    terms_[channel.get()] = s;

    bool direct = (req->Path() == "/ws/shell");
    if (direct) {
        // 免密模式：admin 作为 RTNP client 直连 relay，OPEN_SHELL 由 agent forkpty
        s->direct = true;
        audit(role, "terminal.open.shell", devid);
        directOpen(this, s.get());
        return;
    }

    s->agent_pid = spawnAgentClient(this, devid, s->fwd_port);
    audit(role, "terminal.open", devid + " user=" + user);
    logf_("[terminal] open devid=%s fwd=127.0.0.1:%d agent_pid=%d", devid.c_str(), s->fwd_port, s->agent_pid);

    // 等 A 子进程完成认证后启动 ssh
    hloop_t* loop = adminLoop(this);
    htimer_t* t = htimer_add(loop, spawnSsh, 600, 1);
    hevent_set_userdata(t, s.get());
}

void AdminServer::onWsMessage(const WebSocketChannelPtr& channel, const std::string& msg) {
    auto it = terms_.find(channel.get());
    if (it == terms_.end()) return;
    auto& s = it->second;

    if (s->direct) {
        // direct：输入 → DATA；resize → RESIZE 帧
        if (msg.compare(0, 7, "{\"type\"") == 0) {
            if (msg.find("\"resize\"") != std::string::npos) {
                int cols = 80, rows = 24;
                if (sscanf(msg.c_str(), "{\"type\":\"resize\",\"cols\":%d,\"rows\":%d", &cols, &rows) == 2) {
                    rtnp::Writer w;
                    w.u16((uint16_t)cols);
                    w.u16((uint16_t)rows);
                    rtnp::send(s->link_io, rtnp::RESIZE, s->sid, w.data, (uint16_t)w.len);
                }
            }
            return;
        }
        if (s->link_io && !msg.empty()) {
            if (s->sid == 0) {
                // OPEN_OK 前不允许发 DATA（sid=0 会被 relay 判协议错误），先缓冲
                s->pending_input.append(msg.data(), msg.size());
            } else {
                rtnp::send(s->link_io, rtnp::DATA, s->sid, msg.data(), (uint16_t)msg.size());
            }
        }
        return;
    }

    // 控制消息（JSON）与原始键盘输入分离
    if (msg.compare(0, 7, "{\"type\"") == 0) {
        if (msg.find("\"resize\"") != std::string::npos && s->pty_io) {
            int cols = 80, rows = 24;
            if (sscanf(msg.c_str(), "{\"type\":\"resize\",\"cols\":%d,\"rows\":%d", &cols, &rows) == 2) {
                struct winsize wsz;
                wsz.ws_col = (unsigned short)cols;
                wsz.ws_row = (unsigned short)rows;
                wsz.ws_xpixel = wsz.ws_ypixel = 0;
                ioctl(hio_fd(s->pty_io), TIOCSWINSZ, &wsz);
            }
        }
        return;
    }
    if (s->pty_io && !msg.empty()) {
        // 键盘输入 → ssh stdin（pty 非阻塞写，小数据量）
        ssize_t n = write(hio_fd(s->pty_io), msg.data(), msg.size());
        (void)n;
    }
}

void AdminServer::onWsClose(const WebSocketChannelPtr& channel) {
    auto it = terms_.find(channel.get());
    if (it == terms_.end()) return;
    sessionClose(it->second.get());
    terms_.erase(it);
}

// admin worker loop 的 hloop_t*（http_server 内部 EventLoop）
static hloop_t* adminLoop(AdminServer* adm) {
    return adm->httpServer().loop(0)->loop();
}

// start() 里调用：绑定 ws 回调
void AdminServer::initTerminal() {
    server_.ws = &ws_service_;
    ws_service_.onopen = [this](const WebSocketChannelPtr& c, const HttpRequestPtr& r) { onWsOpen(c, r); };
    ws_service_.onmessage = [this](const WebSocketChannelPtr& c, const std::string& m) { onWsMessage(c, m); };
    ws_service_.onclose = [this](const WebSocketChannelPtr& c) { onWsClose(c); };
}
