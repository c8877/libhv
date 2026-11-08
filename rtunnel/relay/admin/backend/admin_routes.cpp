// 管理台路由注册
#include "admin_server.hpp"

#include "cpputil/json.hpp"

void AdminServer::registerRoutes() {

    auto access = [this](int need, const char* method, const char* path,
                         void (AdminServer::*h)(const HttpContextPtr&)) {
        service_.Handle(method, path, [this, need, h](const HttpContextPtr& ctx) {
            std::string role;
            int rc = requireRole(ctx, need, role);
            if (rc) return rc;
            (this->*h)(ctx);
            return 200;
        });
    };
    // viewer：概览 + 设备列表
    access(ROLE_VIEWER, "GET", "/api/v1/overview", &AdminServer::handleOverview);
    access(ROLE_VIEWER, "GET", "/api/v1/agents", &AdminServer::handleAgents);
    // operator：详情/会话/文件读写/ACL 读
    access(ROLE_VIEWER, "GET", "/api/v1/agents/:devid", &AdminServer::handleAgentDetail);
    access(ROLE_VIEWER, "GET", "/api/v1/settings", &AdminServer::handleSettingsGet);
    access(ROLE_VIEWER, "GET", "/api/v1/me/profile", &AdminServer::handleProfileGet);
    access(ROLE_VIEWER, "GET", "/api/v1/agents/:devid/ttys", &AdminServer::handleTtyList);
    access(ROLE_MANAGER, "PUT", "/api/v1/agents/:devid/ttys", &AdminServer::handleTtyPut);
    access(ROLE_MANAGER, "DELETE", "/api/v1/agents/:devid/ttys", &AdminServer::handleTtyDelete);
    access(ROLE_MANAGER, "GET", "/api/v1/agents/:devid/ttys/discovered", &AdminServer::handleTtyDiscovered);
    access(ROLE_VIEWER, "GET", "/api/v1/agents/:devid/metrics", &AdminServer::handleAgentMetrics);
    access(ROLE_VIEWER, "GET", "/api/v1/agents/:devid/sysinfo", &AdminServer::handleAgentSysinfo);
    service_.Handle("PUT", "/api/v1/settings", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_ADMIN, role);
        if (rc) return rc;
        handleSettingsSet(ctx);
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/me/profile", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_VIEWER, role);
        if (rc) return rc;
        handleProfileSet(ctx);
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/me/logout-all", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_VIEWER, role);
        if (rc) return rc;
        handleLogoutAll(ctx);
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleDistCreate(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    access(ROLE_MANAGER, "GET", "/api/v1/distributions", &AdminServer::handleDistList);
    access(ROLE_MANAGER, "GET", "/api/v1/distributions/summary", &AdminServer::handleDistSummary);
    access(ROLE_MANAGER, "GET", "/api/v1/distributions/:id", &AdminServer::handleDistGet);
    service_.Handle("POST", "/api/v1/distributions/:id/retry", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleDistRetry(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions/:id/cancel", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleDistCancel(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions/:id/verify", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleDistVerify(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions/:id/force", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleDistVerify(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions/:id/extend", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleDistExtend(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/agents/:devid/caps", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleAgentCaps(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    access(ROLE_OPERATOR, "GET", "/api/v1/clients", &AdminServer::handleClients);
    access(ROLE_OPERATOR, "GET", "/api/v1/streams", &AdminServer::handleStreams);
    access(ROLE_OPERATOR, "GET", "/api/v1/acl/file", &AdminServer::handleAclGet);
    access(ROLE_OPERATOR, "GET", "/metrics", &AdminServer::handleMetrics);
    access(ROLE_OPERATOR, "GET", "/api/v1/files", &AdminServer::handleFiles);
    access(ROLE_OPERATOR, "GET", "/api/v1/files/:name", &AdminServer::handleFileDownload);
    access(ROLE_OPERATOR, "GET", "/api/v1/uploads/:name", &AdminServer::handleUploadStatus);
    // manager：审计/证书列表
    access(ROLE_MANAGER, "GET", "/api/v1/audit", &AdminServer::handleAudit);
    access(ROLE_MANAGER, "GET", "/api/v1/certs", &AdminServer::handleCerts);

    service_.Handle("DELETE", "/api/v1/streams/:a_sid", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleKickStream(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/agents/:devid/kick", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleKickAgent(ctx, role);
        return 200;
    });
    service_.Handle("PUT", "/api/v1/acl/file", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleAclPut(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/acl/reload", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleAclReload(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/certs/issue", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_ADMIN, role);
        if (rc) return rc;
        handleCertIssue(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/certs/revoke", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_ADMIN, role);
        if (rc) return rc;
        handleCertRevoke(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/certs/sign", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_ADMIN, role);
        if (rc) return rc;
        handleCertSign(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/files/:name", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
        if (rc) return rc;
        handleFileUpload(ctx, role);
        return 200;
    });
    service_.Handle("DELETE", "/api/v1/files/:name", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleFileDelete(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/shutdown", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_ADMIN, role);
        if (rc) return rc;
        handleShutdown(ctx, role);
        return 200;
    });

    // 账号/会话（公开接口由 handler 自己校验）
    service_.Handle("GET", "/api/v1/setup/status", [this](const HttpContextPtr& ctx) {
        handleSetupStatus(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/setup", [this](const HttpContextPtr& ctx) {
        handleSetup(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/login", [this](const HttpContextPtr& ctx) {
        handleLogin(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/logout", [this](const HttpContextPtr& ctx) {
        handleLogout(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("GET", "/api/v1/me", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_VIEWER, role);
        if (rc) return rc;
        handleMe(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("GET", "/api/v1/users", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleUsers(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/users", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleUserCreate(ctx, currentActor());
        return 200;
    });
    service_.Handle("PUT", "/api/v1/users/:id/password", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_VIEWER, role);
        if (rc) return rc;
        handleUserPassword(ctx, currentActor());
        return 200;
    });
    service_.Handle("PUT", "/api/v1/users/:id/status", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleUserStatus(ctx, currentActor());
        return 200;
    });
    service_.Handle("PUT", "/api/v1/users/:id/role", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleUserRole(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("DELETE", "/api/v1/users/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleUserDelete(ctx, currentActor());
        return 200;
    });
    service_.Handle("GET", "/api/v1/devices", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleDeviceList(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/devices/:devid/owner", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_MANAGER, role);
        if (rc) return rc;
        handleDeviceBind(ctx, currentActor());
        return 200;
    });

    auto devproxy = [this](const HttpContextPtr& ctx) {
        handleDeviceProxy(ctx);
        return (int)ctx->response->status_code;
    };
    service_.Handle("GET", "/device/*", devproxy);
    service_.Handle("POST", "/device/*", devproxy);
    service_.Handle("PUT", "/device/*", devproxy);
    service_.Handle("DELETE", "/device/*", devproxy);
    service_.Handle("OPTIONS", "/device/*", devproxy);
    service_.Handle("HEAD", "/device/*", devproxy);

    service_.GET("/", [this](const HttpContextPtr& ctx) {
        return serveFrontend(ctx, "admin.html", "text/html");
    });
    service_.GET("/admin_agent.js", [this](const HttpContextPtr& ctx) {
        return serveFrontend(ctx, "admin_agent.js", "application/javascript");
    });
    service_.GET("/admin_pages.js", [this](const HttpContextPtr& ctx) {
        return serveFrontend(ctx, "admin_pages.js", "application/javascript");
    });
    service_.GET("/admin_dist.js", [this](const HttpContextPtr& ctx) {
        return serveFrontend(ctx, "admin_dist.js", "application/javascript");
    });
    service_.GET("/admin.js", [this](const HttpContextPtr& ctx) {
        return serveFrontend(ctx, "admin.js", "application/javascript");
    });
    service_.GET("/admin.css", [this](const HttpContextPtr& ctx) {
        return serveFrontend(ctx, "admin.css", "text/css");
    });
    service_.GET("/vendor/:name", [this](const HttpContextPtr& ctx) {
        std::string name = ctx->param("name");
        std::string ctype = name.size() > 4 && name.compare(name.size() - 4, 4, ".css") == 0
                                ? "text/css" : "application/javascript";
        return serveFrontend(ctx, ("vendor/" + name).c_str(), ctype.c_str());
    });
}

