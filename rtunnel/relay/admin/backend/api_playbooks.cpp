// 自愈规则（playbook）：告警 → 生成 operation →（可选）自动执行 → verify。
//
// 三条约束必须同时满足才“自动动手”，任何一条不满足就只是生成一个待批作业：
//   1) 规则自身 auto_apply=1
//   2) 命令风险 <= 全局 playbook.auto_apply_max_risk（默认 1，只有只读命令）
//   3) 全局 playbook.enabled=1
// 这样“自愈”不会变成绕过审批的后门：想把 systemctl restart 变成自动，必须有人在
// 系统设置里显式把上限提到 2，这是单独的、只有超管能改的动作。
//
// 防抖：规则级 cooldown_s + max_per_hour，加全局 playbook.max_per_hour。
// 执行串行：单个 worker 顺序跑，避免故障时一屏设备同时重启把 relay 打满。
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/json.hpp"

#include <chrono>
#include <deque>
#include <thread>
#include <unistd.h>

using nlohmann::json;

extern thread_local AdminActor tl_actor;

namespace {

// 简单通配：* 任意串，? 单字符。设备 ID 里不会有 []，所以不做字符集。
bool globMatch(const char* pat, const char* s) {
    const char* star = nullptr;
    const char* ss = s;
    while (*s) {
        if (*pat == '?' || *pat == *s) {
            ++pat;
            ++s;
        } else if (*pat == '*') {
            star = pat++;
            ss = s;
        } else if (star) {
            pat = star + 1;
            s = ++ss;
        } else {
            return false;
        }
    }
    while (*pat == '*') ++pat;
    return *pat == '\0';
}

// 与 api_operations.cpp 里的 argvRisk 同一套口径；这里只在“手动建规则”时用于校验，
// 真正落库的 risk 由服务端算，不接受客户端传入。
int argvRiskOf(const std::vector<std::string>& argv) {
    if (argv.empty()) return 2;
    std::string cmd = argv[0];
    size_t slash = cmd.find_last_of('/');
    if (slash != std::string::npos) cmd = cmd.substr(slash + 1);
    static const char* kRo[] = {"cat", "ls", "df", "du", "free", "uptime", "uname", "ps", "top",
                                "head", "tail", "grep", "find", "date", "id", "ip", "ifconfig",
                                "netstat", "ss", "lsof", "journalctl", "dmesg", "mount", "stat",
                                "wc", "sort", "uniq", "echo", "printf", "true", "which", "env"};
    static const char* kDestroy[] = {"mkfs", "dd", "shutdown", "poweroff", "reboot", "fdisk",
                                     "parted", "wipefs", "mkswap"};
    for (const char* d : kDestroy)
        if (cmd == d) return 3;
    if (cmd == "systemctl" && argv.size() >= 2) {
        const std::string& sub = argv[1];
        if (sub == "status" || sub == "show" || sub == "list-units" || sub == "is-active" ||
            sub == "is-enabled" || sub == "cat")
            return 1;
        return 2;
    }
    for (const char* r : kRo)
        if (cmd == r) return 1;
    return 2;
}

std::vector<std::string> jsonStrArray(const json& arr) {
    std::vector<std::string> out;
    if (arr.is_array()) {
        for (auto& v : arr)
            if (v.is_string()) out.push_back(v.get<std::string>());
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------- 匹配与派发

// 告警线程调用：匹配规则、生成 operation，（必要时）入队执行。
// 这里不阻塞：opsCreate 是一次本地写库，执行全部交给 worker。
void AdminServer::playbookOnAlert(const AlertEvent& ev) {
    if (!settingInt("playbook.enabled", 1)) return;

    json det = json::parse(ev.details, nullptr, false);
    if (det.is_discarded() || !det.is_object()) return;
    if (det.value("source", std::string()) == "playbook") return;   // 防止自触发
    std::string devid = det.value("devid", std::string());
    if (devid.empty()) return;   // 证书/系统级告警没有单台设备，不做自愈

    std::string list;
    if (!db_.playbookList(list)) return;
    json doc = json::parse(list, nullptr, false);
    if (doc.is_discarded() || !doc.contains("items")) return;

    int auto_max_risk = settingInt("playbook.auto_apply_max_risk", 1);
    if (auto_max_risk < 1) auto_max_risk = 1;
    if (auto_max_risk > 3) auto_max_risk = 3;
    int global_hour = settingInt("playbook.max_per_hour", 30);
    long long now = AdminDb::nowMs();
    int threshold = settingInt("ops.require_approval_risk", 2);
    if (threshold < 1) threshold = 1;

    for (auto& pb : doc["items"]) {
        if (!pb.value("enabled", false)) continue;
        std::string ev_pat = pb.value("event", std::string("*"));
        if (ev_pat != "*" && ev_pat != ev.event) continue;
        std::string glob = pb.value("devid_glob", std::string("*"));
        if (!globMatch(glob.c_str(), devid.c_str())) continue;

        long long pb_id = pb.value("id", (long long)0);
        if (pb_id <= 0) continue;
        int cooldown = pb.value("cooldown_s", 300);
        int per_hour = pb.value("max_per_hour", 6);

        // 冷却：内存里记最近一次触发（跨进程重启会丢，但下面还有库里的每小时上限兜底）
        {
            std::lock_guard<std::mutex> lk(playbook_mu_);
            auto it = playbook_last_fire_ms_.find(pb_id);
            if (it != playbook_last_fire_ms_.end() && cooldown > 0 &&
                now - it->second < (long long)cooldown * 1000) {
                continue;
            }
        }
        if (per_hour > 0 && db_.playbookCountRecent(pb_id, now - 3600LL * 1000) >= per_hour) {
            json skip;
            skip["playbook_id"] = pb_id;
            skip["playbook"] = pb.value("name", std::string());
            skip["devid"] = devid;
            skip["event"] = ev.event;
            skip["reason"] = "hourly_limit";
            bus_.publish("playbook.skipped", skip.dump());
            continue;
        }
        if (global_hour > 0) {
            long long total = db_.playbookCountRecentAll(now - 3600LL * 1000);
            if (total >= global_hour) {
                json skip;
                skip["devid"] = devid;
                skip["event"] = ev.event;
                skip["reason"] = "global_limit";
                bus_.publish("playbook.skipped", skip.dump());
                return;
            }
        }

        std::vector<std::string> argv = jsonStrArray(pb.value("argv", json::array()));
        if (argv.empty()) continue;
        std::vector<std::string> ver = jsonStrArray(pb.value("verify_argv", json::array()));
        int risk = pb.value("risk", 2);
        bool auto_apply = pb.value("auto_apply", false) && risk <= auto_max_risk;

        json plan;
        plan["kind"] = "exec";
        plan["devid"] = devid;
        plan["argv"] = argv;
        plan["timeout_ms"] = 30000;
        plan["max_bytes"] = (long long)65536;
        plan["source"] = "playbook";
        plan["playbook_id"] = pb_id;
        plan["playbook"] = pb.value("name", std::string());
        plan["trigger"] = ev.event;
        if (!ver.empty()) plan["verify_argv"] = ver;

        std::string actor = "playbook:" + std::to_string(pb_id) + ":" + pb.value("name", std::string());
        long long op_id = 0;
        std::string err;
        if (!db_.opsCreate(actor, "playbook", "exec", "device", devid, plan.dump(),
                           AdminDb::sha256Hex(plan.dump()), risk, !auto_apply && risk >= threshold, "",
                           op_id, err)) {
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(playbook_mu_);
            playbook_last_fire_ms_[pb_id] = now;
        }

        if (auto_apply) playbookEnqueueOp(op_id);

        json evj;
        evj["playbook_id"] = pb_id;
        evj["playbook"] = pb.value("name", std::string());
        evj["devid"] = devid;
        evj["event"] = ev.event;
        evj["op_id"] = op_id;
        evj["risk"] = risk;
        evj["auto"] = auto_apply;
        evj["status"] = auto_apply ? "queued" : (risk >= threshold ? "pending_approval" : "planned");
        bus_.publish("playbook.fired", evj.dump());

        AuditEntry ae;
        ae.actor = actor;
        ae.action = "playbook.fire";
        ae.target_type = "device";
        ae.target_id = devid;
        ae.result = "ok";
        ae.detail = "trigger=" + ev.event + " op=" + std::to_string(op_id) +
                    (auto_apply ? " auto" : " manual-approval");
        ae.op_id = std::to_string(op_id);
        audit_.writeEntry(ae);
    }
}

bool AdminServer::playbookEnqueueOp(long long op_id) {
    std::lock_guard<std::mutex> lk(playbook_mu_);
    if (playbook_jobs_.size() >= 64) {
        ++playbook_dropped_;
        return false;
    }
    playbook_jobs_.push_back(op_id);
    return true;
}

// 单线程串行执行：同一时刻只有一个自愈动作在跑，避免故障时把设备群同时重启。
void AdminServer::playbookWorker() {
    while (!waitStop(200)) {
        long long op_id = 0;
        {
            std::lock_guard<std::mutex> lk(playbook_mu_);
            if (playbook_jobs_.empty()) continue;
            op_id = playbook_jobs_.front();
            playbook_jobs_.pop_front();
        }
        playbookRunOp(op_id);
    }
}

void AdminServer::playbookRunOp(long long op_id) {
    std::string cur;
    if (!db_.opsGet(op_id, cur)) return;
    json op = json::parse(cur, nullptr, false);
    if (op.is_discarded()) return;
    std::string status = op.value("status", std::string());
    if (status != "planned" && status != "approved") return;   // 已被处理或还在等审批
    std::string devid = op.value("target_id", std::string());
    std::string actor = op.value("actor", std::string("playbook"));

    json plan = op.value("plan", json::object());
    std::vector<std::string> argv = jsonStrArray(plan.value("argv", json::array()));
    std::vector<std::string> verify = jsonStrArray(plan.value("verify_argv", json::array()));

    auto finish = [&](const std::string& st, const json& payload, const std::string& err) {
        db_.opsSetStatus(op_id, st, payload.is_null() ? std::string() : payload.dump(), err);
        json ev;
        ev["op_id"] = op_id;
        ev["playbook"] = plan.value("playbook", std::string());
        ev["playbook_id"] = plan.value("playbook_id", (long long)0);
        ev["devid"] = devid;
        ev["status"] = st;
        if (!err.empty()) ev["error"] = err;
        bus_.publish("playbook.done", ev.dump());

        AuditEntry ae;
        ae.actor = actor;
        ae.action = "playbook.done";
        ae.target_type = "device";
        ae.target_id = devid;
        ae.result = st;
        std::string cmd;
        for (size_t i = 0; i < argv.size(); ++i) {
            if (i) cmd += ' ';
            cmd += argv[i];
        }
        if (cmd.size() > 200) cmd = cmd.substr(0, 200) + "…";
        ae.detail = "op=" + std::to_string(op_id) + " `" + cmd + "`" + (err.empty() ? "" : " " + err);
        ae.op_id = std::to_string(op_id);
        audit_.writeEntry(ae);
    };

    bool online = callRelay([this, &devid]() {
                      return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
                  }) == "1";
    if (!online) {
        finish("failed", json::object(), "device offline");
        return;
    }
    int cap = -1;
    callRelay([this, &devid, &cap]() {
        cap = relay_->agentExecCap(devid.c_str());
        return std::string("1");
    });
    if (cap != 1) {
        finish("failed", json::object(), "agent 未启用命令执行");
        return;
    }

    db_.opsSetStatus(op_id, "running", "", "");
    json payload;
    payload["argv"] = argv;
    payload["timeout_ms"] = plan.value("timeout_ms", 30000);
    payload["max_bytes"] = plan.value("max_bytes", (long long)65536);

    json resp;
    std::string err;
    int timeout_ms = plan.value("timeout_ms", 30000);
    bool ok = execOnDevice(devid, payload.dump(), timeout_ms, resp, err);
    if (!ok || !resp.value("ok", false)) {
        finish("failed", ok ? resp : json::object(), ok ? std::string("command failed") : err);
        return;
    }
    if (verify.empty()) {
        finish("applied", resp, "");
        return;
    }

    // verify：探针 exit_code=0 才算自愈成功，否则保留为 failed 并记录探针输出
    json probe;
    probe["argv"] = verify;
    probe["timeout_ms"] = 30000;
    probe["max_bytes"] = (long long)65536;
    json presp;
    std::string perr;
    bool pok = execOnDevice(devid, probe.dump(), 30000, presp, perr);
    bool passed = pok && presp.value("ok", false) && presp.value("exit_code", -1) == 0;
    json combined;
    combined["apply"] = resp;
    combined["verify"] = pok ? presp : json{{"error", perr}};
    finish(passed ? "verified" : "failed", combined, passed ? "" : "verify 探针未通过");
}

// ---------------------------------------------------------------- API

void AdminServer::handlePlaybooksList(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.playbookList(out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    ctx->send(out);
}

void AdminServer::handlePlaybookGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out;
    if (id <= 0 || !db_.playbookGet(id, out)) {
        apiNotFound(ctx, "规则不存在");
        return;
    }
    ctx->send(out);
}

void AdminServer::handlePlaybookFires(const HttpContextPtr& ctx) {
    int limit = 50;
    for (const auto& kv : ctx->request->query_params) {
        if (kv.first == "limit") limit = atoi(kv.second.c_str());
    }
    std::string out;
    if (!db_.playbookFires(limit, out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    // 受限账号只看自己设备上的触发记录
    if (scopeRestricted()) {
        json doc = json::parse(out, nullptr, false);
        json keep = json::array();
        for (auto& x : doc.value("items", json::array())) {
            if (ownsDevid(x.value("devid", std::string()))) keep.push_back(x);
        }
        json resp;
        resp["items"] = keep;
        ctx->send(resp.dump());
        return;
    }
    ctx->send(out);
}

void AdminServer::handlePlaybookSave(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        apiBad(ctx, "bad json");
        return;
    }
    long long id = req.value("id", (long long)0);
    if (id <= 0) id = atoll(ctx->param("id").c_str());

    std::vector<std::string> argv;
    for (auto& v : req.value("argv", json::array()))
        if (v.is_string()) argv.push_back(v.get<std::string>());
    if (argv.empty()) {
        apiBad(ctx, "argv 不能为空");
        return;
    }
    // risk 服务端自己算：客户端不能把「重启服务」标成只读来绕过审批
    int risk = argvRiskOf(argv);

    std::string verify_json;
    if (req.contains("verify_argv") && req["verify_argv"].is_array() && !req["verify_argv"].empty())
        verify_json = req["verify_argv"].dump();

    long long out_id = 0;
    std::string err;
    if (!db_.playbookSave(id, req.value("name", std::string()), req.value("event", std::string("*")),
                          req.value("devid_glob", std::string("*")), json(argv).dump(), verify_json, risk,
                          req.value("auto_apply", false), req.value("cooldown_s", 300),
                          req.value("max_per_hour", 6), req.value("note", std::string()),
                          req.value("enabled", true), out_id, err)) {
        apiBad(ctx, err.empty() ? "db error" : err);
        return;
    }
    auditEx(id > 0 ? "playbook.update" : "playbook.create", "playbook", std::to_string(out_id), "ok",
            req.value("name", std::string()) + " risk=" + std::to_string(risk));
    std::string out;
    if (!db_.playbookGet(out_id, out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    ctx->send(out);
}

void AdminServer::handlePlaybookDelete(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;
    long long id = atoll(ctx->param("id").c_str());
    std::string cur;
    if (id <= 0 || !db_.playbookGet(id, cur)) {
        apiNotFound(ctx, "规则不存在");
        return;
    }
    if (!db_.playbookDelete(id)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    // 只删规则，不改历史作业：已经触发的流水要能回查
    auditEx("playbook.delete", "playbook", std::to_string(id), "ok",
            json::parse(cur, nullptr, false).value("name", std::string()));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePlaybookEnable(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    bool enabled = req.is_object() ? req.value("enabled", true) : true;
    if (id <= 0 || !db_.playbookSetEnabled(id, enabled)) {
        apiNotFound(ctx, "规则不存在");
        return;
    }
    auditEx("playbook.enable", "playbook", std::to_string(id), "ok", enabled ? "on" : "off");
    std::string out;
    if (!db_.playbookGet(id, out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    ctx->send(out);
}

// 手动跑一次：用规则自己的 argv 给指定设备生成作业。
// 走和自动触发完全一样的路径（同一套审批与 auto_apply 判定），不会因为“人点的”就免批。
void AdminServer::handlePlaybookRun(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;
    long long id = atoll(ctx->param("id").c_str());
    std::string cur;
    if (id <= 0 || !db_.playbookGet(id, cur)) {
        apiNotFound(ctx, "规则不存在");
        return;
    }
    json pb = json::parse(cur, nullptr, false);
    json req = json::parse(ctx->body(), nullptr, false);
    std::string devid = req.is_object() ? req.value("devid", std::string()) : std::string();
    if (devid.empty()) {
        apiBad(ctx, "缺少 devid");
        return;
    }
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    std::vector<std::string> argv = jsonStrArray(pb.value("argv", json::array()));
    std::vector<std::string> ver = jsonStrArray(pb.value("verify_argv", json::array()));
    if (argv.empty()) {
        apiBad(ctx, "规则没有 argv");
        return;
    }
    int risk = pb.value("risk", 2);
    int auto_max_risk = settingInt("playbook.auto_apply_max_risk", 1);
    if (auto_max_risk < 1) auto_max_risk = 1;
    if (auto_max_risk > 3) auto_max_risk = 3;
    bool auto_apply = pb.value("auto_apply", false) && risk <= auto_max_risk;
    int threshold = settingInt("ops.require_approval_risk", 2);
    if (threshold < 1) threshold = 1;

    json plan;
    plan["kind"] = "exec";
    plan["devid"] = devid;
    plan["argv"] = argv;
    plan["timeout_ms"] = 30000;
    plan["max_bytes"] = (long long)65536;
    plan["source"] = "playbook";
    plan["playbook_id"] = id;
    plan["playbook"] = pb.value("name", std::string());
    plan["trigger"] = "manual";
    if (!ver.empty()) plan["verify_argv"] = ver;

    std::string actor = "playbook:" + std::to_string(id) + ":" + pb.value("name", std::string());
    long long op_id = 0;
    std::string err;
    if (!db_.opsCreate(actor, "playbook", "exec", "device", devid, plan.dump(),
                       AdminDb::sha256Hex(plan.dump()), risk, !auto_apply && risk >= threshold, "", op_id,
                       err)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err.empty() ? "db error" : err);
        return;
    }
    if (auto_apply) playbookEnqueueOp(op_id);
    auditEx("playbook.run", "device", devid, "ok",
            "playbook=" + std::to_string(id) + " op=" + std::to_string(op_id) +
                (auto_apply ? " auto" : ""));
    std::string out;
    if (!db_.opsGet(op_id, out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    ctx->send(out);
}
