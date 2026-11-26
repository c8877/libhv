// 操作作业 API：plan →（审批）→ apply → verify。
//
// 为什么要有 plan：AI/脚本先拿到一份“将要做什么”的确定性摘要与 plan_hash，
// apply 必须回传同一个 hash。设备状态在这期间变了（例如 argv 被替换、
// 目标设备换了归属）就拒绝，避免 TOCTOU。
//
// 幂等：apply 可带 Idempotency-Key。同 actor 同 key 的作业已执行过就直接返回
// 原结果，重试不会把 systemctl restart 跑两遍。
//
// 审批：risk >= ops.require_approval_risk（默认 2）的作业落库即 pending_approval，
// 必须由**另一个** actor 批准才能 apply。审批闸门只拦“执行”，不拦“提议”。
//
// 配额：ops.max_per_minute（每 actor）、ops.max_per_hour_device（每设备）默认 0 不限；
// ops.window 形如 "08:00-20:00"，为空表示全天可执行（权限仍按组求值）。
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/json.hpp"

#include <algorithm>
#include <thread>
#include <unistd.h>

using nlohmann::json;

namespace {

// 风险分级决定要不要审批，阈值见 ops.require_approval_risk。
//   1 = 只读（查看类命令）
//   2 = 修改运行状态（重启服务、发信号、动文件）
//   3 = 破坏性/难以恢复（格式化、关机、改系统文件）
int argvRisk(const std::vector<std::string>& argv) {
    if (argv.empty()) return 2;
    std::string cmd = argv[0];
    size_t slash = cmd.find_last_of('/');
    if (slash != std::string::npos) cmd = cmd.substr(slash + 1);

    static const char* kReadOnly[] = {"cat", "ls", "df", "du", "free", "uptime", "uname", "ps", "top",
                                      "head", "tail", "grep", "find", "date", "id", "ip", "ifconfig",
                                      "netstat", "ss", "lsof", "journalctl", "dmesg", "mount", "stat",
                                      "wc", "sort", "uniq", "echo", "printf", "true", "which", "env"};
    static const char* kDestructive[] = {"mkfs", "dd", "shutdown", "poweroff", "reboot", "fdisk",
                                         "parted", "wipefs", "mkswap"};
    static const char* kMutating[] = {"rm", "mv", "cp", "chmod", "chown", "kill", "killall", "pkill",
                                      "systemctl", "service", "touch", "truncate", "tee", "sed", "tar"};

    for (const char* d : kDestructive)
        if (cmd == d) return 3;
    // systemctl 的只读子命令单独放行
    if (cmd == "systemctl" && argv.size() >= 2) {
        const std::string& sub = argv[1];
        if (sub == "status" || sub == "show" || sub == "list-units" || sub == "is-active" ||
            sub == "is-enabled" || sub == "cat")
            return 1;
        return 2;
    }
    for (const char* r : kReadOnly)
        if (cmd == r) return 1;
    for (const char* m : kMutating)
        if (cmd == m) return 2;
    // sh/bash -c 这种拿不准的一律按可写算
    if (cmd == "sh" || cmd == "bash" || cmd == "su" || cmd == "sudo") return 2;
    return 2;
}

// "08:00-20:00" 形式的执行窗口；跨零点（"22:00-06:00"）也正确处理。
// 空字符串或格式不合法一律视为不限，避免配置写错把系统锁死。
bool inExecWindow(const std::string& win, int now_min) {
    if (win.size() != 11 || win[2] != ':' || win[5] != '-' || win[8] != ':') return true;
    auto toMin = [](const std::string& s, size_t off) {
        int h = atoi(s.substr(off, 2).c_str());
        int m = atoi(s.substr(off + 3, 2).c_str());
        if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
        return h * 60 + m;
    };
    int a = toMin(win, 0), b = toMin(win, 6);
    if (a < 0 || b < 0) return true;
    if (a <= b) return now_min >= a && now_min <= b;
    return now_min >= a || now_min <= b;
}

std::string localHHMM() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    return buf;
}

