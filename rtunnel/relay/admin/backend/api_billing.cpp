// 费用：价目表与按租户试算（用量 × 单价，不产生发票）
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/json.hpp"

#include <string>

using nlohmann::json;

extern thread_local AdminActor tl_actor;

void AdminServer::handleBillingPrices(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.billingPrices(out)) { apiBad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handleBillingPriceSet(const HttpContextPtr& ctx, const std::string& role) {
    std::string metric = ctx->param("metric");
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    long long unit_size = req.value("unit_size", (long long)1);
    long long price_micro = req.value("price_micro", (long long)0);
    // 也接受“元”为单位的浮点，避免前端自己乘 1e6 丢精度
    if (req.contains("price_yuan") && req["price_yuan"].is_number()) {
        price_micro = (long long)(req["price_yuan"].get<double>() * 1000000.0 + 0.5);
    }
    std::string err;
    if (!db_.billingPriceSet(metric, unit_size, price_micro, req.value("currency", std::string("CNY")),
                             err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "billing.price.update",
          metric + " unit=" + std::to_string(unit_size) + " price_micro=" + std::to_string(price_micro));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleBillingSummary(const HttpContextPtr& ctx) {
    std::string from, to, tenant_arg;
    for (const auto& kv : ctx->request->query_params) {
        if (kv.first == "from") from = kv.second;
        else if (kv.first == "to") to = kv.second;
        else if (kv.first == "tenant_id") tenant_arg = kv.second;
    }
    // 接入令牌只算自己的租户
    long long tenant_id = 0;
    if (tl_actor.is_client) tenant_id = tl_actor.tenant_id;
    else if (!tenant_arg.empty()) tenant_id = atoll(tenant_arg.c_str());

    std::string out, err;
    if (!db_.billingSummary(from, to, tenant_id, out, err)) {
        ctx->response->status_code =
            err == "bad date" ? HTTP_STATUS_BAD_REQUEST : HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    ctx->send(out);
}
