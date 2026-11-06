#include "admin_server.hpp"
#include "common/logging.hpp"

#include <chrono>
#include <future>
#include <memory>
#include <string.h>
#include <sys/stat.h>

// 编译期内嵌前端（embed.py 生成 embedded_frontend.cpp）
struct EmbeddedFile {
    const char*          name;
    const char*          ctype;
    const unsigned char* data;
    unsigned int         len;
};
extern const EmbeddedFile g_embedded_frontend[];
extern const int          g_embedded_frontend_count;

// ---------------------------------------------------------------- 跨线程调用

// 在 relay 线程执行 fn 并同步等结果。relay 状态只在 relay 线程被访问。
std::string AdminServer::callRelay(std::function<std::string()> fn) {
    if (relay_->loop == NULL) return "{\"error\":\"relay not running\"}";

    struct Task {
        std::function<std::string()> fn;
        std::promise<std::string>    promise;
    };
    auto task = std::make_shared<Task>();
    task->fn = std::move(fn);
    std::future<std::string> fut = task->promise.get_future();

    // holder 保证事件被执行前 task 一直存活（即使等待方超时返回）
    auto* holder = new std::shared_ptr<Task>(task);
    hevent_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.event_type = (hevent_type_e)(HEVENT_TYPE_CUSTOM + 1);
    ev.cb = [](hevent_t* e) {
        auto* holder = (std::shared_ptr<Task>*)e->userdata;
        std::string result;
        try {
            result = (*holder)->fn();
        } catch (...) {
            result = "{\"error\":\"internal\"}";
        }
        (*holder)->promise.set_value(result);
        delete holder;
    };
    ev.userdata = holder;
    hloop_post_event(relay_->loop, &ev);

    if (fut.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        return "{\"error\":\"relay timeout\"}";
    }
    return fut.get();
}

// ---------------------------------------------------------------- 鉴权与角色

thread_local AdminActor tl_actor;

static std::string cookieValue(const HttpContextPtr& ctx, const std::string& key) {
    const HttpCookie& ck = ctx->request->GetCookie(key.c_str());
    if (!ck.name.empty()) return ck.value;
    std::string c = ctx->header("Cookie");
    size_t pos = 0;
    while (pos < c.size()) {
        size_t semi = c.find(';', pos);
        std::string item = c.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = item.substr(0, eq);
            while (!k.empty() && (k[0] == ' ' || k[0] == '\t')) k.erase(0, 1);
            if (k == key) return item.substr(eq + 1);
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return "";
}

std::string AdminServer::roleOf(const HttpContextPtr& ctx) {
    tl_actor = AdminActor{};

    // 1) 会话 Cookie
    std::string sid = cookieValue(ctx, "rt_sid");
    if (!sid.empty() && db_.ok()) {
        DbUser u;
        bool got = db_.getSession(AdminDb::sha256Hex(sid), u, AdminDb::nowMs());
        if (got) {
            tl_actor.id = u.id;
            tl_actor.username = u.username;
            tl_actor.role = u.role;
            if (u.role == "superadmin") return "admin";
            if (u.role == "admin") return "operator";
            return "viewer";
        }
    }

    // 2) 首次初始化前，或显式配置了紧急 token 时，允许 Bearer
    bool initialized = db_.ok() && db_.initialized();
    if (!cfg_.token.empty() || !users_.empty()) {
        std::string auth = ctx->header("Authorization");
        const std::string prefix = "Bearer ";
        if (auth.compare(0, prefix.size(), prefix) == 0) {
            std::string tok = auth.substr(prefix.size());
            if (!cfg_.token.empty() && tok == cfg_.token) {
                bool allow = !initialized;
                if (allow && !initialized) {
                    // 初始化窗口只放行 setup 相关接口，其余仍需账号登录
                    const std::string& path = ctx->request->path;
                    if (path.find("/api/v1/setup") != 0) allow = false;
                }
                if (allow) {
                    tl_actor.username = "bootstrap";
                    tl_actor.role = "superadmin";
                    return "admin";
                }
            }
            auto it = users_.find(tok);
            if (it != users_.end()) {
                tl_actor.username = "token";
                tl_actor.role = it->second;
                return it->second;
            }
        }
        return "";
    }

    return "";
}

static int roleWeight(const std::string& role) {
    if (role == "admin") return ROLE_ADMIN;
    if (role == "operator") return ROLE_OPERATOR;
    if (role == "viewer") return ROLE_VIEWER;
    return 0;
}

int AdminServer::requireRole(const HttpContextPtr& ctx, int need, std::string& role) {
    role = roleOf(ctx);
    int w = roleWeight(role);
    if (w == 0) {
        ctx->response->status_code = HTTP_STATUS_UNAUTHORIZED;
        ctx->response->content_type = APPLICATION_JSON;
        ctx->response->body = "{\"error\":\"unauthorized\"}";
        return 401;
    }
    if (w < need) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->response->content_type = APPLICATION_JSON;
        ctx->response->body = "{\"error\":\"forbidden\",\"need_role\":" + std::to_string(need) + "}";
        return 403;
    }
    return 0;
}

