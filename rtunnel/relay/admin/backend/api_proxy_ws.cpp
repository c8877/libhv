// 设备 WebSocket 代理
#include "admin_server.hpp"
#include "proxy_util.hpp"

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
    std::string inbuf;      // 设备→浏览器 的 WS 帧缓冲
    std::string frag;       // 分片聚合
    int         frag_op = 0;
    bool        closed = false;
};

std::mutex g_ws_mu;
std::unordered_map<hv::WebSocketChannel*, std::shared_ptr<WsProxySession>> g_ws;

std::string b64(const unsigned char* p, size_t n) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        o += t[(v >> 18) & 63]; o += t[(v >> 12) & 63];
        o += (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        o += (i + 2 < n) ? t[v & 63] : '=';
    }
    return o;
}

std::string buildClientFrame(const std::string& payload, int opcode) {
    std::string f;
    f.push_back((char)(0x80 | (opcode & 0x0f)));
    size_t n = payload.size();
    if (n < 126) f.push_back((char)(0x80 | n));
    else if (n < 65536) { f.push_back((char)(0x80 | 126)); f.push_back((char)(n >> 8)); f.push_back((char)n); }
    else { f.push_back((char)(0x80 | 127)); for (int i = 7; i >= 0; --i) f.push_back((char)((uint64_t)n >> (8 * i))); }
    unsigned char mask[4];
    std::random_device rd;
    for (int i = 0; i < 4; ++i) mask[i] = (unsigned char)(rd() & 0xff);
    f.append((char*)mask, 4);
    for (size_t i = 0; i < n; ++i) f.push_back((char)(payload[i] ^ mask[i & 3]));
    return f;
}

void wsParse(WsProxySession* s);

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
            s->inbuf += payload;
            sendWindow(hio_fd(io), sid ? sid : 1, (uint32_t)payload.size());
        } else if (type == 0x15 || type == 0x16) {
            if (s->ws && s->ws->isConnected()) s->ws->close();
            s->closed = true;
            hio_close(io);
            return;
        }
    }
    wsParse(s);
}

void wsParse(WsProxySession* s) {
    while (!s->closed) {
        if (s->inbuf.size() < 2) return;
        unsigned char b0 = (unsigned char)s->inbuf[0];
        unsigned char b1 = (unsigned char)s->inbuf[1];
        int opcode = b0 & 0x0f;
        bool fin = (b0 & 0x80) != 0;
        size_t len = b1 & 0x7f, off = 2;
        if (len == 126) { if (s->inbuf.size() < 4) return; len = ((unsigned char)s->inbuf[2] << 8) | (unsigned char)s->inbuf[3]; off = 4; }
        else if (len == 127) {
            if (s->inbuf.size() < 10) return;
            len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | (unsigned char)s->inbuf[2 + i];
            off = 10;
        }
        if (b1 & 0x80) off += 4;   // 设备→浏览器不应带 mask，容错跳过
        if (s->inbuf.size() < off + len) return;
        std::string payload = s->inbuf.substr(off, len);
        s->inbuf.erase(0, off + len);

        if (opcode == 0x9) {        // ping → pong
            std::string pf = buildClientFrame(payload, 0xA);
            sendData(hio_fd(s->link), 1, pf);
            continue;
        }
        if (opcode == 0xA) continue;
        if (opcode == 0x8) {
            if (s->ws && s->ws->isConnected()) s->ws->close();
            s->closed = true;
            if (s->link) hio_close(s->link);
            return;
        }
        if (!fin || opcode == 0) {
            if (opcode != 0) s->frag_op = opcode;
            s->frag += payload;
            if (!fin) continue;
            payload.swap(s->frag);
            s->frag.clear();
            opcode = s->frag_op;
        }
        if (s->ws && s->ws->isConnected()) {
            s->ws->send(payload, opcode == 0x1 ? WS_OPCODE_TEXT : WS_OPCODE_BINARY);
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
    req += "Sec-WebSocket-Key: " + b64(rnd, 16) + "\r\nSec-WebSocket-Version: 13\r\n";
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
    std::string devid = proxyUrlDecode(rest.substr(0, s1));
    rest = rest.substr(s1 + 1);
    size_t s2 = rest.find('/');
    std::string port_s = (s2 == std::string::npos) ? rest : rest.substr(0, s2);
    std::string sub = (s2 == std::string::npos) ? "/" : rest.substr(s2);
    int port = atoi(port_s.c_str());
    if (devid.empty() || port <= 0) { channel->close(); return; }

    std::string role;
    AdminActor actor;
    if (!wsAuthorize(req, actor, role) ||
        (role != "admin" && role != "manager" && role != "operator") || !ownsDevid(devid)) {
        channel->close();
        return;
    }

    std::string err;
    int fd = openStream(cfg_, devid, port, err);
    if (fd < 0) { channel->close(); return; }

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
        close(fd);
        channel->close();
        return;
    }

    auto s = std::make_shared<WsProxySession>();
    s->ws = channel;
    s->devid = devid;
    s->port = port;
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
