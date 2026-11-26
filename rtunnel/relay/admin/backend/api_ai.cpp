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
#include "hversion.h"

#include <algorithm>
#include <cerrno>
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

// 每个账号一份配置：控制台账号 u-<users.id>，接入账号 c-<client_accounts.id>。
// 同一个账号的多个 atk 令牌共享这一份（本来就是同一个人的凭据）。
std::string aiSubject(const AdminActor& a) {
    if (a.is_client && a.client_id > 0) return "c-" + std::to_string(a.client_id);
    if (a.id > 0) return "u-" + std::to_string(a.id);
    return std::string();
}

// 密钥文件与库同目录、按主体分文件：不进库，备份库不会带出密钥，
// 也不会被 --admin-file-dir 暴露给文件管理页
std::string aiKeyDir(const std::string& db_path) {
    std::string dir = db_path;
    size_t slash = dir.find_last_of('/');
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    return dir + "/ai";
}

std::string aiKeyPath(const std::string& db_path, const std::string& subject) {
    return aiKeyDir(db_path) + "/" + subject + ".key";
}

// 目录不存在就建（0700），否则第一次写密钥会失败
bool ensureKeyDir(const std::string& dir) {
    if (dir.empty()) return false;
    if (mkdir(dir.c_str(), 0700) == 0) return true;
    return errno == EEXIST;
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
    size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && !ensureKeyDir(path.substr(0, slash))) {
        err = "无法创建密钥目录";
        return false;
    }
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
    return a.perms.count(key) > 0;
}

// ---------------------------------------------------------------- 账号配置

// 预置厂商：用户只需要选一家、填 key；模型名有默认值，想换随手改。
// endpoint 留空表示由 provider 决定，避免把网关地址写两遍。
struct ProviderDef {
    const char* key;
    const char* label;
    const char* endpoint;
    const char* model;
    const char* hint;
};

const ProviderDef kProviders[] = {
    {"deepseek", "DeepSeek", "https://api.deepseek.com/v1", "deepseek-chat",
     "官方直连；模型可填 deepseek-chat / deepseek-reasoner"},
    {"moonshot", "Kimi（月之暗面）", "https://api.moonshot.cn/v1", "moonshot-v1-8k",
     "每月有免费额度，长文本便宜"},
    {"openai", "OpenAI", "https://api.openai.com/v1", "gpt-4o-mini", "需要可访问 api.openai.com"},
    {"qwen", "通义千问", "https://dashscope.aliyuncs.com/compatible-mode/v1", "qwen-plus",
     "阿里云百炼的 OpenAI 兼容地址"},
    {"zhipu", "智谱 GLM", "https://open.bigmodel.cn/api/paas/v4", "glm-4-flash",
     "glm-4-flash 免费，适合日常问答"},
    {"ollama", "本地 Ollama", "http://127.0.0.1:11434/v1", "qwen2.5:7b",
     "本机/内网跑模型，密钥随便填一个非空值"},
    {"custom", "自定义 / 网关", "", "", "填完整的 .../v1 地址和模型名（如 deepseek-v4.1-flash）"},
};

const ProviderDef* findProvider(const std::string& key) {
    for (const auto& p : kProviders)
        if (key == p.key) return &p;
    return nullptr;
}

json providerCatalog() {
    json arr = json::array();
    for (const auto& p : kProviders) {
        json o;
        o["key"] = p.key;
        o["label"] = p.label;
        o["endpoint"] = p.endpoint;
        o["model"] = p.model;
        o["hint"] = p.hint;
        arr.push_back(o);
    }
    return arr;
}

struct AiProfile {
    std::string provider = "deepseek";
    std::string endpoint;
    std::string model;
    std::string key;
    std::string key_hint;
    long long   key_updated_ms = 0;
    bool        key_set = false;
    bool        insecure = false;
    bool        saved = false;

    // 三项齐了才算可用；缺哪项由调用方给具体提示
    bool ready() const { return !endpoint.empty() && !model.empty() && key_set; }

    // 缺什么就说缺什么，不要笼统地报“配置不完整”
    std::string missing() const {
        if (endpoint.empty()) return "还没配置模型：请在「AI 助手 → 模型设置」里选一家服务商";
        if (model.empty()) return "还没填模型名：请在「AI 助手 → 模型设置」里补上";
        if (!key_set) return "还没填 API Key：请在「AI 助手 → 模型设置」里保存密钥";
        return std::string();
    }
};

