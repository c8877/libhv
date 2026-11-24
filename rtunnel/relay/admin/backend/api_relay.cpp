// 中继地址字典、有序下发列表、同步任务（运维 → 中继列表）
//
// 下发列表 = 全局有序的期望状态（顺序即故障切换顺序），只存端点的引用；
// 保存只更新期望状态，主动下发走同步任务（复用 distribution_tasks，kind='relay'），
// 设备上线时由 AUTH_OK 自动对齐当前列表。
#include "admin_server.hpp"
#include "common/endpoint.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

using nlohmann::json;

static long long relayNowMs() { return AdminDb::nowMs(); }

// ---------------------------------------------------------------- 字典

// 把已保存的有序列表转成 relay 的 servers 字符串。
// 任何一条无效都返回 false：静默跳过会让“列表里有 3 个、下发只有 2 个”难以发现。
bool AdminServer::relayListSpec(std::string& servers) {
    servers.clear();
    std::string js;
    if (!db_.relayListJson(js)) return false;
    auto arr = json::parse(js, nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return false;
    std::vector<TunnelEndpoint> eps;
    for (auto& j : arr) {
        TunnelEndpoint ep;
        ep.host = j.value("host", std::string());
        ep.port = j.value("port", 0);
        if (ep.host.empty() || ep.port <= 0 || ep.port > 65535) return false;
        eps.push_back(ep);
    }
    servers = eps.empty() ? std::string() : tunnel_format_endpoints(eps);
    return true;
}

void AdminServer::handleRelayDictList(const HttpContextPtr& ctx) {
    std::string q = ctx->param("q");
    int page = atoi(ctx->param("page").c_str());
    int page_size = atoi(ctx->param("page_size").c_str());
    if (page <= 0) page = 1;
    if (page_size <= 0) page_size = 50;
    std::string out;
    int total = 0;
    if (!db_.relayDictPage(q, page, page_size, out, total)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    auto j = json::parse(out, nullptr, false);
    if (!j.is_discarded()) {
        j["total"] = total;
        j["page"] = page;
        j["page_size"] = page_size;
        ctx->send(j.dump());
    } else {
        ctx->send(out);
    }
}

void AdminServer::handleRelayDictAdd(const HttpContextPtr& ctx) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::string host = req.value("host", std::string());
    int port = req.value("port", 18443);
    std::string name = req.value("name", std::string());
    std::string desc = req.value("description", std::string());
    if (name.size() > 64 || desc.size() > 256) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"name/description too long\"}");
        return;
    }
    long long id = 0;
    std::string err;
    if (!db_.relayDictAdd(host, port, name, desc, id, err)) {
        ctx->response->status_code = err == "该地址已存在" ? HTTP_STATUS_CONFLICT : HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    audit(currentActor().username, "relay.dict.add", host + ":" + std::to_string(port));
    json j;
    j["id"] = id;
    ctx->send(j.dump());
}

