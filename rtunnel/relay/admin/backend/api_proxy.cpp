// 设备 Web 代理（路径式）：/device/<devid>/<port>/<rest>
// 认证复用管理台会话；数据面走本机 relay 的 RTNP OPEN，与 Web 终端同一机制。
#include "admin_server.hpp"
#include "http/server/HttpResponseWriter.h"
#include "cpputil/json.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <random>
#include <atomic>

namespace {


std::atomic<int> g_proxy_active{0};

struct ProxyGuard {
    ProxyGuard() { ++g_proxy_active; }
    ~ProxyGuard() { --g_proxy_active; }
};

int tcpConnect(const char* host, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 || connect(fd, (sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool writeAll(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n > 0) {
        ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        c += w; n -= (size_t)w;
    }
    return true;
}

bool readAll(int fd, void* p, size_t n) {
    char* c = (char*)p;
    while (n > 0) {
        ssize_t r = recv(fd, c, n, 0);
        if (r <= 0) return false;
        c += r; n -= (size_t)r;
    }
    return true;
}

void put16(std::string& s, uint16_t v) { s.push_back((char)(v >> 8)); s.push_back((char)v); }
void put32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back((char)(v >> (8 * i))); }

void sendFrame(int fd, uint8_t type, uint32_t sid, const std::string& payload, uint16_t flags = 0) {
    std::string h = "RTNP";
    h.push_back(1); h.push_back((char)type);
    put16(h, flags);
    put32(h, sid);
    put16(h, (uint16_t)payload.size());
    put16(h, 0);
    writeAll(fd, h.data(), h.size());
    if (!payload.empty()) writeAll(fd, payload.data(), payload.size());
}

void sendData(int fd, uint32_t sid, const std::string& p) { sendFrame(fd, 0x13, sid, p); }
void sendWindow(int fd, uint32_t sid, uint32_t n) {
    std::string w; put32(w, n); sendFrame(fd, 0x14, sid, w);
}

bool recvFrame(int fd, uint8_t& type, uint32_t& sid, std::string& payload) {
    char h[16];
    if (!readAll(fd, h, sizeof(h))) return false;
    if (memcmp(h, "RTNP", 4) != 0) return false;
    type = (uint8_t)h[5];
    sid = ((uint32_t)(uint8_t)h[8] << 24) | ((uint32_t)(uint8_t)h[9] << 16) |
          ((uint32_t)(uint8_t)h[10] << 8) | (uint8_t)h[11];
    uint16_t len = (uint16_t)((uint8_t)h[12] << 8 | (uint8_t)h[13]);
    payload.resize(len);
    if (len && !readAll(fd, &payload[0], len)) return false;
    return true;
}

std::string rtnpStr(const std::string& s) {
    std::string o;
    put16(o, (uint16_t)s.size());
    o += s;
    return o;
}

std::string urlDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            auto hx = [](char c) { return (c <= '9') ? c - '0' : (tolower((unsigned char)c) - 'a' + 10); };
            out.push_back((char)((hx(in[i + 1]) << 4) | hx(in[i + 2])));
            i += 2;
        } else out.push_back(in[i]);
    }
    return out;
}

std::string lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string dechunk(const std::string& in) {
    std::string out;
    size_t p = 0;
    while (p < in.size()) {
        size_t eol = in.find("\r\n", p);
        if (eol == std::string::npos) break;
        long n = strtol(in.substr(p, eol - p).c_str(), nullptr, 16);
        if (n <= 0) break;
        size_t start = eol + 2;
        if (start + (size_t)n > in.size()) { out += in.substr(start); break; }
        out.append(in, start, (size_t)n);
        p = start + (size_t)n + 2;
    }
    return out;
}