AiProfile loadAiProfile(AdminDb& db, const std::string& db_path, const std::string& subject) {
    AiProfile p;
    if (subject.empty()) return p;
    std::string js;
    if (!db.aiProfileGet(subject, js)) return p;
    json j = json::parse(js, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return p;
    p.saved = j.value("saved", false);
    p.provider = j.value("provider", std::string("deepseek"));
    p.endpoint = j.value("endpoint", std::string());
    p.model = j.value("model", std::string());
    p.key_hint = j.value("key_hint", std::string());
    p.key_updated_ms = j.value("key_updated_ms", (long long)0);
    if (j.contains("advanced") && j["advanced"].is_object())
        p.insecure = j["advanced"].value("insecure", false);

    std::string kp = aiKeyPath(db_path, subject);
    p.key = readAiKey(kp);
    p.key_set = !p.key.empty();
    if (p.key_set && p.key_updated_ms == 0) {
        struct stat st;
        if (stat(kp.c_str(), &st) == 0) p.key_updated_ms = (long long)st.st_mtime * 1000;
    }
    return p;
}

// 密钥指纹：只留前 3 后 4，够本人确认是哪把，不至于泄露
std::string keyHint(const std::string& key) {
    if (key.size() <= 8) return std::string("已配置");
    return key.substr(0, 3) + "…" + key.substr(key.size() - 4);
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
    // —— 平台自身：任务、租户、费用、账号、权限、证书、中继、文件仓库 ——
    arr.push_back(fn("platform_overview",
                     "平台总览：版本、运行时长、mTLS/ACL 状态、在线设备与客户端、活跃会话、"
                     "累计流量、告警状态。回答“平台现在怎么样/有没有异常”先看它。",
                     json::object(), json::array()));
    arr.push_back(fn("list_streams",
                     "当前活跃会话：设备、接入用户、状态、建立时间。回答“谁正连着设备/有多少会话”。",
                     {{"devid", {{"type", "string"}, {"description", "只看该设备"}}}}, json::array()));
    arr.push_back(fn("list_clients",
                     "当前接入客户端连接：用户名、来源地址、涉及设备、流量。回答“谁在线/谁在连”。",
                     json::object(), json::array()));
    arr.push_back(fn("list_tasks",
                     "分发与 OTA 任务：状态、设备、文件、版本、失败原因、重试次数。"
                     "回答“升级到哪了/为什么失败/有多少卡住”。",
                     {{"status", {{"type", "string"},
                                  {"enum", {"active", "done", "failed_group", "all"}},
                                  {"description", "active=进行中 done=成功 failed_group=失败超时；默认 all"}}},
                      {"limit", {{"type", "integer"}, {"description", "1-50，默认 20"}}}},
                     json::array()));
    arr.push_back(fn("list_tenants",
                     "租户及其套餐（设备/并发/月流量上限）与当月用量。"
                     "回答“有几个客户/谁的额度快满了”。",
                     json::object(), json::array()));
    arr.push_back(fn("billing_summary",
                     "按租户的费用试算（用量×价目，单位 1e-6 元）。回答“这个月大概多少钱”。",
                     {{"days", {{"type", "integer"}, {"description", "最近天数，1-90，默认 30"}}},
                      {"tenant_id", {{"type", "integer"}, {"description", "只看该租户，0=全部"}}}},
                     json::array()));
    arr.push_back(fn("list_accounts",
                     "接入账号（客户侧）：用户名、租户、状态、令牌数、最近使用。"
                     "回答“有哪些客户账号/谁被停用”。",
                     {{"q", {{"type", "string"}, {"description", "按用户名或展示名搜索"}}}},
                     json::array()));
    arr.push_back(fn("list_users",
                     "管理台账号：用户名、角色、状态、所属权限组。回答“谁有管理台账号/谁是管理员”。",
                     json::object(), json::array()));
    arr.push_back(fn("list_permission_groups",
                     "权限组：勾选的资源键、设备范围、端口范围、成员数。"
                     "回答“谁能做什么/这个组给了什么权限”。",
                     json::object(), json::array()));
    arr.push_back(fn("list_certs",
                     "证书台账：设备身份、签发/到期时间、状态（valid/expiring/expired/revoked）。"
                     "回答“哪些证书快过期/有没有被吊销”。",
                     json::object(), json::array()));
    arr.push_back(fn("list_relays",
                     "中继下发列表：地址、名称、优先级顺序。回答“设备会连哪台 relay/故障切换顺序”。",
                     json::object(), json::array()));
    arr.push_back(fn("list_files",
                     "文件仓库某一层的条目（目录/文件、大小）。回答“仓库里有没有某个包/文件”。",
                     {{"dir", {{"type", "string"},
                               {"description", "相对仓库根的目录，如 packages/agent；空=根"}}}},
                     json::array()));
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

// 每个工具要求的资源键。列出来而不是塞进 schema，避免把权限信息发给模型。
const char* toolPerm(const std::string& name) {
    if (name == "usage_summary") return "usage.read";
    if (name == "billing_summary") return "billing.read";
    if (name == "list_tenants") return "tenant.read";
    if (name == "list_tasks") return "dist.read";
    if (name == "list_accounts" || name == "list_users") return "account.read";
    if (name == "list_permission_groups") return "group.read";
    if (name == "list_certs") return "cert.read";
    if (name == "list_relays") return "relay.read";
    if (name == "list_files") return "repo.read";
    if (name == "propose_exec") return "device.exec";
    if (name == "draft_playbook") return "playbook.write";
    return "device.view";   // 设备/会话/客户端/平台总览/审计/作业/自愈
}

// 只把当前账号真正能用的工具交给模型：少一个工具就少一次“没权限”的往返，
// 也不会让模型以为它能碰费率、证书或别人的账号。
json toolDefsFor(const AiActor& a) {
    json out = json::array();
    for (auto& t : toolDefs()) {
        std::string n = t["function"].value("name", std::string());
        if (aiCan(a, toolPerm(n))) out.push_back(t);
    }
    return out;
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

AiActor AdminServer::aiSnapshot() {
    AiActor a;
    AdminActor cur = currentActor();
    a.is_client = cur.is_client;
    a.super = cur.super;
    a.all_devices = cur.all_devices;
    a.tenant_id = cur.tenant_id;
    a.devices = cur.devices;
    a.perms = cur.perms;
    a.label = actorLabel();
    if (a.label.empty()) a.label = "anonymous";
    return a;
}

// ---------------------------------------------------------------- 配置接口

void AdminServer::handleAiConfigGet(const HttpContextPtr& ctx) {
    std::string subject = aiSubject(currentActor());
    AiProfile p = loadAiProfile(db_, cfg_.db_path, subject);
    json j;
    j["subject"] = subject;
    j["saved"] = p.saved;
    j["provider"] = p.provider;
    j["endpoint"] = p.endpoint;
    j["model"] = p.model;
    j["key_set"] = p.key_set;
    j["key_hint"] = p.key_hint;
    j["key_updated_ms"] = p.key_updated_ms;
    j["insecure"] = p.insecure;
    // 前端只需要知道“能不能直接用”，不用自己拼判断
    j["ready"] = p.ready();
    j["providers"] = providerCatalog();
    // 当前账号实际可用的工具数（toolDefsFor 会按权限裁剪）
    j["tools"] = (int)toolDefsFor(aiSnapshot()).size();
    j["tools_total"] = (int)toolDefs().size();
    ctx->send(j.dump());
}

// 只接受三件事：provider、model、可选的 endpoint 覆盖（自定义/网关时需要）。
// max_tokens/temperature/timeout 之类不再暴露：运维答问题用默认值足够，
// 暴露出来只会让人不知道该填什么。
void AdminServer::handleAiConfigSet(const HttpContextPtr& ctx, const std::string& role) {
    std::string subject = aiSubject(currentActor());
    if (subject.empty()) {
        apiError(ctx, HTTP_STATUS_UNAUTHORIZED, "请先登录管理台再配置模型");
        return;
    }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        apiBad(ctx, "bad json");
        return;
    }
    AiProfile cur = loadAiProfile(db_, cfg_.db_path, subject);
    std::string provider = trimStr(req.value("provider", cur.provider));
    if (provider.empty()) provider = "deepseek";
    const ProviderDef* def = findProvider(provider);
    if (def == nullptr) {
        apiBad(ctx, "未知的 provider: " + provider);
        return;
    }

    std::string endpoint = req.contains("endpoint") && req["endpoint"].is_string()
                               ? trimStr(req["endpoint"].get<std::string>())
                               : cur.endpoint;
    // 换厂商时没给地址：用该厂商的预置地址
    if (endpoint.empty() || (provider != cur.provider && !req.contains("endpoint")))
        endpoint = def->endpoint;
    if (endpoint.empty()) {
        apiBad(ctx, "自定义厂商需要填写接口地址（…/v1）");
        return;
    }
    if (endpoint.rfind("http://", 0) != 0 && endpoint.rfind("https://", 0) != 0) {
        apiBad(ctx, "接口地址必须以 http:// 或 https:// 开头");
        return;
    }
    if (endpoint.size() > 300) {
        apiBad(ctx, "接口地址过长");
        return;
    }

    std::string model = req.contains("model") && req["model"].is_string()
                            ? trimStr(req["model"].get<std::string>())
                            : cur.model;
    if (model.empty() || (provider != cur.provider && !req.contains("model")))
        model = def->model;
    if (model.empty()) {
        apiBad(ctx, "请填写模型名");
        return;
    }
    if (model.size() > 128) {
        apiBad(ctx, "模型名过长");
        return;
    }

    json adv = json::object();
    bool insecure = req.contains("insecure") && req["insecure"].is_boolean()
                        ? req["insecure"].get<bool>()
                        : cur.insecure;
    if (insecure) adv["insecure"] = true;

    std::string err;
    if (!db_.aiProfileSave(subject, provider, endpoint, model, adv.dump(), err)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err.empty() ? "db error" : err);
        return;
    }
    auditEx("ai.profile", "ai", subject, "ok", provider + " / " + model);
    handleAiConfigGet(ctx);
}

void AdminServer::handleAiKeySet(const HttpContextPtr& ctx, const std::string& role) {
    std::string subject = aiSubject(currentActor());
    if (subject.empty()) {
        apiError(ctx, HTTP_STATUS_UNAUTHORIZED, "请先登录管理台再配置模型");
        return;
    }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        apiBad(ctx, "bad json");
        return;
    }
    std::string key = trimStr(req.value("key", std::string()));
    if (key.empty()) {
        apiBad(ctx, "API Key 不能为空");
        return;
    }
    if (key.size() > 512) {
        apiBad(ctx, "API Key 过长");
        return;
    }
    if (key.find_first_of(" \t\r\n") != std::string::npos) {
        apiBad(ctx, "API Key 里不能有空格或换行，请检查是否多粘了内容");
        return;
    }
    std::string err;
    std::string kp = aiKeyPath(cfg_.db_path, subject);
    if (!writeAiKey(kp, key, err)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err);
        return;
    }
    long long now = AdminDb::nowMs();
    std::string hint = keyHint(key);
    if (!db_.aiProfileSetKey(subject, hint, now, err)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err.empty() ? "db error" : err);
        return;
    }
    // 审计只记指纹，不记内容
    auditEx("ai.key.set", "ai", subject, "ok", "更新模型密钥 " + hint);
    handleAiConfigGet(ctx);
}

