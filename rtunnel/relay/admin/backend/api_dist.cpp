// 分发任务：创建、查询、重试、取消、延长 + 后台调度
#include "admin_server.hpp"
#include "hash_util.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <chrono>


using nlohmann::json;

static long long nowMs() { return AdminDb::nowMs(); }

void AdminServer::handleDistCreate(const HttpContextPtr& ctx, const AdminActor& actor) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::string file = req.value("file", "");
    std::string sub = req.value("dir", "");
    std::vector<std::string> devids;
    if (req.contains("devids") && req["devids"].is_array()) {
        for (auto& v : req["devids"]) if (v.is_string()) devids.push_back(v.get<std::string>());
    }
    long long now = nowMs();
    long long start = req.value("start_at_ms", now);
    long long deadline = req.value("deadline_at_ms", start + 24LL * 3600 * 1000);
    int max_retries = req.value("max_retries", 3);
    if (file.empty() || file.find("..") != std::string::npos || file.find('/') != std::string::npos ||
        sub.size() > 128 || sub.find("..") != std::string::npos || (!sub.empty() && sub[0] == '/')) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file or dir\"}");
        return;
    }
    if (devids.empty() || devids.size() > 100) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"devids required (1-100)\"}");
        return;
    }
    if (deadline <= start || deadline - start > 30LL * 86400 * 1000) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad time window (max 30 days)\"}");
        return;
    }
    if (max_retries < 0 || max_retries > 10) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"max_retries 0-10\"}");
        return;
    }
    std::string path = cfg_.file_dir + "/" + file;
    struct stat st{};
    if (cfg_.file_dir.empty() || stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"file not found\"}");
        return;
    }
    std::string sha = adminSha256File(path);
    std::string batch = "b" + std::to_string(now) + "-" + AdminDb::randomHex(3);
    std::vector<long long> ids;
    std::string bad;
    for (auto& devid : devids) {
        bool online = callRelay([this, &devid]() {
            return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
        }) == "1";
        if (!online) { bad += (bad.empty() ? "" : ",") + devid; continue; }
        std::string base = callRelay([this, &devid]() { return relay_->fileDirOf(devid.c_str()); });
        std::string target = base.empty() ? ("/" + sub) : (base + (sub.empty() ? "" : "/" + sub));
        bool dup = db_.distAlreadyPushed(devid, sha);
        std::string status = dup ? "conflict" : ((start <= now) ? "queued" : "scheduled");
        long long id = 0;
        if (!db_.distInsert(batch, devid, file, (long long)st.st_size, sha, sub, target,
                            status, start, deadline, max_retries, actor.username, &id)) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"db error\"}");
            return;
        }
        ids.push_back(id);
    }
    if (ids.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"all devices offline: " + bad + "\"}");
        return;
    }
    audit(actor.username, "distribution.create", batch + " file=" + file + " n=" + std::to_string(ids.size()));
    json out;
    out["batch_id"] = batch;
    out["tasks"] = ids;
    out["size"] = (long long)st.st_size;
    if (!bad.empty()) out["skipped_offline"] = bad;
    int conflicts = 0;
    for (auto id : ids) {
        std::string tj;
        if (db_.distGet(id, tj)) {
            auto tjx = json::parse(tj, nullptr, false);
            if (!tjx.is_discarded() && tjx.value("status", "") == "conflict") conflicts++;
        }
    }
    out["conflicts"] = conflicts;
    ctx->response->status_code = HTTP_STATUS_ACCEPTED;
    ctx->send(out.dump());
}

void AdminServer::handleDistList(const HttpContextPtr& ctx) {
    std::string status = ctx->param("status");
    std::string q = ctx->param("q");
    std::string batch = ctx->param("batch");
    int page = atoi(ctx->param("page").c_str());
    int per = atoi(ctx->param("per_page").c_str());
    if (page < 1) page = 1;
    if (per < 1 || per > 100) per = 20;
    std::string arr;
    int total = 0;
    db_.distList(status, q, batch, page, per, arr, total);
    ctx->send("{\"total\":" + std::to_string(total) + ",\"page\":" + std::to_string(page) +
              ",\"per_page\":" + std::to_string(per) + ",\"items\":" + arr + "}");
}

