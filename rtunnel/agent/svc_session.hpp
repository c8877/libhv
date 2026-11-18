#pragma once
// 服务管理 WebSocket 协议：文本 JSON 控制帧。
// 本地 Web 与 relay 隧道共用；所有 systemctl/journalctl 调用都在 Service 的后台线程。

#include "svc_service.hpp"

#include <atomic>
#include <memory>
#include <stdint.h>
#include <string>

struct hloop_s;
using hloop_t = struct hloop_s;

namespace rtvs {

class Sink {
public:
    virtual ~Sink() {}
    virtual void send(const std::string& msg) = 0;
};

class Session {
public:
    Session(Service* svc, Sink* sink, hloop_t* loop, const std::string& label, bool trusted,
            bool own_sink = false);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    void onText(const std::string& msg);
    void onClose();

    // 后台线程回投事件循环时用来判断会话是否还活着
    struct Core {
        std::atomic<bool> alive{true};
    };

private:
    void sendJson(const nlohmann::json& j);
    void sendError(uint32_t id, int code, const std::string& text);
    void handleHello(const nlohmann::json& j, uint32_t id);
    void handleList(const nlohmann::json& j, uint32_t id);
    void handleLogs(const nlohmann::json& j, uint32_t id);
    void handleAction(const nlohmann::json& j, uint32_t id);

    Service* svc_ = nullptr;
    Sink* sink_ = nullptr;
    hloop_t* loop_ = nullptr;
    std::string label_;
    bool trusted_ = false;
    bool authed_ = false;
    bool closed_ = false;
    bool own_sink_ = false;
    std::shared_ptr<Core> core_;
};

} // namespace rtvs
