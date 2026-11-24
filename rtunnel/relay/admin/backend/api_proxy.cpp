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
#include <set>
#include <algorithm>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <chrono>
#include <unordered_map>
#include <random>
#include <atomic>


namespace {

// /device 代理跑在独立线程：openStream/recvFrame 是阻塞调用，
// 不能占着管理面 HTTP worker；每个请求一个线程，总量由 proxy.max_concurrent 限制。
struct ProxyJob {
    std::atomic<int>  fd{-1};
    std::atomic<bool> stop{false};
};

std::mutex                           g_proxy_mu;
std::set<std::shared_ptr<ProxyJob>>  g_proxy_jobs;
std::condition_variable              g_proxy_cv;

void proxyJobEnd(const std::shared_ptr<ProxyJob>& job) {
    std::lock_guard<std::mutex> lk(g_proxy_mu);
    g_proxy_jobs.erase(job);
    g_proxy_cv.notify_all();
}

// 关闭数据面 fd 前先把 job->fd 置空：shutdown 侧在同一把锁里 exchange，
// 避免「这边刚 close、那边拿旧 fd 号 shutdown 到别的连接」。
void closeJobFd(const std::shared_ptr<ProxyJob>& job, int fd) {
    {
        std::lock_guard<std::mutex> lk(g_proxy_mu);
        if (job->fd.load() == fd) job->fd.store(-1);
    }
    if (fd >= 0) close(fd);
}

void runDeviceProxy(std::shared_ptr<ProxyJob> job, const HttpContextPtr& ctx,
                    std::string req, const std::string& prefix, const std::string& devid,
                    const std::string& port_s, int port, const AdminConfig& cfg,
                    bool head_request) {
    ProxyGuard guard;
    std::string err;
    int fd = openStream(cfg, devid, port, err);
    if (fd < 0) {
        if (!job->stop.load()) {
            ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
            ctx->send("{\"error\":\"" + err + "\"}");
        }
        proxyJobEnd(job);
        return;
    }
    job->fd.store(fd);
    if (job->stop.load()) {
        closeJobFd(job, fd);
        proxyJobEnd(job);
        return;
    }

    // 只发请求，不做半关闭：上游的异步接口（procs/net/metrics）会延后写响应，
    // 半关闭会让本地 Web 先看到 EOF 并断开，响应就丢了。
    // 请求边界由 Content-Length / GET 无 body 确定，响应由 Connection: close 结束。
    sendData(fd, 1, req);

    // 先读到响应头
    std::string raw, p;
    uint8_t t; uint32_t sid;
    size_t hdrEnd = std::string::npos;
    while (!job->stop.load() && recvFrame(fd, t, sid, p)) {
        if (t == 0x13 && !p.empty()) {
            raw += p;
            sendWindow(fd, 1, (uint32_t)p.size());
            if (raw.size() > 64 * 1024) break;   // 响应头上限
            hdrEnd = raw.find("\r\n\r\n");
            if (hdrEnd != std::string::npos) break;
        } else if (t == 0x15 || t == 0x16) break;
    }
    if (hdrEnd == std::string::npos) {
        closeJobFd(job, fd);
        if (!job->stop.load()) {
            ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
            ctx->send("bad upstream response");
        }
        proxyJobEnd(job);
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
    if (!w) { closeJobFd(job, fd); proxyJobEnd(job); return; }
    bool no_body = (status == 204 || status == 304 || head_request);
    w->Begin();
    if (no_body) {
        w->EndHeaders();
        closeJobFd(job, fd);
        w->End();
        proxyJobEnd(job);
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
    while (!done && !client_gone && !job->stop.load() && recvFrame(fd, t, sid, p)) {
        if (t == 0x13 && !p.empty()) {
            sendWindow(fd, 1, (uint32_t)p.size());
            feed(p);
        } else if (t == 0x15 || t == 0x16) {
            break;
        }
    }
    closeJobFd(job, fd);
    if (!job->stop.load()) w->End();
    proxyJobEnd(job);
}

} // namespace

void adminProxyShutdown() {
    {
        std::lock_guard<std::mutex> lk(g_proxy_mu);
        for (auto& j : g_proxy_jobs) {
            j->stop.store(true);
            int fd = j->fd.exchange(-1);
            if (fd >= 0) shutdown(fd, SHUT_RDWR);
        }
    }
    std::unique_lock<std::mutex> lk(g_proxy_mu);
    g_proxy_cv.wait_for(lk, std::chrono::seconds(6), [] { return g_proxy_jobs.empty(); });
}

int AdminServer::handleDeviceProxy(const HttpContextPtr& ctx) {
    std::string path = ctx->path();
    // /device/<devid>/<port>/<rest>
    if (path.compare(0, 8, "/device/") != 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("not found");
        return (int)ctx->response->status_code;
    }
    std::string rest = path.substr(8);
    size_t s1 = rest.find('/');
    if (s1 == std::string::npos) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("bad device path");
        return (int)ctx->response->status_code;
    }
    std::string devid = urlDecode(rest.substr(0, s1));
    rest = rest.substr(s1 + 1);
    size_t s2 = rest.find('/');
    std::string port_s = (s2 == std::string::npos) ? rest : rest.substr(0, s2);
    std::string sub = (s2 == std::string::npos) ? "/" : rest.substr(s2);
    int port = atoi(port_s.c_str());
    if (devid.empty() || port <= 0 || port > 65535) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("bad device/port");
        return (int)ctx->response->status_code;
    }

    std::string role;
    // 等价于一次端口转发：需要 device.port，且端口在自己的范围里
    if (requirePerm(ctx, "device.port", role)) return (int)ctx->response->status_code;
    if (!adminPortsCovered(currentActor().ports, std::to_string(port))) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"port not allowed\"}");
        return (int)ctx->response->status_code;
    }
    if (g_proxy_active.load() >= settingInt("proxy.max_concurrent", 32)) {
        ctx->response->status_code = HTTP_STATUS_SERVICE_UNAVAILABLE;
        ctx->send("{\"error\":\"too many concurrent proxy requests\"}");
        return (int)ctx->response->status_code;
    }
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("forbidden");
        return (int)ctx->response->status_code;
    }
    if (sub.empty() || sub[0] != '/') sub = "/" + sub;
    // 转发查询串：本地页的 procs/net/metrics 依赖 page/q/series 等参数
    std::string qs = hv::dump_query_params(ctx->request->query_params);
    if (!qs.empty()) sub += "?" + qs;

    // 构造上游请求（只读 ctx，必须在 HTTP 线程完成）
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

    bool head_request = (ctx->request->method == HTTP_HEAD);
    auto job = std::make_shared<ProxyJob>();
    {
        std::lock_guard<std::mutex> lk(g_proxy_mu);
        g_proxy_jobs.insert(job);
    }
    AdminConfig cfg = cfg_;
    std::thread([job, ctx, req = std::move(req), prefix = std::move(prefix), devid = std::move(devid),
                 port_s = std::move(port_s), port, cfg = std::move(cfg), head_request]() mutable {
        runDeviceProxy(std::move(job), ctx, std::move(req), prefix, devid, port_s, port, cfg, head_request);
    }).detach();
    // 返回 HTTP_STATUS_NEXT：响应由数据面线程写，管理面 worker 立即释放
    return (int)HTTP_STATUS_NEXT;
}
