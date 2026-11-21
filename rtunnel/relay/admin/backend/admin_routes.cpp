// 管理台路由注册
#include "admin_server.hpp"

#include "cpputil/json.hpp"

void AdminServer::registerRoutes() {

    auto access = [this](const char* need, const char* method, const char* path,
                         void (AdminServer::*h)(const HttpContextPtr&)) {
        service_.Handle(method, path, [this, need, h](const HttpContextPtr& ctx) {
            std::string role;
            int rc = requirePerm(ctx, need, role);
            if (rc) return rc;
            (this->*h)(ctx);
            return 200;
        });
    };
    // viewer：概览 + 设备列表
    access("device.view", "GET", "/api/v1/overview", &AdminServer::handleOverview);
    access("device.view", "GET", "/api/v1/agents", &AdminServer::handleAgents);
    // operator：详情/会话/文件读写/ACL 读
    access("device.view", "GET", "/api/v1/agents/:devid", &AdminServer::handleAgentDetail);
    access("settings.read", "GET", "/api/v1/settings", &AdminServer::handleSettingsGet);
    access("auth", "GET", "/api/v1/me/profile", &AdminServer::handleProfileGet);
    access("device.view", "GET", "/api/v1/agents/:devid/metrics/series/config", &AdminServer::handleMetricSeriesCfgGet);
    access("device.metrics.write", "PUT", "/api/v1/agents/:devid/metrics/series/config", &AdminServer::handleMetricSeriesCfgPut);
    access("device.view", "GET", "/api/v1/agents/:devid/metrics/series", &AdminServer::handleMetricSeriesData);
    access("device.tty.read", "GET", "/api/v1/agents/:devid/ttys", &AdminServer::handleTtyList);
    access("device.tty.write", "PUT", "/api/v1/agents/:devid/ttys", &AdminServer::handleTtyPut);
    access("device.tty.write", "DELETE", "/api/v1/agents/:devid/ttys", &AdminServer::handleTtyDelete);
    access("device.tty.read", "GET", "/api/v1/agents/:devid/ttys/discovered", &AdminServer::handleTtyDiscovered);
    access("device.tty.read", "GET", "/api/v1/ttylogs/:name", &AdminServer::handleTtyLogDownload);
    // sysinfo 要等设备回帧，放到线程池里等，避免阻塞管理台唯一的 HTTP 线程
    service_.Handle("GET", "/api/v1/agents/:devid/sysinfo", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.view", role);
        if (rc) return rc;
        return handleAgentSysinfo(ctx);
    });
    service_.Handle("GET", "/api/v1/agents/:devid/net", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.view", role);
        if (rc) return rc;
        return handleAgentNet(ctx);
    });
    service_.Handle("GET", "/api/v1/agents/:devid/procs", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.proc.read", role);
        if (rc) return rc;
        return handleAgentProcs(ctx);
    });
    service_.Handle("GET", "/api/v1/agents/:devid/procs/:pid", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.proc.read", role);
        if (rc) return rc;
        return handleAgentProcDetail(ctx);
    });
    service_.Handle("POST", "/api/v1/agents/:devid/procs/signal", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.proc.signal", role);
        if (rc) return rc;
        return handleAgentProcSignal(ctx);
    });
    service_.Handle("GET", "/api/v1/agents/:devid/storage", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.view", role);
        if (rc) return rc;
        return handleAgentStorage(ctx);
    });
    service_.Handle("POST", "/api/v1/agents/:devid/power", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.power", role);
        if (rc) return rc;
        return handleAgentPower(ctx);
    });
    service_.Handle("GET", "/api/v1/agents/:devid/diag", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.diag", role);
        if (rc) return rc;
        return handleAgentDiag(ctx);
    });
    service_.Handle("PUT", "/api/v1/settings", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "settings.write", role);
        if (rc) return rc;
        handleSettingsSet(ctx);
        return (int)ctx->response->status_code;
    });
    access("settings.read", "GET", "/api/v1/alerts/status", &AdminServer::handleAlertStatus);
    service_.Handle("POST", "/api/v1/alerts/test", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "settings.write", role);
        if (rc) return rc;
        handleAlertTest(ctx);
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/me/profile", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "auth", role);
        if (rc) return rc;
        handleProfileSet(ctx);
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/me/logout-all", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "auth", role);
        if (rc) return rc;
        handleLogoutAll(ctx);
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dist.write", role);
        if (rc) return rc;
        handleDistCreate(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    access("dist.read", "GET", "/api/v1/distributions", &AdminServer::handleDistList);
    access("dist.read", "GET", "/api/v1/distributions/summary", &AdminServer::handleDistSummary);
    access("dist.read", "GET", "/api/v1/distributions/:id", &AdminServer::handleDistGet);
    service_.Handle("POST", "/api/v1/distributions/:id/retry", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dist.write", role);
        if (rc) return rc;
        handleDistRetry(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions/:id/cancel", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dist.write", role);
        if (rc) return rc;
        handleDistCancel(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions/:id/verify", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dist.write", role);
        if (rc) return rc;
        handleDistVerify(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions/:id/force", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dist.write", role);
        if (rc) return rc;
        handleDistVerify(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/distributions/:id/extend", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dist.write", role);
        if (rc) return rc;
        handleDistExtend(ctx, currentActor());
        return (int)ctx->response->status_code;
    });

    service_.Handle("PUT", "/api/v1/agents/:devid/caps", [this](const HttpContextPtr& ctx) {
        std::string role;
        // 逐字段鉴权在 handler 内完成：能力/OTA/串口分别对应不同资源
        int rc = requirePerm(ctx, "auth", role);
        if (rc) return rc;
        handleAgentCaps(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    access("device.view", "GET", "/api/v1/clients", &AdminServer::handleClients);
    access("device.view", "GET", "/api/v1/streams", &AdminServer::handleStreams);
    access("dict.read", "GET", "/api/v1/acl/file", &AdminServer::handleAclGet);
    access("device.view", "GET", "/metrics", &AdminServer::handleMetrics);
    access("file.read", "GET", "/api/v1/files", &AdminServer::handleFiles);
    access("file.read", "GET", "/api/v1/files/:name", &AdminServer::handleFileDownload);
    access("file.write", "GET", "/api/v1/uploads/:name", &AdminServer::handleUploadStatus);
    // manager：审计/证书列表
    access("audit.read", "GET", "/api/v1/audit", &AdminServer::handleAudit);
    access("cert.read", "GET", "/api/v1/certs", &AdminServer::handleCerts);

    service_.Handle("DELETE", "/api/v1/streams/:a_sid", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.kick", role);
        if (rc) return rc;
        handleKickStream(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/agents/:devid/kick", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.kick", role);
        if (rc) return rc;
        handleKickAgent(ctx, role);
        return 200;
    });
    service_.Handle("PUT", "/api/v1/acl/file", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handleAclPut(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/acl/reload", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handleAclReload(ctx, role);
        return 200;
    });
    // 接入账号与 Token
    access("account.read", "GET", "/api/v1/client-accounts", &AdminServer::handleClientAccounts);
    access("account.read", "GET", "/api/v1/client-accounts/:id", &AdminServer::handleClientAccountGet);
    service_.Handle("POST", "/api/v1/client-accounts", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "account.write", role); if (rc) return rc;
        handleClientAccountCreate(ctx, role); return 200;
    });
    service_.Handle("PUT", "/api/v1/client-accounts/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "account.write", role); if (rc) return rc;
        handleClientAccountUpdate(ctx, role); return 200;
    });
    service_.Handle("DELETE", "/api/v1/client-accounts/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "account.delete", role); if (rc) return rc;
        handleClientAccountDelete(ctx, role); return 200;
    });
    access("account.read", "GET", "/api/v1/client-accounts/:id/effective", &AdminServer::handleClientAccountEffective);
    access("account.read", "GET", "/api/v1/client-accounts/:id/tokens", &AdminServer::handleClientTokens);
    service_.Handle("POST", "/api/v1/client-accounts/:id/tokens", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "account.write", role); if (rc) return rc;
        handleClientTokenCreate(ctx, role); return 200;
    });
    service_.Handle("PUT", "/api/v1/client-tokens/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "account.write", role); if (rc) return rc;
        handleClientTokenUpdate(ctx, role); return 200;
    });
    service_.Handle("POST", "/api/v1/client-tokens/:id/rotate", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "account.write", role); if (rc) return rc;
        handleClientTokenRotate(ctx, role); return 200;
    });
    service_.Handle("DELETE", "/api/v1/client-tokens/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "account.write", role); if (rc) return rc;
        handleClientTokenDelete(ctx, role); return 200;
    });
    // 管理台账号 API Token（atk-）
    service_.Handle("GET", "/api/v1/admin-tokens", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "token.read", role); if (rc) return rc;
        handleAdminTokens(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/admin-tokens", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "token.write", role); if (rc) return rc;
        handleAdminTokenCreate(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/admin-tokens/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "token.write", role); if (rc) return rc;
        handleAdminTokenUpdate(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/admin-tokens/:id/rotate", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "token.write", role); if (rc) return rc;
        handleAdminTokenRotate(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("DELETE", "/api/v1/admin-tokens/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "token.write", role); if (rc) return rc;
        handleAdminTokenDelete(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    // 资源字典与权限组
    access("dict.read", "GET", "/api/v1/permission/dicts", &AdminServer::handlePermissionDicts);
    access("dict.read", "GET", "/api/v1/permission/resources", &AdminServer::handlePermResources);
    access("dict.read", "GET", "/api/v1/permission/groups", &AdminServer::handlePermGroups);
    access("dict.read", "GET", "/api/v1/permission/subjects", &AdminServer::handlePermSubjects);
    access("dict.read", "GET", "/api/v1/permission/groups/:id", &AdminServer::handlePermGroupDetail);
    access("dict.read", "GET", "/api/v1/permission/subjects/:type/:id/groups",
           &AdminServer::handleSubjectGroups);
    service_.Handle("PUT", "/api/v1/permission/subjects/:type/:id/groups", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handleSubjectGroupsSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/permission/devices/:devid", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handleResDeviceUpdate(ctx, role);
        return (int)ctx->response->status_code;
    });
    access("dict.read", "GET", "/api/v1/permission/groups/:id/members", &AdminServer::handlePermGroupMembers);
    service_.Handle("PUT", "/api/v1/permission/resources/:key", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handlePermResourceUpdate(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/permission/groups", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handlePermGroupSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/permission/groups/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handlePermGroupSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("DELETE", "/api/v1/permission/groups/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handlePermGroupDelete(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/permission/groups/:id/members", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handlePermGroupMembersSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    access("account.read", "GET", "/api/v1/client-accounts/:id/perm-groups", &AdminServer::handleClientPermGroups);
    service_.Handle("PUT", "/api/v1/client-accounts/:id/perm-groups", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "account.write", role);
        if (rc) return rc;
        handleClientPermGroupsSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    access("account.read", "GET", "/api/v1/users/:id/perm-groups", &AdminServer::handleUserPermGroups);
    service_.Handle("PUT", "/api/v1/users/:id/perm-groups", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "account.write", role);
        if (rc) return rc;
        handleUserPermGroupsSave(ctx, role);
        return (int)ctx->response->status_code;
    });

    access("dict.read", "GET", "/api/v1/permission/dict/ports", &AdminServer::handleDictPorts);
    access("dict.read", "GET", "/api/v1/permission/dict/ttys", &AdminServer::handleDictTtys);

    service_.Handle("POST", "/api/v1/permission/dict/ports/batch-delete", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictBatchDelete(ctx, role, "ports"); return 200;
    });
    service_.Handle("POST", "/api/v1/permission/dict/ttys/batch-delete", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictBatchDelete(ctx, role, "ttys"); return 200;
    });
    service_.Handle("POST", "/api/v1/permission/dict/ports/batch", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictBatchAdd(ctx, role, "ports"); return 200;
    });
    service_.Handle("POST", "/api/v1/permission/dict/ttys/batch", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictBatchAdd(ctx, role, "ttys"); return 200;
    });
    service_.Handle("PUT", "/api/v1/permission/dict/ports/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictUpdate(ctx, role, "ports"); return 200;
    });
    service_.Handle("PUT", "/api/v1/permission/dict/ttys/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictUpdate(ctx, role, "ttys"); return 200;
    });
    service_.Handle("DELETE", "/api/v1/permission/dict/ports/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictDelete(ctx, role, "ports"); return 200;
    });
    service_.Handle("DELETE", "/api/v1/permission/dict/ttys/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictDelete(ctx, role, "ttys"); return 200;
    });
    service_.Handle("POST", "/api/v1/permission/dict/ports", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictPortAdd(ctx, role); return 200;
    });
    service_.Handle("POST", "/api/v1/permission/dict/ttys", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "dict.write", role); if (rc) return rc;
        handleDictTtyAdd(ctx, role); return 200;
    });
    service_.Handle("POST", "/api/v1/certs/issue", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "cert.write", role);
        if (rc) return rc;
        handleCertIssue(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/certs/revoke", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "cert.write", role);
        if (rc) return rc;
        handleCertRevoke(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/certs/sign", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "cert.write", role);
        if (rc) return rc;
        handleCertSign(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/files/:name", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "file.write", role);
        if (rc) return rc;
        std::string action = ctx->param("action");
        if (action == "mkdir") handleFileMkdir(ctx, role);
        else if (action == "move") handleFileMove(ctx, role);
        else handleFileUpload(ctx, role);
        return 200;
    });
    service_.Handle("DELETE", "/api/v1/files/:name", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "file.delete", role);
        if (rc) return rc;
        handleFileDelete(ctx, role);
        return 200;
    });
    service_.Handle("PUT", "/api/v1/files/:name", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "file.write", role);
        if (rc) return rc;
        handleFilePut(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/shutdown", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "system.shutdown", role);
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
        int rc = requirePerm(ctx, "auth", role);
        if (rc) return rc;
        handleMe(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("GET", "/api/v1/users", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "account.read", role);
        if (rc) return rc;
        handleUsers(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/users", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "account.write", role);
        if (rc) return rc;
        handleUserCreate(ctx, currentActor());
        return 200;
    });
    service_.Handle("PUT", "/api/v1/users/:id/password", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "auth", role);
        if (rc) return rc;
        handleUserPassword(ctx, currentActor());
        return 200;
    });
    service_.Handle("PUT", "/api/v1/users/:id/status", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "account.write", role);
        if (rc) return rc;
        handleUserStatus(ctx, currentActor());
        return 200;
    });
    service_.Handle("PUT", "/api/v1/users/:id/role", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "account.write", role);
        if (rc) return rc;
        handleUserRole(ctx, currentActor());
        return (int)ctx->response->status_code;
    });
    service_.Handle("DELETE", "/api/v1/users/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "account.delete", role);
        if (rc) return rc;
        handleUserDelete(ctx, currentActor());
        return 200;
    });
    service_.Handle("GET", "/api/v1/devices", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.view", role);
        if (rc) return rc;
        handleDeviceList(ctx); return (int)ctx->response->status_code;
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
        int rc = serveFrontend(ctx, "admin.html", "text/html");
        if (rc == HTTP_STATUS_NOT_FOUND) rc = serveFrontend(ctx, "index.html", "text/html");
        return rc;
    });
    // 前端静态产物（Vite）
    auto static_asset = [this](const HttpContextPtr& ctx) {
        std::string path = ctx->path();
        auto ends_with = [&path](const char* suffix) {
            size_t n = strlen(suffix);
            return path.size() >= n && path.compare(path.size() - n, n, suffix) == 0;
        };
        const char* ctype = "application/javascript";
        if (ends_with(".css")) ctype = "text/css";
        else if (ends_with(".json")) ctype = "application/json";
        else if (ends_with(".svg")) ctype = "image/svg+xml";
        else if (ends_with(".png")) ctype = "image/png";
        else if (ends_with(".ico")) ctype = "image/x-icon";
        else if (ends_with(".woff2")) ctype = "font/woff2";
        else if (ends_with(".woff")) ctype = "font/woff";
        else if (ends_with(".ttf")) ctype = "font/ttf";
        int rc = serveFrontend(ctx, path.c_str() + 1, ctype);
        if (rc == 200) ctx->response->headers["Cache-Control"] = "public, max-age=31536000, immutable";
        return rc;
    };
    service_.GET("/assets/*", static_asset);
}

