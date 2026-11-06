// 设备 Web 代理（路径式）：/device/<devid>/<port>/<rest>
// 认证复用管理台会话；数据面走本机 relay 的 RTNP OPEN，与 Web 终端同一机制。
#include "admin_server.hpp"
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

namespace {

const size_t MAX_PROXY_BODY = 8 * 1024 * 1024;

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
    if (!recvFrame(fd, t, sid, p) || t != 0x11) { err = "open failed"; close(fd); return -1; }
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
    if (requireRole(ctx, ROLE_VIEWER, role)) return;
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

    std::string upstream;
    std::string data = rtnpStr(devid);  // 占位，实际发送 DATA
    (void)data;
    std::string payload = req;
    sendFrame(fd, 0x13, 1, payload);
    sendFrame(fd, 0x15, 1, std::string());   // CLOSE：请求结束

    uint8_t t; uint32_t sid; std::string p;
    while (recvFrame(fd, t, sid, p)) {
        if (t == 0x13 && !p.empty()) {
            upstream += p;
            std::string win; put32(win, (uint32_t)p.size());
            sendFrame(fd, 0x14, 1, win);   // 归还 credit
            if (upstream.size() > MAX_PROXY_BODY) break;
        } else if (t == 0x15 || t == 0x16) {
            break;
        }
    }
    close(fd);

    size_t hdrEnd = upstream.find("\r\n\r\n");
    if (hdrEnd == std::string::npos) {
        ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
        ctx->send("bad upstream response");
        return;
    }
    std::string head = upstream.substr(0, hdrEnd);
    std::string rbody = upstream.substr(hdrEnd + 4);

    int status = 502;
    std::vector<std::pair<std::string, std::string>> hdrs;
    {
        size_t eol = head.find("\r\n");
        std::string line = head.substr(0, eol);
        sscanf(line.c_str(), "HTTP/%*s %d", &status);
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
        if (k == "transfer-encoding" && h.second.find("chunked") != std::string::npos) chunked = true;
        if (k == "content-type") ctype = h.second;
    }
    if (chunked) rbody = dechunk(rbody);

    ctx->response->status_code = (http_status)status;
    for (auto& h : hdrs) {
        std::string k = lower(h.first);
        if (k == "transfer-encoding" || k == "connection" || k == "content-length") continue;
        if (k == "set-cookie") {
            std::string cprefix = "d_" + devid + "_" + port_s + "_";
            std::string v = h.second;
            size_t eq = v.find('=');
            if (eq != std::string::npos) {
                std::string name = trim(v.substr(0, eq));
                std::string tail = v.substr(eq);
                // Path 改成代理前缀
                size_t ppos = lower(tail).find("; path=");
                if (ppos != std::string::npos) {
                    size_t end = tail.find(';', ppos + 2);
                    tail = tail.substr(0, ppos) + "; Path=" + prefix + "/" +
                           (end == std::string::npos ? "" : tail.substr(end));
                }
                h.second = cprefix + name + tail;
            }
            ctx->response->headers["Set-Cookie"] = h.second;
            continue;
        }
        if (k == "location" && !h.second.empty() && h.second[0] == '/') {
            h.second = prefix + h.second;
        }
        ctx->response->headers[h.first] = h.second;
    }

    // text/html、text/css 里以 / 开头的绝对引用加前缀（best-effort）
    std::string lct = lower(ctype);
    if (lct.find("text/html") != std::string::npos || lct.find("text/css") != std::string::npos) {
        rbody = replaceAll(rbody, "href=\"/", "href=\"" + prefix + "/");
        rbody = replaceAll(rbody, "src=\"/", "src=\"" + prefix + "/");
        rbody = replaceAll(rbody, "action=\"/", "action=\"" + prefix + "/");
        rbody = replaceAll(rbody, "url(/", "url(" + prefix + "/");
    }
    ctx->response->body = rbody;
    if (!ctype.empty()) ctx->response->headers["Content-Type"] = ctype;
}