int localMinutes() {
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    return tmv.tm_hour * 60 + tmv.tm_min;
}

std::string planHash(const json& plan) {
    // 规范化：对象按键排序输出，保证同样内容得到同样 hash
    return AdminDb::sha256Hex(plan.dump());
}

} // namespace

int AdminServer::opsArgvRisk(const std::vector<std::string>& argv) {
    return argvRisk(argv);
}

bool AdminServer::opsCreateExecPlan(const std::string& actor, const std::string& actor_type,
                                   const std::string& devid, const std::vector<std::string>& argv,
                                   const std::string& cwd, int timeout_ms, long long max_bytes,
                                   const std::string& idem, long long& out_id, std::string& out_json,
                                   std::string& err) {
    out_id = 0;
    if (devid.empty()) {
        err = "缺少 devid";
        return false;
    }
    if (argv.empty() || argv.size() > 64) {
        err = "argv 不能为空且最多 64 段";
        return false;
    }
    if (timeout_ms < 1000) timeout_ms = 1000;
    if (timeout_ms > 30000) timeout_ms = 30000;
    if (max_bytes < 1024) max_bytes = 1024;
    if (max_bytes > 1048576) max_bytes = 1048576;

    json plan;
    plan["kind"] = "exec";
    plan["devid"] = devid;
    plan["argv"] = argv;
    plan["timeout_ms"] = timeout_ms;
    plan["max_bytes"] = max_bytes;
    if (!cwd.empty()) plan["cwd"] = cwd;

    int risk = argvRisk(argv);
    int threshold = settingInt("ops.require_approval_risk", 2);
    if (threshold < 1) threshold = 1;   // 0 也当 1：只读命令不需要审批
    bool need_approval = risk >= threshold;

    std::string who = actor.empty() ? std::string("anonymous") : actor;
    std::string type = actor_type.empty() ? (who.rfind("ai:", 0) == 0 ? "ai" : "user") : actor_type;
    if (!db_.opsCreate(who, type, "exec", "device", devid, plan.dump(), planHash(plan), risk,
                       need_approval, idem, out_id, err)) {
        return false;
    }
    if (!db_.opsGet(out_id, out_json)) {
        err = "db error";
        return false;
    }
    return true;
}

void AdminServer::handleOpsPlan(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        apiBad(ctx, "bad json");
        return;
    }
    std::string kind = req.value("kind", std::string("exec"));
    if (kind != "exec") {
        apiBad(ctx, "目前只支持 kind=exec");
        return;
    }
    std::string devid = req.value("devid", std::string());
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    std::vector<std::string> argv;
    if (req.contains("argv") && req["argv"].is_array()) {
        for (auto& v : req["argv"]) {
            if (!v.is_string()) {
                apiBad(ctx, "argv 必须是字符串数组");
                return;
            }
            argv.push_back(v.get<std::string>());
        }
    }
    std::string idem = ctx->header("Idempotency-Key");
    if (idem.size() > 128) idem = idem.substr(0, 128);
    std::string cwd = req.contains("cwd") && req["cwd"].is_string()
                          ? req["cwd"].get<std::string>()
                          : std::string();

    long long id = 0;
    std::string out, err;
    if (!opsCreateExecPlan(actorLabel(), std::string(), devid, argv, cwd,
                           req.value("timeout_ms", 30000), req.value("max_bytes", (long long)65536),
                           idem, id, out, err)) {
        apiBad(ctx, err.empty() ? "db error" : err);
        return;
    }
    ctx->send(out);
}