void AdminServer::handleRelayDictBatchAdd(const HttpContextPtr& ctx) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    struct Item { std::string host, name, desc; int port = 18443; };
    std::vector<Item> items;
    if (req.contains("text") && req["text"].is_string()) {
        // 每行：地址 [名称] [备注...]，地址可写 host、host:port 或 [v6]:port
        std::string text = req["text"].get<std::string>();
        size_t pos = 0;
        while (pos <= text.size()) {
            size_t nl = text.find('\n', pos);
            std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
            for (auto& c : line) {
                if (c == '\t') c = ' ';
            }
            size_t b = line.find_first_not_of(" \r");
            if (b == std::string::npos) continue;
            line = line.substr(b, line.find_last_not_of(" \r") - b + 1);
            if (line.empty() || line[0] == '#') continue;
            auto nextTok = [](const std::string& s, size_t& p) {
                p = s.find_first_not_of(" ", p);
                if (p == std::string::npos) return std::string();
                size_t e = s.find(' ', p);
                std::string t = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
                p = (e == std::string::npos) ? std::string::npos : e;
                return t;
            };
            size_t p = 0;
            std::string addr = nextTok(line, p);
            if (addr.empty()) continue;
            Item it;
            std::vector<TunnelEndpoint> eps;
            std::string perr;
            if (!tunnel_parse_endpoints(addr, 18443, eps, perr) || eps.empty()) continue;
            it.host = eps[0].host;
            it.port = eps[0].port;
            it.name = nextTok(line, p);
            if (p != std::string::npos) {
                p = line.find_first_not_of(" ", p);
                if (p != std::string::npos) it.desc = line.substr(p);
            }
            items.push_back(it);
        }
    }
    if (items.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"no valid endpoint\"}");
        return;
    }
    if (items.size() > 200) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"too many items (max 200)\"}");
        return;
    }
    int added = 0, dup = 0, bad = 0;
    for (auto& it : items) {
        long long id = 0;
        std::string err;
        if (db_.relayDictAdd(it.host, it.port, it.name, it.desc, id, err)) added++;
        else if (err == "该地址已存在") dup++;
        else bad++;
    }
    audit(currentActor().username, "relay.dict.batch",
          "added=" + std::to_string(added) + " dup=" + std::to_string(dup) +
          " bad=" + std::to_string(bad));
    json j;
    j["added"] = added;
    j["duplicated"] = dup;
    j["invalid"] = bad;
    ctx->send(j.dump());
}