void AdminServer::handleAiKeyClear(const HttpContextPtr& ctx, const std::string& role) {
    std::string subject = aiSubject(currentActor());
    if (subject.empty()) {
        apiError(ctx, HTTP_STATUS_UNAUTHORIZED, "请先登录管理台再配置模型");
        return;
    }
    std::string err;
    std::string kp = aiKeyPath(cfg_.db_path, subject);
    if (!writeAiKey(kp, std::string(), err)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err);
        return;
    }
    db_.aiProfileSetKey(subject, std::string(), 0, err);
    auditEx("ai.key.clear", "ai", subject, "ok", "清除模型密钥");
    handleAiConfigGet(ctx);
}

// ---------------------------------------------------------------- 工具执行

bool AdminServer::aiRunTool(const AiActor& a, const std::string& name, const json& args,
                            std::string& out, std::string& err) {
    // 权限按工具逐个判定（toolDefsFor 已经过滤过一次，这里仍然要查：
    // 模型可以编造工具名，不能指望它只用列表里的）。
    if (!aiCan(a, toolPerm(name))) {
        err = std::string("当前账号没有 ") + toolPerm(name) + " 权限，无法使用 " + name;
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

    if (name == "platform_overview") {
        // relay 线程只做一次原始快照（不碰 DB），过滤放到管理面线程
        std::string raw = callRelay([this]() {
            json j;
            json agents = json::array();
            for (RelayConn* c = relay_->agents; c; c = c->reg_next) {
                int n = 0;
                for (RelayStream* st = c->streams; st; st = st->next_c) ++n;
                json x;
                x["devid"] = c->devid;
                x["streams"] = n;
                x["bytes_in"] = (long long)c->bytes_in;
                x["bytes_out"] = (long long)c->bytes_out;
                agents.push_back(x);
            }
            json clients = json::array();
            for (RelayConn* c = relay_->conns; c; c = c->next_all) {
                if (c->role != rtnp::ROLE_CLIENT || !c->authed || c->internal) continue;
                json dv = json::array();
                for (RelayStream* st = c->streams; st; st = st->next_c)
                    if (st->a && st->a->devid[0]) dv.push_back(st->a->devid);
                json x;
                x["user"] = c->user;
                x["devids"] = dv;
                x["bytes_in"] = (long long)c->bytes_in;
                x["bytes_out"] = (long long)c->bytes_out;
                clients.push_back(x);
            }
            j["agents"] = agents;
            j["clients"] = clients;
            j["uptime_ms"] = (long long)(hloop_now_ms(relay_->loop) - relay_->started_at_ms);
            j["mtls"] = (bool)relay_->require_mtls;
            j["allow_all"] = (bool)relay_->allow_all;
            return j.dump();
        });
        json r = json::parse(raw, nullptr, false);
        if (r.is_discarded()) {
            err = "查询失败";
            return false;
        }
        bool scoped = !(a.super || a.all_devices);
        int nagent = 0, nstream = 0, nclient = 0;
        long long bin = 0, bout = 0;
        for (auto& x : r["agents"]) {
            if (!owns(x.value("devid", std::string()))) continue;
            nagent++;
            nstream += x.value("streams", 0);
            bin += x.value("bytes_in", (long long)0);
            bout += x.value("bytes_out", (long long)0);
        }
        for (auto& c : r["clients"]) {
            bool rel = !scoped;
            for (auto& d : c["devids"]) {
                if (d.is_string() && owns(d.get<std::string>())) {
                    rel = true;
                    break;
                }
            }
            if (!rel) continue;
            nclient++;
            bin += c.value("bytes_in", (long long)0);
            bout += c.value("bytes_out", (long long)0);
        }
        json o;
        o["version"] = hv_version();
        o["uptime_ms"] = r.value("uptime_ms", (long long)0);
        o["mtls"] = r.value("mtls", false);
        o["allow_all"] = r.value("allow_all", false);
        o["agents_online"] = nagent;
        o["clients_online"] = nclient;
        o["streams"] = nstream;
        o["bytes_in"] = bin;
        o["bytes_out"] = bout;
        o["scoped"] = scoped;
        json al = json::parse(alertStatusJson(), nullptr, false);
        if (!al.is_discarded()) o["alerts"] = al;
        out = o.dump();
        return true;
    }

    if (name == "list_streams") {
        std::string only = args.value("devid", std::string());
        if (!only.empty() && !owns(only)) {
            err = "设备不在授权范围内: " + only;
            return false;
        }
        std::string raw = callRelay([this]() {
            json arr = json::array();
            for (RelayConn* c = relay_->agents; c; c = c->reg_next) {
                for (RelayStream* st = c->streams; st; st = st->next_c) {
                    json x;
                    x["devid"] = c->devid;
                    x["user"] = st->a ? st->a->user : "";
                    x["state"] = !st->opened ? "OPENING"
                                 : (st->a_closed || st->c_closed) ? "HALF_CLOSED" : "OPEN";
                    x["opened_at_ms"] = (long long)st->opened_at_ms;
                    x["bytes_up"] = (long long)st->bytes_up;
                    x["bytes_down"] = (long long)st->bytes_down;
                    arr.push_back(x);
                }
            }
            return arr.dump();
        });
        json arr = json::parse(raw, nullptr, false);
        json keep = json::array();
        if (arr.is_array()) {
            for (auto& x : arr) {
                std::string d = x.value("devid", std::string());
                if (!owns(d)) continue;
                if (!only.empty() && d != only) continue;
                keep.push_back(x);
            }
        }
        json o;
        o["count"] = (int)keep.size();
        o["streams"] = keep;
        out = o.dump();
        return true;
    }

    if (name == "list_clients") {
        bool scoped = !(a.super || a.all_devices);
        std::string raw = callRelay([this]() {
            json arr = json::array();
            for (RelayConn* c = relay_->conns; c; c = c->next_all) {
                if (c->role != rtnp::ROLE_CLIENT || !c->authed || c->internal) continue;
                json dv = json::array();
                for (RelayStream* st = c->streams; st; st = st->next_c)
                    if (st->a && st->a->devid[0]) dv.push_back(st->a->devid);
                json x;
                x["user"] = c->user;
                x["remote_addr"] = c->remote_addr;
                x["devids"] = dv;
                x["bytes_in"] = (long long)c->bytes_in;
                x["bytes_out"] = (long long)c->bytes_out;
                arr.push_back(x);
            }
            return arr.dump();
        });
        json arr = json::parse(raw, nullptr, false);
        json keep = json::array();
        if (arr.is_array()) {
            for (auto& x : arr) {
                bool rel = !scoped;
                for (auto& d : x["devids"]) {
                    if (d.is_string() && owns(d.get<std::string>())) {
                        rel = true;
                        break;
                    }
                }
                if (!rel) continue;
                keep.push_back(x);
            }
        }
        json o;
        o["count"] = (int)keep.size();
        o["clients"] = keep;
        out = o.dump();
        return true;
    }

    if (name == "list_tasks") {
        std::string status = args.value("status", std::string("all"));
        if (status == "all") status.clear();
        int limit = args.value("limit", 20);
        if (limit < 1) limit = 1;
        if (limit > 50) limit = 50;
        std::string js;
        int total = 0;
        if (!db_.distList(status, std::string(), std::string(), std::string(),
                          std::vector<std::string>(), std::string("created_ms"), -1, 1, limit, js,
                          total)) {
            err = "查询失败";
            return false;
        }
        json arr = json::parse(js, nullptr, false);
        if (arr.is_discarded()) {
            err = "查询失败";
            return false;
        }
        json keep = json::array();
        for (auto& x : arr) {
            if (!owns(x.value("devid", std::string()))) continue;
            json y;
            y["id"] = x.value("id", (long long)0);
            y["batch_id"] = x.value("batch_id", std::string());
            y["devid"] = x.value("devid", std::string());
            y["kind"] = x.value("kind", std::string());
            y["file"] = x.value("file_name", std::string());
            y["status"] = x.value("status", std::string());
            y["retry"] = x.value("retry_count", 0);
            if (x.contains("target_version")) y["target_version"] = x["target_version"];
            if (x.contains("error") && x["error"].is_string() && !x["error"].get<std::string>().empty())
                y["error"] = x["error"];
            y["created_ms"] = x.value("created_ms", (long long)0);
            keep.push_back(y);
        }
        json o;
        o["total"] = total;
        o["shown"] = (int)keep.size();
        o["tasks"] = keep;
        out = o.dump();
        return true;
    }

    if (name == "list_tenants") {
        std::string js;
        if (!db_.tenantList(js)) {
            err = "查询失败";
            return false;
        }
        json doc = json::parse(js, nullptr, false);
        if (doc.is_discarded()) {
            err = "查询失败";
            return false;
        }
        json items = doc.value("items", json::array());
        if (a.is_client) {
            // 接入令牌只看自己所属的租户（与 /api/v1/tenants 一致）
            json keep = json::array();
            for (auto& t : items)
                if (t.value("id", (long long)0) == a.tenant_id) keep.push_back(t);
            items = keep;
        }
        json o;
        o["items"] = items;
        out = o.dump();
        return true;
    }

    if (name == "billing_summary") {
        int days = args.value("days", 30);
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
        if (!db_.billingSummary(from, to, tenant_id, js, e)) {
            err = e.empty() ? "查询失败" : e;
            return false;
        }
        out = js;
        return true;
    }

    if (name == "list_accounts") {
        std::string q = args.value("q", std::string());
        std::string js;
        if (!db_.clientAccountList(q, js)) {
            err = "查询失败";
            return false;
        }
        json arr = json::parse(js, nullptr, false);
        if (arr.is_discarded()) {
            err = "查询失败";
            return false;
        }
        json keep = json::array();
        for (auto& x : arr) {
            if (a.is_client && x.value("tenant_id", (long long)1) != a.tenant_id) continue;
            json y;
            y["id"] = x.value("id", (long long)0);
            y["username"] = x.value("username", std::string());
            if (x.contains("display_name")) y["display_name"] = x["display_name"];
            y["status"] = x.value("status", std::string());
            y["tenant_id"] = x.value("tenant_id", (long long)1);
            y["tokens"] = x.value("token_count", 0);
            y["last_used_ms"] = x.value("last_used_ms", (long long)0);
            keep.push_back(y);
        }
        json o;
        o["count"] = (int)keep.size();
        o["accounts"] = keep;
        out = o.dump();
        return true;
    }

    if (name == "list_users") {
        if (a.is_client) {
            // 控制台账号不对接入令牌暴露（和 /permission/subjects 一致）
            err = "接入令牌看不到管理台账号";
            return false;
        }
        std::vector<DbUser> users;
        if (!db_.listUsers(users)) {
            err = "查询失败";
            return false;
        }
        json arr = json::array();
        for (const auto& u : users) {
            json y;
            y["id"] = u.id;
            y["username"] = u.username;
            y["role"] = u.role;
            y["status"] = u.status;
            std::vector<long long> gids;
            if (db_.permGroupsOfUser(u.id, gids)) {
                y["groups"] = (int)gids.size();
                y["group_ids"] = gids;
            }
            arr.push_back(y);
        }
        json o;
        o["count"] = (int)arr.size();
        o["users"] = arr;
        out = o.dump();
        return true;
    }

    if (name == "list_permission_groups") {
        std::string js;
        if (!db_.permGroups(js)) {
            err = "查询失败";
            return false;
        }
        json arr = json::parse(js, nullptr, false);
        if (arr.is_discarded()) {
            err = "查询失败";
            return false;
        }
        json keep = json::array();
        for (auto& g : arr) {
            json y;
            y["id"] = g.value("id", (long long)0);
            y["name"] = g.value("name", std::string());
            y["builtin_key"] = g.value("builtin_key", std::string());
            y["super"] = g.value("super", false);
            y["all_devices"] = g.value("all_devices", false);
            y["ports"] = g.value("ports", std::string());
            y["members"] = g.value("member_count", 0);
            y["devices"] = g.value("device_count", 0);
            y["resources"] = g.value("resources", json::array());
            keep.push_back(y);
        }
        json o;
        o["count"] = (int)keep.size();
        o["groups"] = keep;
        out = o.dump();
        return true;
    }

    if (name == "list_certs") {
        json arr = json::parse(certsJson(), nullptr, false);
        if (arr.is_discarded()) {
            err = "查询失败";
            return false;
        }
        // 只保留状态摘要 + 快到期/失效的条目，避免台账把上下文撑满
        int valid = 0, expiring = 0, expired = 0, revoked = 0;
        json bad = json::array();
        for (auto& c : arr) {
            std::string st = c.value("status", std::string());
            if (st == "valid") valid++;
            else if (st == "expiring") expiring++;
            else if (st == "expired") expired++;
            else if (st == "revoked") revoked++;
            if (st != "valid") bad.push_back(c);
        }
        json o;
        o["valid"] = valid;
        o["expiring"] = expiring;
        o["expired"] = expired;
        o["revoked"] = revoked;
        o["attention"] = bad;
        out = o.dump();
        return true;
    }

    if (name == "list_relays") {
        std::string js;
        if (!db_.relayListJson(js)) {
            err = "查询失败";
            return false;
        }
        out = js;
        return true;
    }

    if (name == "list_files") {
        std::string dir = args.value("dir", std::string());
        out = repoListJson(dir, 200);
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
    // 每个账号用自己的配置：换人登录就是换模型/换密钥，不共享
    std::string subject = aiSubject(currentActor());
    AiProfile prof = loadAiProfile(db_, cfg_.db_path, subject);
    if (!prof.ready()) {
        apiError(ctx, HTTP_STATUS_SERVICE_UNAVAILABLE, prof.missing());
        return;
    }
    std::string endpoint = prof.endpoint;
    std::string model = prof.model;
    std::string key = prof.key;

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

    // 会话：0/缺省表示新建（线程里落库后把 id 带回去），>0 表示续在已有会话上。
    // 归属校验交给 DB 层（带 subject 条件），别人的 id 会直接写不进去。
    long long session_id = req.value("session_id", (long long)0);
    if (session_id < 0) session_id = 0;
    std::string user_text = msgs[msgs.size() - 1].value("content", std::string());

    // 参数固定成一组对运维问答合适的值：这些不是给用户调的东西
    int max_tokens = 2048;
    int timeout_s = 90;
    int max_rounds = kDefaultMaxRounds;
    bool insecure = prof.insecure;
    double temperature = 0.3;

    std::thread([this, ctx, msgs, endpoint, model, key, max_tokens, timeout_s, max_rounds,
                 temperature, insecure, devid, actor, session_id, subject, user_text]() {
      long long sid = session_id;
      // 记一轮问答：新建会话用问句做标题；失败也记下来，历史里能看到当时为什么没答上来
      auto save_turn = [&](const std::string& reply, const json& extra) {
          std::string e;
          if (sid > 0) {
              if (db_.aiSessionAddTurn(sid, subject, user_text, reply, extra.dump(), e)) return;
              logf_("[ai] save turn failed: %s (session %lld)", e.c_str(), sid);
              if (e != "会话不存在") return;
              // 不是自己的会话（或已被删）：别把消息塞给别人，另起一个
              sid = 0;
          }
          if (!db_.aiSessionCreate(subject, std::string(), sid, e) || sid <= 0) {
              logf_("[ai] create session failed: %s", e.c_str());
              sid = 0;
              return;
          }
          if (!db_.aiSessionAddTurn(sid, subject, user_text, reply, extra.dump(), e))
              logf_("[ai] save turn failed: %s", e.c_str());
      };
      try {
        json tools = toolDefsFor(actor);
        if (tools.empty()) {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"当前账号没有可用的查询工具（至少需要 device.view）\"}");
            return;
        }
        json messages = json::array();
        json sys;
        sys["role"] = "system";
        sys["content"] =
            "你是 rtunnel 运维平台的管理台助手，可以读取整个平台的状态，不只是设备。"
            "平台模块与对应工具：平台总览/在线与告警→platform_overview；"
            "设备→list_devices/device_detail/device_metrics；会话与客户端→list_streams/list_clients；"
            "升级与分发任务→list_tasks；租户与计费→list_tenants/billing_summary/list_accounts；"
            "权限→list_users/list_permission_groups；安全与接入→list_certs/list_relays；"
            "文件仓库→list_files；变更与审计→recent_audit/recent_operations；自愈→list_playbooks。"
            "用户问什么就选对应模块的工具，不要一律查设备；一个问题涉及多个方面时连续调用多个工具。"
            "工具列表里没有的工具说明当前账号没有该权限，不要假装能查，也不要把没有权限说成系统故障。"
            "只能依据工具返回的数据回答，不要编造状态、日志或历史。回答用简体中文，"
            "先给结论再给依据；涉及数量/时间/版本要写出具体值。"
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
                hint["content"] = "用户当前没有选定设备；问题涉及设备时先调用 list_devices 确定设备，"
                                  "涉及平台/账号/任务/证书/费用时用对应模块的工具。";
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
            json extra;
            extra["error"] = err;
            extra["actions"] = actions;
            extra["tools"] = used;
            save_turn(std::string(), extra);
            resp["session_id"] = sid;
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
        {
            json extra;
            json meta;
            if (rounds > 0) meta["rounds"] = rounds;
            if (usage_c > 0) meta["usage"] = usage_c;
            if (!meta.empty()) {
                std::string ms;
                if (meta.contains("rounds")) ms += std::to_string(rounds) + " 轮";
                if (meta.contains("usage"))
                    ms += (ms.empty() ? "" : " · ") + std::to_string(usage_c) + " tokens";
                extra["meta"] = ms;
            }
            if (!actions.empty()) extra["actions"] = actions;
            if (!used.empty()) extra["tools"] = used;
            save_turn(reply, extra);
        }
        resp["session_id"] = sid;
        aiAudit(audit_, "ai:assistant|" + actor.label, "ai.chat", "device", devid, "ok",
                "tools=" + std::to_string(used.size()) + " actions=" + std::to_string(actions.size()) +
                    " rounds=" + std::to_string(rounds) +
                    (devid.empty() ? "" : " devid=" + devid), std::string());
        ctx->send(resp.dump(-1, ' ', false, json::error_handler_t::replace));
      } catch (const std::exception& e) {
        logf_("[ai] chat exception: %s", e.what());
        json resp;
        resp["error"] = std::string("处理失败: ") + e.what();
        json extra;
        extra["error"] = resp["error"];
        save_turn(std::string(), extra);
        resp["session_id"] = sid;
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send(resp.dump(-1, ' ', false, json::error_handler_t::replace));
      } catch (...) {
        logf_("[ai] chat exception: unknown");
        json extra;
        extra["error"] = "处理失败";
        save_turn(std::string(), extra);
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"处理失败\",\"session_id\":" + std::to_string(sid) + "}");
      }
    }).detach();

    // 线程已接管响应；路由层返回 HTTP_STATUS_NEXT 表示不要收尾
}

