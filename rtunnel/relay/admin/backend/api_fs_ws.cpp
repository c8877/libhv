// 管理台设备内建服务 WebSocket 桥（文件管理 / 服务管理）
//
// 浏览器 <-> relay-admin <-> RTNP(OPEN_FS / OPEN_SVC) <-> agent
// relay-admin 只搬运 WebSocket 消息，不解析业务协议；权限由 relay 校验、数据由 agent 校验。
#include "admin_server.hpp"
#include "cpputil/json.hpp"
#include "hash_util.hpp"
#include "proxy_util.hpp"
#include "rtnp/codec.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

struct PushJob {
    std::atomic<bool> cancel{false};
    std::atomic<bool> done{false};
};

struct DevWsSession {
    WebSocketChannelPtr ws;
    int fd = -1;
    std::mutex io_mu;   // 保护 fd 的发送/关闭，避免 close 后 fd 号被复用
    std::mutex ws_mu;   // 桥接线程和部署线程都会往同一个 WS 写
    std::string devid;
    std::string role;
    std::string actor_label;   // 打开时抓取，部署线程写审计用
    std::string tag;    // fs / svc，用于审计
    bool        can_deploy = false;   // 打开时判定的部署权限，消息路径不能读 tl_actor
    std::thread th;
    std::atomic<bool> closed{false};
    std::shared_ptr<PushJob> push;
};

std::mutex g_dev_mu;
std::map<hv::WebSocketChannel*, std::shared_ptr<DevWsSession>> g_dev_ws;

static std::string pathDir(const std::string& p) {
    size_t i = p.rfind('/');
    return i == std::string::npos || i == 0 ? "/" : p.substr(0, i);
}

static std::string pathName(const std::string& p) {
    size_t i = p.rfind('/');
    return i == std::string::npos ? p : p.substr(i + 1);
}

// 把仓库文件直推到设备路径：单独开一条 OPEN_FS 流，浏览器只收进度，不中转文件内容。
static void runRepoPush(const AdminConfig& cfg, const std::string& devid, const std::string& role,
                        const std::string& repo, const std::string& target, bool overwrite,
                        uint32_t req_id, const std::shared_ptr<PushJob>& job,
                        std::function<void(const std::string&)> wsSend,
                        std::function<void(const std::string&, const std::string&, const std::string&)> auditFn) {
    auto reply = [&](bool ok, const std::string& err, bool conflict, uint64_t sent) {
        nlohmann::json j;
        j["op"] = "push_done";
        j["id"] = req_id;
        j["ok"] = ok;
        j["path"] = target;
        j["sent"] = sent;
        if (conflict) j["conflict"] = true;
        if (!err.empty()) j["message"] = err;
        wsSend(j.dump());
        job->done = true;
    };

    std::string src;
    struct stat st{};
    if (cfg.file_dir.empty() || !adminRepoPath(cfg.file_dir, repo, src) ||
        stat(src.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        reply(false, "仓库文件不存在", false, 0);
        return;
    }
    uint64_t total = (uint64_t)st.st_size;
    FILE* fp = fopen(src.c_str(), "rb");
    if (!fp) {
        reply(false, "读取仓库文件失败", false, 0);
        return;
    }
    std::string err;
    int fd = openFsStream(cfg, devid, true, err);
    if (fd < 0) {
        fclose(fp);
        reply(false, err, false, 0);
        return;
    }
    struct timeval tv = {20, 0};   // 等待设备 ack 的超时
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    auto sendCtrl = [fd](const nlohmann::json& j) {
        std::string p;
        p.push_back(0);   // JSON
        p += j.dump();
        sendData(fd, 1, p);
    };
    auto recvCtrl = [fd](nlohmann::json& out) -> bool {
        uint8_t t;
        uint32_t sid;
        std::string p;
        for (;;) {
            if (!recvFrame(fd, t, sid, p)) return false;
            if (t != 0x13 || p.empty() || p[0] != 0) continue;
            out = nlohmann::json::parse(p.begin() + 1, p.end(), nullptr, false);
            if (!out.is_discarded() && out.is_object()) return true;
        }
    };
    auto finish = [&](bool ok, const std::string& msg, bool conflict, uint64_t sent) {
        close(fd);
        fclose(fp);
        reply(ok, msg, conflict, sent);
    };

    // 每条 FS 流都必须先 hello，否则 agent 一律按“hello required”拒绝
    nlohmann::json hi;
    hi["op"] = "hello";
    hi["id"] = 1;
    sendCtrl(hi);
    nlohmann::json hello;
    if (!recvCtrl(hello) || hello.value("op", "") != "welcome") {
        finish(false, "设备文件服务握手失败", false, 0);
        return;
    }

    uint32_t ctrl = 2;
    nlohmann::json op;
    op["op"] = "upload_open";
    op["id"] = ctrl;
    op["dir"] = pathDir(target);
    op["name"] = pathName(target);
    op["size"] = total;
    op["overwrite"] = overwrite;
    op["upload_id"] = "";
    sendCtrl(op);

    nlohmann::json r;
    if (!recvCtrl(r)) { finish(false, "设备未响应", false, 0); return; }
    std::string rop = r.value("op", "");
    if (rop == "error") {
        finish(false, r.value("message", std::string("部署失败")), r.value("code", -1) == 5, 0);
        return;
    }
    if (rop != "upload_ready") { finish(false, "设备返回异常: " + rop, false, 0); return; }

    std::string upload_id = r.value("upload_id", "");
    uint32_t chunk = r.value("chunk", 30720u);
    uint32_t win = r.value("inflight", 1u);
    if (chunk == 0 || chunk > 30000) chunk = 30000;
    if (win == 0 || win > 8) win = 1;

    std::vector<char> buf(chunk);
    uint64_t next = 0, acked = 0;
    uint32_t inflight = 0;
    auto last = std::chrono::steady_clock::now();
    while (acked < total) {
        if (job->cancel) {
            nlohmann::json c;
            c["op"] = "upload_cancel";
            c["id"] = ctrl + 1;
            c["upload_id"] = upload_id;
            sendCtrl(c);
            finish(false, "已取消", false, acked);
            return;
        }
        while (inflight < win && next < total) {
            size_t n = fread(buf.data(), 1, chunk, fp);
            if (n == 0) { finish(false, "读取仓库文件失败", false, acked); return; }
            std::string b;
            b.resize(16 + n);
            b[0] = (char)(ctrl >> 24); b[1] = (char)(ctrl >> 16);
            b[2] = (char)(ctrl >> 8);  b[3] = (char)ctrl;
            b[4] = 2;   // upload
            b[5] = b[6] = b[7] = 0;
            for (int i = 0; i < 8; ++i) b[8 + i] = (char)(next >> (8 * (7 - i)));
            memcpy(&b[16], buf.data(), n);
            std::string p;
            p.push_back(2);   // binary
            p += b;
            sendData(fd, 1, p);
            next += n;
            ++inflight;
        }
        if (!recvCtrl(r)) { finish(false, "等待设备确认超时", false, acked); return; }
        rop = r.value("op", "");
        if (rop == "error") { finish(false, r.value("message", std::string("写入失败")), false, acked); return; }
        acked = r.value("received", acked);
        if (inflight) --inflight;

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last).count() >= 200 || acked >= total) {
            nlohmann::json p;
            p["op"] = "push_progress";
            p["id"] = req_id;
            p["sent"] = acked;
            p["total"] = total;
            wsSend(p.dump());
            last = now;
        }
    }

    nlohmann::json f;
    f["op"] = "upload_finish";
    f["id"] = ctrl + 1;
    f["upload_id"] = upload_id;
    f["path"] = target;
    sendCtrl(f);
    if (!recvCtrl(r)) { finish(false, "等待落盘超时", false, acked); return; }
    if (r.value("op", "") == "error") {
        finish(false, r.value("message", std::string("落盘失败")), false, acked);
        return;
    }
    if (r.value("op", "") != "upload_done") {
        finish(false, "设备返回异常: " + r.value("op", std::string()), false, acked);
        return;
    }
    auditFn(role, "files.deploy", devid + " " + target + " " + std::to_string(total));
    finish(true, "", false, total);
}

} // namespace