// 打开一条到本机 relay 的 RTNP 会话并 OPEN(devid, port)
int openStream(const AdminConfig& cfg, const std::string& devid, int port, std::string& err) {
    int fd = tcpConnect("127.0.0.1", cfg.shell_relay_port, 5000);
    if (fd < 0) { err = "relay connect failed"; return -1; }

    std::string hello;
    hello.push_back(2);            // ROLE_CLIENT
    hello.push_back(1);            // ver
    hello += rtnpStr("admin-proxy");
    put32(hello, 0);
    sendFrame(fd, 0x01, 0, hello);

    uint8_t t; uint32_t sid; std::string p;
    if (!recvFrame(fd, t, sid, p) || t != 0x02) { err = "no HELLO_ACK"; close(fd); return -1; }

    std::string auth = rtnpStr(cfg.shell_user);
    auth.push_back(cfg.shell_token.empty() ? 0 : 1);
    auth += rtnpStr(cfg.shell_token);
    sendFrame(fd, 0x03, 0, auth);
    if (!recvFrame(fd, t, sid, p) || t != 0x04) { err = "auth failed"; close(fd); return -1; }

    std::string open = rtnpStr(devid);
    put16(open, (uint16_t)port);
    sendFrame(fd, 0x10, 1, open);
    if (!recvFrame(fd, t, sid, p) || t != 0x11) {
        if (t == 0x12 && p.size() > 2) {
            // OPEN_FAIL: u16 code + str msg
            err = "open failed: " + p.substr(4);
        } else {
            err = "open failed";
        }
        close(fd);
        return -1;
    }
    return fd;
}

} // namespace

