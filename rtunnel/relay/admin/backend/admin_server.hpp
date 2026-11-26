#pragma once
// relay 管理台后端：HttpServer 用多个 worker 线程跑 HTTP（--admin-threads），
// 请求级身份放 thread_local tl_actor；所有 relay 状态访问经 hloop_post_event
// 投递回 relay 线程执行（promise/future 同步），relay 状态因此永远只在 relay
// 线程被访问，无锁。数据面（/device 代理、终端/文件 WS）另有独立线程，不占
// 管理面 worker。角色：admin（唯一）/ manager / operator / viewer，权限见 api_users.cpp。

#include <functional>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <deque>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <unordered_map>

#include "http/server/HttpServer.h"
#include "http/server/HttpContext.h"
#include "http/server/WebSocketServer.h"

#include "relay/relay_server.hpp"
#include "relay/audit_log.hpp"
#include "admin_db.hpp"
#include "event_bus.hpp"

struct TerminalSession;

struct AdminConfig {
    std::string addr;        // 形如 127.0.0.1:9443
    std::string token;       // 仅首次初始化使用的 token
    std::string docroot;     // 前端磁盘目录（开发用；为空时用内嵌前端，普通构建没有内嵌）
    std::string ca_cert;     // 签发用 CA 证书
    std::string ca_key;      // 签发用 CA 私钥
    std::string cert_dir;    // 证书台账目录（index.txt/revoked.txt/audit.log）
    std::string file_dir;    // 文件管理目录（--admin-file-dir）
    // Web 终端 / 文件 / 服务代理：admin 作为 RTNP client 直连本机 relay。
    // 身份由 relay 启动时生成的进程内密钥认证（auth_type=3），不落盘、不配置。
    std::string proxy_secret;
    int         shell_relay_port = 8443; // direct 模式连接的 relay 端口（本机）
    std::string db_path = "rtunnel-admin.db"; // SQLite 账号库
    // 管理台 TLS：显式证书或自签；都为空时保持明文 HTTP
    std::string tls_cert;                // --admin-tls-cert
    std::string tls_key;                 // --admin-tls-key
    bool        tls_selfsigned = false;  // --admin-tls-selfsigned
    std::string tls_dir;                 // --admin-tls-dir（默认 cert_dir/admin-tls）
    int         tls_days = 3650;         // --admin-tls-days
    int         threads = 4;             // --admin-threads（HTTP worker 数，1..32）
};

struct AdminActor {
    long long   id = 0;          // 0 = bootstrap token
    std::string username;
    std::string role;            // superadmin / admin / user / token-role
    std::string ip;              // 请求来源地址，审计用
    long long   token_id = 0;    // >0 表示本次请求由 atk- Token 鉴权
    bool        super = false;   // 超级权限组：跳过资源校验
    std::set<std::string> perms; // 有效资源键（权限组并集）
    bool        is_client = false;   // 本次请求来自 rtk_ 接入账号 Token
    long long   client_id = 0;       // 接入账号 id（client_accounts.id）
    long long   tenant_id = 0;       // 接入账号归属租户（0 = 运营方/控制台账号）
    bool        all_devices = false; // 有效授权包含全部设备
    bool        global_scope = false; // 有效授权包含全局权限（仓库/设置/审计等跨设备资源）
    bool        token_scoped = false;// 令牌自带权限组（设备范围需按令牌收窄）
    std::set<std::string> devices;   // 有效设备列表（all_devices 时为空）
    std::string ports;               // 有效端口范围（组级并集，空 = 无端口）
    std::string ai_actor;            // X-Actor: ai:...（模型调用的标记，只影响审计标签）
};

// AI 对话的权限快照：对话跑在独立线程，tl_actor 在那里是空的
struct AiActor {
    bool is_client = false;
    bool super = false;
    bool all_devices = false;
    long long tenant_id = 0;
    std::set<std::string> devices;
    std::set<std::string> perms;
    std::string label;
};

// 一条待发告警：由采集线程入队，通知线程发送
struct AlertEvent {
    std::string key;        // 去重键（同键在冷却期内只发一次）
    std::string event;      // device.offline / device.online / cert.expiring / task.failed / test
    std::string level;      // info / warning / critical
    std::string title;
    std::string text;
    std::string details;    // JSON 对象字符串
    int         cooldown_s = 0;   // 0 = 用全局 alert.cooldown_s
};

