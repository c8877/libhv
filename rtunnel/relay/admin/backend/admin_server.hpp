#pragma once
// relay 管理台后端：HttpServer 独立 worker 线程跑 HTTP，
// 所有 relay 状态访问经 hloop_post_event 投递回 relay 线程执行
// （promise/future 同步），relay 状态因此永远只在 relay 线程被访问，无锁。
// 角色：viewer（只读）/ operator（踢会话、ACL）/ admin（证书）。

#include <functional>
#include <string>
#include <unordered_map>

#include "http/server/HttpServer.h"
#include "http/server/HttpContext.h"

#include "relay/relay_server.hpp"
#include "relay/audit_log.hpp"

struct AdminConfig {
    std::string addr;        // 形如 127.0.0.1:9443
    std::string token;       // 单个全权 token（等价 admin）
    std::string users;       // token:role 账户文件（viewer/operator/admin）
    std::string docroot;     // 前端磁盘目录（调试用，空则内嵌）
    std::string ca_cert;     // 签发用 CA 证书
    std::string ca_key;      // 签发用 CA 私钥
    std::string cert_dir;    // 证书台账目录（index.txt/revoked.txt/audit.log）
};

class AdminServer {
public:
    explicit AdminServer(RelayServer* relay) : relay_(relay), server_(&service_) {}

    int  start(const AdminConfig& cfg);
    void stop();

private:
    std::string callRelay(std::function<std::string()> fn);
    std::string roleOf(const HttpContextPtr& ctx);          // "" = 未授权
    int         requireRole(const HttpContextPtr& ctx, int need, std::string& role);
    void        audit(const std::string& role, const std::string& action, const std::string& detail);
    void        registerRoutes();
    int         serveFrontend(const HttpContextPtr& ctx, const char* name, const char* ctype);

    // handlers（api_*.cpp）
    void handleOverview(const HttpContextPtr& ctx);
    void handleAgents(const HttpContextPtr& ctx);
    void handleAgentDetail(const HttpContextPtr& ctx);
    void handleClients(const HttpContextPtr& ctx);
    void handleStreams(const HttpContextPtr& ctx);
    // api_ops.cpp
    void handleKickStream(const HttpContextPtr& ctx, const std::string& role);
    void handleKickAgent(const HttpContextPtr& ctx, const std::string& role);
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

    RelayServer*    relay_;
    hv::HttpService service_;
    hv::HttpServer  server_;
    AdminConfig     cfg_;
    std::unordered_map<std::string, std::string> users_;   // token -> role
    AuditLog        audit_;
};

// 角色权重：viewer=1 operator=2 admin=3
enum AdminRole { ROLE_VIEWER = 1, ROLE_OPERATOR = 2, ROLE_ADMIN = 3 };

