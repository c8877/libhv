// 管理台串口 WebSocket 调试
#include "admin_server.hpp"
#include "proxy_util.hpp"
#include "cpputil/json.hpp"
#include "common/logging.hpp"

#include <atomic>
#include <dirent.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

using nlohmann::json;

namespace {

struct TtyWsSession {
    WebSocketChannelPtr ws;
    int fd = -1;
    bool readonly = false;
    std::thread th;
    std::atomic<bool> closed{false};
    FILE* cap = nullptr;
    std::mutex cap_mu;
    std::string file;
};

std::mutex g_tty_mu;
std::map<hv::WebSocketChannel*, std::shared_ptr<TtyWsSession>> g_tty_ws;

std::string b64encode(const unsigned char* p, size_t n) {
    static const char* tb = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = p[i] << 16;
        if (i + 1 < n) v |= p[i + 1] << 8;
        if (i + 2 < n) v |= p[i + 2];
        o += tb[(v >> 18) & 63]; o += tb[(v >> 12) & 63];
        o += (i + 1 < n) ? tb[(v >> 6) & 63] : '=';
        o += (i + 2 < n) ? tb[v & 63] : '=';
    }
    return o;
}

void capWrite(TtyWsSession* s, const char* dir, const unsigned char* p, size_t n) {
    if (!s->cap) return;
    std::lock_guard<std::mutex> lk(s->cap_mu);
    fprintf(s->cap, "{\"ts\":%lld,\"dir\":\"%s\",\"len\":%zu,\"data\":\"%s\"}\n",
            (long long)time(NULL) * 1000, dir, n, b64encode(p, n).c_str());
    fflush(s->cap);
}