bool AdminServer::wsAuthorize(const HttpRequestPtr& req, AdminActor& actor, std::string& role) {
    actor = AdminActor{};
    role.clear();
    const HttpCookie& ck = req->GetCookie("rt_sid");
    if (!ck.name.empty() && db_.ok()) {
        DbUser u;
        if (db_.getSession(AdminDb::sha256Hex(ck.value), u, AdminDb::nowMs())) {
            actor.id = u.id; actor.username = u.username; actor.role = u.role;
            role = (u.role == "superadmin") ? "admin" : (u.role == "admin") ? "operator" : "viewer";
            return true;
        }
    }
    // 兼容：紧急 token / 初始化前的 token
    std::string auth = req->GetHeader("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.compare(0, prefix.size(), prefix) == 0) {
        std::string tok = auth.substr(prefix.size());
        bool initialized = db_.ok() && db_.initialized();
        if (!cfg_.token.empty() && tok == cfg_.token && !initialized) {
            actor.id = 0; actor.username = "bootstrap"; actor.role = "superadmin";
            role = "admin";
            return true;
        }
        auto it = users_.find(tok);
        if (it != users_.end()) {
            actor.id = 0; actor.username = "token"; actor.role = it->second;
            role = it->second;
            return true;
        }
    }
    return false;
}

void AdminServer::audit(const std::string& role, const std::string& action, const std::string& detail) {
    audit_.write(role, action, detail);
}

// ---------------------------------------------------------------- 前端

int AdminServer::serveFrontend(const HttpContextPtr& ctx, const char* name, const char* ctype) {
    ctx->response->headers["Cache-Control"] = "no-store";
    if (!cfg_.docroot.empty()) {
        std::string path = cfg_.docroot + "/" + name;
        FILE* fp = fopen(path.c_str(), "rb");
        if (fp) {
            fseek(fp, 0, SEEK_END);
            long n = ftell(fp);
            fseek(fp, 0, SEEK_SET);
            std::string body;
            body.resize(n > 0 ? n : 0);
            if (n > 0 && fread(&body[0], 1, n, fp) != (size_t)n) body.clear();
            fclose(fp);
            if (!body.empty()) {
                ctx->response->content_type = http_content_type_enum(ctype);
                ctx->response->body = body;
                return 200;
            }
        }
    }
    for (int i = 0; i < g_embedded_frontend_count; ++i) {
        const EmbeddedFile& f = g_embedded_frontend[i];
        if (strcmp(f.name, name) == 0) {
            ctx->response->content_type = http_content_type_enum(ctype);
            ctx->response->body.assign((const char*)f.data, f.len);
            return 200;
        }
    }
    ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
    return 404;
}

// ---------------------------------------------------------------- 路由与启动

