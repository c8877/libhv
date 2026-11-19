#include "svc_session.hpp"
#include "fs_service.hpp"   // 复用 ErrCode 取值（与文件管理同一套 WS 错误码）
#include "hloop.h"

#include <atomic>
#include <cstring>

namespace rtvs {

namespace {

struct PostTask {
    hevent_t ev;
    std::function<void()> fn;
};

void postCb(hevent_t* ev) {
    auto* t = static_cast<PostTask*>(hevent_userdata(ev));
    if (t->fn) t->fn();
    delete t;
}

void postTo(hloop_t* loop, const std::shared_ptr<Session::Core>& core, std::function<void()> fn) {
    if (loop == nullptr) return;
    auto* t = new PostTask;
    memset(&t->ev, 0, sizeof(t->ev));
    t->ev.cb = postCb;
    t->ev.userdata = t;
    t->fn = [core, fn]() {
        if (core->alive.load(std::memory_order_acquire)) fn();
    };
    hloop_post_event(loop, &t->ev);
}

} // namespace

Session::Session(Service* svc, Sink* sink, hloop_t* loop, const std::string& label, bool trusted,
                 bool own_sink)
    : svc_(svc), sink_(sink), loop_(loop), label_(label), trusted_(trusted), own_sink_(own_sink),
      core_(std::make_shared<Core>()) {}

Session::~Session() {
    onClose();
    if (own_sink_) delete sink_;
    sink_ = nullptr;
}

void Session::onClose() {
    if (closed_) return;
    closed_ = true;
    core_->alive.store(false, std::memory_order_release);
}

void Session::sendJson(const nlohmann::json& j) {
    if (closed_ || sink_ == nullptr) return;
    sink_->send(j.dump());
}

void Session::sendError(uint32_t id, int code, const std::string& text) {
    nlohmann::json e;
    e["op"] = "error";
    e["id"] = id;
    e["code"] = code;
    e["message"] = text;
    sendJson(e);
}

void Session::onText(const std::string& msg) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(msg);
    }
    catch (...) {
        sendError(0, rtfs::BAD_REQUEST, "bad json");
        return;
    }
    if (!j.is_object()) {
        sendError(0, rtfs::BAD_REQUEST, "bad json");
        return;
    }
    uint32_t id = 0;
    if (j.contains("id") && j["id"].is_number()) id = j["id"].get<uint32_t>();
    std::string op = j.value("op", std::string());
    if (op.empty()) {
        sendError(id, rtfs::BAD_REQUEST, "missing op");
        return;
    }
    if (op == "hello") {
        handleHello(j, id);
        return;
    }
    if (!authed_) {
        sendError(id, rtfs::UNAUTHORIZED, "hello required");
        return;
    }
    if (op == "list") handleList(j, id);
    else if (op == "logs") handleLogs(j, id);
    else if (op == "action") handleAction(j, id);
    else sendError(id, rtfs::BAD_REQUEST, "unknown op: " + op);
}

void Session::handleHello(const nlohmann::json& j, uint32_t id) {
    bool token_ok = true;
    if (svc_->tokenRequired() && !trusted_) {
        token_ok = svc_->checkToken(j.value("token", std::string()));
    }
    if (!token_ok) {
        authed_ = false;
        sendError(id, rtfs::UNAUTHORIZED, "invalid service token");
        return;
    }
    authed_ = true;
    nlohmann::json w;
    w["op"] = "welcome";
    w["id"] = id;
    w["v"] = 1;
    w["label"] = label_;
    w["systemd"] = svc_->systemd();
    w["writable"] = svc_->writable();
    w["readonly"] = !svc_->writable();
    w["token_required"] = svc_->tokenRequired() && !trusted_;
    sendJson(w);
}

void Session::handleList(const nlohmann::json& j, uint32_t id) {
    rtvs::ListOptions opt;
    opt.page = j.value("page", 1);
    opt.page_size = j.value("page_size", 10);
    opt.sort = j.value("sort", std::string());
    opt.dir = j.value("dir", 1);
    opt.q = j.value("q", std::string());
    opt.tab = j.value("tab", std::string("all"));
    auto core = core_;
    Sink* sink = sink_;
    hloop_t* loop = loop_;
    svc_->listAsync(opt, [core, sink, loop, id](const nlohmann::json& r) {
        nlohmann::json out = r;
        out["op"] = "list";
        out["id"] = id;
        std::string body = out.dump();
        postTo(loop, core, [sink, body]() { if (sink) sink->send(body); });
    });
}

void Session::handleLogs(const nlohmann::json& j, uint32_t id) {
    std::string unit = j.value("unit", std::string());
    int lines = j.value("lines", 200);
    auto core = core_;
    Sink* sink = sink_;
    hloop_t* loop = loop_;
    svc_->logsAsync(unit, lines, [core, sink, loop, id](const nlohmann::json& r) {
        nlohmann::json out = r;
        out["op"] = "logs";
        out["id"] = id;
        std::string body = out.dump();
        postTo(loop, core, [sink, body]() { if (sink) sink->send(body); });
    });
}

void Session::handleAction(const nlohmann::json& j, uint32_t id) {
    std::string unit = j.value("unit", std::string());
    std::string verb = j.value("verb", std::string());
    auto core = core_;
    Sink* sink = sink_;
    hloop_t* loop = loop_;
    svc_->actionAsync(unit, verb, [core, sink, loop, id](const nlohmann::json& r) {
        nlohmann::json out = r;
        out["op"] = "action";
        out["id"] = id;
        std::string body = out.dump();
        postTo(loop, core, [sink, body]() { if (sink) sink->send(body); });
    });
}

} // namespace rtvs
