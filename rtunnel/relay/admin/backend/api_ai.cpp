// AI 助手：模型/密钥配置与对话（OpenAI 兼容 + function calling，只读工具）
//
// 设计要点
// - 密钥不进数据库、不进接口响应：写到 <admin.db 同目录>/ai.key（0600），
//   接口只回 key_set / key_updated_ms，避免备份库或看设置的人拿到明文。
// - 模型调用在独立线程里同步发（HttpClient 是阻塞的），不占 hv 的 worker。
// - 工具全是只读，权限按**请求线程快照**判定：新线程里 tl_actor 是空的，
//   直接 hasPerm 会变成匿名（exec 那次已经踩过同类坑）。
// - 工具定义与 /api/v1/* 的真实接口一一对应，测试里会比对路径存在，
//   防止文档/工具漂移。
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"
#include "http/client/HttpClient.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

using nlohmann::json;

namespace {

const size_t kMaxMessages = 40;          // 带上文的条数上限（不含 system）
const size_t kMaxToolResultBytes = 6000; // 单个工具结果进上下文的字节上限
const int    kDefaultMaxRounds = 4;      // 工具调用轮次上限

std::string trimStr(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 密钥文件与库同目录：不依赖进程的 prefix，也不会被 --admin-file-dir 暴露
std::string aiKeyPath(const std::string& db_path) {
    std::string dir = db_path;
    size_t slash = dir.find_last_of('/');
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    return dir + "/ai.key";
}

std::string readAiKey(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (f == nullptr) return std::string();
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    return trimStr(std::string(buf, n));
}

bool writeAiKey(const std::string& path, const std::string& key, std::string& err) {
    // 先写临时文件再 rename：避免写到一半留下空 key
    std::string tmp = path + ".tmp";
    mode_t old = umask(0077);
    FILE* f = fopen(tmp.c_str(), "w");
    umask(old);
    if (f == nullptr) {
        err = "无法写入密钥文件";
        return false;
    }
    if (!key.empty()) fwrite(key.data(), 1, key.size(), f);
    fflush(f);
    fclose(f);
    chmod(tmp.c_str(), 0600);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        err = "无法替换密钥文件";
        return false;
    }
    return true;
}

bool aiCan(const AiActor& a, const char* key) {
    if (a.super) return true;
    // 接入令牌可用只读助手（工具内部仍按它自己的设备范围收窄），但不能改模型配置
    if (a.is_client && std::string(key) == "ai.config") return false;
    return a.perms.count(key) > 0;
}

std::string joinPath(const std::string& base, const char* tail) {
    std::string b = base;
    while (!b.empty() && b.back() == '/') b.pop_back();
    return b + tail;
}

// ---------------------------------------------------------------- 工具定义

json toolDefs() {
    auto fn = [](const char* name, const char* desc, json props, json required) {
        json t;
        t["type"] = "function";
        t["function"]["name"] = name;
        t["function"]["description"] = desc;
        t["function"]["parameters"]["type"] = "object";
        t["function"]["parameters"]["properties"] = std::move(props);
        if (!required.empty()) t["function"]["parameters"]["required"] = std::move(required);
        return t;
    };
    json arr = json::array();
    arr.push_back(fn("list_devices",
                     "列出设备及在线状态、版本、架构、所属租户。回答“有哪些设备/哪台离线”先用它。",
                     {{"online_only", {{"type", "boolean"}, {"description", "只看在线设备"}}}}, json::array()));
    arr.push_back(fn("device_detail",
                     "单台设备的运行详情：在线状态、版本、负载、内存、磁盘、网卡、已开放能力、当前会话数。",
                     {{"devid", {{"type", "string"}, {"description", "设备 ID"}}}}, json::array({"devid"})));
    arr.push_back(fn("device_metrics",
                     "设备的分钟级历史指标（CPU/内存/负载），用于回答“过去几小时怎么样”。",
                     {{"devid", {{"type", "string"}}},
                      {"hours", {{"type", "integer"}, {"description", "回看小时数，1-168，默认 6"}}}},
                     json::array({"devid"})));
    arr.push_back(fn("recent_audit",
                     "最近的审计记录（谁在什么时候做了什么）。回答“谁改过配置/谁连过设备”用。",
                     {{"limit", {{"type", "integer"}, {"description", "1-50，默认 20"}}},
                      {"action_prefix", {{"type", "string"},
                                         {"description", "只看某类动作，例如 permission. / device. / certs."}}}},
                     json::array()));
    arr.push_back(fn("recent_operations",
                     "最近的操作作业（含人工与自愈规则触发的命令执行、审批状态、结果）。",
                     {{"devid", {{"type", "string"}, {"description", "只看该设备"}}},
                      {"limit", {{"type", "integer"}, {"description", "1-50，默认 20"}}}},
                     json::array()));
    arr.push_back(fn("usage_summary",
                     "按指标汇总的用量（流量/会话/在线时长），可按租户。用于回答“用了多少”。",
                     {{"days", {{"type", "integer"}, {"description", "最近天数，1-90，默认 7"}}},
                      {"tenant_id", {{"type", "integer"}, {"description", "只看该租户，0=全部"}}}},
                     json::array()));
    arr.push_back(fn("list_playbooks",
                     "自愈规则及其最近触发记录。回答“有没有自动处理/为什么没自动重启”用。",
                     json::object(), json::array()));
    // 写操作只“提议”：生成一个待确认的 operation，绝不在工具里执行
    arr.push_back(fn(
        "propose_exec",
        "当用户明确要求对设备做变更（重启服务、清理文件、跑修复/巡检命令）时调用。"
        "只会生成一个待用户确认的作业，不会执行；返回 id/风险/是否需要审批。",
        {{"devid", {{"type", "string"}, {"description", "目标设备 ID"}}},
         {"argv", {{"type", "array"}, {"items", {{"type", "string"}}},
                    {"description", "命令参数数组，不经过 shell；例如 [\"systemctl\",\"restart\",\"nginx\"]"}}},
         {"reason", {{"type", "string"}, {"description", "为什么要这么做，一句话，会显示给用户"}}},
         {"timeout_ms", {{"type", "integer"}, {"description", "1000-30000，默认 30000"}}},
         {"max_bytes", {{"type", "integer"}, {"description", "输出上限，默认 65536"}}}},
        json::array({"devid", "argv"})));
    arr.push_back(fn(
        "draft_playbook",
        "把排障结论固化成自愈规则草稿（不落库、不启用）：什么事件、哪类设备、执行什么、怎么复核。"
        "用户确认后由页面创建为停用规则。",
        {{"name", {{"type", "string"}, {"description", "规则名，例如 磁盘告警清理"}}},
         {"event", {{"type", "string"}, {"enum", {"device.online", "device.offline", "task.failed", "cert.expiring"}}}},
         {"devid_glob", {{"type", "string"}, {"description", "设备通配，默认 *"}}},
         {"argv", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "触发后执行的命令"}}},
         {"verify_argv", {{"type", "array"}, {"items", {{"type", "string"}}},
                          {"description", "复核命令，exit 0 视为成功，可空"}}},
         {"reason", {{"type", "string"}, {"description", "适用场景，一句话"}}}},
        json::array({"name", "event", "argv"})));
    return arr;
}