void AdminServer::handleDeviceProxy(const HttpContextPtr& ctx) {
    std::string path = ctx->path();
    // /device/<devid>/<port>/<rest>
    if (path.compare(0, 8, "/device/") != 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("not found");
        return;
    }
    std::string rest = path.substr(8);
    size_t s1 = rest.find('/');
    if (s1 == std::string::npos) { ctx->response->status_code = HTTP_STATUS_BAD_REQUEST; ctx->send("bad device path"); return; }
    std::string devid = urlDecode(rest.substr(0, s1));
    rest = rest.substr(s1 + 1);
    size_t s2 = rest.find('/');
    std::string port_s = (s2 == std::string::npos) ? rest : rest.substr(0, s2);
    std::string sub = (s2 == std::string::npos) ? "/" : rest.substr(s2);
    int port = atoi(port_s.c_str());
    if (devid.empty() || port <= 0 || port > 65535) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("bad device/port");
        return;
    }

    std::string role;
    if (requireRole(ctx, ROLE_OPERATOR, role)) return;
    if (g_proxy_active.load() >= settingInt("proxy.max_concurrent", 32)) {
        ctx->response->status_code = HTTP_STATUS_SERVICE_UNAVAILABLE;
        ctx->send("{\"error\":\"too many concurrent proxy requests\"}");
        return;
    }
    ProxyGuard guard;
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("forbidden");
        return;
    }
    if (sub.empty() || sub[0] != '/') sub = "/" + sub;

    std::string err;
    int fd = openStream(cfg_, devid, port, err);
    if (fd < 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }

    // 构造上游请求
    std::string prefix = "/device/" + devid + "/" + port_s;
    const char* mstr = "GET";
    switch (ctx->request->method) {
        case HTTP_POST: mstr = "POST"; break;
        case HTTP_PUT: mstr = "PUT"; break;
        case HTTP_DELETE: mstr = "DELETE"; break;
        case HTTP_HEAD: mstr = "HEAD"; break;
        case HTTP_OPTIONS: mstr = "OPTIONS"; break;
        default: mstr = "GET"; break;
    }
    std::string req = std::string(mstr) + " " + sub + " HTTP/1.1\r\n";
    req += "Host: " + devid + "\r\n";
    static const char* keep[] = { "User-Agent", "Accept", "Accept-Language", "Content-Type",
                                  "Range", "If-Modified-Since", "If-None-Match", "Referer", "Origin" };
    for (const char* h : keep) {
        std::string v = ctx->header(h);
        if (!v.empty()) req += std::string(h) + ": " + v + "\r\n";
    }
    // Cookie：只转发本设备命名空间下的
    std::string cookie = ctx->header("Cookie");
    std::string cookieOut;
    if (!cookie.empty()) {
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
    if (!cookieOut.empty()) req += "Cookie: " + cookieOut + "\r\n";
    const std::string& body = ctx->body();
    if (!body.empty()) req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    req += "Connection: close\r\n\r\n";
    if (!body.empty()) req += body;

    // 发送请求并半关闭
    sendData(fd, 1, req);
    sendFrame(fd, 0x15, 1, std::string());

    // 先读到响应头
    std::string raw, p;
    uint8_t t; uint32_t sid;
    size_t hdrEnd = std::string::npos;
    while (recvFrame(fd, t, sid, p)) {
        if (t == 0x13 && !p.empty()) {
            raw += p;
            sendWindow(fd, 1, (uint32_t)p.size());
            hdrEnd = raw.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) break;
        } else if (t == 0x15 || t == 0x16) break;
    }
    if (hdrEnd == std::string::npos) {
        close(fd);
        ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
        ctx->send("bad upstream response");
        return;
    }

    std::string head = raw.substr(0, hdrEnd);
    int status = 502;
    std::vector<std::pair<std::string, std::string>> hdrs;
    {
        size_t eol = head.find("\r\n");
        sscanf(head.substr(0, eol).c_str(), "HTTP/%*s %d", &status);
        size_t pos = (eol == std::string::npos) ? head.size() : eol + 2;
        while (pos < head.size()) {
            size_t e = head.find("\r\n", pos);
            std::string ln = head.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
            size_t colon = ln.find(':');
            if (colon != std::string::npos) hdrs.push_back({ trim(ln.substr(0, colon)), trim(ln.substr(colon + 1)) });
            if (e == std::string::npos) break;
            pos = e + 2;
        }
    }
    bool chunked = false;
    std::string ctype;
    for (auto& h : hdrs) {
        std::string k = lower(h.first);
        if (k == "transfer-encoding" && lower(h.second).find("chunked") != std::string::npos) chunked = true;
        if (k == "content-type") ctype = h.second;
    }

    ctx->response->status_code = (http_status)status;
    for (auto& h : hdrs) {
        std::string k = lower(h.first);
        if (k == "transfer-encoding" || k == "connection" || k == "content-length") continue;
        if (k == "set-cookie") {
            std::string cprefix = "d_" + devid + "_" + std::to_string(port) + "_";
            std::string v = h.second;
            size_t eq = v.find('=');
            if (eq != std::string::npos) {
                std::string name = trim(v.substr(0, eq));
                std::string tail = v.substr(eq);
                size_t ppos = lower(tail).find("; path=");
                if (ppos != std::string::npos) {
                    size_t e2 = tail.find(';', ppos + 2);
                    tail = tail.substr(0, ppos) + "; Path=" + prefix +
                           (e2 == std::string::npos ? "" : tail.substr(e2));
                }
                h.second = cprefix + name + tail;
            }
            ctx->response->headers["Set-Cookie"] = h.second;
            continue;
        }
        if (k == "location" && !h.second.empty() && h.second[0] == '/') h.second = prefix + h.second;
        ctx->response->headers[h.first] = h.second;
    }
    if (!ctype.empty()) ctx->response->headers["Content-Type"] = ctype;

    std::string pending = raw.substr(hdrEnd + 4);
    bool done = false;
    int cstate = 0;      // 0=size 1=data
    size_t need = 0;

    hv::HttpResponseWriter* w = ctx->writer.get();
    bool no_body = (status == 204 || status == 304 || ctx->request->method == HTTP_HEAD);
    w->Begin();
    if (no_body) {
        w->EndHeaders();
        close(fd);
        w->End();
        return;
    }
    w->EndHeaders("Transfer-Encoding", "chunked");
    bool client_gone = false;
    auto emit = [&](const char* d, size_t n) {
        if (n && w->WriteChunked(d, (int)n) < 0) client_gone = true;
    };
    auto feed = [&](const std::string& in) {
        if (!chunked) { emit(in.data(), in.size()); return; }
        pending += in;
        size_t pos = 0;
        while (!done) {
            if (cstate == 0) {
                size_t e = pending.find("\r\n", pos);
                if (e == std::string::npos) break;
                long n = strtol(pending.substr(pos, e - pos).c_str(), nullptr, 16);
                pos = e + 2;
                if (n <= 0) { done = true; break; }
                need = (size_t)n; cstate = 1;
            } else {
                if (pending.size() - pos < need + 2) break;
                emit(pending.data() + pos, need);
                pos += need + 2;
                cstate = 0;
            }
        }
        if (pos > 0) pending.erase(0, pos);
    };

    if (!pending.empty()) feed(pending);
    pending.clear();
    while (!done && recvFrame(fd, t, sid, p)) {
        if (client_gone) break;
        if (t == 0x13 && !p.empty()) {
            sendWindow(fd, 1, (uint32_t)p.size());
            feed(p);
        } else if (t == 0x15 || t == 0x16) {
            break;
        }
    }
    close(fd);
    w->End();
}

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
    std::string devid = urlDecode(rest.substr(0, s1));
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