void AdminServer::handleRelayDictUpdate(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    auto req = json::parse(ctx->body(), nullptr, false);
    if (id <= 0 || req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad request\"}");
        return;
    }
    std::string host = req.value("host", std::string());
    int port = req.value("port", 18443);
    std::string name = req.value("name", std::string());
    std::string desc = req.value("description", std::string());
    int enabled = req.value("enabled", true) ? 1 : 0;
    std::string err;
    if (!db_.relayDictUpdate(id, host, port, name, desc, enabled, err)) {
        ctx->response->status_code = err == "该地址已存在" ? HTTP_STATUS_CONFLICT : HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    // 改的是下发列表里的地址：运行时列表要一起更新，否则界面显示新地址、设备还在用旧的
    std::string items;
    if (db_.relayListJson(items)) {
        auto arr = json::parse(items, nullptr, false);
        bool in_list = false;
        if (!arr.is_discarded() && arr.is_array()) {
            for (auto& it : arr) {
                if (it.value("endpoint_id", 0LL) == id) { in_list = true; break; }
            }
        }
        if (in_list) {
            std::string spec;
            if (!relayListSpec(spec)) {
                ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
                ctx->send("{\"error\":\"下发列表包含无效地址\"}");
                return;
            }
            bool leave = settingInt("relay.allow_leave", 0) != 0;
            std::string e = callRelay([this, spec, leave]() {
                std::string x;
                relay_->setRelayList(spec, leave, x);
                return x;
            });
            if (!e.empty()) {
                ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
                ctx->send("{\"error\":\"" + e + "\"}");
                return;
            }
        }
    }
    audit(currentActor().username, "relay.dict.update", std::to_string(id) + " " + host + ":" + std::to_string(port));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleRelayDictDelete(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    if (id <= 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad id\"}");
        return;
    }
    std::string err;
    if (!db_.relayDictDelete(id, err)) {
        ctx->response->status_code = err.find("下发列表") != std::string::npos ? HTTP_STATUS_CONFLICT
                                                                             : HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    audit(currentActor().username, "relay.dict.delete", std::to_string(id));
    ctx->send("{\"ok\":true}");
}

// ---------------------------------------------------------------- 下发列表

void AdminServer::handleRelayListGet(const HttpContextPtr& ctx) {
    std::string items;
    if (!db_.relayListJson(items)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    std::string spec;
    relayListSpec(spec);
    std::string status = callRelay([this]() { return relay_->relayListStatus(); });
    auto st = json::parse(status, nullptr, false);
    json out;
    out["items"] = json::parse(items, nullptr, false);
    out["servers"] = spec;
    out["allow_leave"] = settingInt("relay.allow_leave", 0) != 0;
    if (!st.is_discarded() && st.is_object()) {
        out["rev"] = (long long)st.value<unsigned long long>("rev", 0ULL);
        out["configured"] = st.value("configured", false);
        // 每台在线设备最近一次 ACK，前端用它算“已同步/旧版本/未响应”
        json ags = st.contains("agents") ? st["agents"] : json::array();
        if (scopeRestricted() && ags.is_array()) {
            std::vector<std::string> mine = myDevids();
            json kept = json::array();
            for (auto& a : ags) {
                std::string d = a.value("devid", std::string());
                if (std::find(mine.begin(), mine.end(), d) != mine.end()) kept.push_back(a);
            }
            ags = kept;
        }
        out["agents"] = ags;
    } else {
        out["rev"] = 0;
        out["configured"] = false;
        out["agents"] = json::array();
    }
    ctx->send(out.dump());
}

void AdminServer::handleRelayListSet(const HttpContextPtr& ctx) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("endpoint_ids") ||
        !req["endpoint_ids"].is_array()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"endpoint_ids required\"}");
        return;
    }
    std::vector<long long> ids;
    for (auto& v : req["endpoint_ids"]) {
        if (!v.is_number_integer()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad endpoint_ids\"}");
            return;
        }
        ids.push_back(v.get<long long>());
    }
    bool leave = req.value("allow_leave", settingInt("relay.allow_leave", 0) != 0);
    std::string err;
    if (!db_.relayListSet(ids, currentActor().username, err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    db_.setSetting("relay.allow_leave", leave ? "1" : "0", currentActor().username);
    {
        std::lock_guard<std::mutex> lk(settings_mu_);
        settings_["relay.allow_leave"] = leave ? "1" : "0";
    }
    std::string spec;
    if (!relayListSpec(spec)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"下发列表包含无效地址\"}");
        return;
    }
    std::string er = callRelay([this, spec, leave]() {
        std::string e;
        if (!relay_->setRelayList(spec, leave, e)) return std::string("ERR:") + e;
        return std::to_string((unsigned long long)relay_->relay_list_rev);
    });
    if (er.rfind("ERR:", 0) == 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + er.substr(4) + "\"}");
        return;
    }
    long long new_rev = atoll(er.c_str());
    // 任务快照里的列表已经过期：先暂停，避免旧快照把设备拉回旧地址。
    // 恢复时会再校验一次 rev，过期任务只能取消后重建。
    int paused = 0;
    if (db_.relayTaskPauseStale(new_rev, paused) && paused > 0) {
        logf_("[admin] relay list changed (rev=%lld), paused %d stale sync task(s)", new_rev, paused);
    }
    audit(currentActor().username, "relay.list.set",
          spec + (leave ? " allow_leave=1" : " allow_leave=0"));
    handleRelayListGet(ctx);
}

void AdminServer::handleRelayListPush(const HttpContextPtr& ctx) {
    auto req = json::parse(ctx->body(), nullptr, false);
    json body = (req.is_discarded() || !req.is_object()) ? json::object() : req;
    if (!body.contains("devids")) {
        std::string online = callRelay([this]() { return relay_->onlineAgentDevids(); });
        auto arr = json::parse(online, nullptr, false);
        body["devids"] = arr.is_discarded() ? json::array() : arr;
    }
    if (!body.contains("devids") || !body["devids"].is_array() || body["devids"].empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"没有在线设备\"}");
        return;
    }
    body["name"] = body.value("name", std::string("中继列表全量下发"));
    createRelaySyncTask(ctx, body);
}