// 对话跑在独立线程里，actorLabel() 在那里为空；审计必须显式带主体，
// 否则会写成一堆 anonymous，事后分不清是谁/哪个模型触发的。
void aiAudit(AuditLog& log, const std::string& actor, const std::string& action,
             const std::string& target_type, const std::string& target_id,
             const std::string& result, const std::string& detail, const std::string& op_id) {
    AuditEntry e;
    e.actor = actor.empty() ? "ai:assistant" : actor;
    e.action = action;
    e.target_type = target_type;
    e.target_id = target_id;
    e.result = result.empty() ? "ok" : result;
    e.detail = detail;
    e.op_id = op_id;
    log.writeEntry(e);
}

// 把工具结果裁到上下文可接受的大小
std::string clampResult(const std::string& s) {
    if (s.size() <= kMaxToolResultBytes) return s;
    return s.substr(0, kMaxToolResultBytes) + "\n…(已截断)";
}

}  // namespace

// ---------------------------------------------------------------- 配置接口

void AdminServer::handleAiConfigGet(const HttpContextPtr& ctx) {
    std::string kp = aiKeyPath(cfg_.db_path);
    struct stat st;
    bool key_set = (stat(kp.c_str(), &st) == 0 && st.st_size > 0);
    json j;
    j["enabled"] = settingInt("ai.enabled", 0) != 0;
    j["endpoint"] = settingStr("ai.endpoint", "https://api.deepseek.com/v1");
    j["model"] = settingStr("ai.model", "deepseek-chat");
    j["max_tokens"] = settingInt("ai.max_tokens", 2048);
    j["temperature"] = atof(settingStr("ai.temperature", "0.3").c_str());
    j["timeout_s"] = settingInt("ai.timeout_s", 60);
    j["max_rounds"] = settingInt("ai.max_rounds", kDefaultMaxRounds);
    j["insecure"] = settingInt("ai.insecure", 0) != 0;
    j["key_set"] = key_set;
    j["key_updated_ms"] = key_set ? (long long)st.st_mtime * 1000 : 0;
    j["key_path"] = kp;   // 只回路径，不回内容
    j["tools"] = (int)toolDefs().size();
    ctx->send(j.dump());
}

