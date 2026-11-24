// 设备 WebSocket 代理
#include "admin_server.hpp"
#include "proxy_util.hpp"
#include "common/logging.hpp"
#include "http/WebSocketParser.h"
#include "http/wsdef.h"
#include "util/base64.h"

#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <string.h>

// ---------------------------------------------------------------- WebSocket 代理
// 浏览器 WS 已在 admin 终结；这里再向设备做一次 WS 握手，然后按帧转发。
namespace {

struct WsProxySession {
    WebSocketChannelPtr ws;
    hio_t*     link = nullptr;
    std::string devid;
    int         port = 0;
    std::string rtnpbuf;    // relay 帧缓冲
    WebSocketParserPtr parser;  // 设备→浏览器 的 WS 帧解析
    bool        closed = false;
};

std::mutex g_ws_mu;
std::unordered_map<hv::WebSocketChannel*, std::shared_ptr<WsProxySession>> g_ws;

std::string buildClientFrame(const std::string& payload, int opcode) {
    int data_len = (int)payload.size();
    std::string f((size_t)ws_calc_frame_size(data_len, true), '\0');
    int n = ws_client_build_frame(&f[0], payload.data(), data_len, (enum ws_opcode)opcode);
    if (n > 0) f.resize((size_t)n);
    return f;
}

void wsProxyReadCb(hio_t* io, void* buf, int n) {
    auto* s = (WsProxySession*)hio_context(io);
    if (!s || n <= 0) return;
    s->rtnpbuf.append((char*)buf, (size_t)n);
    while (!s->closed) {
        if (s->rtnpbuf.size() < 16) break;
        if (s->rtnpbuf.compare(0, 4, "RTNP") != 0) { s->rtnpbuf.clear(); break; }
        uint8_t type = (uint8_t)s->rtnpbuf[5];
        uint32_t sid = ((uint32_t)(uint8_t)s->rtnpbuf[8] << 24) | ((uint32_t)(uint8_t)s->rtnpbuf[9] << 16) |
                       ((uint32_t)(uint8_t)s->rtnpbuf[10] << 8) | (uint8_t)s->rtnpbuf[11];
        uint16_t len = (uint16_t)((uint8_t)s->rtnpbuf[12] << 8 | (uint8_t)s->rtnpbuf[13]);
        if (s->rtnpbuf.size() < (size_t)16 + len) break;
        std::string payload = s->rtnpbuf.substr(16, len);
        s->rtnpbuf.erase(0, 16 + len);
        if (type == 0x13 && !payload.empty()) {
            if (s->parser && s->parser->FeedRecvData(payload.data(), payload.size()) < 0) {
                if (s->ws && s->ws->isConnected()) s->ws->close();
                s->closed = true;
                hio_close(io);
                return;
            }
            sendWindow(hio_fd(io), sid ? sid : 1, (uint32_t)payload.size());
        } else if (type == 0x15 || type == 0x16) {
            if (s->ws && s->ws->isConnected()) s->ws->close();
            s->closed = true;
            hio_close(io);
            return;
        }
    }
}

void wsProxyCloseCb(hio_t* io) {
    auto* s = (WsProxySession*)hio_context(io);
    if (s) s->closed = true;
}

// 对设备做 WS 握手（阻塞读响应头）
bool deviceWsHandshake(int fd, const std::string& devid, int port, const std::string& sub,
                       const std::string& cookie, const std::string& proto) {
    unsigned char rnd[16];
    std::random_device rd;
    for (int i = 0; i < 16; ++i) rnd[i] = (unsigned char)(rd() & 0xff);
    std::string req = "GET " + sub + " HTTP/1.1\r\nHost: " + devid + ":" + std::to_string(port) + "\r\n";
    req += "Upgrade: websocket\r\nConnection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: " + hv::Base64Encode(rnd, 16) + "\r\nSec-WebSocket-Version: 13\r\n";
    if (!proto.empty()) req += "Sec-WebSocket-Protocol: " + proto + "\r\n";
    if (!cookie.empty()) req += "Cookie: " + cookie + "\r\n";
    req += "\r\n";
    sendData(fd, 1, req);
    std::string head, p;
    uint8_t t; uint32_t sid;
    while (recvFrame(fd, t, sid, p)) {
        if (t == 0x13 && !p.empty()) {
            head += p;
            sendWindow(fd, 1, (uint32_t)p.size());
            if (head.find("\r\n\r\n") != std::string::npos) break;
        } else if (t == 0x15 || t == 0x16) break;
    }
    return head.find(" 101 ") != std::string::npos;
}

} // namespace

