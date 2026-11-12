#include "admin_server.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

using nlohmann::json;
#include <thread>
#include <chrono>

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
            return u.role;
        }
    }

    // 2) 首次初始化前，或显式配置了紧急 token 时，允许 Bearer
    bool initialized = db_.ok() && db_.initialized();
    if (!cfg_.token.empty()) {
        std::string auth = ctx->header("Authorization");
        const std::string prefix = "Bearer ";
        if (auth.compare(0, prefix.size(), prefix) == 0) {
            std::string tok = auth.substr(prefix.size());
            if (!cfg_.token.empty() && tok == cfg_.token) {
                bool allow = !initialized;
                if (allow) {
                    // 初始化前 token 只放行 setup 接口
                    if (ctx->request->path.find("/api/v1/setup") != 0) allow = false;
                }
                if (allow) {
                    tl_actor.username = "bootstrap";
                    tl_actor.role = "admin";
                    return "admin";
                }
            }
        }
        return "";
    }

    return "";
}

static int roleWeight(const std::string& role) {
    if (role == "admin") return ROLE_ADMIN;
    if (role == "manager") return ROLE_MANAGER;
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
    std::string origin = req->GetHeader("Origin");
    if (!origin.empty()) {
        std::string host = req->GetHeader("Host");
        if (host.empty() || origin.find(host) == std::string::npos) {
            logf_("[admin] ws origin mismatch: %s host=%s", origin.c_str(), host.c_str());
            return false;
        }
    }
    const HttpCookie& ck = req->GetCookie("rt_sid");
    if (!ck.name.empty() && db_.ok()) {
        DbUser u;
        if (db_.getSession(AdminDb::sha256Hex(ck.value), u, AdminDb::nowMs())) {
            actor.id = u.id; actor.username = u.username; actor.role = u.role;
            role = u.role;
            return true;
        }
    }
    // 初始化前仅放行 /api/v1/setup*
    std::string auth = req->GetHeader("Authorization");
    const std::string prefix = "Bearer ";
    if (auth.compare(0, prefix.size(), prefix) == 0) {
        std::string tok = auth.substr(prefix.size());
        bool initialized = db_.ok() && db_.initialized();
        if (!cfg_.token.empty() && tok == cfg_.token && !initialized) {
            actor.id = 0; actor.username = "bootstrap"; actor.role = "admin";
            role = "admin";
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
    if (name == nullptr || strstr(name, "..") != nullptr) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        return 404;
    }
    auto read_file = [](const std::string& path, std::string& body) -> bool {
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) return false;
        fseek(fp, 0, SEEK_END);
        long n = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        body.resize(n > 0 ? n : 0);
        bool ok = n > 0 && fread(&body[0], 1, n, fp) == (size_t)n;
        fclose(fp);
        if (!ok) body.clear();
        return ok;
    };
    if (!cfg_.docroot.empty()) {
        std::string path = cfg_.docroot + "/" + name;
        // 构建期生成的 .br/.gz 预压缩文件，按 Accept-Encoding 直接返回
        std::string ae = ctx->header("Accept-Encoding");
        const char* encoding = NULL;
        const char* ext = NULL;
        if (ae.find("br") != std::string::npos) {
            encoding = "br"; ext = ".br";
        } else if (ae.find("gzip") != std::string::npos) {
            encoding = "gzip"; ext = ".gz";
        }
        if (encoding) {
            std::string body;
            if (read_file(path + ext, body)) {
                ctx->response->content_type = http_content_type_enum(ctype);
                ctx->response->headers["Content-Encoding"] = encoding;
                ctx->response->headers["Vary"] = "Accept-Encoding";
                ctx->response->body = std::move(body);
                return 200;
            }
        }
        std::string body;
        if (read_file(path, body)) {
            ctx->response->content_type = http_content_type_enum(ctype);
            ctx->response->body = std::move(body);
            return 200;
        }
        // 指定 docroot 时以磁盘产物为准，不再回退内嵌前端
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        return 404;
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

int AdminServer::start(const AdminConfig& cfg) {
    cfg_ = cfg;

    {
        std::string db_err;
        if (db_.open(cfg_.db_path, db_err)) {
            loadSettings();
            db_.permMigrateLegacy();
            db_.permSyncDictIds();
            logf_("[admin] db: %s", cfg_.db_path.c_str());
        } else {
            logf_("[admin] db open failed: %s (%s)", cfg_.db_path.c_str(), db_err.c_str());
        }
    }

    if (!cfg_.cert_dir.empty()) {
        relay_->cert_dir = cfg_.cert_dir;
        mkdir(cfg_.cert_dir.c_str(), 0700);
        relay_->loadRevoked();
        audit_.init(cfg_.cert_dir + "/audit.log");
    }

    std::thread([this]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        refreshGroupAcl();
        refreshTokenCache();
    }).detach();

    db_.purgeExpiredSessions(AdminDb::nowMs());
    if (cfg_.addr.rfind("0.0.0.0", 0) == 0) {
        logf_("[admin] WARNING: 明文监听 %s，请使用反向代理 TLS 或绑定 127.0.0.1",
              cfg_.addr.c_str());
    }
    std::thread([this]() { distLoop(); }).detach();
    // Token 用量：relay 只保留内存计数，这里按周期拉快照落库
    std::thread([this]() {
        for (;;) {
            int ms = settingInt("token.usage_flush_ms", 5000);
            if (ms < 1000) ms = 1000;
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            if (!db_.ok()) continue;
            std::string snap = callRelay([this]() { return relay_->tokenUsageSnapshot(); });
            if (snap.empty() || snap[0] != '[') continue;
            std::string err;
            db_.clientTokenUsageApply(snap, err);
        }
    }).detach();
    std::thread([this]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            std::vector<std::string> devs;
            db_.ttyDevids(devs);
            for (auto& d : devs) {
                if (!relay_->hasAgent(d.c_str())) continue;
                std::string out;
                if (db_.ttyList(d, out)) relay_->pushTtyCfg(d.c_str(), out);
            }
            std::vector<std::string> mdevs;
            db_.metricSeriesDevids(mdevs);
            for (auto& d : mdevs) {
                if (!relay_->hasAgent(d.c_str())) continue;
                pushMetricSeriesConfig(d);
                std::string st = relay_->takeMetricStatus(d);
                auto sj = json::parse(st, nullptr, false);
                if (!sj.is_discarded() && sj.value("applied", false)) {
                    db_.metricSeriesAck(d, sj.value("revision", 0LL));
                }
            }
        }
    }).detach();
    std::thread([this]() {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::hours(1));
            cleanupAudit(settingInt("audit.retention_days", 30));
            cleanupUploads();
            cleanupTtyLogs();
            audit_.rotate(100 * 1024 * 1024);
        }
    }).detach();

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
    if (!loopback && cfg_.token.empty()) {
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
          cfg_.token.empty() ? "no auth (loopback)" : "bearer token", 0);
    return 0;
}

void AdminServer::stop() {
    server_.stop();
}