// 关闭所有进行中的 /device 代理线程并等待退出（api_proxy.cpp）
void adminProxyShutdown();

class AdminServer {
public:
    explicit AdminServer(RelayServer* relay) : relay_(relay), server_(&service_) {}

    int  start(const AdminConfig& cfg);
    void stop();

    // 供 terminal.cpp 等协作模块使用
    const AdminConfig& config() const { return cfg_; }
    hv::HttpServer&    httpServer() { return server_; }
    void auditPublic(const std::string& role, const std::string& action, const std::string& detail) {
        audit(role, action, detail);
    }
    void initTerminal();

private:
    std::string callRelay(std::function<std::string()> fn);
    std::string roleOf(const HttpContextPtr& ctx);          // "" = 未授权
    void        loadActorPerms();
    // 资源鉴权：need 为 perm_resources.key（"auth" 表示只要求登录）
    int         requirePerm(const HttpContextPtr& ctx, const std::string& need, std::string& role);
    bool        hasPerm(const std::string& key) const;
    AdminActor  currentActor() const;
    std::vector<std::string> myDevids();
    bool        ownsDevid(const std::string& devid);
    bool        clientGroupDevids(std::vector<std::string>& out);
    bool        scopeRestricted();
    bool        tokenScoped();   // 仅当本次请求由绑定了权限组的 atk Token 发起
    // 给予边界：返回空串表示允许，否则是拒绝原因。
    // want 描述要授予的资源/设备/端口；一律要求是本操作者有效权限的子集。
    std::string grantDenied(const DbPermScope& want) const;
    // 一组权限组是否都能被当前操作者授予（组的 super 与授权面都要在自己范围内）
    std::string grantGroupsDenied(const std::vector<long long>& ids);
    // 能否操作这个主体：目标当前有效权限必须是自己权限的子集（防止横向降权/越权改组）
    std::string manageDenied(const DbPermScope& target) const;
    // 接入账号是否在自己可管理的范围内（不在返回原因，空串表示可以）
    std::string accountDenied(long long account_id);
    // 接入令牌只能操作本租户的接入账号（控制台账号不受限）
    std::string accountTenantDenied(long long account_id);
    // 授权对象（client/rtk）同样受租户约束；user/atk 不对接入令牌开放
    std::string subjectTenantDenied(const std::string& type, const std::string& id);
    // 主体（user/atk/client/rtk）的当前有效范围；主体不存在返回 false
    bool        subjectScope(const std::string& type, const std::string& id, DbPermScope& out);
    // 主体换组后按类型断开已收窄的会话
    void        kickSubjectShrunk(const std::string& type, const std::string& id,
                                  const DbPermScope& before, const char* reason);
    // atk Token 授权范围内的设备；未绑定权限组时返回 false
    bool        tokenGroupDevids(std::vector<std::string>& out);
    // WebSocket 握手鉴权（浏览器自动带 Cookie）
    bool        wsAuthorize(const HttpRequestPtr& req, AdminActor& actor, std::string& role);
    void        audit(const std::string& role, const std::string& action, const std::string& detail);
    // 结构化审计：target/result/op_id 进 JSONL，AI 与外部系统按字段聚合
    void        auditEx(const std::string& action, const std::string& target_type,
                        const std::string& target_id, const std::string& result,
                        const std::string& detail, const std::string& op_id = std::string());
    // 后台线程没有请求上下文，用打开时抓取的主体串写审计
    void        auditAs(const std::string& who, const std::string& action, const std::string& detail) {
        audit_.write(who, action, detail);
    }
    std::string actorLabel() const;
    // 发布一条事件（线程安全）：写审计的地方顺手发，AI 与页面都从这里订阅
    void emitEvent(const std::string& type, const std::string& data_json);
    // 撤权联动：关闭接入账号的在线会话（token_id>0 时只关该令牌）。
    // 令牌吊销/轮换、账号停用/删除、权限组变更都会调用，避免旧会话继续用已撤销的权限。
    int         kickClients(const std::vector<std::string>& users, long long token_id, const char* reason);
    void        kickClientsById(long long account_id, const char* reason);
    // 管理台 WS（终端/串口/文件/服务/设备代理）主体登记：撤权、停用、吊销后主动断开
    void        registerConsoleWs(const WebSocketChannelPtr& ws, const AdminActor& a, const char* kind);
    void        unregisterConsoleWs(const hv::WebSocketChannel* ws);
    int         kickConsoleWsOfUser(long long user_id, const char* reason);
    int         kickConsoleWsOfToken(long long token_id, const char* reason);
    // 权限组变更前后对比：只断开有效权限变小的管理账号
    std::map<long long, DbPermScope> snapshotUserScopes();
    void        kickConsoleWsShrunk(const std::map<long long, DbPermScope>& before, const char* reason);
    void        refreshGroupAcl();
    void        refreshTokenCache();
    void        registerRoutes();
    // 前端来源：磁盘目录与内嵌资源可以分别尝试。/ 先后试 admin.html 与 index.html，
    // 必须先两个都查完磁盘再查内嵌，否则内嵌的 admin.html 会盖掉磁盘的 index.html。
    enum AdminWebSource { kAdminWebBoth = 0, kAdminWebDiskOnly, kAdminWebEmbeddedOnly };
    int         serveFrontend(const HttpContextPtr& ctx, const char* name, const char* ctype,
                              int source = kAdminWebBoth);