// ---------------------------------------------------------------- 同步任务

void AdminServer::createRelaySyncTask(const HttpContextPtr& ctx, const json& req) {
    std::vector<std::string> devids;
    if (!req.contains("devids") || !req["devids"].is_array()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"devids required\"}");
        return;
    }
    // 去重但保留顺序：顺序决定分批，重复会虚增目标数和批次统计
    std::set<std::string> seen;
    for (auto& v : req["devids"]) {
        if (!v.is_string()) continue;
        std::string d = v.get<std::string>();
        if (d.empty() || !seen.insert(d).second) continue;
        devids.push_back(d);
    }
    if (devids.empty() || devids.size() > 200) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"devids required (1-200)\"}");
        return;
    }
    for (auto& d : devids) {
        if (!ownsDevid(d)) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"forbidden device: " + d + "\"}");
            return;
        }
    }

    std::string name = req.value("name", std::string());
    if (name.empty()) name = "中继列表同步";
    if (name.size() > 64) name = name.substr(0, 64);
    std::string mode = req.value("mode", std::string("once"));
    if (mode != "once" && mode != "wave") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"mode must be once|wave\"}");
        return;
    }
    int wave_size = req.value("wave_size", (int)devids.size());
    int wave_interval_s = req.value("wave_interval_s", 600);
    int fail_pct = req.value("fail_threshold_pct", 20);
    int max_retries = req.value("max_retries", 1);
    if (wave_size < 1 || wave_size > 200 || wave_interval_s < 0 || wave_interval_s > 7 * 86400 ||
        fail_pct < 0 || fail_pct > 100 || max_retries < 0 || max_retries > 10) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad task params\"}");
        return;
    }
    long long now = relayNowMs();
    long long start = req.value("start_at_ms", now);
    long long deadline = req.value("deadline_at_ms", start + 24LL * 3600 * 1000);
    if (start < now - 60000) start = now;
    if (deadline <= now || deadline - now > 30LL * 86400 * 1000) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad time window (max 30 days)\"}");
        return;
    }
    if (deadline < start + 60000) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"deadline must be later than start\"}");
        return;
    }

    // 快照 = 已保存的有序列表；顺带把运行时列表对齐到它并取 rev
    std::string spec;
    if (!relayListSpec(spec) || spec.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"下发列表为空，请先保存中继地址\"}");
        return;
    }
    bool leave = settingInt("relay.allow_leave", 0) != 0;
    std::string snap = callRelay([this, spec, leave]() {
        std::string e;
        if (!relay_->setRelayList(spec, leave, e)) {
            return std::string("ERR:") + e;
        }
        if (relay_->relay_list_json.empty()) return std::string("ERR:empty");
        json j;
        j["payload"] = relay_->relay_list_json;
        j["rev"] = relay_->relay_list_rev;
        return j.dump();
    });
    if (snap.rfind("ERR:", 0) == 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + snap.substr(4) + "\"}");
        return;
    }
    auto sj = json::parse(snap, nullptr, false);
    if (sj.is_discarded() || !sj.is_object()) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"relay unavailable\"}");
        return;
    }
    std::string payload = sj.value("payload", std::string());
    long long rev = sj.value("rev", 0LL);

    std::string batch = "r" + std::to_string(now) + "-" + AdminDb::randomHex(3);
    std::string err;
    if (!db_.relayTaskCreate(batch, name, payload, rev, leave ? 1 : 0, mode, wave_size,
                             wave_interval_s, fail_pct, devids, start, deadline, max_retries,
                             currentActor().username, err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    // 立即开始的任务不用等下一轮调度：直接进入 running/第 1 批，
    // 否则界面会先显示一秒钟的“待开始 0/N”
    if (start <= now) {
        db_.relayTaskSetStatus(batch, "running", 0);
        db_.relayTaskSetWave(batch, 1, 0);
    }
    // 这次调用可能把运行时列表对齐到了新 rev，之前的任务快照随之过期
    int paused = 0;
    if (db_.relayTaskPauseStale(rev, paused) && paused > 0) {
        logf_("[admin] relay list changed to rev=%lld, paused %d stale sync task(s)", rev, paused);
    }
    audit(currentActor().username, "relay.sync.create",
          batch + " " + name + " targets=" + std::to_string(devids.size()) + " rev=" + std::to_string(rev));
    json j;
    j["batch"] = batch;
    j["rev"] = rev;
    j["total"] = (int)devids.size();
    j["total_waves"] = (mode == "wave") ? (int)((devids.size() + wave_size - 1) / wave_size) : 1;
    if (!spec.empty()) j["servers"] = spec;
    ctx->send(j.dump());
}

void AdminServer::handleRelaySyncCreate(const HttpContextPtr& ctx) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    createRelaySyncTask(ctx, req);
}