void AdminServer::handleDistSummary(const HttpContextPtr& ctx) {
    std::string j;
    if (!db_.distSummary(j)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send(j);
}

void AdminServer::handleDistGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string j;
    if (!db_.distGet(id, j)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    ctx->send(j);
}

void AdminServer::handleDistRetry(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string js;
    if (!db_.distGet(id, js)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto j = json::parse(js);
    long long now = nowMs();
    if (j.value("status", "") != "failed") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"only failed tasks can retry\"}");
        return;
    }
    if (now >= j.value("deadline_at_ms", 0LL)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"deadline passed, extend not available for failed\"}");
        return;
    }
    int retry = j.value("retry_count", 0) + 1;
    db_.distUpdate(id, "queued", "", j.value("sent", 0LL), 0, 0, 0, retry);
    audit(actor.username, "distribution.retry", std::to_string(id));
    ctx->send("{\"ok\":true,\"retry_count\":" + std::to_string(retry) + "}");
}

void AdminServer::handleDistCancel(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string js;
    if (!db_.distGet(id, js)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto j = json::parse(js);
    std::string st = j.value("status", "");
    if (st != "scheduled" && st != "queued") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"only scheduled/queued can cancel\"}");
        return;
    }
    db_.distUpdate(id, "canceled", "canceled", j.value("sent", 0LL), 0, nowMs(), 0,
                   j.value("retry_count", 0));
    audit(actor.username, "distribution.cancel", std::to_string(id));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDistVerify(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string js;
    if (!db_.distGet(id, js)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto j = json::parse(js);
    if (j.value("status", "") != "conflict") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"only conflict tasks can verify/force\"}");
        return;
    }
    std::string devid = j.value("devid", std::string());
    std::string dir = j.value("sub_dir", std::string());
    std::string name = j.value("file_name", std::string());
    std::string want = j.value("file_sha", std::string());
    uint32_t cid = 0;
    std::string err;
    std::string started = callRelay([this, &devid, &dir, &name, &err, &cid]() {
        return relay_->startFileCheck(devid.c_str(), dir.c_str(), name.c_str(), cid, err) ? "1" : "0";
    });
    if (started != "1") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    std::string acc;
    for (int i = 0; i < 30 && acc.empty(); ++i) {
        usleep(100000);
        acc = callRelay([this, cid]() { return relay_->takeCheckResult(cid); });
    }
    if (acc.empty()) {
        ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
        ctx->send("{\"error\":\"device check timeout\"}");
        return;
    }
    auto a = json::parse(acc, nullptr, false);
    bool exists = !a.is_discarded() && a.value("exists", false);
    std::string got = a.is_discarded() ? std::string() : a.value("sha256", std::string());
    long long now = nowMs();
    if (exists && !want.empty() && got == want) {
        db_.distUpdate(id, "success", "", j.value("file_size", 0LL), 0, now, 0,
                       j.value("retry_count", 0));
        audit(actor.username, "distribution.verify_same", std::to_string(id));
        ctx->send("{\"ok\":true,\"same\":true,\"status\":\"success\"}");
        return;
    }
    db_.distUpdate(id, "queued", "device file differs; queued", 0, 0, 0, 0,
                   j.value("retry_count", 0));
    audit(actor.username, "distribution.verify_diff", std::to_string(id) + " exists=" +
          (exists ? "1" : "0"));
    ctx->send("{\"ok\":true,\"same\":false,\"exists\":" + std::string(exists ? "true" : "false") +
              ",\"status\":\"queued\"}");
}

void AdminServer::handleDistExtend(const HttpContextPtr& ctx, const AdminActor& actor) {
    long long id = atoll(ctx->param("id").c_str());
    std::string js;
    if (!db_.distGet(id, js)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"not found\"}");
        return;
    }
    auto j = json::parse(js);
    auto req = json::parse(ctx->body(), nullptr, false);
    long long nd = req.is_discarded() ? 0 : req.value("deadline_at_ms", 0LL);
    long long now = nowMs();
    long long old = j.value("deadline_at_ms", 0LL);
    long long start = j.value("start_at_ms", 0LL);
    if (j.value("status", "") != "timeout" || j.value("extend_count", 0) != 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"only timeout tasks can extend once\"}");
        return;
    }
    if (nd <= now || nd > old + 24LL * 3600 * 1000 || nd > start + 30LL * 86400 * 1000) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad new deadline\"}");
        return;
    }
    db_.distExtend(id, nd);
    audit(actor.username, "distribution.extend", std::to_string(id) + " -> " + std::to_string(nd));
    ctx->send("{\"ok\":true,\"deadline_at_ms\":" + std::to_string(nd) + "}");
}
