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

// ---- 结算单 ----

void AdminServer::handleBillingStatementGenerate(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string period = req.value("period", std::string());
    long long tenant_id = req.value("tenant_id", (long long)0);
    std::string out, err;
    if (!db_.billingStatementGenerate(period, tenant_id, role, out, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "billing.statement.generate",
          period + " tenant=" + std::to_string(tenant_id) + " " +
              std::to_string(json::parse(out, nullptr, false).value("created", json::array()).size()) +
              " created");
    ctx->send(out);
}

void AdminServer::handleBillingStatements(const HttpContextPtr& ctx) {
    std::string period, status, tenant_arg;
    for (const auto& kv : ctx->request->query_params) {
        if (kv.first == "period") period = kv.second;
        else if (kv.first == "status") status = kv.second;
        else if (kv.first == "tenant_id") tenant_arg = kv.second;
    }
    long long tenant_id = 0;
    if (tl_actor.is_client) tenant_id = tl_actor.tenant_id;   // 客户只看本租户
    else if (!tenant_arg.empty()) tenant_id = atoll(tenant_arg.c_str());
    std::string out;
    if (!db_.billingStatementList(period, tenant_id, status, out)) { apiBad(ctx, "db error"); return; }
    ctx->send(out);
}

// 取详情后校验租户：接入令牌不能读别家的结算单
bool AdminServer::billingStatementVisible(long long id, const HttpContextPtr& ctx) {
    std::string js, err;
    if (!db_.billingStatementGet(id, js, err)) { apiNotFound(ctx, err); return false; }
    if (tl_actor.is_client) {
        json s = json::parse(js, nullptr, false);
        if (s.is_discarded() || s.value("tenant_id", (long long)0) != tl_actor.tenant_id) {
            apiError(ctx, HTTP_STATUS_FORBIDDEN, "不在本租户");
            return false;
        }
    }
    return true;
}

void AdminServer::handleBillingStatementGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    if (!billingStatementVisible(id, ctx)) return;
    std::string out, err;
    if (!db_.billingStatementGet(id, out, err)) { apiNotFound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handleBillingStatementStatus(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string status = req.value("status", std::string());
    bool set_note = req.contains("note") && req["note"].is_string();
    std::string out, err;
    if (!db_.billingStatementSetStatus(id, status, req.value("note", std::string()), set_note, out, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "billing.statement.status", std::to_string(id) + " -> " + status);
    ctx->send(out);
}

void AdminServer::handleBillingStatementCsv(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    if (!billingStatementVisible(id, ctx)) return;
    std::string out, err;
    if (!db_.billingStatementCsv(id, out, err)) { apiNotFound(ctx, err); return; }
    ctx->response->SetHeader("Content-Type", "text/csv; charset=utf-8");
    ctx->response->SetHeader("Content-Disposition",
                             "attachment; filename=\"statement-" + std::to_string(id) + ".csv\"");
    ctx->send(out);
}