void AdminServer::handleOpsApply(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;
    long long id = atoll(ctx->param("id").c_str());
    std::string cur;
    if (id <= 0 || !db_.opsGet(id, cur)) {
        apiNotFound(ctx, "操作不存在");
        return;
    }
    json op = json::parse(cur, nullptr, false);
    if (op.is_discarded()) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    std::string devid = op.value("target_id", std::string());
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    std::string status = op.value("status", std::string());
    std::string who = actorLabel();
    if (who.empty()) who = "anonymous";

    // 幂等：同 actor + key 已经执行过，直接返回那次的结果
    std::string idem = ctx->header("Idempotency-Key");
    if (idem.size() > 128) idem = idem.substr(0, 128);
    if (!idem.empty()) {
        std::string prev;
        if (db_.opsFindByIdem(who, idem, prev)) {
            json p = json::parse(prev, nullptr, false);
            if (!p.is_discarded() && p.value("id", (long long)0) != id &&
                p.value("status", std::string()) != "planned") {
                ctx->send(prev);
                return;
            }
        }
    }

    if (status == "applied" || status == "verified") {
        ctx->send(cur);   // 已经执行过：重放不会重复执行
        return;
    }
    if (status == "pending_approval") {
        apiError(ctx, HTTP_STATUS_CONFLICT,
                 "作业待审批（risk=" + std::to_string(op.value("risk", 0)) + "），先由其他账号批准");
        return;
    }
    if (status == "rejected") {
        apiError(ctx, HTTP_STATUS_CONFLICT, "作业已被拒绝，需要重新 plan");
        return;
    }
    if (status != "planned" && status != "approved") {
        apiError(ctx, HTTP_STATUS_CONFLICT, "操作状态不允许执行: " + status);
        return;
    }

    json body = json::parse(ctx->body(), nullptr, false);
    std::string want_hash = body.is_object() ? body.value("plan_hash", std::string()) : std::string();
    if (want_hash.empty() || want_hash != op.value("plan_hash", std::string())) {
        apiError(ctx, HTTP_STATUS_CONFLICT, "plan_hash 不匹配，请重新 plan");
        return;
    }

    // 变更窗口：配置了 ops.window 就只在时段内允许执行写操作
    std::string win = settingStr("ops.window", std::string());
    if (!win.empty() && !inExecWindow(win, localMinutes())) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "当前不在允许执行的时段（" + win + "），现在 " + localHHMM());
        return;
    }

    // 配额：只统计真正执行过的（running/applied/verified/failed），plan 不占额度
    long long now_ms = AdminDb::nowMs();
    int per_min = settingInt("ops.max_per_minute", 0);
    if (per_min > 0) {
        long long n = db_.opsCountRecent(who, std::string(), now_ms - 60 * 1000);
        if (n >= per_min) {
            apiError(ctx, HTTP_STATUS_TOO_MANY_REQUESTS,
                     "每分钟最多执行 " + std::to_string(per_min) + " 个作业，稍后再试");
            return;
        }
    }
    int per_hour_dev = settingInt("ops.max_per_hour_device", 0);
    if (per_hour_dev > 0) {
        long long n = db_.opsCountRecent(std::string(), devid, now_ms - 3600LL * 1000);
        if (n >= per_hour_dev) {
            apiError(ctx, HTTP_STATUS_TOO_MANY_REQUESTS,
                     "该设备每小时最多执行 " + std::to_string(per_hour_dev) + " 个作业");
            return;
        }
    }

    json plan = op.value("plan", json::object());
    std::vector<std::string> argv;
    for (auto& v : plan.value("argv", json::array())) argv.push_back(v.get<std::string>());
    int timeout_ms = plan.value("timeout_ms", 30000);

    json payload;
    payload["argv"] = argv;
    payload["timeout_ms"] = timeout_ms;
    payload["max_bytes"] = plan.value("max_bytes", (long long)65536);
    if (plan.contains("cwd")) payload["cwd"] = plan["cwd"];

    // 设备在线与能力检查：失败就不能把作业留在 running
    bool online = callRelay([this, &devid]() {
                      return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
                  }) == "1";
    if (!online) {
        db_.opsSetStatus(id, "failed", "", "device offline");
        apiError(ctx, HTTP_STATUS_NOT_FOUND, "device offline");
        return;
    }
    int cap = -1;
    callRelay([this, &devid, &cap]() {
        cap = relay_->agentExecCap(devid.c_str());
        return std::string("1");
    });
    if (cap != 1) {
        db_.opsSetStatus(id, "failed", "", "agent 未启用命令执行");
        apiError(ctx, HTTP_STATUS_NOT_IMPLEMENTED, "agent 未启用命令执行（需要 --allow-exec）");
        return;
    }

    if (!db_.opsSetStatus(id, "running", "", "")) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }

    std::thread([this, ctx, id, devid, payload, timeout_ms, argv, who]() {
        json resp;
        std::string err;
        bool ok = execOnDevice(devid, payload.dump(), timeout_ms, resp, err);
        std::string result = ok ? resp.dump() : std::string();
        if (!ok) resp["error"] = err;
        std::string status = ok && resp.value("ok", false) ? "applied" : "failed";
        db_.opsSetStatus(id, status, result, ok ? std::string() : err);

        std::string cmdline;
        for (size_t k = 0; k < argv.size(); ++k) {
            if (k) cmdline += ' ';
            cmdline += argv[k];
        }
        if (cmdline.size() > 300) cmdline = cmdline.substr(0, 300) + "…";
        AuditEntry ae;
        ae.actor = who;
        ae.action = "operation.apply";
        ae.target_type = "device";
        ae.target_id = devid;
        ae.result = status;
        ae.detail = "op=" + std::to_string(id) + " `" + cmdline + "` exit=" +
                    std::to_string(resp.value("exit_code", -1));
        ae.op_id = std::to_string(id);
        audit_.writeEntry(ae);

        std::string out;
        if (!db_.opsGet(id, out)) out = resp.dump();
        ctx->send(out);
    }).detach();
    return;   // 响应在线程里发送
}

