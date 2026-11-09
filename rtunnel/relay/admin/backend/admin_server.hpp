#pragma once
// relay 管理台后端：HttpServer 独立 worker 线程跑 HTTP，
// 所有 relay 状态访问经 hloop_post_event 投递回 relay 线程执行
// （promise/future 同步），relay 状态因此永远只在 relay 线程被访问，无锁。
// 角色：admin（唯一）/ manager / operator / viewer，权限见 api_users.cpp。

#include <functional>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <unordered_map>

#include "http/server/HttpServer.h"
#include "http/server/HttpContext.h"
#include "http/server/WebSocketServer.h"

#include "relay/relay_server.hpp"
#include "relay/audit_log.hpp"
#include "admin_db.hpp"

struct TerminalSession;

struct AdminConfig {
    std::string addr;        // 形如 127.0.0.1:9443
    std::string token;       // 仅首次初始化使用的 token
    std::string docroot;     // 前端磁盘目录（调试用，空则内嵌）
    std::string ca_cert;     // 签发用 CA 证书
    std::string ca_key;      // 签发用 CA 私钥
    std::string cert_dir;    // 证书台账目录（index.txt/revoked.txt/audit.log）
    std::string file_dir;    // 文件管理目录（--admin-file-dir）
    // Web 终端（direct 免密）：admin 作为 RTNP client 直连本机 relay 发 OPEN_SHELL
    std::string shell_user = "devuser";  // direct 模式登录 relay 的用户（须被 ACL 授权）
    std::string shell_token;             // 该用户连 relay 的凭证（--admin-shell-token）
    int         shell_relay_port = 8443; // direct 模式连接的 relay 端口（本机）
    std::string db_path = "rtunnel-admin.db"; // SQLite 账号库
};

struct AdminActor {
    long long   id = 0;          // 0 = bootstrap token
    std::string username;
    std::string role;            // superadmin / admin / user / token-role
};

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
    int         requireRole(const HttpContextPtr& ctx, int need, std::string& role);
    AdminActor  currentActor() const;
    std::vector<std::string> myDevids();
    bool        ownsDevid(const std::string& devid);
    bool        scopeRestricted();
    // WebSocket 握手鉴权（浏览器自动带 Cookie）
    bool        wsAuthorize(const HttpRequestPtr& req, AdminActor& actor, std::string& role);
    void        audit(const std::string& role, const std::string& action, const std::string& detail);
    void        refreshGroupAcl();
    void        registerRoutes();
    int         serveFrontend(const HttpContextPtr& ctx, const char* name, const char* ctype);

    // handlers（api_*.cpp）
    void handleOverview(const HttpContextPtr& ctx);
    void handleAgents(const HttpContextPtr& ctx);
    void handleAgentDetail(const HttpContextPtr& ctx);
    void handleAgentSysinfo(const HttpContextPtr& ctx);
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
    // api_settings.cpp
    void loadSettings();
    int  settingInt(const std::string& key, int def);
    void handleSettingsGet(const HttpContextPtr& ctx);
    void handleSettingsSet(const HttpContextPtr& ctx);
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
    void handleGroups(const HttpContextPtr& ctx);
    void handleGroupCreate(const HttpContextPtr& ctx, const std::string& role);
    void handleGroupGet(const HttpContextPtr& ctx);
    void handleGroupUpdate(const HttpContextPtr& ctx, const std::string& role);
    void handleGroupDelete(const HttpContextPtr& ctx, const std::string& role);
    void handleGroupAddDevices(const HttpContextPtr& ctx, const std::string& role);
    void handleGroupRemoveDevice(const HttpContextPtr& ctx, const std::string& role);
    void handleGroupAddSubject(const HttpContextPtr& ctx, const std::string& role);
    void handleGroupRemoveSubject(const HttpContextPtr& ctx, const std::string& role);
    void handleGroupDeviceMap(const HttpContextPtr& ctx);
    void handleAccessEffective(const HttpContextPtr& ctx);    // api_permission.cpp
    void handlePermOverview(const HttpContextPtr& ctx);
    void handlePermClientGet(const HttpContextPtr& ctx);
    void handlePermClientSave(const HttpContextPtr& ctx, const std::string& role);
    void handlePermClientDelete(const HttpContextPtr& ctx, const std::string& role);
    void handlePermAgentGet(const HttpContextPtr& ctx);
    void handlePermAgentSave(const HttpContextPtr& ctx, const std::string& role);
    void handlePermAgentDelete(const HttpContextPtr& ctx, const std::string& role);
    void handlePermEffective(const HttpContextPtr& ctx);    void handlePermAgentCandidates(const HttpContextPtr& ctx);
    void handlePermAgentAddDevices(const HttpContextPtr& ctx, const std::string& role);


    void handleAclGet(const HttpContextPtr& ctx);
    void handleAclPut(const HttpContextPtr& ctx, const std::string& role);
    void handleAclReload(const HttpContextPtr& ctx, const std::string& role);
    void handleAudit(const HttpContextPtr& ctx);
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
    void handleDeviceList(const HttpContextPtr& ctx);
    void handleDeviceBind(const HttpContextPtr& ctx, const AdminActor& actor);
    // api_proxy.cpp
    void handleDeviceProxy(const HttpContextPtr& ctx);
    void handleDeviceWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
    bool handleTtyWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
    bool handleTtyWsMessage(const WebSocketChannelPtr& channel, const std::string& msg);
    bool handleTtyWsClose(const WebSocketChannelPtr& channel);
    void handleTtyLogDownload(const HttpContextPtr& ctx);
    void cleanupTtyLogs();
    bool handleDeviceWsMessage(const WebSocketChannelPtr& channel, const std::string& msg);
    bool handleDeviceWsClose(const WebSocketChannelPtr& channel);
    // api_files.cpp
    void handleFiles(const HttpContextPtr& ctx);
    void handleFileDownload(const HttpContextPtr& ctx);
    void handleFileUpload(const HttpContextPtr& ctx, const std::string& role);
    void handleUploadStatus(const HttpContextPtr& ctx);
    void handleFileDelete(const HttpContextPtr& ctx, const std::string& role);

    RelayServer*    relay_;
    hv::HttpService service_;
    hv::HttpServer  server_;
    AdminConfig     cfg_;
    AdminDb          db_;
    AuditLog        audit_;
    std::map<std::string, std::string> settings_;
    std::mutex settings_mu_;
    std::map<long long, uint32_t> dist_active;   // task id -> relay tid
    std::set<std::string> dist_busy_dev;

    // Web 终端
    hv::WebSocketService ws_service_;
    std::map<hv::WebSocketChannel*, std::shared_ptr<TerminalSession>> terms_;
    void onWsOpen(const WebSocketChannelPtr& channel, const HttpRequestPtr& req);
    void onWsMessage(const WebSocketChannelPtr& channel, const std::string& msg);
    void onWsClose(const WebSocketChannelPtr& channel);
};

// 角色权重：viewer=1 operator=2 admin=3
enum AdminRole { ROLE_VIEWER = 1, ROLE_OPERATOR = 2, ROLE_MANAGER = 3, ROLE_ADMIN = 4 };

