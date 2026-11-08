// 分指标采样配置与实时查询
#include "admin_server.hpp"
#include "cpputil/json.hpp"
#include "common/gzip_util.hpp"
#include "common/lz4_util.hpp"

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
    std::string st = relay_->takeMetricStatus(devid);
    auto sj = nlohmann::json::parse(st, nullptr, false);
    if (!sj.is_discarded() && sj.value("applied", false)) {
        db_.metricSeriesAck(devid, sj.value("revision", (long long)0));
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
    if (data[0] == '\2') {
        std::string plain;
        if (!lz4Decompress(data.data() + 1, data.size() - 1, plain)) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"lz4 failed\"}");
            return;
        }
        data.swap(plain);
    } else if (data[0] == '\1') {
        std::string plain;
        if (!gzipDecompress(data.data() + 1, data.size() - 1, plain)) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"gunzip failed\"}");
            return;
        }
        data.swap(plain);
    } else if (data[0] == '\0') {
        data.erase(0, 1);
    }
    bool full = since.empty() || since == "0";
    std::string ae = ctx->header("Accept-Encoding");
    if (full && ae.find("gzip") != std::string::npos) {
        std::string gz;
        if (gzipCompress(data, gz)) {
            ctx->response->headers["Content-Encoding"] = "gzip";
            ctx->response->content_type = APPLICATION_JSON;
            ctx->send(gz);
            return;
        }
    }
    ctx->response->content_type = APPLICATION_JSON;
    ctx->send(data);
}