void AdminServer::handleAiConfigSet(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        apiBad(ctx, "bad json");
        return;
    }
    if (req.contains("endpoint")) {
        std::string ep = trimStr(req.value("endpoint", std::string()));
        if (!ep.empty() && ep.rfind("http://", 0) != 0 && ep.rfind("https://", 0) != 0) {
            apiBad(ctx, "endpoint 必须以 http:// 或 https:// 开头");
            return;
        }
        if (!setSettingCached("ai.endpoint", ep, role)) {
            apiBad(ctx, "db error");
            return;
        }
    }
    if (req.contains("model")) {
        std::string m = trimStr(req.value("model", std::string()));
        if (m.size() > 128) {
            apiBad(ctx, "model 过长");
            return;
        }
        setSettingCached("ai.model", m, role);
    }
    auto setNum = [&](const char* key, int lo, int hi) {
        if (!req.contains(key) || !req[key].is_number()) return;
        int v = req[key].get<int>();
        if (v < lo) v = lo;
        if (v > hi) v = hi;
        setSettingCached(key, std::to_string(v), role);
    };
    setNum("max_tokens", 128, 16384);
    setNum("timeout_s", 5, 300);
    setNum("max_rounds", 1, 8);
    if (req.contains("temperature") && req["temperature"].is_number()) {
        double t = req["temperature"].get<double>();
        if (t < 0) t = 0;
        if (t > 2) t = 2;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", t);
        setSettingCached("ai.temperature", buf, role);
    }
    if (req.contains("enabled")) {
        setSettingCached("ai.enabled", req["enabled"].is_boolean() && req["enabled"].get<bool>() ? "1" : "0",
                       role);
    }
    if (req.contains("insecure")) {
        setSettingCached("ai.insecure",
                       req["insecure"].is_boolean() && req["insecure"].get<bool>() ? "1" : "0", role);
    }
    auditEx("ai.config", "ai", "", "ok", "更新模型配置");
    handleAiConfigGet(ctx);
}

void AdminServer::handleAiKeySet(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        apiBad(ctx, "bad json");
        return;
    }
    std::string key = trimStr(req.value("key", std::string()));
    if (key.empty()) {
        apiBad(ctx, "key 不能为空");
        return;
    }
    if (key.size() > 512) {
        apiBad(ctx, "key 过长");
        return;
    }
    std::string err;
    std::string kp = aiKeyPath(cfg_.db_path);
    if (!writeAiKey(kp, key, err)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err);
        return;
    }
    // 审计只记长度与指纹，不记内容
    std::string hint = key.substr(0, 3) + "…" + key.substr(key.size() > 4 ? key.size() - 4 : 0);
    auditEx("ai.key.set", "ai", "", "ok", "更新模型密钥 " + hint);
    handleAiConfigGet(ctx);
}

void AdminServer::handleAiKeyClear(const HttpContextPtr& ctx, const std::string& role) {
    std::string err;
    std::string kp = aiKeyPath(cfg_.db_path);
    if (!writeAiKey(kp, std::string(), err)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err);
        return;
    }
    auditEx("ai.key.clear", "ai", "", "ok", "清除模型密钥");
    handleAiConfigGet(ctx);
}

// ---------------------------------------------------------------- 工具执行