void AdminServer::handleOpsVerify(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;
    long long id = atoll(ctx->param("id").c_str());
    std::string cur;
    if (id <= 0 || !db_.opsGet(id, cur)) {
        apiNotFound(ctx, "操作不存在");
        return;
    }
    json op = json::parse(cur, nullptr, false);
    if (op.is_discarded()) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    std::string devid = op.value("target_id", std::string());
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    std::string status = op.value("status", std::string());
    if (status != "applied" && status != "failed") {
        apiError(ctx, HTTP_STATUS_CONFLICT, "只有已执行的操作可以复核");
        return;
    }

    json body = json::parse(ctx->body(), nullptr, false);
    json probe;
    if (body.is_object() && body.contains("argv") && body["argv"].is_array()) {
        std::vector<std::string> argv;
        for (auto& v : body["argv"]) {
            if (v.is_string()) argv.push_back(v.get<std::string>());
        }
        if (argv.empty()) {
            apiBad(ctx, "argv 不能为空");
            return;
        }
        probe["argv"] = argv;
        probe["timeout_ms"] = body.value("timeout_ms", 30000);
        probe["max_bytes"] = body.value("max_bytes", (long long)65536);
    }

    std::string who = actorLabel();
    if (who.empty()) who = "anonymous";
    if (probe.is_null()) {
        // 没有探针：以 apply 的结果为准
        json r = op.value("result", json::object());
        bool ok = r.is_object() && r.value("ok", false);
        json out;
        out["id"] = id;
        out["verified"] = ok;
        out["detail"] = ok ? "apply 已成功" : "apply 失败";
        db_.opsSetStatus(id, ok ? "verified" : "failed", r.is_object() ? r.dump() : std::string(),
                         ok ? std::string() : std::string("apply failed"));
        ctx->send(out.dump());
        return;
    }

    std::thread([this, ctx, id, devid, probe, who]() {
        json resp;
        std::string err;
        int timeout_ms = probe.value("timeout_ms", 30000);
        bool ok = execOnDevice(devid, probe.dump(), timeout_ms, resp, err);
        bool passed = ok && resp.value("ok", false) && resp.value("exit_code", -1) == 0;
        json out;
        out["id"] = id;
        out["verified"] = passed;
        out["probe"] = resp;
        if (!err.empty()) out["error"] = err;
        db_.opsSetStatus(id, passed ? "verified" : "failed", resp.is_object() ? resp.dump() : std::string(),
                         err);
        AuditEntry ae;
        ae.actor = who;
        ae.action = "operation.verify";
        ae.target_type = "device";
        ae.target_id = devid;
        ae.result = passed ? "ok" : "error";
        ae.detail = "op=" + std::to_string(id);
        ae.op_id = std::to_string(id);
        audit_.writeEntry(ae);
        ctx->send(out.dump());
    }).detach();
}