    // handlers（api_*.cpp）
    void handleOverview(const HttpContextPtr& ctx);
    void handleAgents(const HttpContextPtr& ctx);
    void handleAgentDetail(const HttpContextPtr& ctx);
    int  handleAgentSysinfo(const HttpContextPtr& ctx); // 返回 HTTP_STATUS_NEXT 表示异步回包
    int  handleAgentNet(const HttpContextPtr& ctx);     // 网络/IPC 快照，返回 HTTP_STATUS_NEXT
    int  handleAgentStorage(const HttpContextPtr& ctx); // 分区目录占用，返回 HTTP_STATUS_NEXT
    int  handleAgentPower(const HttpContextPtr& ctx);   // 重启/关机，返回 HTTP_STATUS_NEXT
    int  handleAgentDiag(const HttpContextPtr& ctx);    // 诊断包下载，返回 HTTP_STATUS_NEXT
    int  handleAgentProcs(const HttpContextPtr& ctx);   // 进程列表，返回 HTTP_STATUS_NEXT
    int  handleAgentProcDetail(const HttpContextPtr& ctx);
    int  handleAgentExec(const HttpContextPtr& ctx, const std::string& role);
    // 同步执行并等待单片结果（工作线程调用）；exec 接口与 operations 共用
    bool execOnDevice(const std::string& devid, const std::string& payload, int timeout_ms,
                      nlohmann::json& out, std::string& err);
    void handleOpsPlan(const HttpContextPtr& ctx, const std::string& role);
    void handleOpsApply(const HttpContextPtr& ctx, const std::string& role);
    void handleOpsVerify(const HttpContextPtr& ctx, const std::string& role);
    void handlePlaybooksList(const HttpContextPtr& ctx);
    void handlePlaybookGet(const HttpContextPtr& ctx);
    void handlePlaybookSave(const HttpContextPtr& ctx, const std::string& role);
    void handlePlaybookDelete(const HttpContextPtr& ctx, const std::string& role);
    void handlePlaybookEnable(const HttpContextPtr& ctx, const std::string& role);
    void handlePlaybookRun(const HttpContextPtr& ctx, const std::string& role);
    void handlePlaybookFires(const HttpContextPtr& ctx);
    // AI 助手：配置、密钥、对话
    void handleAiConfigGet(const HttpContextPtr& ctx);
    void handleAiConfigSet(const HttpContextPtr& ctx, const std::string& role);
    void handleAiKeySet(const HttpContextPtr& ctx, const std::string& role);
    void handleAiKeyClear(const HttpContextPtr& ctx, const std::string& role);
    void handleAiChat(const HttpContextPtr& ctx, const std::string& role);
    void handleAiTest(const HttpContextPtr& ctx);
    // 会话记录：按账号隔离，永久保留；列表默认最近 10 个，可翻页
    void handleAiSessions(const HttpContextPtr& ctx);
    void handleAiSessionCreate(const HttpContextPtr& ctx);
    void handleAiSessionGet(const HttpContextPtr& ctx);
    void handleAiSessionRename(const HttpContextPtr& ctx);
    void handleAiSessionDelete(const HttpContextPtr& ctx);
    void handleOpsApprove(const HttpContextPtr& ctx, const std::string& role);
    void handleOpsList(const HttpContextPtr& ctx);
    int  handleAgentProcSignal(const HttpContextPtr& ctx);
    int  proxyAgentProc(const HttpContextPtr& ctx, const std::string& devid, const nlohmann::json& req);
    void handleTtyList(const HttpContextPtr& ctx);
    void handleTtyPut(const HttpContextPtr& ctx);
    void handleTtyDelete(const HttpContextPtr& ctx);
    void handleTtyDiscovered(const HttpContextPtr& ctx);
    void handleMetricSeriesCfgGet(const HttpContextPtr& ctx);
    void handleMetricSeriesCfgPut(const HttpContextPtr& ctx);
    void handleMetricSeriesData(const HttpContextPtr& ctx);
    bool pushMetricSeriesConfig(const std::string& devid);
    void handleAgentCaps(const HttpContextPtr& ctx, const AdminActor& actor);
    // api_dist.cpp
    void handleDistCreate(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleDistList(const HttpContextPtr& ctx);
    void handleDistSummary(const HttpContextPtr& ctx);
    void handleDistGet(const HttpContextPtr& ctx);
    void handleDistRetry(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleDistCancel(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleDistExtend(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleDistVerify(const HttpContextPtr& ctx, const AdminActor& actor);
    void distLoop();
    void distTick();
    // api_relay.cpp：中继地址字典、有序下发列表、同步任务
    void handleRelayDictList(const HttpContextPtr& ctx);
    void handleRelayDictAdd(const HttpContextPtr& ctx);
    void handleRelayDictBatchAdd(const HttpContextPtr& ctx);
    void handleRelayDictUpdate(const HttpContextPtr& ctx);
    void handleRelayDictDelete(const HttpContextPtr& ctx);
    void handleRelayListGet(const HttpContextPtr& ctx);
    void handleRelayListSet(const HttpContextPtr& ctx);
    void handleRelayListPush(const HttpContextPtr& ctx);
    void handleRelaySyncCreate(const HttpContextPtr& ctx);
    void createRelaySyncTask(const HttpContextPtr& ctx, const nlohmann::json& req);
    void handleRelaySyncList(const HttpContextPtr& ctx);
    void handleRelaySyncGet(const HttpContextPtr& ctx);
    void handleRelaySyncAction(const HttpContextPtr& ctx, const std::string& action);
    bool relayListSpec(std::string& servers);   // DB 有序列表 → "h:p,..."（启动与保存后应用）
    void relaySyncLoop();
    void relaySyncTick();
    // api_deploy.cpp：通过 SSH/SCP 向目标主机装 agent/relay/client
    void handleDeployHosts(const HttpContextPtr& ctx);
    void handleDeployHostCreate(const HttpContextPtr& ctx);
    void handleDeployHostUpdate(const HttpContextPtr& ctx);
    void handleDeployHostDelete(const HttpContextPtr& ctx);
    void handleDeployProbe(const HttpContextPtr& ctx);
    void handleDeployScan(const HttpContextPtr& ctx);
    void handleDeployPackages(const HttpContextPtr& ctx);
    void handleDeployJobCreate(const HttpContextPtr& ctx);
    void handleDeployJobs(const HttpContextPtr& ctx);
    void handleDeployJobGet(const HttpContextPtr& ctx);
    void handleDeployJobCancel(const HttpContextPtr& ctx);
    void deployRunJob(long long job_id);          // 后台 worker
    // api_settings.cpp
    void loadSettings();
    // 写库 + 同步内存缓存：所有运行期改设置的地方都走它，否则 settingStr 读不到新值
    bool setSettingCached(const std::string& key, const std::string& val, const std::string& by);
    int  settingInt(const std::string& key, int def);
    std::string settingStr(const std::string& key, const std::string& def);
    void handleSettingsGet(const HttpContextPtr& ctx);
    void handleSettingsSet(const HttpContextPtr& ctx);
    // api_alerts.cpp + alert_notifier.cpp
    void alertLoop();
    void alertEnqueue(const AlertEvent& ev);
    // 自愈：告警匹配 → 生成 operation →（可选）自动执行 → verify
    void playbookOnAlert(const AlertEvent& ev);
    void playbookWorker();
    bool playbookEnqueueOp(long long op_id);
    void playbookRunOp(long long op_id);
    // AI 工具执行：按请求线程快照的权限判定，不读 tl_actor。
    // 读工具直接查；写工具只“提议”，落一个 operation 等人确认。
    bool aiRunTool(const AiActor& a, const std::string& name, const nlohmann::json& args,
                   std::string& out, std::string& err);
    // 请求线程里抓一份身份快照（对话线程里 tl_actor 为空，不能直接读）
    AiActor aiSnapshot();
    // 证书台账 / 文件仓库列表：分别给 /certs、/files 与 AI 工具共用
    std::string certsJson();
    std::string repoListJson(const std::string& dir, int limit);
    // 内部路径（AI/自愈）创建 exec 作业：与 /operations/plan 共用风险分级、
    // plan_hash 与审批阈值，避免两处各算一套。
    // 命令风险分级（1 只读 / 2 改状态 / 3 破坏性）：plan 与自愈规则草稿共用
    int  opsArgvRisk(const std::vector<std::string>& argv);
    bool opsCreateExecPlan(const std::string& actor, const std::string& actor_type,
                           const std::string& devid, const std::vector<std::string>& argv,
                           const std::string& cwd, int timeout_ms, long long max_bytes,
                           const std::string& idem, long long& out_id, std::string& out_json,
                           std::string& err);
    void alertCheckDevices(long long now_ms);
    void alertCheckCerts(long long now_ms);
    bool alertSend(const AlertEvent& ev);
    std::string alertStatusJson();
    void handleAlertStatus(const HttpContextPtr& ctx);
    void handleAlertTest(const HttpContextPtr& ctx);
    void handleProfileGet(const HttpContextPtr& ctx);
    void handleProfileSet(const HttpContextPtr& ctx);
    void handleLogoutAll(const HttpContextPtr& ctx);
    void cleanupAudit(int days);
    void cleanupUploads();
    void handleClients(const HttpContextPtr& ctx);
    void handleStreams(const HttpContextPtr& ctx);
    // api_ops.cpp
    void handleKickStream(const HttpContextPtr& ctx, const std::string& role);
    void handleKickAgent(const HttpContextPtr& ctx, const std::string& role);
    // api_groups.cpp
    void handlePermissionDicts(const HttpContextPtr& ctx);
    void handlePermResources(const HttpContextPtr& ctx);
    void handlePermGroupDetail(const HttpContextPtr& ctx);
    void handlePermSubjects(const HttpContextPtr& ctx);
    void handleResDeviceUpdate(const HttpContextPtr& ctx, const std::string& role);
    void handleSubjectGroups(const HttpContextPtr& ctx);
    void handleSubjectGroupsSave(const HttpContextPtr& ctx, const std::string& role);
    void handlePermResourceUpdate(const HttpContextPtr& ctx, const std::string& role);
    void handlePermGroups(const HttpContextPtr& ctx);
    void handlePermGroupSave(const HttpContextPtr& ctx, const std::string& role);
    void handlePermGroupDelete(const HttpContextPtr& ctx, const std::string& role);
    void handlePermGroupMembers(const HttpContextPtr& ctx);
    void handlePermGroupMembersSave(const HttpContextPtr& ctx, const std::string& role);
    void handleClientPermGroups(const HttpContextPtr& ctx);
    void handleClientPermGroupsSave(const HttpContextPtr& ctx, const std::string& role);
    void handleUserPermGroups(const HttpContextPtr& ctx);
    void handleUserPermGroupsSave(const HttpContextPtr& ctx, const std::string& role);
    void handleDictPorts(const HttpContextPtr& ctx);
    void handleDictPortAdd(const HttpContextPtr& ctx, const std::string& role);
    void handleDictTtys(const HttpContextPtr& ctx);
    void handleDictTtyAdd(const HttpContextPtr& ctx, const std::string& role);
    void handleDictBatchAdd(const HttpContextPtr& ctx, const std::string& role, const std::string& kind);
    void handleDictUpdate(const HttpContextPtr& ctx, const std::string& role, const std::string& kind);
    void handleDictDelete(const HttpContextPtr& ctx, const std::string& role, const std::string& kind);
    void handleDictBatchDelete(const HttpContextPtr& ctx, const std::string& role, const std::string& kind);




    void handleAudit(const HttpContextPtr& ctx);
    // api_usage.cpp：用量流水日聚合查询
    void handleEvents(const HttpContextPtr& ctx);          // SSE 事件流
    void handleMetricsHistory(const HttpContextPtr& ctx);  // 分钟级指标
    static void sseTickCb(htimer_t* t);
    void handleUsage(const HttpContextPtr& ctx);
    void handleTenants(const HttpContextPtr& ctx);
    void handleTenantCreate(const HttpContextPtr& ctx, const std::string& role);
    void handleTenantUpdate(const HttpContextPtr& ctx, const std::string& role);
    void handleBillingPrices(const HttpContextPtr& ctx);
    void handleBillingPriceSet(const HttpContextPtr& ctx, const std::string& role);
    void handleBillingSummary(const HttpContextPtr& ctx);
    void handleBillingStatementGenerate(const HttpContextPtr& ctx, const std::string& role);
    void handleBillingStatements(const HttpContextPtr& ctx);
    void handleBillingStatementGet(const HttpContextPtr& ctx);
    void handleBillingStatementStatus(const HttpContextPtr& ctx, const std::string& role);
    void handleBillingStatementCsv(const HttpContextPtr& ctx);
    // 结算单存在且在当前身份可见（接入令牌限本租户）
    bool billingStatementVisible(long long id, const HttpContextPtr& ctx);
    // api_certs.cpp
    void handleCerts(const HttpContextPtr& ctx);
    void handleCertIssue(const HttpContextPtr& ctx, const std::string& role);
    void handleCertRevoke(const HttpContextPtr& ctx, const std::string& role);
    void handleCertSign(const HttpContextPtr& ctx, const std::string& role);
    void handleShutdown(const HttpContextPtr& ctx, const std::string& role);
    void handleMetrics(const HttpContextPtr& ctx);
    // api_users.cpp
    void handleSetupStatus(const HttpContextPtr& ctx);
    void handleSetup(const HttpContextPtr& ctx);
    void handleLogin(const HttpContextPtr& ctx);
    void handleLogout(const HttpContextPtr& ctx);
    void handleMe(const HttpContextPtr& ctx);
    void handleUsers(const HttpContextPtr& ctx);
    void handleUserCreate(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleUserPassword(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleUserStatus(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleUserRole(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleUserDelete(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleClientAccounts(const HttpContextPtr& ctx);
    void handleClientAccountGet(const HttpContextPtr& ctx);
    void handleClientAccountCreate(const HttpContextPtr& ctx, const std::string& role);
    void handleClientAccountUpdate(const HttpContextPtr& ctx, const std::string& role);
    void handleClientAccountDelete(const HttpContextPtr& ctx, const std::string& role);
    void handleClientAccountKick(const HttpContextPtr& ctx, const std::string& role);
    void handleClientAccountEffective(const HttpContextPtr& ctx);
    void handleClientTokens(const HttpContextPtr& ctx);
    void handleClientTokenCreate(const HttpContextPtr& ctx, const std::string& role);
    void handleClientTokenUpdate(const HttpContextPtr& ctx, const std::string& role);
    void handleClientTokenRotate(const HttpContextPtr& ctx, const std::string& role);
    void handleClientTokenDelete(const HttpContextPtr& ctx, const std::string& role);
    // 管理台账号 API Token（api_admin_tokens.cpp）
    void handleAdminTokens(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleAdminTokenCreate(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleAdminTokenUpdate(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleAdminTokenRotate(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleAdminTokenDelete(const HttpContextPtr& ctx, const AdminActor& actor);
    void handleDeviceList(const HttpContextPtr& ctx);
    // api_proxy.cpp
    int  handleDeviceProxy(const HttpContextPtr& ctx);
    void handleDeviceWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
    bool handleTtyWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
    bool handleTtyWsMessage(const WebSocketChannelPtr& channel, const std::string& msg);
    bool handleTtyWsClose(const WebSocketChannelPtr& channel);
    // api_fs_ws.cpp：文件管理 / 服务管理共用的设备内建通道桥
    bool handleFsWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
    bool handleSvcWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
    bool handleAgentWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req,
                           uint16_t flags, const std::string& prefix, const char* tag);
    bool handleAgentWsMessage(const WebSocketChannelPtr& channel, const std::string& msg);
    bool handleAgentWsClose(const WebSocketChannelPtr& channel);
    void handleTtyLogDownload(const HttpContextPtr& ctx);
    void cleanupTtyLogs();
    bool handleDeviceWsMessage(const WebSocketChannelPtr& channel, const std::string& msg);
    bool handleDeviceWsClose(const WebSocketChannelPtr& channel);
    // api_files.cpp
    void handleFiles(const HttpContextPtr& ctx);
    void handleFileDownload(const HttpContextPtr& ctx);
    // forced_dir 非空时忽略请求里的 dir（部署包固定落 deploy/）
    void handleFileUpload(const HttpContextPtr& ctx, const std::string& role,
                          const std::string& forced_dir = std::string());
    void handleFileMkdir(const HttpContextPtr& ctx, const std::string& role);
    void handleFileMove(const HttpContextPtr& ctx, const std::string& role);
    void handleFilePut(const HttpContextPtr& ctx, const std::string& role);
    void handleUploadStatus(const HttpContextPtr& ctx);
    void handleFileDelete(const HttpContextPtr& ctx, const std::string& role);

    RelayServer*    relay_;
    hv::HttpService service_;
    hv::HttpServer  server_;
    AdminConfig     cfg_;
    AdminDb          db_;
    AuditLog        audit_;
    EventBus        bus_;        // SSE 事件流：审计/设备上下线/会话/任务/告警
    std::map<std::string, std::string> settings_;
    std::mutex settings_mu_;
    std::map<long long, uint32_t> dist_active;   // task id -> relay tid
    std::set<std::string> dist_busy_dev;
    std::atomic<int>      dist_active_count_{0};  // 仅供 /metrics 近似读取（每秒更新）

    // 后台 worker：统一受 stop() 管理，避免 detached 线程在析构后访问 this
    std::atomic<bool>       stopping_{false};
    std::vector<std::thread> workers_;
    std::mutex              worker_mu_;
    std::condition_variable worker_cv_;
    void spawnWorker(std::function<void()> fn);
    bool waitStop(int ms);       // 等待 ms；stop 请求时立即返回 true
    void joinWorkers();

    // 管理面 HTTP 统计（pre/postprocessor 写入，/metrics 读取）
    std::atomic<uint64_t> http_req_total_{0};
    std::atomic<uint64_t> http_req_2xx_{0};
    std::atomic<uint64_t> http_req_4xx_{0};
    std::atomic<uint64_t> http_req_5xx_{0};
    std::atomic<uint64_t> http_us_total_{0};
    std::atomic<uint64_t> http_slow_total_{0};
    std::atomic<int64_t>  http_inflight_{0};

    // Web 终端
    hv::WebSocketService ws_service_;
    std::map<hv::WebSocketChannel*, std::shared_ptr<TerminalSession>> terms_;
    std::mutex terms_mu_;          // 多个 HTTP worker 会并发进出 WS open/message/close
    // 管理台 WS 主体（所有 WS 入口共用，key 为 channel 指针，value 里存 weak 避免续命）
    struct ConsoleWsEntry {
        std::weak_ptr<hv::WebSocketChannel> ws;
        long long   user_id = 0;
        long long   token_id = 0;
        std::string username;
        std::string kind;          // terminal / tty / fs / svc / proxy
        long long   opened_ms = 0;
    };
    std::map<const hv::WebSocketChannel*, ConsoleWsEntry> console_ws_;
    std::mutex console_ws_mu_;
    void onWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
    void onWsMessage(const WebSocketChannelPtr& channel, const std::string& msg);
    void onWsClose(const WebSocketChannelPtr& channel);

public:
    // 分发任务失败/超时（dist_scheduler 调用，随时可能来自 worker 线程）
    void notifyTaskFailed(long long id, const std::string& devid,
                          const std::string& status, const std::string& err,
                          const std::string& kind = "file");

private:
    // 告警状态（notifier 线程 + 采集共用，全部走 alert_mu_）
    // 自愈规则：告警线程匹配后入队，单个 worker 串行执行
    std::mutex                      playbook_mu_;
    std::deque<long long>           playbook_jobs_;
    std::map<long long, long long>  playbook_last_fire_ms_;
    uint64_t                        playbook_dropped_ = 0;
    std::deque<AlertEvent>          alert_q_;
    std::mutex                      alert_mu_;
    std::map<std::string, long long> alert_last_sent_;    // key -> ms
    std::map<std::string, long long> alert_agent_seen_;   // devid -> 最后在线 ms
    std::map<std::string, int>       alert_agent_online_; // devid -> 1 在线
    std::map<std::string, int>       alert_cert_level_;   // san -> 已发级别
    long long alert_last_ok_ms_ = 0;
    long long alert_last_try_ms_ = 0;
    long long alert_sent_total_ = 0;
    std::string alert_last_error_;
    bool alert_baseline_done_ = false;
    std::string admin_cert_path_;   // 管理台 TLS 证书（自签时为生成路径）
};

// 角色权重：viewer=1 operator=2 admin=3