bool AdminServer::aiRunTool(const AiActor& a, const std::string& name, const json& args,
                            std::string& out, std::string& err) {
    if (!aiCan(a, "device.view")) {
        err = "当前账号没有 device.view 权限";
        return false;
    }
    auto owns = [&](const std::string& devid) {
        if (a.super || a.all_devices) {
            if (!a.is_client) return true;
        }
        if (a.is_client) {
            long long t = 0;
            if (!db_.deviceTenant(devid, t) || t != a.tenant_id) return false;
        }
        if (a.super || a.all_devices) return true;
        return a.devices.count(devid) > 0;
    };

    if (name == "list_devices") {
        bool online_only = args.value("online_only", false);
        json online = json::parse(callRelay([this]() { return relay_->onlineAgentDevids(); }),
                                  nullptr, false);
        std::set<std::string> on;
        if (online.is_array()) {
            for (auto& d : online) {
                if (d.is_string()) on.insert(d.get<std::string>());
            }
        }
        std::vector<std::string> all;
        db_.deviceList(all);
        json arr = json::array();
        for (const auto& d : all) {
            if (!owns(d)) continue;
            bool is_on = on.count(d) > 0;
            if (online_only && !is_on) continue;
            json item;
            item["devid"] = d;
            item["online"] = is_on;
            long long t = 1;
            if (db_.deviceTenant(d, t)) item["tenant_id"] = t;
            if (is_on) {
                std::string ver = callRelay([this, &d]() { return relay_->agentVersion(d.c_str()); });
                std::string arch = callRelay([this, &d]() { return relay_->agentArch(d.c_str()); });
                if (!ver.empty()) item["version"] = ver;
                if (!arch.empty()) item["arch"] = arch;
            }
            arr.push_back(item);
        }
        json r;
        r["total"] = (int)arr.size();
        r["online"] = (int)on.size();
        r["devices"] = arr;
        out = r.dump();
        return true;
    }

    if (name == "device_detail" || name == "device_metrics") {
        std::string devid = args.value("devid", std::string());
        if (devid.empty()) {
            err = "缺少 devid";
            return false;
        }
        if (!owns(devid)) {
            err = "设备不在授权范围内: " + devid;
            return false;
        }
        if (name == "device_metrics") {
            int hours = args.value("hours", 6);
            if (hours < 1) hours = 1;
            if (hours > 168) hours = 168;
            long long to = AdminDb::nowMs();
            long long from = to - (long long)hours * 3600 * 1000;
            std::string js;
            std::string e;
            if (!db_.metricsQuery(devid, from, to, 300, js, e)) {
                err = e.empty() ? "查询失败" : e;
                return false;
            }
            out = js;
            return true;
        }
        // 详情：只用 relay 的公开查询，不依赖 api_agents.cpp 里的 static 辅助
        std::string body = callRelay([this, &devid]() {
            json j;
            RelayConn* c = relay_->findAgent(devid.c_str());
            j["devid"] = devid;
            j["online"] = (c != nullptr);
            if (c != nullptr) {
                uint64_t now = hloop_now_ms(relay_->loop);
                j["peer_proto"] = c->peer_proto;
                j["connected_ms"] = now > c->connected_at_ms ? (long long)(now - c->connected_at_ms) : 0;
                j["remote_addr"] = c->remote_addr;
                int streams = 0;
                for (RelayStream* s = c->streams; s; s = s->next_c) ++streams;
                j["streams"] = streams;
            }
            std::string ver = relay_->agentVersion(c ? c->devid : devid.c_str());
            if (!ver.empty()) j["version"] = ver;
            std::string arch = relay_->agentArch(c ? c->devid : devid.c_str());
            if (!arch.empty()) j["arch"] = arch;
            j["caps_ports"] = relay_->agentPorts(devid.c_str());
            j["caps_proc"] = relay_->agentProcCap(devid.c_str());
            j["caps_exec"] = relay_->agentExecCap(devid.c_str());
            auto it = relay_->sysinfo_last.find(devid);
            if (it != relay_->sysinfo_last.end()) {
                json si = json::parse(it->second.json, nullptr, false);
                if (!si.is_discarded()) j["sysinfo"] = si;
                j["sysinfo_age_ms"] = it->second.ts > 0
                                          ? (long long)(hloop_now_ms(relay_->loop) - it->second.ts)
                                          : 0;
            }
            return j.dump();
        });
        out = body;
        return true;
    }

    if (name == "recent_audit") {
        int limit = args.value("limit", 20);
        if (limit < 1) limit = 1;
        if (limit > 50) limit = 50;
        std::string prefix = args.value("action_prefix", std::string());
        json arr = json::parse(audit_.tailJson(limit * 4), nullptr, false);   // 留过滤余量
        json keep = json::array();
        if (arr.is_array()) {
            for (auto& e : arr) {
                if (!e.is_object()) continue;
                std::string act = e.value("action", std::string());
                if (!prefix.empty() && act.rfind(prefix, 0) != 0) continue;
                // 受限账号只看自己设备相关的记录
                if (!a.super && !a.all_devices) {
                    std::string tt = e.value("target_type", std::string());
                    std::string tid = e.value("target_id", std::string());
                    if (tt == "device" && !owns(tid)) continue;
                    if (tt != "device") continue;
                }
                keep.push_back(e);
                if ((int)keep.size() >= limit) break;
            }
        }
        out = keep.dump();
        return true;
    }

    if (name == "recent_operations") {
        int limit = args.value("limit", 20);
        if (limit < 1) limit = 1;
        if (limit > 50) limit = 50;
        std::string devid = args.value("devid", std::string());
        if (!devid.empty() && !owns(devid)) {
            err = "设备不在授权范围内: " + devid;
            return false;
        }
        std::string js;
        if (!db_.opsList(devid, std::string(), limit, js)) {
            err = "查询失败";
            return false;
        }
        json doc = json::parse(js, nullptr, false);
        if (!doc.is_discarded() && doc.contains("items") && (!a.super && !a.all_devices)) {
            json keep = json::array();
            for (auto& x : doc["items"]) {
                if (owns(x.value("target_id", std::string()))) keep.push_back(x);
            }
            doc["items"] = keep;
        }
        out = doc.is_discarded() ? js : doc.dump();
        return true;
    }

    if (name == "usage_summary") {
        if (!aiCan(a, "usage.read")) {
            err = "当前账号没有 usage.read 权限";
            return false;
        }
        int days = args.value("days", 7);
        if (days < 1) days = 1;
        if (days > 90) days = 90;
        long long tenant_id = args.value("tenant_id", (long long)0);
        if (a.is_client) tenant_id = a.tenant_id;
        char from[16], to[16];
        time_t now = time(nullptr);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(to, sizeof(to), "%Y-%m-%d", &tmv);
        time_t back = now - (time_t)(days - 1) * 86400;
        localtime_r(&back, &tmv);
        strftime(from, sizeof(from), "%Y-%m-%d", &tmv);
        std::string js;
        std::string e;
        if (!db_.usageQuery(from, to, std::string(), std::string(), std::string(), tenant_id, true, js, e)) {
            err = e.empty() ? "查询失败" : e;
            return false;
        }
        out = js;
        return true;
    }

    if (name == "list_playbooks") {
        std::string js;
        if (!db_.playbookList(js)) {
            err = "查询失败";
            return false;
        }
        json doc = json::parse(js, nullptr, false);
        std::string fires;
        db_.playbookFires(10, fires);
        json r;
        r["playbooks"] = doc.is_discarded() ? json::array() : doc.value("items", json::array());
        json fj = json::parse(fires, nullptr, false);
        r["recent_fires"] = fj.is_discarded() ? json::array() : fj.value("items", json::array());
        out = r.dump();
        return true;
    }

    if (name == "propose_exec") {
        if (!aiCan(a, "device.exec")) {
            err = "当前账号没有 device.exec 权限，不能生成变更作业";
            return false;
        }
        std::string devid = args.value("devid", std::string());
        if (devid.empty()) {
            err = "缺少 devid";
            return false;
        }
        if (!owns(devid)) {
            err = "设备不在授权范围内: " + devid;
            return false;
        }
        std::vector<std::string> argv;
        if (args.contains("argv") && args["argv"].is_array()) {
            for (auto& v : args["argv"]) {
                if (!v.is_string()) {
                    err = "argv 必须是字符串数组";
                    return false;
                }
                argv.push_back(v.get<std::string>());
            }
        }
        if (argv.empty()) {
            err = "argv 不能为空";
            return false;
        }
        // 设备没开受控执行时，作业发下去必然失败；提前说清楚比让用户点两下更好
        std::string caps = callRelay(
            [this, &devid]() { return std::to_string(relay_->agentExecCap(devid.c_str())); });
        int cap = atoi(caps.c_str());
        if (cap != 1) {
            err = "设备 " + devid +
                  " 未启用受控执行（agent 需要配置 --allow-exec），无法生成可执行作业";
            return false;
        }
        long long id = 0;
        std::string opjs, e2;
        std::string actor = "ai:assistant|" + a.label;
        if (!opsCreateExecPlan(actor, "ai", devid, argv, std::string(),
                               args.value("timeout_ms", 30000),
                               args.value("max_bytes", (long long)65536), std::string(), id, opjs,
                               e2)) {
            err = e2.empty() ? "生成作业失败" : e2;
            return false;
        }
        json op = json::parse(opjs, nullptr, false);
        int risk = op.is_discarded() ? 2 : op.value("risk", 2);
        json r;
        r["id"] = id;
        r["devid"] = devid;
        r["argv"] = argv;
        r["risk"] = risk;
        r["status"] = op.is_discarded() ? std::string("planned")
                                        : op.value("status", std::string("planned"));
        r["plan_hash"] = op.is_discarded() ? std::string()
                                           : op.value("plan_hash", std::string());
        r["need_approval"] = r["status"] == "pending_approval";
        r["reason"] = args.value("reason", std::string());
        r["hint"] = "作业已生成，尚未执行：需要用户在对话里点确认（必要时还要另一账号批准）";
        aiAudit(audit_, actor, "ai.propose", "device", devid, "ok",
                "op=" + std::to_string(id) + " risk=" + std::to_string(risk) + " cmd=" +
                    (argv.empty() ? std::string() : argv[0]),
                std::to_string(id));
        out = r.dump();
        return true;
    }

    if (name == "draft_playbook") {
        if (!aiCan(a, "playbook.write")) {
            err = "当前账号没有 playbook.write 权限，不能创建规则草稿";
            return false;
        }
        std::string ev = args.value("event", std::string());
        static const char* kEvents[] = {"device.online", "device.offline", "task.failed",
                                        "cert.expiring"};
        bool ok_ev = false;
        for (const char* k : kEvents)
            if (ev == k) ok_ev = true;
        if (!ok_ev) {
            err = "event 只能是 device.online / device.offline / task.failed / cert.expiring";
            return false;
        }
        std::vector<std::string> argv;
        if (args.contains("argv") && args["argv"].is_array()) {
            for (auto& v : args["argv"])
                if (v.is_string()) argv.push_back(v.get<std::string>());
        }
        if (argv.empty()) {
            err = "argv 不能为空";
            return false;
        }
        std::vector<std::string> verify;
        if (args.contains("verify_argv") && args["verify_argv"].is_array()) {
            for (auto& v : args["verify_argv"])
                if (v.is_string()) verify.push_back(v.get<std::string>());
        }
        std::string nm = args.value("name", std::string("AI 规则"));
        if (nm.size() > 64) nm = nm.substr(0, 64);
        std::string glob = args.value("devid_glob", std::string("*"));
        if (glob.empty() || glob.size() > 128) glob = "*";
        json d;
        d["name"] = nm;
        d["event"] = ev;
        d["devid_glob"] = glob;
        d["argv"] = argv;
        d["verify_argv"] = verify;
        d["note"] = args.value("reason", std::string("由 AI 助手生成"));
        json r;
        r["draft"] = d;
        r["risk"] = opsArgvRisk(argv);
        r["hint"] = "草稿不会自动生效：用户在页面上创建后默认停用，需要显式启用";
        out = r.dump();
        return true;
    }

    err = "未知工具: " + name;
    return false;
}