void AdminServer::registerRoutes() {
    auto viewer = [this](const char* method, const char* path, void (AdminServer::*h)(const HttpContextPtr&)) {
        service_.Handle(method, path, [this, h](const HttpContextPtr& ctx) {
            std::string role;
            int rc = requireRole(ctx, ROLE_VIEWER, role);
            if (rc) return rc;
            (this->*h)(ctx);
            return 200;
        });
    };
    viewer("GET", "/api/v1/overview", &AdminServer::handleOverview);
    viewer("GET", "/api/v1/agents", &AdminServer::handleAgents);
    viewer("GET", "/api/v1/agents/:devid", &AdminServer::handleAgentDetail);
    viewer("GET", "/api/v1/clients", &AdminServer::handleClients);
    viewer("GET", "/api/v1/streams", &AdminServer::handleStreams);
    viewer("GET", "/api/v1/acl/file", &AdminServer::handleAclGet);
    viewer("GET", "/api/v1/audit", &AdminServer::handleAudit);
    viewer("GET", "/api/v1/certs", &AdminServer::handleCerts);
    viewer("GET", "/metrics", &AdminServer::handleMetrics);
    viewer("GET", "/api/v1/files", &AdminServer::handleFiles);
    viewer("GET", "/api/v1/files/:name", &AdminServer::handleFileDownload);

    service_.Handle("DELETE", "/api/v1/streams/:a_sid", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
        if (rc) return rc;
        handleKickStream(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/agents/:devid/kick", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
        if (rc) return rc;
        handleKickAgent(ctx, role);
        return 200;
    });
    service_.Handle("PUT", "/api/v1/acl/file", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
        if (rc) return rc;
        handleAclPut(ctx, role);
        return 200;
    });
    service_.Handle("POST", "/api/v1/acl/reload", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
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
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
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
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
        if (rc) return rc;
        handleUsers(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("POST", "/api/v1/users", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
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
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
        if (rc) return rc;
        handleUserStatus(ctx, currentActor());
        return 200;
    });
    service_.Handle("DELETE", "/api/v1/users/:id", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
        if (rc) return rc;
        handleUserDelete(ctx, currentActor());
        return 200;
    });
    service_.Handle("GET", "/api/v1/devices", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
        if (rc) return rc;
        handleDeviceList(ctx); return (int)ctx->response->status_code;
    });
    service_.Handle("PUT", "/api/v1/devices/:devid/owner", [this](const HttpContextPtr& ctx) {
        std::string role;
        int rc = requireRole(ctx, ROLE_OPERATOR, role);
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

int AdminServer::start(const AdminConfig& cfg) {
    cfg_ = cfg;

    {
        std::string db_err;
        if (db_.open(cfg_.db_path, db_err)) {
            logf_("[admin] db: %s", cfg_.db_path.c_str());
        } else {
            logf_("[admin] db open failed: %s (%s)", cfg_.db_path.c_str(), db_err.c_str());
        }
    }

    // 账户文件：每行 token:role（viewer/operator/admin），# 注释
    if (!cfg_.users.empty()) {
        FILE* fp = fopen(cfg_.users.c_str(), "r");
        if (fp == NULL) {
            logf_("[admin] cannot open users file %s", cfg_.users.c_str());
            return -1;
        }
        char line[512];
        while (fgets(line, sizeof(line), fp)) {
            std::string s(line);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
            if (s.empty() || s[0] == '#') continue;
            size_t colon = s.rfind(':');
            if (colon == std::string::npos) continue;
            std::string tok = s.substr(0, colon), role = s.substr(colon + 1);
            if (roleWeight(role) > 0 && !tok.empty()) users_[tok] = role;
        }
        fclose(fp);
    }

    if (!cfg_.cert_dir.empty()) {
        relay_->cert_dir = cfg_.cert_dir;
        mkdir(cfg_.cert_dir.c_str(), 0700);
        relay_->loadRevoked();
        audit_.init(cfg_.cert_dir + "/audit.log");
    }

    char host[64] = "127.0.0.1";
    int port = 9443;
    const char* colon = strrchr(cfg_.addr.c_str(), ':');
    if (colon) {
        snprintf(host, sizeof(host), "%.*s", (int)(colon - cfg_.addr.c_str()), cfg_.addr.c_str());
        port = atoi(colon + 1);
    } else {
        port = atoi(cfg_.addr.c_str());
    }
    // 安全护栏：非回环监听必须配置凭据
    bool loopback = strcmp(host, "127.0.0.1") == 0 || strcmp(host, "localhost") == 0 ||
                    strcmp(host, "::1") == 0;
    if (!loopback && cfg_.token.empty() && users_.empty()) {
        logf_("[admin] refuse to listen on %s without credentials", host);
        return -1;
    }

    registerRoutes();
    initTerminal();
    server_.setHost(host);
    server_.setPort(port);
    server_.setThreadNum(1);
    int rc = server_.start();
    if (rc != 0) {
        logf_("[admin] listen %s:%d failed rc=%d", host, port, rc);
        return rc;
    }
    logf_("[admin] listening on http://%s:%d/ (%s, users=%zu)", host, port,
          (cfg_.token.empty() && users_.empty()) ? "no auth (loopback)" : "bearer token", users_.size());
    return 0;
}

void AdminServer::stop() {
    server_.stop();
}
