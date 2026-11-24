// 管理台路由注册
#include "admin_server.hpp"

#include "cpputil/json.hpp"

#include <chrono>

static uint64_t routeNowUs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void AdminServer::registerRoutes() {
    // 请求计数与耗时：只在 pre/postprocessor 里碰原子变量，
    // 业务 handler 不感知；异步接口（返回 NEXT）统计的是 handler 派发耗时。
    static thread_local uint64_t tl_req_start_us = 0;
    static thread_local bool     tl_req_timed = false;
    service_.preprocessor = [this](const HttpContextPtr&) {
        tl_req_start_us = routeNowUs();
        tl_req_timed = true;
        http_req_total_.fetch_add(1, std::memory_order_relaxed);
        http_inflight_.fetch_add(1, std::memory_order_relaxed);
        return (int)HTTP_STATUS_NEXT;
    };
    service_.postprocessor = [this](const HttpContextPtr& ctx) {
        if (tl_req_timed) {
            uint64_t us = routeNowUs() - tl_req_start_us;
            http_us_total_.fetch_add(us, std::memory_order_relaxed);
            if (us >= 1000000) http_slow_total_.fetch_add(1, std::memory_order_relaxed);
            http_inflight_.fetch_sub(1, std::memory_order_relaxed);
            tl_req_timed = false;
        }
        // /device 的响应由数据面线程写，状态码也在那边设置；这里不读，
        // 避免和代理线程并发改 response。其余路由照常按状态码分桶。
        bool device_proxy = ctx->request && ctx->request->path.compare(0, 8, "/device/") == 0;
        if (device_proxy) {
            http_req_2xx_.fetch_add(1, std::memory_order_relaxed);
        } else {
            int code = ctx->response ? (int)ctx->response->status_code : 0;
            if (code >= 500) http_req_5xx_.fetch_add(1, std::memory_order_relaxed);
            else if (code >= 400) http_req_4xx_.fetch_add(1, std::memory_order_relaxed);
            else http_req_2xx_.fetch_add(1, std::memory_order_relaxed);
        }
        return (int)HTTP_STATUS_NEXT;
    };

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
    // 运维 → 中继列表：地址字典 + 有序下发列表 + 同步任务
    access("relay.read", "GET", "/api/v1/relay-dict", &AdminServer::handleRelayDictList);
    access("relay.write", "POST", "/api/v1/relay-dict", &AdminServer::handleRelayDictAdd);
    access("relay.write", "POST", "/api/v1/relay-dict/batch", &AdminServer::handleRelayDictBatchAdd);
    access("relay.write", "PUT", "/api/v1/relay-dict/:id", &AdminServer::handleRelayDictUpdate);
    access("relay.write", "DELETE", "/api/v1/relay-dict/:id", &AdminServer::handleRelayDictDelete);
    access("relay.read", "GET", "/api/v1/relay-list", &AdminServer::handleRelayListGet);
    service_.Handle("PUT", "/api/v1/relay-list", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "relay.write", role);
        if (rc) return rc;
        handleRelayListSet(ctx);
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/relay-list/push", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "relay.write", role);
        if (rc) return rc;
        handleRelayListPush(ctx);
        return (int)ctx->response->status_code;
    });
    access("relay.read", "GET", "/api/v1/relay-sync/tasks", &AdminServer::handleRelaySyncList);
    access("relay.write", "POST", "/api/v1/relay-sync/tasks", &AdminServer::handleRelaySyncCreate);
    access("relay.read", "GET", "/api/v1/relay-sync/tasks/:batch", &AdminServer::handleRelaySyncGet);
    service_.Handle("POST", "/api/v1/relay-sync/tasks/:batch/pause", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "relay.write", role);
        if (rc) return rc;
        handleRelaySyncAction(ctx, "pause");
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/relay-sync/tasks/:batch/resume", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "relay.write", role);
        if (rc) return rc;
        handleRelaySyncAction(ctx, "resume");
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/relay-sync/tasks/:batch/cancel", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "relay.write", role);
        if (rc) return rc;
        handleRelaySyncAction(ctx, "cancel");
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/relay-sync/tasks/:batch/retry", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "relay.write", role);
        if (rc) return rc;
        handleRelaySyncAction(ctx, "retry");
        return (int)ctx->response->status_code;
    });
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
    access("device.view", "GET", "/metrics", &AdminServer::handleMetrics);
    access("repo.read", "GET", "/api/v1/files", &AdminServer::handleFiles);
    access("repo.read", "GET", "/api/v1/files/:name", &AdminServer::handleFileDownload);
    access("repo.write", "GET", "/api/v1/uploads/:name", &AdminServer::handleUploadStatus);
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
    service_.Handle("POST", "/api/v1/client-accounts/:id/kick", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "account.write", role); if (rc) return rc;
        handleClientAccountKick(ctx, role); return 200;
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
    // 权限字典（资源/设备/端口/串口）与权限组
    access("dict.read", "GET", "/api/v1/permission/dicts", &AdminServer::handlePermissionDicts);
    access("group.read", "GET", "/api/v1/permission/resources", &AdminServer::handlePermResources);
    access("group.read", "GET", "/api/v1/permission/groups", &AdminServer::handlePermGroups);
    access("group.read", "GET", "/api/v1/permission/subjects", &AdminServer::handlePermSubjects);
    access("group.read", "GET", "/api/v1/permission/groups/:id", &AdminServer::handlePermGroupDetail);
    access("group.read", "GET", "/api/v1/permission/subjects/:type/:id/groups",
           &AdminServer::handleSubjectGroups);
    access("group.read", "GET", "/api/v1/permission/groups/:id/members", &AdminServer::handlePermGroupMembers);
    // 给主体换组 = 改变授权面，单独用 group.member 控制
    service_.Handle("PUT", "/api/v1/permission/subjects/:type/:id/groups", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "group.member", role);
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
    service_.Handle("PUT", "/api/v1/permission/resources/:key", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "dict.write", role);
        if (rc) return rc;
        handlePermResourceUpdate(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/permission/groups", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "group.write", role);
        if (rc) return rc;
        handlePermGroupSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/permission/groups/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "group.write", role);
        if (rc) return rc;
        handlePermGroupSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("DELETE", "/api/v1/permission/groups/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "group.write", role);
        if (rc) return rc;
        handlePermGroupDelete(ctx, role);
        return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/permission/groups/:id/members", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "group.member", role);
        if (rc) return rc;
        handlePermGroupMembersSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    access("account.read", "GET", "/api/v1/client-accounts/:id/perm-groups", &AdminServer::handleClientPermGroups);
    service_.Handle("PUT", "/api/v1/client-accounts/:id/perm-groups", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "group.member", role);
        if (rc) return rc;
        handleClientPermGroupsSave(ctx, role);
        return (int)ctx->response->status_code;
    });
    access("account.read", "GET", "/api/v1/users/:id/perm-groups", &AdminServer::handleUserPermGroups);
    service_.Handle("PUT", "/api/v1/users/:id/perm-groups", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "group.member", role);
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
        int rc = requirePerm(ctx, "repo.write", role);
        if (rc) return rc;
        std::string action = ctx->param("action");
        if (action == "mkdir") handleFileMkdir(ctx, role);
        else if (action == "move") handleFileMove(ctx, role);
        else handleFileUpload(ctx, role);
        return 200;
    });
    service_.Handle("DELETE", "/api/v1/files/:name", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "repo.delete", role);
        if (rc) return rc;
        handleFileDelete(ctx, role);
        return 200;
    });
    service_.Handle("PUT", "/api/v1/files/:name", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "repo.write", role);
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
        return handleDeviceProxy(ctx);
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

