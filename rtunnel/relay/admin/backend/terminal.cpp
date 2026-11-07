// Web 终端（direct 免密）：浏览器 xterm.js ↔ WebSocket ↔ admin 作为 RTNP client
// 直连本机 relay，OPEN_SHELL 由 agent forkpty 起 shell。
// 只有一种实现：不 fork ssh、不起 A 子进程、不经手密码。
#include "admin_server.hpp"
#include "common/logging.hpp"
#include "EventLoop.h"

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

struct TerminalSession {
    AdminServer*         admin = nullptr;
    WebSocketChannelPtr  ws;
    hio_t*               link_io = nullptr;   // 到 relay 的 RTNP 连接
    rtnp::Decoder        in;
    uint32_t             sid = 0;
    std::string          devid;
    std::string          user;                // RTNP AUTH 用户（空则用默认 shell_user）
    std::string          actor;               // 管理台账号（用于并发限制）
    bool                 closed = false;
    rtnp::Buffer         pending_input;       // OPEN_OK 前的输入先缓冲
};

static hloop_t* adminLoop(AdminServer* adm);

static void sessionClose(TerminalSession* s) {
    if (s->closed) return;
    s->closed = true;
    if (s->link_io) {
        hio_close(s->link_io);
        s->link_io = nullptr;
    }
    if (s->admin) {
        s->admin->auditPublic("operator", "terminal.close", s->devid);
    }
}

// ---------------------------------------------------------------- direct：RTNP client 直连 relay

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
        // 身份固定为配置的 shell_user：不再接受浏览器传 user，
        // 否则 operator 可以冒用任意用户身份开 shell。
        const std::string& auth_user = s->admin->config().shell_user;
        const std::string& auth_token = s->admin->config().shell_token;
        w.str(auth_user.c_str());
        w.u8(auth_token.empty() ? 0 : 1);
        w.str(auth_token.c_str());
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
    if (req->path.compare(0, 8, "/device/") == 0) {
        handleDeviceWsOpen(channel, req);
        return;
    }
    // 权限：query 里的 token（浏览器 WebSocket 无法自定义 Authorization 头）
    std::string token, devid, user;
    for (const auto& kv : req->query_params) {
        if (kv.first == "token") token = kv.second;
        else if (kv.first == "devid") devid = kv.second;
        else if (kv.first == "user") user = kv.second;
    }
    std::string role;
    AdminActor actor;
    if (!wsAuthorize(req, actor, role)) role.clear();
    {
        int maxs = settingInt("term.max_sessions", 4);
        int cur = 0;
        for (auto& kv : terms_) if (kv.second && kv.second->actor == actor.username) cur++;
        if (cur >= maxs) {
            logf_("[terminal] reject: too many sessions for %s", actor.username.c_str());
            channel->close();
            return;
        }
    }
    if (role != "admin" && role != "manager" && role != "operator") {
        logf_("[terminal] reject: unauthorized (devid=%s)", devid.c_str());
        channel->close();
        return;
    }
    audit(role, "terminal.open", devid);
    if (devid.empty()) {
        logf_("[terminal] reject: bad devid");
        channel->close();
        return;
    }

    auto s = std::make_shared<TerminalSession>();
    s->actor = actor.username;
    s->admin = this;
    s->ws = channel;
    s->devid = devid;
    s->user = user;
    terms_[channel.get()] = s;

    directOpen(this, s.get());
}

void AdminServer::onWsMessage(const WebSocketChannelPtr& channel, const std::string& msg) {
    if (handleDeviceWsMessage(channel, msg)) return;
    auto it = terms_.find(channel.get());
    if (it == terms_.end()) return;
    auto& s = it->second;

    // 控制消息（JSON）与原始键盘输入分离
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
}

void AdminServer::onWsClose(const WebSocketChannelPtr& channel) {
    if (handleDeviceWsClose(channel)) return;
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