void AdminServer::handleRelaySyncList(const HttpContextPtr& ctx) {
    std::string status = ctx->param("status");
    std::string q = ctx->param("q");
    int page = atoi(ctx->param("page").c_str());
    int page_size = atoi(ctx->param("page_size").c_str());
    if (page <= 0) page = 1;
    if (page_size <= 0) page_size = 20;
    std::vector<std::string> devids;
    bool scoped = scopeRestricted();
    if (scoped) devids = myDevids();
    std::string out;
    int total = 0;
    if (!db_.relayTaskList(status, q, page, page_size, devids, scoped, out, total)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    auto j = json::parse(out, nullptr, false);
    if (!j.is_discarded()) {
        j["total"] = total;
        j["page"] = page;
        j["page_size"] = page_size;
        ctx->send(j.dump());
    } else {
        ctx->send(out);
    }
}

void AdminServer::handleRelaySyncGet(const HttpContextPtr& ctx) {
    std::string batch = ctx->param("batch");
    std::string out, err;
    if (!db_.relayTaskGet(batch, out, err)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    if (scopeRestricted()) {
        std::string its;
        db_.relayTaskItems(batch, its);
        auto arr = json::parse(its, nullptr, false);
        bool mine = false;
        if (!arr.is_discarded() && arr.is_array()) {
            for (auto& it : arr) {
                if (ownsDevid(it.value("devid", std::string()))) { mine = true; break; }
            }
        }
        if (!mine) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"forbidden\"}");
            return;
        }
    }
    auto j = json::parse(out, nullptr, false);
    if (j.is_discarded()) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    std::string items;
    db_.relayTaskItems(batch, items);
    auto arr = json::parse(items, nullptr, false);
    std::string acks = callRelay([this]() { return relay_->relayListAcksJson(); });
    auto aj = json::parse(acks, nullptr, false);
    if (!arr.is_discarded() && !aj.is_discarded()) {
        for (auto& it : arr) {
            std::string devid = it.value("devid", std::string());
            if (!aj.contains(devid)) continue;
            auto& a = aj[devid];
            it["ack_ok"] = a.value("ok", false);
            it["ack_rev"] = (long long)a.value<unsigned long long>("rev", 0ULL);
            it["persisted"] = a.value("persisted", false);
            it["switching"] = a.value("switching", false);
            if (a.contains("error")) it["ack_error"] = a["error"];
        }
    }
    j["items"] = arr.is_discarded() ? json::array() : arr;
    if (scopeRestricted()) {
        std::vector<std::string> mine = myDevids();
        json kept = json::array();
        for (auto& it : j["items"]) {
            std::string d = it.value("devid", std::string());
            if (std::find(mine.begin(), mine.end(), d) != mine.end()) kept.push_back(it);
        }
        j["items"] = kept;
        j["scoped"] = true;
    }
    ctx->send(j.dump());
}