// ---------------------------------------------------------------- 连通性测试

// 用当前配置发一次最小请求：不写业务数据、不带工具，只回答“配置是否能用”。
// 运维最容易出问题的是 endpoint 多了/少了 /v1、模型名写错、密钥没权限，
// 这里把真实错误原文回给页面，比让人猜日志快。
void AdminServer::handleAiTest(const HttpContextPtr& ctx) {
    std::string subject = aiSubject(currentActor());
    AiProfile prof = loadAiProfile(db_, cfg_.db_path, subject);
    if (!prof.ready()) {
        apiError(ctx, HTTP_STATUS_SERVICE_UNAVAILABLE, prof.missing());
        return;
    }
    std::string endpoint = prof.endpoint;
    std::string model = prof.model;
    std::string key = prof.key;
    bool insecure = prof.insecure;
    int timeout_s = 90;
    std::thread([this, ctx, endpoint, model, key, insecure, timeout_s, prof]() {
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
        resp["provider"] = prof.provider;
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

// ---------------------------------------------------------------- 会话记录
//
// 会话永久保留：这里没有任何清理逻辑，只有用户主动删除。
// 列表默认最近 10 个（updated_ms 倒序），传 before_ms 继续往前翻。

void AdminServer::handleAiSessions(const HttpContextPtr& ctx) {
    std::string subject = aiSubject(currentActor());
    if (subject.empty()) {
        apiError(ctx, HTTP_STATUS_UNAUTHORIZED, "请先登录管理台");
        return;
    }
    int limit = 10;
    long long before_ms = 0, before_id = 0;
    for (const auto& kv : ctx->request->query_params) {
        if (kv.first == "limit") limit = atoi(kv.second.c_str());
        else if (kv.first == "before") before_ms = atoll(kv.second.c_str());
        else if (kv.first == "before_id") before_id = atoll(kv.second.c_str());
    }
    if (limit < 1) limit = 10;
    if (limit > 100) limit = 100;
    std::string out;
    if (!db_.aiSessionList(subject, limit, before_ms, before_id, out)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, "db error");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleAiSessionCreate(const HttpContextPtr& ctx) {
    std::string subject = aiSubject(currentActor());
    if (subject.empty()) {
        apiError(ctx, HTTP_STATUS_UNAUTHORIZED, "请先登录管理台");
        return;
    }
    json req = json::parse(ctx->body(), nullptr, false);
    std::string title = req.is_object() ? req.value("title", std::string()) : std::string();
    long long id = 0;
    std::string err;
    if (!db_.aiSessionCreate(subject, title, id, err)) {
        apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err.empty() ? "db error" : err);
        return;
    }
    json resp;
    resp["id"] = id;
    resp["title"] = title;
    resp["msg_count"] = 0;
    resp["created_ms"] = AdminDb::nowMs();
    ctx->send(resp.dump());
}

void AdminServer::handleAiSessionGet(const HttpContextPtr& ctx) {
    std::string subject = aiSubject(currentActor());
    if (subject.empty()) {
        apiError(ctx, HTTP_STATUS_UNAUTHORIZED, "请先登录管理台");
        return;
    }
    long long id = atoll(ctx->param("id").c_str());
    if (id <= 0) {
        apiBad(ctx, "会话 id 不合法");
        return;
    }
    std::string out;
    if (!db_.aiSessionGet(id, subject, out)) {
        apiNotFound(ctx, "会话不存在");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleAiSessionRename(const HttpContextPtr& ctx) {
    std::string subject = aiSubject(currentActor());
    if (subject.empty()) {
        apiError(ctx, HTTP_STATUS_UNAUTHORIZED, "请先登录管理台");
        return;
    }
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    std::string title = req.is_object() ? trimStr(req.value("title", std::string())) : std::string();
    if (id <= 0 || title.empty()) {
        apiBad(ctx, "需要会话 id 和非空标题");
        return;
    }
    std::string err;
    if (!db_.aiSessionRename(id, subject, title, err)) {
        if (err == "会话不存在")
            apiNotFound(ctx, err);
        else
            apiBad(ctx, err.empty() ? "db error" : err);
        return;
    }
    json resp;
    resp["id"] = id;
    resp["title"] = title.size() > 80 ? title.substr(0, 80) : title;
    ctx->send(resp.dump());
}

void AdminServer::handleAiSessionDelete(const HttpContextPtr& ctx) {
    std::string subject = aiSubject(currentActor());
    if (subject.empty()) {
        apiError(ctx, HTTP_STATUS_UNAUTHORIZED, "请先登录管理台");
        return;
    }
    long long id = atoll(ctx->param("id").c_str());
    if (id <= 0) {
        apiBad(ctx, "会话 id 不合法");
        return;
    }
    std::string err;
    if (!db_.aiSessionDelete(id, subject, err)) {
        if (err == "会话不存在")
            apiNotFound(ctx, err);
        else
            apiError(ctx, HTTP_STATUS_INTERNAL_SERVER_ERROR, err.empty() ? "db error" : err);
        return;
    }
    auditEx("ai.session.delete", "ai", subject, "ok", "删除会话 " + std::to_string(id));
    ctx->send("{\"ok\":true}");
}
