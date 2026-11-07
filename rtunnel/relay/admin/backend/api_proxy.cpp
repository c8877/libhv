// 设备 Web 代理（路径式）：/device/<devid>/<port>/<rest>
// 认证复用管理台会话；数据面走本机 relay 的 RTNP OPEN，与 Web 终端同一机制。
#include "admin_server.hpp"
#include "proxy_util.hpp"
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
    std::string devid = proxyUrlDecode(rest.substr(0, s1));
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
            if (raw.size() > 64 * 1024) break;   // 响应头上限
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