bool AdminServer::handleFsWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    return handleAgentWsOpen(channel, req, rtnp::OPEN_FS, "/ws/fs/", "fs");
}

bool AdminServer::handleSvcWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req) {
    return handleAgentWsOpen(channel, req, rtnp::OPEN_SVC, "/ws/svc/", "svc");
}

bool AdminServer::handleAgentWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req,
                                    uint16_t flags, const std::string& prefix, const char* tag) {
    std::string rest = req->path.substr(prefix.size());
    size_t q = rest.find('?');
    if (q != std::string::npos) rest = rest.substr(0, q);
    std::string devid = urlDecode(rest);
    if (devid.empty()) {
        channel->close();
        return true;
    }
    std::string role;
    AdminActor actor;
    bool svc_stream = (flags & rtnp::OPEN_SVC) != 0;
    if (!wsAuthorize(req, actor, role) || !ownsDevid(devid) ||
        (svc_stream ? !hasPerm("device.svc")
                    : !(hasPerm("file.read") || hasPerm("file.write")))) {
        channel->close();
        return true;
    }
    bool want_rw = hasPerm("file.write");
    // 部署权限在打开时判定并落到会话上：WS 消息回调与 HTTP 请求共用 worker 线程，
    // tl_actor 会被同线程上其它请求覆盖，消息路径不能再读它。
    bool can_deploy = !svc_stream && hasPerm("repo.read") && want_rw;
    std::string err;
    int fd = (flags & rtnp::OPEN_SVC) ? openSvcStream(cfg_, devid, err)
                                      : openFsStream(cfg_, devid, want_rw, err);
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
    bool is_svc = (flags & rtnp::OPEN_SVC) != 0;
    audit(role, std::string(tag) + ".open", devid + (is_svc ? "" : (want_rw ? " rw" : " ro")));

    auto s = std::make_shared<DevWsSession>();
    s->ws = channel;
    s->fd = fd;
    s->devid = devid;
    s->role = role;
    s->actor_label = actorLabel();
    s->tag = tag;
    s->can_deploy = can_deploy;
    {
        std::lock_guard<std::mutex> lk(g_dev_mu);
        g_dev_ws[channel.get()] = s;
    }
    registerConsoleWs(channel, actor, tag);
    s->th = std::thread([s, channel]() {
        uint8_t t;
        uint32_t sid;
        std::string p;
        while (!s->closed.load()) {
            if (!recvFrame(s->fd, t, sid, p)) break;
            if (t == 0x13 && !p.empty()) {
                // 与 handleAgentWsMessage 的 sendData 同锁：一帧分两次 write，
                // 两个线程同时写会把帧头和数据插花，relay 侧会判 bad frame 断链
                {
                    std::lock_guard<std::mutex> lk(s->io_mu);
                    if (s->fd >= 0) sendWindow(s->fd, 1, (uint32_t)p.size());
                }
                bool binary = p[0] != 0;
                if (channel->isConnected()) {
                    std::string out = p.substr(1);
                    std::lock_guard<std::mutex> lk(s->ws_mu);
                    channel->send(out.data(), (int)out.size(), binary ? WS_OPCODE_BINARY : WS_OPCODE_TEXT);
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

bool AdminServer::handleAgentWsMessage(const WebSocketChannelPtr& channel, const std::string& msg) {
    std::shared_ptr<DevWsSession> s;
    {
        std::lock_guard<std::mutex> lk(g_dev_mu);
        auto it = g_dev_ws.find(channel.get());
        if (it == g_dev_ws.end()) return false;
        s = it->second;
    }
    if (s->closed) return true;

    // 部署：浏览器只发一个命令，文件由 relay 从本地仓库直推给设备（仅文件通道）
    if (s->tag == "fs" && channel->opcode == WS_OPCODE_TEXT && msg.size() < 4096 &&
        msg.find("push_") != std::string::npos) {
        nlohmann::json j = nlohmann::json::parse(msg, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            std::string op = j.value("op", "");
            if (op == "push_open") {
                uint32_t req_id = j.value("id", (uint32_t)0);
                std::string repo = j.value("repo", std::string());
                std::string target = j.value("path", std::string());
                bool overwrite = j.value("overwrite", false);
                auto err = [&](const std::string& m, bool conflict) {
                    nlohmann::json r;
                    r["op"] = "push_done";
                    r["id"] = req_id;
                    r["ok"] = false;
                    r["path"] = target;
                    if (conflict) r["conflict"] = true;
                    r["message"] = m;
                    std::lock_guard<std::mutex> lk(s->ws_mu);
                    if (s->ws && s->ws->isConnected()) s->ws->send(r.dump());
                };
                // 仓库是全局资源，推送到设备属于设备写操作：两者都要有
                if (!s->can_deploy) { err("没有部署权限", false); return true; }
                if (repo.empty() || !adminValidRepoPath(repo)) { err("非法仓库文件路径", false); return true; }
                if (target.size() < 2 || target[0] != '/' || target.size() > 1024 ||
                    target.find("..") != std::string::npos || target.back() == '/') {
                    err("非法目标路径", false);
                    return true;
                }
                if (s->push && !s->push->done) { err("已有部署任务正在进行", false); return true; }

                auto job = std::make_shared<PushJob>();
                s->push = job;
                std::string devid = s->devid, role = s->role;
                std::string repoName = repo, dest = target;
                std::shared_ptr<DevWsSession> ss = s;
                std::thread([this, ss, job, devid, role, repoName, dest, overwrite, req_id]() {
                    auto wsSend = [ss](const std::string& t) {
                        std::lock_guard<std::mutex> lk(ss->ws_mu);
                        if (ss->ws && ss->ws->isConnected()) ss->ws->send(t);
                    };
                    std::string who = ss->actor_label;
                    auto auditFn = [this, who](const std::string& r, const std::string& a, const std::string& d) {
                        auditAs(who.empty() ? r : who, a, d);
                    };
                    runRepoPush(cfg_, devid, role, repoName, dest, overwrite, req_id, job, wsSend, auditFn);
                }).detach();
                return true;
            }
            if (op == "push_cancel") {
                if (s->push && !s->push->done) s->push->cancel = true;
                return true;
            }
        }
    }
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

bool AdminServer::handleAgentWsClose(const WebSocketChannelPtr& channel) {
    std::shared_ptr<DevWsSession> s;
    {
        std::lock_guard<std::mutex> lk(g_dev_mu);
        auto it = g_dev_ws.find(channel.get());
        if (it == g_dev_ws.end()) return false;
        s = it->second;
        g_dev_ws.erase(it);
    }
    s->closed = true;
    if (s->push && !s->push->done) s->push->cancel = true;
    {
        std::lock_guard<std::mutex> lk(s->io_mu);
        if (s->fd >= 0) {
            sendFrame(s->fd, 0x15, 1, std::string());
            shutdown(s->fd, SHUT_RDWR);
        }
    }
    audit(s->role, s->tag + ".close", s->devid);
    return true;
}