void AdminServer::handleRelaySyncAction(const HttpContextPtr& ctx, const std::string& action) {
    std::string batch = ctx->param("batch");
    std::string out, err;
    if (!db_.relayTaskGet(batch, out, err)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    auto t = json::parse(out, nullptr, false);
    if (t.is_discarded()) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    // 受限账号只能操作包含自己设备范围的任务
    if (scopeRestricted()) {
        std::string its;
        db_.relayTaskItems(batch, its);
        auto arr = json::parse(its, nullptr, false);
        bool mine = false;
        if (!arr.is_discarded() && arr.is_array()) {
            for (auto& it : arr) {
                if (ownsDevid(it.value("devid", std::string()))) { mine = true; break; }
            }
        }
        if (!mine) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"forbidden\"}");
            return;
        }
    }
    std::string st = t.value("status", std::string());
    long long now = relayNowMs();
    int wave = t.value("current_wave", 0);
    if (action == "pause") {
        if (st != "running" && st != "scheduled") {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"只能暂停进行中的任务\"}");
            return;
        }
        db_.relayTaskSetStatus(batch, "paused", 0);
    } else if (action == "resume") {
        if (st != "paused") {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"任务未暂停\"}");
            return;
        }
        long long start = t.value("start_at_ms", 0LL);
        long long wave_done = t.value("wave_done_ms", 0LL);
        int total_waves = t.value("total_waves", 1);
        // 列表已变更时不能恢复：任务快照会把设备拉回旧地址，只能取消后重建
        long long task_rev = t.value("rev", 0LL);
        std::string cur = callRelay([this]() { return relay_->relayListStatus(); });
        auto cj = json::parse(cur, nullptr, false);
        long long cur_rev = (!cj.is_discarded() && cj.is_object())
                                ? (long long)cj.value<unsigned long long>("rev", 0ULL)
                                : 0;
        if (task_rev != cur_rev) {
            ctx->response->status_code = HTTP_STATUS_CONFLICT;
            ctx->send("{\"error\":\"下发列表已变更（任务 rev=" + std::to_string(task_rev) +
                      "，当前 rev=" + std::to_string(cur_rev) + "），请取消后重新创建任务\"}");
            return;
        }
        // 当前批已经跑完（paused 在波次之间）→ 立即释放下一批；
        // 当前批没跑完 → 只恢复状态，继续把这一批发完，不能跳批。
        if (wave_done != 0 && wave < total_waves) {
            std::string e;
            if (!db_.relayTaskReleaseWave(batch, wave + 1, now, e)) {
                ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
                ctx->send("{\"error\":\"" + e + "\"}");
                return;
            }
            db_.relayTaskSetWave(batch, wave + 1, 0);
        }
        db_.relayTaskSetStatus(batch, start > now ? "scheduled" : "running", 0);
    } else if (action == "cancel") {
        int n = 0;
        db_.relayTaskCancelPending(batch, n);
        db_.relayTaskSetStatus(batch, "cancelled", now);
        audit(currentActor().username, "relay.sync.cancel", batch + " cancelled=" + std::to_string(n));
        json j;
        j["ok"] = true;
        j["cancelled"] = n;
        ctx->send(j.dump());
        return;
    } else if (action == "retry") {
        int n = 0, rw = 0;
        db_.relayTaskRetryFailed(batch, n, rw);
        if (n > 0) {
            // 回到最早被重试的那一批，清掉完成时刻让闸门重新评估
            db_.relayTaskSetWave(batch, rw > 0 ? rw : wave, 0);
            db_.relayTaskSetStatus(batch, "running", 0);
        }
        audit(currentActor().username, "relay.sync.retry", batch + " retried=" + std::to_string(n));
        json j;
        j["ok"] = true;
        j["retried"] = n;
        ctx->send(j.dump());
        return;
    } else {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"unknown action\"}");
        return;
    }
    audit(currentActor().username, "relay.sync." + action, batch);
    ctx->send("{\"ok\":true}");
}
