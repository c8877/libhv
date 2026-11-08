// 设备采样配置
#include "admin_server.hpp"
#include "cpputil/json.hpp"

using nlohmann::json;

static std::string joinList(const json& a) {
    std::string out;
    if (!a.is_array()) return out;
    for (auto& v : a) {
        if (!v.is_string()) continue;
        if (!out.empty()) out += ",";
        out += v.get<std::string>();
    }
    return out;
}

void AdminServer::handleMetricCfgGet(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    std::string out;
    db_.metricGet(devid, out);
    relay_->pushMetricCfg(devid.c_str(), out);
    ctx->send(out);
}

void AdminServer::handleMetricCfgPut(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    json body;
    try { body = json::parse(ctx->body()); } catch (...) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    int enabled = body.value("enabled", true) ? 1 : 0;
    int interval = body.value("interval_sec", 5);
    int points = body.value("points", 720);
    int persist = body.value("persist", false) ? 1 : 0;
    json m = body.value("metrics", json::array());
    std::string metrics = joinList(m);
    std::string ifaces = joinList(body.value("interfaces", json::array()));
    std::string ttys = joinList(body.value("ttys", json::array()));

    if (interval < 1 || interval > 300 || points < 60 || points > 8640 || metrics.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad metric config\"}");
        return;
    }
    static const char* allow[] = {"cpu", "mem", "load", "net", "serial"};
    size_t pos = 0;
    while (pos <= metrics.size()) {
        size_t c = metrics.find(',', pos);
        std::string one = metrics.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
        bool ok = false;
        for (auto a : allow) if (one == a) ok = true;
        if (!ok) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"unknown metric\"}");
            return;
        }
        if (c == std::string::npos) break;
        pos = c + 1;
    }
    AdminActor actor = currentActor();
    std::string err;
    if (!db_.metricPut(devid, enabled, interval, points, metrics, ifaces, ttys, persist,
                       actor.username, err)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    audit(actor.username, "metrics.config", devid);
    std::string out;
    db_.metricGet(devid, out);
    relay_->pushMetricCfg(devid.c_str(), out);
    ctx->send(out);
}