// ---------------------------------------------------------------- 对话

// 单次模型调用（阻塞，必须在工作线程里跑）
static bool aiCallModel(const std::string& endpoint, const std::string& key, const std::string& model,
                        const json& messages, const json& tools, int max_tokens, double temperature,
                        int timeout_s, bool insecure, json& out, std::string& err) {
    json req_body;
    req_body["model"] = model;
    req_body["messages"] = messages;
    req_body["max_tokens"] = max_tokens;
    req_body["temperature"] = temperature;
    req_body["stream"] = false;
    if (!tools.empty()) {
        req_body["tools"] = tools;
        req_body["tool_choice"] = "auto";
    }

    hv::HttpClient cli;
    cli.setTimeout(timeout_s);
    if (endpoint.rfind("https://", 0) == 0 && insecure) {
        hssl_ctx_opt_t opt;
        memset(&opt, 0, sizeof(opt));
        opt.verify_peer = 0;
        cli.newSslCtx(&opt);
    }
    HttpRequest req;
    req.method = HTTP_POST;
    req.url = joinPath(endpoint, "/chat/completions");
    req.headers["Content-Type"] = "application/json";
    req.headers["Authorization"] = "Bearer " + key;
    req.body = req_body.dump();
    HttpResponse resp;
    int rc = cli.send(&req, &resp);
    if (rc != 0) {
        err = "连接模型服务失败（rc=" + std::to_string(rc) + "），检查 endpoint 与网络";
        return false;
    }
    if (resp.status_code < 200 || resp.status_code >= 300) {
        json e = json::parse(resp.body, nullptr, false);
        std::string msg;
        if (!e.is_discarded() && e.contains("error")) {
            if (e["error"].is_object()) msg = e["error"].value("message", std::string());
            else if (e["error"].is_string()) msg = e["error"].get<std::string>();
        }
        if (msg.empty()) msg = resp.body.substr(0, 200);
        err = "模型返回 " + std::to_string(resp.status_code) + ": " + msg;
        return false;
    }
    json doc = json::parse(resp.body, nullptr, false);
    if (doc.is_discarded() || !doc.contains("choices") || !doc["choices"].is_array() ||
        doc["choices"].empty()) {
        err = "模型响应格式不对（缺少 choices）";
        return false;
    }
    out = doc;
    return true;
}