void AdminServer::handleOpsList(const HttpContextPtr& ctx) {
    std::string devid, status;
    int limit = 50;
    for (const auto& kv : ctx->request->query_params) {
        if (kv.first == "devid") devid = kv.second;
        else if (kv.first == "status") status = kv.second;
        else if (kv.first == "limit") limit = atoi(kv.second.c_str());
    }
    // 接入令牌与受限账号只看自己范围内的设备
    if (!devid.empty() && !ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    std::string out;
    if (!db_.opsList(devid, status, limit, out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    if (!scopeRestricted()) {
        ctx->send(out);
        return;
    }
    // 受限账号：过滤掉不在授权范围内的设备
    json doc = json::parse(out, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    std::vector<std::string> mine = myDevids();
    json keep = json::array();
    for (auto& o : doc.value("items", json::array())) {
        std::string d = o.value("target_id", std::string());
        if (std::find(mine.begin(), mine.end(), d) != mine.end()) keep.push_back(o);
    }
    json resp;
    resp["items"] = keep;
    ctx->send(resp.dump());
}

// 审批：只有 pending_approval 能批，且不能自己批自己。
// 审批人除此之外还必须能管到目标设备——否则拿一个只读账号就能给任意设备放行。
void AdminServer::handleOpsApprove(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;
    long long id = atoll(ctx->param("id").c_str());
    std::string cur;
    if (id <= 0 || !db_.opsGet(id, cur)) {
        apiNotFound(ctx, "操作不存在");
        return;
    }
    json op = json::parse(cur, nullptr, false);
    if (op.is_discarded()) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    std::string devid = op.value("target_id", std::string());
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    if (op.value("status", std::string()) != "pending_approval") {
        apiError(ctx, HTTP_STATUS_CONFLICT, "只有待审批的作业可以批准或拒绝");
        return;
    }
    std::string who = actorLabel();
    if (who.empty()) who = "anonymous";
    std::string proposer = op.value("actor", std::string());
    if (who == proposer) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能批准自己发起的作业，需要另一个账号");
        return;
    }

    json body = json::parse(ctx->body(), nullptr, false);
    std::string decision = body.is_object() ? body.value("decision", std::string("approve"))
                                            : std::string("approve");
    std::string note = body.is_object() ? body.value("note", std::string()) : std::string();
    if (note.size() > 300) note = note.substr(0, 300);
    if (decision != "approve" && decision != "reject") {
        apiBad(ctx, "decision 只能是 approve 或 reject");
        return;
    }

    std::string err;
    if (!db_.opsSetApproval(id, decision == "approve" ? "approved" : "rejected", who, note, err)) {
        apiError(ctx, HTTP_STATUS_CONFLICT, err.empty() ? "db error" : err);
        return;
    }

    AuditEntry ae;
    ae.actor = who;
    ae.action = decision == "approve" ? "operation.approve" : "operation.reject";
    ae.target_type = "device";
    ae.target_id = devid;
    ae.result = "ok";
    ae.detail = "op=" + std::to_string(id) + " proposer=" + proposer +
                (note.empty() ? std::string() : " note=" + note);
    ae.op_id = std::to_string(id);
    audit_.writeEntry(ae);

    std::string out;
    if (!db_.opsGet(id, out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    ctx->send(out);
}