void AdminServer::handleDeviceWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    std::string path = req->path;
    if (path.compare(0, 8, "/device/") != 0) { channel->close(); return; }
    std::string rest = path.substr(8);
    size_t s1 = rest.find('/');
    if (s1 == std::string::npos) { channel->close(); return; }
    std::string devid = urlDecode(rest.substr(0, s1));
    rest = rest.substr(s1 + 1);
    size_t s2 = rest.find('/');
    std::string port_s = (s2 == std::string::npos) ? rest : rest.substr(0, s2);
    std::string sub = (s2 == std::string::npos) ? "/" : rest.substr(s2);
    int port = atoi(port_s.c_str());
    if (devid.empty() || port <= 0) { channel->close(); return; }

    std::string role;
    AdminActor actor;
    if (!wsAuthorize(req, actor, role) || !hasPerm("device.terminal") || !ownsDevid(devid)) {
        logf_("[admin] device ws denied: %s", devid.c_str());
        channel->close();
        return;
    }

    audit(role, "proxy.open", devid + ":" + port_s);
    std::string err;
    int fd = openStream(cfg_, devid, port, err);
    if (fd < 0) {
        logf_("[admin] device ws open %s:%d failed: %s", devid.c_str(), port, err.c_str());
        channel->close();
        return;
    }

    // 只转发本设备命名空间的 Cookie
    std::string cookieOut;
    {
        const HttpCookie* ck = nullptr;
        (void)ck;
        std::string cookie = req->GetHeader("Cookie");
        std::string cprefix = "d_" + devid + "_" + port_s + "_";
        size_t pos = 0;
        while (pos < cookie.size()) {
            size_t semi = cookie.find(';', pos);
            std::string item = trim(cookie.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
            if (item.compare(0, cprefix.size(), cprefix) == 0) {
                if (!cookieOut.empty()) cookieOut += "; ";
                cookieOut += item.substr(cprefix.size());
            }
            if (semi == std::string::npos) break;
            pos = semi + 1;
        }
    }
    std::string proto = req->GetHeader("Sec-WebSocket-Protocol");
    if (!deviceWsHandshake(fd, devid, port, sub, cookieOut, proto)) {
        logf_("[admin] device ws handshake %s:%d%s failed", devid.c_str(), port, sub.c_str());
        close(fd);
        channel->close();
        return;
    }

    auto s = std::make_shared<WsProxySession>();
    s->ws = channel;
    s->devid = devid;
    s->port = port;
    s->parser = std::make_shared<WebSocketParser>();
    WsProxySession* raw = s.get();
    s->parser->onMessage = [raw](int opcode, const std::string& msg) {
        if (raw->closed) return;
        if (opcode == WS_OPCODE_PING) {
            std::string pong = buildClientFrame(msg, WS_OPCODE_PONG);
            if (raw->link) sendData(hio_fd(raw->link), 1, pong);
            return;
        }
        if (opcode == WS_OPCODE_PONG) return;
        if (opcode == WS_OPCODE_CLOSE) {
            if (raw->ws && raw->ws->isConnected()) raw->ws->close();
            raw->closed = true;
            if (raw->link) hio_close(raw->link);
            return;
        }
        if (raw->ws && raw->ws->isConnected()) {
            raw->ws->send(msg, opcode == WS_OPCODE_TEXT ? WS_OPCODE_TEXT : WS_OPCODE_BINARY);
        }
    };
    hloop_t* loop = hevent_loop(channel->io());
    s->link = hio_get(loop, fd);
    if (!s->link) { close(fd); channel->close(); return; }
    hio_set_context(s->link, s.get());
    hio_setcb_read(s->link, wsProxyReadCb);
    hio_setcb_close(s->link, wsProxyCloseCb);
    hio_read(s->link);
    {
        std::lock_guard<std::mutex> lk(g_ws_mu);
        g_ws[channel.get()] = s;
    }
    registerConsoleWs(channel, actor, "proxy");
}

bool AdminServer::handleDeviceWsMessage(const WebSocketChannelPtr& channel, const std::string& msg) {
    std::shared_ptr<WsProxySession> s;
    {
        std::lock_guard<std::mutex> lk(g_ws_mu);
        auto it = g_ws.find(channel.get());
        if (it == g_ws.end()) return false;
        s = it->second;
    }
    if (!s->link || s->closed) return true;
    int op = (channel->opcode == WS_OPCODE_BINARY) ? 0x2 : 0x1;
    std::string frame = buildClientFrame(msg, op);
    sendData(hio_fd(s->link), 1, frame);
    return true;
}

bool AdminServer::handleDeviceWsClose(const WebSocketChannelPtr& channel) {
    std::shared_ptr<WsProxySession> s;
    {
        std::lock_guard<std::mutex> lk(g_ws_mu);
        auto it = g_ws.find(channel.get());
        if (it == g_ws.end()) return false;
        s = it->second;
        g_ws.erase(it);
    }
    s->closed = true;
    if (s->link) {
        std::string f = buildClientFrame(std::string(), 0x8);
        sendData(hio_fd(s->link), 1, f);
        hio_close(s->link);
        s->link = nullptr;
    }
    return true;
}
