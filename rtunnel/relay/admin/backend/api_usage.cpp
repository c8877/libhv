// 用量报表：查 usage_daily（日聚合），不扫流水
#include "admin_server.hpp"

#include "cpputil/json.hpp"

#include <string>

using nlohmann::json;

extern thread_local AdminActor tl_actor;

void AdminServer::handleUsage(const HttpContextPtr& ctx) {
    std::string from, to, subject_type, subject_id, metric, granularity, tenant_arg;
    for (const auto& kv : ctx->request->query_params) {
        if (kv.first == "from") from = kv.second;
        else if (kv.first == "to") to = kv.second;
        else if (kv.first == "subject_type") subject_type = kv.second;
        else if (kv.first == "subject_id") subject_id = kv.second;
        else if (kv.first == "metric") metric = kv.second;
        else if (kv.first == "granularity") granularity = kv.second;
        else if (kv.first == "tenant_id") tenant_arg = kv.second;
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

    // 接入账号只能看本租户；控制台账号可以看全部，或按 tenant_id 过滤
    long long tenant_id = 0;
    if (tl_actor.is_client) {
        tenant_id = tl_actor.tenant_id;
    } else if (!tenant_arg.empty()) {
        tenant_id = atoll(tenant_arg.c_str());
    }
    std::string out, err;
    if (!db_.usageQuery(from, to, subject_type, subject_id, metric, tenant_id,
                        granularity == "total", out, err)) {
        ctx->response->status_code = err == "bad date" ? HTTP_STATUS_BAD_REQUEST : HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    ctx->send(out);
}
