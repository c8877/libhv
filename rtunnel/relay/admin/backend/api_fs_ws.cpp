// 管理台设备文件管理 WebSocket 桥
//
// 浏览器 <-> relay-admin <-> RTNP(OPEN_FS) <-> agent
// relay-admin 只搬运 WebSocket 消息，不解析文件协议；路径与权限由 agent 校验。
#include "admin_server.hpp"
#include "cpputil/json.hpp"
#include "proxy_util.hpp"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {

struct FsWsSession {
    WebSocketChannelPtr ws;
    int fd = -1;
    std::mutex io_mu;   // 保护 fd 的发送/关闭，避免 close 后 fd 号被复用
    std::string devid;
    std::string role;
    std::thread th;
    std::atomic<bool> closed{false};
};

std::mutex g_fs_mu;
std::map<hv::WebSocketChannel*, std::shared_ptr<FsWsSession>> g_fs_ws;

} // namespace

bool AdminServer::handleFsWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    // /ws/fs/<devid>
    std::string rest = req->path.substr(7);
    size_t q = rest.find('?');
    if (q != std::string::npos) rest = rest.substr(0, q);
    std::string devid = proxyUrlDecode(rest);
    if (devid.empty()) {
        channel->close();
        return true;
    }
    std::string role;
    AdminActor actor;
    if (!wsAuthorize(req, actor, role) || (role != "admin" && role != "manager" && role != "operator") || !ownsDevid(devid)) {
        channel->close();
        return true;
    }
    bool want_rw = role == "admin" || role == "manager";
    std::string err;
    int fd = openFsStream(cfg_, devid, want_rw, err);
    if (fd < 0) {
        nlohmann::json e;
        e["op"] = "error";
        e["id"] = 0;
        e["code"] = 4;
        e["message"] = err;
        channel->send(e.dump());
        channel->close();
        return true;
    }
    audit(role, "fs.open", devid + (want_rw ? " rw" : " ro"));

    auto s = std::make_shared<FsWsSession>();
    s->ws = channel;
    s->fd = fd;
    s->devid = devid;
    s->role = role;
    {
        std::lock_guard<std::mutex> lk(g_fs_mu);
        g_fs_ws[channel.get()] = s;
    }
    s->th = std::thread([s, channel]() {
        uint8_t t;
        uint32_t sid;
        std::string p;
        while (!s->closed.load()) {
            if (!recvFrame(s->fd, t, sid, p)) break;
            if (t == 0x13 && !p.empty()) {
                // 与 handleFsWsMessage 的 sendData 同锁：一帧分两次 write，
                // 两个线程同时写会把帧头和数据插花，relay 侧会判 bad frame 断链
                {
                    std::lock_guard<std::mutex> lk(s->io_mu);
                    if (s->fd >= 0) sendWindow(s->fd, 1, (uint32_t)p.size());
                }
                bool binary = p[0] != 0;
                if (channel->isConnected()) {
                    channel->send(p.data() + 1, (int)p.size() - 1, binary ? WS_OPCODE_BINARY : WS_OPCODE_TEXT);
                }
            }
            else if (t == 0x15 || t == 0x16) {
                break;
            }
        }
        s->closed = true;
        {
            std::lock_guard<std::mutex> lk(s->io_mu);
            if (s->fd >= 0) {
                close(s->fd);
                s->fd = -1;
            }
        }
        if (channel->isConnected()) channel->close();
    });
    s->th.detach();
    return true;
}

bool AdminServer::handleFsWsMessage(const WebSocketChannelPtr& channel, const std::string& msg) {
    std::shared_ptr<FsWsSession> s;
    {
        std::lock_guard<std::mutex> lk(g_fs_mu);
        auto it = g_fs_ws.find(channel.get());
        if (it == g_fs_ws.end()) return false;
        s = it->second;
    }
    if (s->closed) return true;
    // 一条 WS 消息 <= 30 KiB，加 kind 字节后仍远小于 RTNP MAX_PAYLOAD
    if (msg.size() + 1 > 32768) {
        channel->send("{\"op\":\"error\",\"id\":0,\"code\":6,\"message\":\"message too large\"}");
        return true;
    }
    std::string p;
    p.reserve(msg.size() + 1);
    p.push_back(channel->opcode == WS_OPCODE_BINARY ? (char)1 : (char)0);
    p.append(msg);
    std::lock_guard<std::mutex> lk(s->io_mu);
    if (s->fd < 0) return true;
    sendData(s->fd, 1, p);
    return true;
}

bool AdminServer::handleFsWsClose(const WebSocketChannelPtr& channel) {
    std::shared_ptr<FsWsSession> s;
    {
        std::lock_guard<std::mutex> lk(g_fs_mu);
        auto it = g_fs_ws.find(channel.get());
        if (it == g_fs_ws.end()) return false;
        s = it->second;
        g_fs_ws.erase(it);
    }
    s->closed = true;
    {
        std::lock_guard<std::mutex> lk(s->io_mu);
        if (s->fd >= 0) {
            sendFrame(s->fd, 0x15, 1, std::string());
            shutdown(s->fd, SHUT_RDWR);
        }
    }
    audit(s->role, "fs.close", s->devid);
    return true;
}
