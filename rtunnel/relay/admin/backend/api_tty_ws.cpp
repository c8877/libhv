// 管理台串口 WebSocket 调试
#include "admin_server.hpp"
#include "proxy_util.hpp"
#include "cpputil/json.hpp"
#include "common/logging.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

using nlohmann::json;

namespace {

struct TtyWsSession {
    WebSocketChannelPtr ws;
    int fd = -1;
    std::thread th;
    std::atomic<bool> closed{false};
};

std::mutex g_tty_mu;
std::map<hv::WebSocketChannel*, std::shared_ptr<TtyWsSession>> g_tty_ws;

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
    std::string err;
    int fd = openTtyStream(cfg_, devid, path, baud, db, parity.empty() ? 'N' : parity[0], stop, err);
    if (fd < 0) {
        channel->send("{\"op\":\"error\",\"msg\":\"" + err + "\"}");
        channel->close();
        return true;
    }
    auto s = std::make_shared<TtyWsSession>();
    s->ws = channel;
    s->fd = fd;
    {
        std::lock_guard<std::mutex> lk(g_tty_mu);
        g_tty_ws[channel.get()] = s;
    }
    audit(actor.username, "tty.debug_open", devid + " " + path);
    s->th = std::thread([s, channel, devid, path]() {
        uint8_t t; uint32_t sid; std::string p;
        while (!s->closed.load()) {
            if (!recvFrame(s->fd, t, sid, p)) break;
            if (t == 0x13 && !p.empty()) {
                sendWindow(s->fd, 1, (uint32_t)p.size());
                channel->send(p, WS_OPCODE_BINARY);
            } else if (t == 0x15 || t == 0x16) {
                break;
            }
        }
        s->closed = true;
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
    std::string data = b64decode(j.value("data", ""));
    if (!data.empty() && !s->closed.load()) sendData(s->fd, 1, data);
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
    if (s->fd >= 0) { close(s->fd); s->fd = -1; }
    return true;
}
