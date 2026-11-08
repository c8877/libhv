// 分指标采样配置与实时查询
#include "admin_server.hpp"
#include "cpputil/json.hpp"

using nlohmann::json;

bool AdminServer::pushMetricSeriesConfig(const std::string& devid) {
    std::string list;
    if (!db_.metricSeriesList(devid, list)) return false;
    auto rows = json::parse(list, nullptr, false);
    if (rows.is_discarded() || !rows.is_array()) return false;
    long long rev = 0;
    json out;
    json arr = json::array();
    for (auto& r : rows) {
        rev = std::max(rev, r.value("revision", (long long)0));
        json s;
        s["series"] = r.value("series", "");
        s["enabled"] = r.value("enabled", true);
        s["interval_sec"] = r.value("interval_sec", 5);
        s["points"] = r.value("points", 720);
        arr.push_back(s);
    }
    out["revision"] = rev;
    out["series"] = arr;
    return relay_->pushMetricCfg(devid.c_str(), out.dump());
}

void AdminServer::handleMetricSeriesCfgGet(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    std::string out;
    db_.metricSeriesList(devid, out);
    pushMetricSeriesConfig(devid);
    ctx->send(out);
}

void AdminServer::handleMetricSeriesCfgPut(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    auto body = json::parse(ctx->body(), nullptr, false);
    if (body.is_discarded() || !body.is_array() || body.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad config\"}");
        return;
    }
    AdminActor actor = currentActor();
    long long rev = 0;
    std::string err;
    if (!db_.metricSeriesPut(devid, body.dump(), actor.username, rev, err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    audit(actor.username, "metrics.series_config", devid);
    pushMetricSeriesConfig(devid);
    std::string out;
    db_.metricSeriesList(devid, out);
    ctx->send(out);
}

void AdminServer::handleMetricSeriesData(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    std::string series = ctx->param("series");
    std::string since = ctx->param("since_ts");
    if (series.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"series required\"}");
        return;
    }
    json q;
    q["series"] = series;
    q["since_ts"] = since.empty() ? 0LL : atoll(since.c_str());
    uint32_t rid = 0;
    std::string err;
    std::string ok = callRelay([this, &devid, &q, &rid, &err]() {
        return relay_->startMetricQuery(devid.c_str(), q.dump(), rid, err) ? "1" : "0";
    });
    if (ok != "1") {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    std::string data;
    for (int i = 0; i < 30 && data.empty(); ++i) {
        usleep(100000);
        data = callRelay([this, rid]() { return relay_->takeMetricData(rid); });
    }
    if (data.empty()) {
        ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
        ctx->send("{\"error\":\"timeout\"}");
        return;
    }
    ctx->send(data);
}
