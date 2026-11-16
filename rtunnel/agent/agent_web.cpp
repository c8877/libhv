#include "agent_web.hpp"
#include "agent_app.hpp"
#include "agent_web_html.hpp"
#include "cm_editor_js.hpp"
#include "common/gzip_util.hpp"
#include "fs_session.hpp"
#include "fs_ui_js.hpp"

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

} // namespace

int agentWebStart(AgentApp* app, const std::string& listen, std::string& err) {
    if (g_server) return 0;
    std::string host;
    int port = 0;
    if (!parseListen(listen, host, port, err)) return -1;

    // 静态页面只压一次；JSON 快照每次请求再压（体积小，开销可忽略）
    auto html_gz = std::make_shared<std::string>();
    bool html_gz_ok = gzipCompress(kAgentWebHtml, *html_gz, 9);
    auto js_gz = std::make_shared<std::string>();
    bool js_gz_ok = gzipCompress(kFsUiJs, *js_gz, 9);
    auto ed_gz = std::make_shared<std::string>();
    bool ed_gz_ok = gzipCompress(kCmEditorJs, *ed_gz, 9);

    auto* service = new HttpService;
    service->GET("/assets/cm-editor.js", [ed_gz, ed_gz_ok](HttpRequest* req, HttpResponse* resp) {
        resp->content_type = APPLICATION_JAVASCRIPT;
        resp->SetHeader("Cache-Control", "no-cache");
        resp->SetHeader("X-Content-Type-Options", "nosniff");
        resp->SetHeader("Vary", "Accept-Encoding");
        if (ed_gz_ok && acceptsGzip(req)) {
            resp->SetHeader("Content-Encoding", "gzip");
            resp->body = *ed_gz;
        }
        else {
            resp->body = kCmEditorJs;
        }
        return 200;
    });
    service->GET("/assets/fs-ui.js", [js_gz, js_gz_ok](HttpRequest* req, HttpResponse* resp) {
        resp->content_type = APPLICATION_JAVASCRIPT;
        resp->SetHeader("Cache-Control", "no-cache");
        resp->SetHeader("X-Content-Type-Options", "nosniff");
        resp->SetHeader("Vary", "Accept-Encoding");
        if (js_gz_ok && acceptsGzip(req)) {
            resp->SetHeader("Content-Encoding", "gzip");
            resp->body = *js_gz;
        }
        else {
            resp->body = kFsUiJs;
        }
        return 200;
    });
    service->GET("/", [html_gz, html_gz_ok](HttpRequest* req, HttpResponse* resp) {
        setNoCacheHeaders(resp);
        resp->content_type = TEXT_HTML;
        if (html_gz_ok && acceptsGzip(req)) {
            resp->SetHeader("Content-Encoding", "gzip");
            resp->body = *html_gz;
        }
        else {
            resp->body = kAgentWebHtml;
        }
        return 200;
    });
    service->GET("/api/v1/sysinfo", [app](HttpRequest* req, HttpResponse* resp) {
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
    service->GET("/api/v1/health", [](HttpRequest*, HttpResponse* resp) {
        resp->content_type = APPLICATION_JSON;
        resp->SetHeader("Cache-Control", "no-store");
        resp->body = "{\"ok\":true}";
        return 200;
    });
    service->GET("/favicon.ico", [](HttpRequest*, HttpResponse*) { return 204; });

    auto* ws = new WebSocketService;
    ws->onopen = [app](const WebSocketChannelPtr& ch, const HttpRequestPtr& req) {
        if (req->path != "/ws/fs" || !sameOrigin(req.get()) || !app->fsReady()) {
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
    };
    ws->onmessage = [](const WebSocketChannelPtr& ch, const std::string& msg) {
        std::shared_ptr<LocalFsConn> conn;
        {
            std::lock_guard<std::mutex> lk(g_fs_mu);
            auto it = g_fs_conns.find(ch.get());
            if (it == g_fs_conns.end()) return;
            conn = it->second;
        }
        if (!conn->session) return;
        if (ch->opcode == WS_OPCODE_BINARY)
            conn->session->onBinary(msg);
        else
            conn->session->onText(msg);
    };
    ws->onclose = [](const WebSocketChannelPtr& ch) {
        std::shared_ptr<LocalFsConn> conn;
        {
            std::lock_guard<std::mutex> lk(g_fs_mu);
            auto it = g_fs_conns.find(ch.get());
            if (it == g_fs_conns.end()) return;
            conn = it->second;
            g_fs_conns.erase(it);
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
    http_server_stop(g_server);
    delete g_server;
    delete g_service;
    delete g_ws;
    g_server = nullptr;
    g_service = nullptr;
    g_ws = nullptr;
}
