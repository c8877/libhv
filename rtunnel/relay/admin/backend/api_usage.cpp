// 用量报表：查 usage_daily（日聚合），不扫流水
#include "admin_server.hpp"

#include "cpputil/json.hpp"

#include <string>

using nlohmann::json;

void AdminServer::handleUsage(const HttpContextPtr& ctx) {
    std::string from, to, subject_type, subject_id, metric, granularity;
    for (const auto& kv : ctx->request->query_params) {
        if (kv.first == "from") from = kv.second;
        else if (kv.first == "to") to = kv.second;
        else if (kv.first == "subject_type") subject_type = kv.second;
        else if (kv.first == "subject_id") subject_id = kv.second;
        else if (kv.first == "metric") metric = kv.second;
        else if (kv.first == "granularity") granularity = kv.second;
    }
    if (granularity.empty()) granularity = "day";
    if (granularity != "day" && granularity != "total") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"granularity must be day|total\"}");
        return;
    }
    if (subject_type != "" && subject_type != "token" && subject_type != "device") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"subject_type must be token|device\"}");
        return;
    }

    std::string out, err;
    if (!db_.usageQuery(from, to, subject_type, subject_id, metric, granularity == "total", out, err)) {
        ctx->response->status_code = err == "bad date" ? HTTP_STATUS_BAD_REQUEST : HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    ctx->send(out);
}