void AdminServer::handleAiChat(const HttpContextPtr& ctx, const std::string& role) {
    if (settingInt("ai.enabled", 0) == 0) {
        apiError(ctx, HTTP_STATUS_SERVICE_UNAVAILABLE, "AI 助手未启用，请先在「系统设置 → AI 模型」开启");
        return;
    }
    std::string endpoint = trimStr(settingStr("ai.endpoint", ""));
    std::string model = trimStr(settingStr("ai.model", ""));
    std::string kp = aiKeyPath(cfg_.db_path);
    std::string key = readAiKey(kp);
    if (endpoint.empty() || model.empty() || key.empty()) {
        apiError(ctx, HTTP_STATUS_SERVICE_UNAVAILABLE,
                 "模型配置不完整（endpoint / model / key），请先到「系统设置 → AI 模型」补全");
        return;
    }

    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("messages") ||
        !req["messages"].is_array()) {
        apiBad(ctx, "需要 messages 数组");
        return;
    }

    // 快照身份：新线程里 tl_actor 是空的
    AiActor actor;
    AdminActor cur = currentActor();
    actor.is_client = cur.is_client;
    actor.super = cur.super;
    actor.all_devices = cur.all_devices;
    actor.tenant_id = cur.tenant_id;
    actor.devices = cur.devices;
    actor.perms = cur.perms;
    actor.label = actorLabel();
    if (actor.label.empty()) actor.label = "anonymous";

    // 只接受 user/assistant，防客户端伪造 system 覆盖规则
    json msgs = json::array();
    for (auto& m : req["messages"]) {
        if (!m.is_object()) continue;
        std::string r = m.value("role", std::string());
        if (r != "user" && r != "assistant") continue;
        if (!m.contains("content") || !m["content"].is_string()) continue;
        std::string c = m["content"].get<std::string>();
        if (c.size() > 8000) c = c.substr(0, 8000) + "…";
        json o;
        o["role"] = r;
        o["content"] = c;
        msgs.push_back(o);
    }
    if (msgs.empty()) {
        apiBad(ctx, "没有有效消息");
        return;
    }
    while (msgs.size() > kMaxMessages) msgs.erase(msgs.begin());

    std::string devid = req.value("devid", std::string());
    if (!devid.empty() && !ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }

    int max_tokens = settingInt("ai.max_tokens", 2048);
    int timeout_s = settingInt("ai.timeout_s", 60);
    int max_rounds = settingInt("ai.max_rounds", kDefaultMaxRounds);
    if (max_rounds < 1) max_rounds = 1;
    if (max_rounds > 8) max_rounds = 8;
    bool insecure = settingInt("ai.insecure", 0) != 0;
    double temperature = atof(settingStr("ai.temperature", "0.3").c_str());

    std::thread([this, ctx, msgs, endpoint, model, key, max_tokens, timeout_s, max_rounds,
                 temperature, insecure, devid, actor]() {
      try {
        json tools = toolDefs();
        json messages = json::array();
        json sys;
        sys["role"] = "system";
        sys["content"] =
            "你是 rtunnel 运维平台的内置助手。只能依据工具返回的数据回答，不要编造设备状态、"
            "日志或历史。回答用简体中文，先给结论再给依据；涉及数量/时间/版本要写出具体值。"
            "需要设备信息时先调用工具；工具报错就如实说明缺什么权限或设备是否离线。"
            "用户明确要求变更（重启服务、清理磁盘、修复故障）时，先用只读工具确认现状，"
            "再调用 propose_exec 生成待确认作业，或用 draft_playbook 给出自愈规则草稿；"
            "这两个工具都不会真正执行。任何情况下都不要说成已经完成，要明确告诉用户还需要在"
            "对话里确认（风险高的还要另一个账号批准）。";
        messages.push_back(sys);
        for (auto& m : msgs) messages.push_back(m);

        json used = json::array();
        json actions = json::array();
        std::string reply;
        std::string err;
        int rounds = 0;
        long long usage_p = 0;
        long long usage_c = 0;

        for (int i = 0; i < max_rounds; ++i) {
            if (devid.empty() && i == 0) {
                json hint;
                hint["role"] = "system";
                hint["content"] = "用户当前没有选定设备；需要设备上下文时先调用 list_devices。";
                messages.push_back(hint);
            }
            json doc;
            if (!aiCallModel(endpoint, key, model, messages, tools, max_tokens, temperature, timeout_s,
                             insecure, doc, err)) {
                break;
            }
            rounds = i + 1;
            if (doc.contains("usage") && doc["usage"].is_object()) {
                usage_p += doc["usage"].value("prompt_tokens", (long long)0);
                usage_c += doc["usage"].value("completion_tokens", (long long)0);
            }
            json msg = doc["choices"][0].contains("message")
                           ? doc["choices"][0]["message"]
                           : json::object();
            // OpenAI 在 tool_calls 时把 content 置为 null，直接 value<string> 会抛
            std::string content;
            if (msg.contains("content") && msg["content"].is_string())
                content = msg["content"].get<std::string>();
            json calls = json::array();
            if (msg.contains("tool_calls") && msg["tool_calls"].is_array())
                calls = msg["tool_calls"];
            if (calls.empty()) {
                reply = content;
                break;
            }
            // 把 assistant 的 tool_calls 原样回填，再追加 tool 结果
            json asst;
            asst["role"] = "assistant";
            asst["content"] = content;
            asst["tool_calls"] = calls;
            messages.push_back(asst);

            for (auto& c : calls) {
                std::string fn_name;
                json args = json::object();
                std::string cid = c.value("id", std::string());
                if (c.contains("function") && c["function"].is_object()) {
                    fn_name = c["function"].value("name", std::string());
                    std::string raw;
                    if (c["function"].contains("arguments") &&
                        c["function"]["arguments"].is_string())
                        raw = c["function"]["arguments"].get<std::string>();
                    if (!raw.empty()) {
                        json parsed = json::parse(raw, nullptr, false);
                        if (!parsed.is_discarded() && parsed.is_object()) args = parsed;
                    }
                }
                std::string out;
                std::string terr;
                bool ok = !fn_name.empty() && aiRunTool(actor, fn_name, args, out, terr);
                // 提议类工具的结果要单独回给前端渲染成“确认卡片”
                if (ok && (fn_name == "propose_exec" || fn_name == "draft_playbook")) {
                    json a = json::parse(out, nullptr, false);
                    if (!a.is_discarded() && a.is_object()) {
                        a["type"] = fn_name;
                        actions.push_back(a);
                    }
                }
                json rec;
                rec["name"] = fn_name;
                rec["args"] = args;
                rec["ok"] = ok;
                if (!ok) {
                    rec["error"] = terr;
                    out = "{\"error\":" + json(terr).dump() + "}";
                }
                used.push_back(rec);

                json t;
                t["role"] = "tool";
                t["tool_call_id"] = cid;
                t["content"] = ok ? clampResult(out) : out;
                messages.push_back(t);
            }
        }

        if (reply.empty() && err.empty()) {
            err = "模型在 " + std::to_string(max_rounds) + " 轮内没有给出最终回答，请把问题拆小一点";
        }

        json resp;
        if (!err.empty()) {
            resp["error"] = err;
            resp["actions"] = actions;
            resp["tools"] = used;
            aiAudit(audit_, "ai:assistant|" + actor.label, "ai.chat", "ai", devid, "error",
                    (devid.empty() ? "-" : devid) + " err=" + err.substr(0, 160), std::string());
            ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
            ctx->send(resp.dump(-1, ' ', false, json::error_handler_t::replace));
            return;
        }
        resp["reply"] = reply;
        resp["actions"] = actions;
        resp["tools"] = used;
        resp["rounds"] = rounds;
        resp["usage"] = {{"prompt_tokens", usage_p}, {"completion_tokens", usage_c}};
        aiAudit(audit_, "ai:assistant|" + actor.label, "ai.chat", "device", devid, "ok",
                "tools=" + std::to_string(used.size()) + " actions=" + std::to_string(actions.size()) +
                    " rounds=" + std::to_string(rounds) +
                    (devid.empty() ? "" : " devid=" + devid), std::string());
        ctx->send(resp.dump(-1, ' ', false, json::error_handler_t::replace));
      } catch (const std::exception& e) {
        logf_("[ai] chat exception: %s", e.what());
        json resp;
        resp["error"] = std::string("处理失败: ") + e.what();
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send(resp.dump(-1, ' ', false, json::error_handler_t::replace));
      } catch (...) {
        logf_("[ai] chat exception: unknown");
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"处理失败\"}");
      }
    }).detach();

    // 线程已接管响应；路由层返回 HTTP_STATUS_NEXT 表示不要收尾
}

