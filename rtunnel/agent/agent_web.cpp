#include "agent_web.hpp"
#include "agent_app.hpp"
#include "agent_web_html.hpp"
#include "cm_editor_js.hpp"
#include "common/gzip_util.hpp"
#include "fs_session.hpp"
#include "fs_ui_js.hpp"
#include "file_view_js.hpp"
#include "svc_session.hpp"
#include "svc_ui_js.hpp"

#include "HttpServer.h"
#include "HttpService.h"
#include "WebSocketServer.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <string>

using namespace hv;

namespace {

HttpService* g_service = nullptr;
HttpServer* g_server = nullptr;
WebSocketService* g_ws = nullptr;

struct LocalFsConn;

class LocalSink : public rtfs::Sink {
public:
    explicit LocalSink(LocalFsConn* c) : conn_(c) {}
    void send(bool binary, const std::string& msg) override;

private:
    LocalFsConn* conn_;
};

struct LocalFsConn {
    WebSocketChannelPtr ws;
    std::unique_ptr<LocalSink> sink;
    std::unique_ptr<rtfs::Session> session;
};

std::mutex g_fs_mu;
std::map<WebSocketChannel*, std::shared_ptr<LocalFsConn>> g_fs_conns;

struct LocalSvcConn;

class LocalSvcSink : public rtvs::Sink {
public:
    explicit LocalSvcSink(LocalSvcConn* c) : conn_(c) {}
    void send(const std::string& msg) override;

private:
    LocalSvcConn* conn_;
};

struct LocalSvcConn {
    WebSocketChannelPtr ws;
    std::unique_ptr<LocalSvcSink> sink;
    std::unique_ptr<rtvs::Session> session;
};

std::mutex g_svc_mu;
std::map<WebSocketChannel*, std::shared_ptr<LocalSvcConn>> g_svc_conns;

void LocalSvcSink::send(const std::string& msg) {
    if (conn_ && conn_->ws && conn_->ws->isConnected()) {
        conn_->ws->send(msg, WS_OPCODE_TEXT);
    }
}

void LocalSink::send(bool binary, const std::string& msg) {
    if (conn_ && conn_->ws && conn_->ws->isConnected()) {
        conn_->ws->send(msg, binary ? WS_OPCODE_BINARY : WS_OPCODE_TEXT);
    }
}

// "8080" / "127.0.0.1:8080" / ":8080" / "0.0.0.0:8080" / "[::1]:8080"
bool parseListen(const std::string& spec, std::string& host, int& port, std::string& err) {
    std::string s = spec.empty() ? "127.0.0.1:8080" : spec;
    std::string pstr;
    bool had_host = false;
    if (s[0] == '[') {
        size_t close = s.find(']');
        if (close == std::string::npos) {
            err = "监听地址缺少 ']': " + s;
            return false;
        }
        host = s.substr(1, close - 1);
        had_host = true;
        if (close + 1 < s.size()) {
            if (s[close + 1] != ':') {
                err = "监听地址格式应为 [ADDR]:PORT: " + s;
                return false;
            }
            pstr = s.substr(close + 2);
        }
    }
    else {
        size_t pos = s.rfind(':');
        if (pos == std::string::npos) {
            pstr = s;
        }
        else {
            host = s.substr(0, pos);
            had_host = true;
            pstr = s.substr(pos + 1);
        }
    }
    if (!had_host)
        host = "127.0.0.1";
    else if (host.empty())
        host = "0.0.0.0";

    char* end = nullptr;
    long p = pstr.empty() ? 0 : strtol(pstr.c_str(), &end, 10);
    if (pstr.empty() || end == nullptr || *end != '\0' || p <= 0 || p > 65535) {
        err = "端口无效: " + pstr;
        return false;
    }
    port = (int)p;
    return true;
}

// Accept-Encoding: gzip / gzip;q=0.5 / br, gzip;q=0 / *
bool acceptsGzip(HttpRequest* req) {
    std::string ae = req->GetHeader("Accept-Encoding");
    size_t pos = 0;
    while (pos < ae.size()) {
        size_t comma = ae.find(',', pos);
        std::string item = ae.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t b = item.find_first_not_of(" \t");
        size_t e = item.find_last_not_of(" \t");
        if (b == std::string::npos) {
            if (comma == std::string::npos) break;
            pos = comma + 1;
            continue;
        }
        item = item.substr(b, e - b + 1);
        size_t semi = item.find(';');
        std::string token = item.substr(0, semi);
        for (size_t i = 0; i < token.size(); ++i) token[i] = (char)tolower((unsigned char)token[i]);
        if (token == "gzip" || token == "*") {
            if (semi != std::string::npos) {
                size_t q = item.find("q=", semi);
                if (q != std::string::npos && atof(item.c_str() + q + 2) <= 0) return false;
            }
            return true;
        }
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return false;
}

void setNoCacheHeaders(HttpResponse* resp) {
    resp->SetHeader("Cache-Control", "no-store");
    resp->SetHeader("X-Content-Type-Options", "nosniff");
    resp->SetHeader("Vary", "Accept-Encoding");
}

// 浏览器 WS 会带 Origin；只允许与当前 Host 同源，避免跨站劫持。
// 异步 JSON API 的统一响应：no-store + 可选 gzip。
// 处理函数跑在 hv::async 线程池，进程双采样/IPC 采集阻塞不会卡住 HTTP 与 WebSocket。
void sendJson(HttpRequest* req, const HttpResponseWriterPtr& writer, const std::string& body) {
    writer->Begin();
    writer->WriteHeader("Cache-Control", "no-store");
    writer->WriteHeader("X-Content-Type-Options", "nosniff");
    writer->WriteHeader("Vary", "Accept-Encoding");
    writer->WriteHeader("Content-Type", "application/json");
    if (body.size() > 2048 && acceptsGzip(req)) {
        std::string gz;
        if (gzipCompress(body, gz, 6)) {
            writer->WriteHeader("Content-Encoding", "gzip");
            writer->End(gz);
            return;
        }
    }
    writer->End(body);
}

bool sameOrigin(HttpRequest* req) {
    std::string origin = req->GetHeader("Origin");
    if (origin.empty()) return true;
    std::string host = req->GetHeader("Host");
    if (host.empty()) return false;
    size_t p = origin.find("://");
    std::string rest = p == std::string::npos ? origin : origin.substr(p + 3);
    size_t slash = rest.find('/');
    if (slash != std::string::npos) rest = rest.substr(0, slash);
    return rest == host;
}

// 内嵌前端资源：构建时已 gzip，浏览器直接拿压缩体；
// 非 gzip 客户端首次访问时解压一份缓存，之后复用。
struct WebAsset {
    WebAsset(const unsigned char* b, unsigned int n, bool g) : blob(b), len(n), gzip(g) {}
    const unsigned char* blob;
    unsigned int len;
    bool gzip;
    std::mutex mu;
    std::string plain;
    bool plain_ready = false;
};

WebAsset g_html(kAgentWebBlob, kAgentWebBlobLen, kAgentWebBlobGzip);
WebAsset g_fs(kFsUiBlob, kFsUiBlobLen, kFsUiBlobGzip);
WebAsset g_svc(kSvcUiBlob, kSvcUiBlobLen, kSvcUiBlobGzip);
WebAsset g_fileView(kFileViewBlob, kFileViewBlobLen, kFileViewBlobGzip);
WebAsset g_editor(kCmEditorBlob, kCmEditorBlobLen, kCmEditorBlobGzip);

void serveAsset(HttpRequest* req, HttpResponse* resp, WebAsset& a, http_content_type type) {
    resp->content_type = type;
    if (!a.gzip) {
        resp->body.assign((const char*)a.blob, a.len);
        return;
    }
    if (acceptsGzip(req)) {
        resp->SetHeader("Content-Encoding", "gzip");
        resp->body.assign((const char*)a.blob, a.len);
        return;
    }
    std::lock_guard<std::mutex> lk(a.mu);
    if (!a.plain_ready) a.plain_ready = gzipDecompress((const char*)a.blob, a.len, a.plain);
    resp->body = a.plain;
}

} // namespace

int agentWebStart(AgentApp* app, const std::string& listen, std::string& err) {
    if (g_server) return 0;
    std::string host;
    int port = 0;
    if (!parseListen(listen, host, port, err)) return -1;

    // 静态资源在构建时已压成 gzip，这里只按 Accept-Encoding 决定发压缩体还是解压后的正文
    auto* service = new HttpService;
    auto serveJsAsset = [](WebAsset& asset) {
        return [&asset](HttpRequest* req, HttpResponse* resp) {
            resp->SetHeader("Cache-Control", "no-cache");
            resp->SetHeader("X-Content-Type-Options", "nosniff");
            resp->SetHeader("Vary", "Accept-Encoding");
            serveAsset(req, resp, asset, APPLICATION_JAVASCRIPT);
            return 200;
        };
    };
    service->GET("/assets/cm-editor.js", serveJsAsset(g_editor));
    service->GET("/assets/fs-ui.js", serveJsAsset(g_fs));
    service->GET("/assets/svc-ui.js", serveJsAsset(g_svc));
    service->GET("/assets/file-view.js", serveJsAsset(g_fileView));
    service->GET("/", [app](HttpRequest* req, HttpResponse* resp) {
        // 页面一打开就预热进程/服务缓存，切到对应 tab 时不用等 systemctl
        app->requestWarm(90000);
        setNoCacheHeaders(resp);
        serveAsset(req, resp, g_html, TEXT_HTML);
        return 200;
    });
    service->GET("/api/v1/sysinfo", [app](HttpRequest* req, HttpResponse* resp) {
        // 页面打开期间靠 2s 轮询续期，关闭后 90s 内自然停止
        app->requestWarm(90000);
        setNoCacheHeaders(resp);
        resp->content_type = APPLICATION_JSON;
        std::string body = app->webSnapshot();
        if (acceptsGzip(req)) {
            std::string gz;
            if (gzipCompress(body, gz, 6)) {
                resp->SetHeader("Content-Encoding", "gzip");
                resp->body.swap(gz);
                return 200;
            }
        }
        resp->body.swap(body);
        return 200;
    });
    service->GET("/api/v1/procs", [app](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        nlohmann::json q;
        q["sort"] = req->GetParam("sort", "cpu");
        q["q"] = req->GetParam("q");
        q["kernel"] = req->GetParam("kernel", "0") == "1";
        q["page"] = atoi(req->GetParam("page", "1").c_str());
        q["page_size"] = atoi(req->GetParam("page_size", "10").c_str());
        q["refresh"] = req->GetParam("refresh", "0") == "1";
        sendJson(req.get(), writer, app->procsListJson(q));
    });
    service->GET("/api/v1/net", [app](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        nlohmann::json q;
        q["scope"] = req->GetParam("scope", "all");
        q["proc"] = req->GetParam("proc", "1") != "0";
        q["page"] = atoi(req->GetParam("page", "1").c_str());
        q["page_size"] = atoi(req->GetParam("page_size", "10").c_str());
        q["q"] = req->GetParam("q");
        q["listen"] = req->GetParam("listen", "0") == "1";
        q["utype"] = req->GetParam("utype", "all");
        q["ipc_kind"] = req->GetParam("ipc_kind", "shm");
        q["refresh"] = req->GetParam("refresh", "0") == "1";
        sendJson(req.get(), writer, app->netSnapshotJson(q));
    });
    service->GET("/api/v1/metrics", [app](const HttpRequestPtr& req, const HttpResponseWriterPtr& writer) {
        std::string series = req->GetParam("series");
        if (series.empty()) {
            sendJson(req.get(), writer, "{\"error\":\"series required\"}");
            return;
        }
        nlohmann::json q;
        q["series"] = series;
        q["since_ts"] = atoll(req->GetParam("since_ts", "0").c_str());
        sendJson(req.get(), writer, app->metricPointsJson(q));
    });
    service->GET("/api/v1/health", [](HttpRequest*, HttpResponse* resp) {
        resp->content_type = APPLICATION_JSON;
        resp->SetHeader("Cache-Control", "no-store");
        resp->body = "{\"ok\":true}";
        return 200;
    });
    service->GET("/favicon.ico", [](HttpRequest*, HttpResponse*) { return 204; });

    auto* ws = new WebSocketService;
    ws->onopen = [app](const WebSocketChannelPtr& ch, const HttpRequestPtr& req) {
        if (!sameOrigin(req.get())) {
            ch->close();
            return;
        }
        if (req->path == "/ws/fs") {
            if (!app->fsReady()) {
                ch->close();
                return;
            }
            auto conn = std::make_shared<LocalFsConn>();
            conn->ws = ch;
            conn->sink.reset(new LocalSink(conn.get()));
            conn->session.reset(new rtfs::Session(&app->fsService(), conn->sink.get(), app->devid, false, true));
            conn->session->setUnlimited(true);
            std::lock_guard<std::mutex> lk(g_fs_mu);
            g_fs_conns[ch.get()] = conn;
            return;
        }
        if (req->path == "/ws/svc") {
            if (!app->svcReady()) {
                ch->close();
                return;
            }
            // 直连 WS（例如深链直接打开服务页）时也保证缓存已在刷新
            app->requestWarm(90000);
            auto conn = std::make_shared<LocalSvcConn>();
            conn->ws = ch;
            conn->sink.reset(new LocalSvcSink(conn.get()));
            conn->session.reset(new rtvs::Session(&app->svcService(), conn->sink.get(), app->loop,
                                                  app->devid, false));
            std::lock_guard<std::mutex> lk(g_svc_mu);
            g_svc_conns[ch.get()] = conn;
            return;
        }
        ch->close();
    };
    ws->onmessage = [](const WebSocketChannelPtr& ch, const std::string& msg) {
        {
            std::shared_ptr<LocalFsConn> conn;
            {
                std::lock_guard<std::mutex> lk(g_fs_mu);
                auto it = g_fs_conns.find(ch.get());
                if (it != g_fs_conns.end()) conn = it->second;
            }
            if (conn) {
                if (!conn->session) return;
                if (ch->opcode == WS_OPCODE_BINARY)
                    conn->session->onBinary(msg);
                else
                    conn->session->onText(msg);
                return;
            }
        }
        std::shared_ptr<LocalSvcConn> conn;
        {
            std::lock_guard<std::mutex> lk(g_svc_mu);
            auto it = g_svc_conns.find(ch.get());
            if (it == g_svc_conns.end()) return;
            conn = it->second;
        }
        if (conn->session) conn->session->onText(msg);
    };
    ws->onclose = [](const WebSocketChannelPtr& ch) {
        {
            std::shared_ptr<LocalFsConn> conn;
            {
                std::lock_guard<std::mutex> lk(g_fs_mu);
                auto it = g_fs_conns.find(ch.get());
                if (it != g_fs_conns.end()) {
                    conn = it->second;
                    g_fs_conns.erase(it);
                }
            }
            if (conn) {
                if (conn->session) conn->session->onClose();
                return;
            }
        }
        std::shared_ptr<LocalSvcConn> conn;
        {
            std::lock_guard<std::mutex> lk(g_svc_mu);
            auto it = g_svc_conns.find(ch.get());
            if (it == g_svc_conns.end()) return;
            conn = it->second;
            g_svc_conns.erase(it);
        }
        if (conn->session) conn->session->onClose();
    };

    auto* server = new HttpServer(service);
    server->ws = ws;
    server->setHost(host.c_str());
    server->setPort(port);
    server->setThreadNum(1);
    int rc = http_server_run(server, 0);
    if (rc != 0) {
        err = "监听 " + host + ":" + std::to_string(port) + " 失败（端口被占用或地址不可用）";
        delete server;
        delete service;
        delete ws;
        return -1;
    }
    g_server = server;
    g_service = service;
    g_ws = ws;
    return 0;
}

void agentWebStop() {
    if (!g_server) return;
    {
        std::lock_guard<std::mutex> lk(g_fs_mu);
        for (auto& kv : g_fs_conns) {
            if (kv.second->session) kv.second->session->onClose();
        }
        g_fs_conns.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_svc_mu);
        for (auto& kv : g_svc_conns) {
            if (kv.second->session) kv.second->session->onClose();
        }
        g_svc_conns.clear();
    }
    http_server_stop(g_server);
    delete g_server;
    delete g_service;
    delete g_ws;
    g_server = nullptr;
    g_service = nullptr;
    g_ws = nullptr;
}
