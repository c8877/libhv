// 管理台路由注册
#include "admin_server.hpp"
#include "admin_web_assets.hpp"

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
    // 运维 → 部署：SSH/SCP 直连目标主机（凭据只在 relay 本地，接口不回显）
    access("deploy.read", "GET", "/api/v1/deploy/hosts", &AdminServer::handleDeployHosts);
    access("deploy.write", "POST", "/api/v1/deploy/hosts", &AdminServer::handleDeployHostCreate);
    access("deploy.write", "PUT", "/api/v1/deploy/hosts/:id", &AdminServer::handleDeployHostUpdate);
    access("deploy.write", "DELETE", "/api/v1/deploy/hosts/:id", &AdminServer::handleDeployHostDelete);
    access("deploy.read", "POST", "/api/v1/deploy/hosts/:id/probe", &AdminServer::handleDeployProbe);
    access("deploy.read", "POST", "/api/v1/deploy/scan", &AdminServer::handleDeployScan);
    access("deploy.read", "GET", "/api/v1/deploy/packages", &AdminServer::handleDeployPackages);
    access("deploy.read", "GET", "/api/v1/deploy/jobs", &AdminServer::handleDeployJobs);
    access("deploy.read", "GET", "/api/v1/deploy/jobs/:id", &AdminServer::handleDeployJobGet);
    access("deploy.write", "POST", "/api/v1/deploy/jobs", &AdminServer::handleDeployJobCreate);
    access("deploy.write", "POST", "/api/v1/deploy/jobs/:id/cancel", &AdminServer::handleDeployJobCancel);
    // 安装包上传：复用仓库分片上传，固定落到 deploy/，只需要 deploy.write
    service_.Handle("POST", "/api/v1/deploy/packages/:name", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "deploy.write", role);
        if (rc) return rc;
        handleFileUpload(ctx, role, "deploy");
        return 200;
    });
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
    access("device.view", "GET", "/api/v1/agents/:devid/metrics/history",
           &AdminServer::handleMetricsHistory);
    // 事件流：按连接打开时的权限快照过滤（受限账号只收自己设备的事件）
    access("device.view", "GET", "/api/v1/events", &AdminServer::handleEvents);
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
    service_.Handle("POST", "/api/v1/agents/:devid/exec", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.exec", role);
        if (rc) return rc;
        return handleAgentExec(ctx, role);
    });
    // 操作作业：plan 冻结 → apply 执行 → verify 复核
    service_.Handle("POST", "/api/v1/operations/plan", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.exec", role);
        if (rc) return rc;
        handleOpsPlan(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/operations/:id/apply", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.exec", role);
        if (rc) return rc;
        handleOpsApply(ctx, role);
        return HTTP_STATUS_NEXT;
    });
    service_.Handle("POST", "/api/v1/operations/:id/verify", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "device.exec", role);
        if (rc) return rc;
        handleOpsVerify(ctx, role);
        return HTTP_STATUS_NEXT;
    });
    service_.Handle("POST", "/api/v1/operations/:id/approve", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "op.approve", role);
        if (rc) return rc;
        handleOpsApprove(ctx, role);
        return 200;
    });
    access("device.view", "GET", "/api/v1/operations", &AdminServer::handleOpsList);

    // 自愈规则：fires 必须注册在 /:id 之前，否则 "fires" 会被当成 id
    // AI 助手：配置按账号隔离，改的是自己的，所以只需要 ai.chat（不再有 ai.config）
    access("ai.chat", "GET", "/api/v1/ai/config", &AdminServer::handleAiConfigGet);
    service_.Handle("PUT", "/api/v1/ai/config", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "ai.chat", role);
        if (rc) return rc;
        handleAiConfigSet(ctx, role);
        return 200;
    });
    service_.Handle("PUT", "/api/v1/ai/key", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "ai.chat", role);
        if (rc) return rc;
        handleAiKeySet(ctx, role);
        return 200;
    });
    service_.Handle("DELETE", "/api/v1/ai/key", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "ai.chat", role);
        if (rc) return rc;
        handleAiKeyClear(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/ai/chat", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "ai.chat", role);
        if (rc) return rc;
        handleAiChat(ctx, role);
        return HTTP_STATUS_NEXT;
    });
    // 会话记录：按账号隔离、永久保留（列表/新建/读取/改名/删除）
    access("ai.chat", "GET", "/api/v1/ai/sessions", &AdminServer::handleAiSessions);
    access("ai.chat", "GET", "/api/v1/ai/sessions/:id", &AdminServer::handleAiSessionGet);
    service_.Handle("POST", "/api/v1/ai/sessions", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "ai.chat", role);
        if (rc) return rc;
        handleAiSessionCreate(ctx);
        return 200;
    });
    service_.Handle("PUT", "/api/v1/ai/sessions/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "ai.chat", role);
        if (rc) return rc;
        handleAiSessionRename(ctx);
        return 200;
    });
    service_.Handle("DELETE", "/api/v1/ai/sessions/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "ai.chat", role);
        if (rc) return rc;
        handleAiSessionDelete(ctx);
        return 200;
    });

    // 连通性测试：发一次最小请求，把上游错误原文带回来
    service_.Handle("POST", "/api/v1/ai/test", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "ai.chat", role);
        if (rc) return rc;
        handleAiTest(ctx);
        return HTTP_STATUS_NEXT;
    });

    access("playbook.read", "GET", "/api/v1/playbooks/fires", &AdminServer::handlePlaybookFires);
    access("playbook.read", "GET", "/api/v1/playbooks", &AdminServer::handlePlaybooksList);
    access("playbook.read", "GET", "/api/v1/playbooks/:id", &AdminServer::handlePlaybookGet);
    service_.Handle("POST", "/api/v1/playbooks", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "playbook.write", role);
        if (rc) return rc;
        handlePlaybookSave(ctx, role);
        return 200;
    });
    service_.Handle("PUT", "/api/v1/playbooks/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "playbook.write", role);
        if (rc) return rc;
        handlePlaybookSave(ctx, role);
        return 200;
    });
    service_.Handle("DELETE", "/api/v1/playbooks/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "playbook.write", role);
        if (rc) return rc;
        handlePlaybookDelete(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/playbooks/:id/enable", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "playbook.write", role);
        if (rc) return rc;
        handlePlaybookEnable(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/playbooks/:id/run", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requirePerm(ctx, "playbook.write", role);
        if (rc) return rc;
        handlePlaybookRun(ctx, role);
        return 200;
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
    access("usage.read", "GET", "/api/v1/usage", &AdminServer::handleUsage);
    access("tenant.read", "GET", "/api/v1/tenants", &AdminServer::handleTenants);
    service_.Handle("POST", "/api/v1/tenants", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "tenant.write", role); if (rc) return rc;
        handleTenantCreate(ctx, role); return 200;
    });
    service_.Handle("PUT", "/api/v1/tenants/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "tenant.write", role); if (rc) return rc;
        handleTenantUpdate(ctx, role); return 200;
    });
    access("billing.read", "GET", "/api/v1/billing/prices", &AdminServer::handleBillingPrices);
    access("billing.read", "GET", "/api/v1/billing/summary", &AdminServer::handleBillingSummary);
    service_.Handle("PUT", "/api/v1/billing/prices/:metric", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "billing.write", role); if (rc) return rc;
        handleBillingPriceSet(ctx, role); return 200;
    });
    access("billing.read", "GET", "/api/v1/billing/statements", &AdminServer::handleBillingStatements);
    // 具体路径先于 :id 注册，避免被通配吃掉
    access("billing.read", "GET", "/api/v1/billing/statements/:id/csv",
           &AdminServer::handleBillingStatementCsv);
    service_.Handle("POST", "/api/v1/billing/statements", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "billing.write", role); if (rc) return rc;
        handleBillingStatementGenerate(ctx, role); return 200;
    });
    access("billing.read", "GET", "/api/v1/billing/statements/:id", &AdminServer::handleBillingStatementGet);
    service_.Handle("PUT", "/api/v1/billing/statements/:id", [this](const HttpContextPtr& ctx) {
        std::string role; int rc = requirePerm(ctx, "billing.write", role); if (rc) return rc;
        handleBillingStatementStatus(ctx, role); return 200;
    });
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

    // OpenAPI：构建前端时由 api-docs.js 生成（dist/openapi.json），此处只负责按来源提供。
    // MCP / 其它工具靠它拿到工具清单，不另写一份 schema。
    // 不鉴权：与管理台首页同级；真正调接口仍需 atk 与权限组。
    service_.Handle("GET", "/api/v1/openapi.json", [this](const HttpContextPtr& ctx) {
        int rc = serveFrontend(ctx, "openapi.json", "application/json", kAdminWebDiskOnly);
        if (rc == HTTP_STATUS_NOT_FOUND)
            rc = serveFrontend(ctx, "openapi.json", "application/json", kAdminWebEmbeddedOnly);
        return rc;
    });

    service_.GET("/", [this](const HttpContextPtr& ctx) {
        // 两个名字都要在同一来源里找完，再换下一个来源，避免内嵌 admin.html
        // 抢在磁盘 index.html 前面
        int rc = serveFrontend(ctx, "admin.html", "text/html", kAdminWebDiskOnly);
        if (rc == HTTP_STATUS_NOT_FOUND)
            rc = serveFrontend(ctx, "index.html", "text/html", kAdminWebDiskOnly);
        if (rc == HTTP_STATUS_NOT_FOUND)
            rc = serveFrontend(ctx, "admin.html", "text/html", kAdminWebEmbeddedOnly);
        if (rc == HTTP_STATUS_NOT_FOUND)
            rc = serveFrontend(ctx, "index.html", "text/html", kAdminWebEmbeddedOnly);
        // 两种来源都没有：给一条能照着做的提示，而不是 hv 的空白 404
        if (rc == HTTP_STATUS_NOT_FOUND && cfg_.docroot.empty() && !adminWebAssetsAvailable()) {
            ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
            ctx->response->content_type = http_content_type_enum("text/plain");
            ctx->response->body =
                "admin frontend not deployed: start relay with --admin-docroot <web/dist>, "
                "or use rtunnel-relay-embedded (built with -DRTUNNEL_ADMIN_WEB=ON)\n";
            return 404;
        }
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