// ---------------------------------------------------------------- 连通性测试

// 用当前配置发一次最小请求：不写业务数据、不带工具，只回答“配置是否能用”。
// 运维最容易出问题的是 endpoint 多了/少了 /v1、模型名写错、密钥没权限，
// 这里把真实错误原文回给页面，比让人猜日志快。
void AdminServer::handleAiTest(const HttpContextPtr& ctx) {
    std::string endpoint = trimStr(settingStr("ai.endpoint", ""));
    std::string model = trimStr(settingStr("ai.model", ""));
    std::string key = readAiKey(aiKeyPath(cfg_.db_path));
    if (endpoint.empty() || model.empty() || key.empty()) {
        apiError(ctx, HTTP_STATUS_SERVICE_UNAVAILABLE,
                 "模型配置不完整（endpoint / model / key），请先补全");
        return;
    }
    bool insecure = settingInt("ai.insecure", 0) != 0;
    int timeout_s = settingInt("ai.timeout_s", 60);
    std::thread([this, ctx, endpoint, model, key, insecure, timeout_s]() {
        json messages = json::array();
        json m;
        m["role"] = "user";
        m["content"] = "只回复两个字：正常";
        messages.push_back(m);
        auto t0 = std::chrono::steady_clock::now();
        json doc;
        std::string err;
        bool ok = aiCallModel(endpoint, key, model, messages, json::array(), 32, 0, timeout_s,
                              insecure, doc, err);
        long long ms = (long long)std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t0)
                           .count();
        json resp;
        resp["ok"] = ok;
        resp["endpoint"] = endpoint;
        resp["model"] = model;
        resp["latency_ms"] = ms;
        if (!ok) {
            resp["error"] = err;
            ctx->send(resp.dump(-1, ' ', false, json::error_handler_t::replace));
            return;
        }
        std::string reply;
        if (doc.contains("choices") && doc["choices"].is_array() && !doc["choices"].empty()) {
            json msg = doc["choices"][0].contains("message") ? doc["choices"][0]["message"]
                                                            : json::object();
            if (msg.contains("content") && msg["content"].is_string())
                reply = msg["content"].get<std::string>();
        }
        resp["reply"] = reply;
        // 网关可能把请求路由到别的模型；把它回的实际 model 带回来，便于确认
        if (doc.contains("model") && doc["model"].is_string())
            resp["served_model"] = doc["model"].get<std::string>();
        ctx->send(resp.dump(-1, ' ', false, json::error_handler_t::replace));
    }).detach();
}