std::string b64decode(const std::string& in) {
    static const std::string tb = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int val = 0, bits = -8;
    std::string out;
    for (unsigned char c : in) {
        if (c == '=') break;
        size_t p = tb.find((char)c);
        if (p == std::string::npos) continue;
        val = (val << 6) + (int)p;
        bits += 6;
        if (bits >= 0) { out.push_back((char)((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

} // namespace

bool AdminServer::handleTtyWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    // /ws/tty/<devid>/<alias>
    std::string rest = req->path.substr(8);
    size_t s1 = rest.find('/');
    if (s1 == std::string::npos) return false;
    std::string devid = proxyUrlDecode(rest.substr(0, s1));
    std::string alias = proxyUrlDecode(rest.substr(s1 + 1));
    size_t q = alias.find('?');
    if (q != std::string::npos) alias = alias.substr(0, q);
    AdminActor actor;
    std::string role;
    if (!wsAuthorize(req, actor, role) || (role != "admin" && role != "manager" && role != "operator")) {
        channel->close();
        return true;
    }
    std::string path, parity;
    int baud = 115200, db = 8, stop = 1;
    bool enabled = false;
    if (!db_.ttyResolve(devid, alias, path, baud, db, parity, stop, enabled) || !enabled) {
        channel->send("{\"op\":\"error\",\"msg\":\"tty not available\"}");
        channel->close();
        return true;
    }
    bool monitor = req->GetParam("monitor") == "1";
    bool takeover = req->GetParam("takeover") == "1";
    if (takeover && role != "admin") {
        channel->send("{\"op\":\"error\",\"msg\":\"takeover requires admin\"}");
        channel->close();
        return true;
    }
    uint16_t flags = 0x0002;
    if (monitor) flags |= 0x0004;
    if (takeover) flags |= 0x0008;
    std::string err;
    int fd = openTtyStream(cfg_, devid, path, baud, db, parity.empty() ? 'N' : parity[0], stop, flags, err);
    if (fd < 0) {
        channel->send("{\"op\":\"error\",\"msg\":\"" + err + "\"}");
        channel->close();
        return true;
    }
    auto s = std::make_shared<TtyWsSession>();
    s->ws = channel;
    s->fd = fd;
    s->readonly = monitor;
    if (!cfg_.file_dir.empty()) {
        mkdir(cfg_.file_dir.c_str(), 0755);
        std::string dir = cfg_.file_dir + "/ttylogs";
        mkdir(dir.c_str(), 0755);
        char ts[32];
        snprintf(ts, sizeof(ts), "%lld", (long long)time(NULL));
        std::string safe = devid + "-" + alias;
        for (char& c : safe) if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) c = '_';
        std::string fn = "tty-" + safe + "-" + ts + ".jsonl";
        s->cap = fopen((dir + "/" + fn).c_str(), "w");
        if (s->cap) s->file = fn;
    }
    channel->send("{\"op\":\"open_ok\",\"file\":\"" + s->file + "\"}");
    {
        std::lock_guard<std::mutex> lk(g_tty_mu);
        g_tty_ws[channel.get()] = s;
    }
    audit(actor.username, takeover ? "tty.takeover" : monitor ? "tty.monitor" : "tty.debug_open",
          devid + " " + path);
    s->th = std::thread([s, channel, devid, path]() {
        uint8_t t; uint32_t sid; std::string p;
        while (!s->closed.load()) {
            if (!recvFrame(s->fd, t, sid, p)) break;
            if (t == 0x13 && !p.empty()) {
                sendWindow(s->fd, 1, (uint32_t)p.size());
                capWrite(s.get(), "rx", (const unsigned char*)p.data(), p.size());
                channel->send(p, WS_OPCODE_BINARY);
            } else if (t == 0x15 || t == 0x16) {
                break;
            }
        }
        s->closed = true;
        if (s->cap) { fclose(s->cap); s->cap = nullptr; }
        channel->close();
        logf_("[tty] debug closed %s %s", devid.c_str(), path.c_str());
    });
    s->th.detach();
    return true;
}

bool AdminServer::handleTtyWsMessage(const WebSocketChannelPtr& channel, const std::string& msg) {
    std::shared_ptr<TtyWsSession> s;
    {
        std::lock_guard<std::mutex> lk(g_tty_mu);
        auto it = g_tty_ws.find(channel.get());
        if (it == g_tty_ws.end()) return false;
        s = it->second;
    }
    auto j = json::parse(msg, nullptr, false);
    if (j.is_discarded() || j.value("op", "") != "write") return true;
    if (s->readonly) return true;
    std::string data = b64decode(j.value("data", ""));
    if (!data.empty() && !s->closed.load()) {
        sendData(s->fd, 1, data);
        capWrite(s.get(), "tx", (const unsigned char*)data.data(), data.size());
    }
    return true;
}

bool AdminServer::handleTtyWsClose(const WebSocketChannelPtr& channel) {
    std::shared_ptr<TtyWsSession> s;
    {
        std::lock_guard<std::mutex> lk(g_tty_mu);
        auto it = g_tty_ws.find(channel.get());
        if (it == g_tty_ws.end()) return false;
        s = it->second;
        g_tty_ws.erase(it);
    }
    s->closed = true;
    if (s->cap) { fclose(s->cap); s->cap = nullptr; }
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    return true;
}

void AdminServer::handleTtyLogDownload(const HttpContextPtr& ctx) {
    std::string name = ctx->param("name");
    if (name.empty() || name.find('/') != std::string::npos || name.find("..") != std::string::npos ||
        cfg_.file_dir.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad name\"}");
        return;
    }
    std::string path = cfg_.file_dir + "/ttylogs/" + name;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { ctx->response->status_code = HTTP_STATUS_NOT_FOUND; ctx->send("{\"error\":\"not found\"}"); return; }
    std::string data;
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    fclose(f);
    ctx->response->content_type = APPLICATION_OCTET_STREAM;
    ctx->response->headers["Content-Disposition"] = "attachment; filename=\"" + name + "\"";
    ctx->send(data);
}

void AdminServer::cleanupTtyLogs() {
    if (cfg_.file_dir.empty()) return;
    std::string dir = cfg_.file_dir + "/ttylogs";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    time_t cutoff = time(NULL) - 7 * 24 * 3600;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string p = dir + "/" + n;
        struct stat st{};
        if (stat(p.c_str(), &st) == 0 && st.st_mtime < cutoff) unlink(p.c_str());
    }
    closedir(d);
}
