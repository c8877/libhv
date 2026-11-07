// A/C 连接列表与设备详情（在 relay 线程内取快照，JSON 序列化后回 admin 线程）
#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <atomic>
#include <sys/stat.h>
#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#endif
#include <set>
#include <ctype.h>

#include <algorithm>

using nlohmann::json;

static void fillConnCommon(RelayConn* c, json& j) {
    j["remote_addr"] = c->remote_addr;
    j["connected_at_ms"] = c->connected_at_ms;
    j["last_seen_ms"] = c->last_seen_ms;
    j["bytes_in"] = c->bytes_in;
    j["bytes_out"] = c->bytes_out;
    int nstream = 0;
    for (RelayStream* s = c->streams; s; s = (c->role == rtnp::ROLE_AGENT ? s->next_c : s->next_a)) nstream++;
    j["streams"] = nstream;
}

void AdminServer::handleAgents(const HttpContextPtr& ctx) {
    bool all = (currentActor().role == "admin" || currentActor().role == "manager");
    std::vector<std::string> mine = myDevids();
    std::string body = callRelay([this, all, mine]() {
        json arr = json::array();
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (c->role != rtnp::ROLE_AGENT || !c->authed) continue;
            if (!all && std::find(mine.begin(), mine.end(), std::string(c->devid)) == mine.end()) continue;
            json j;
            j["devid"] = c->devid;
            j["online"] = true;   // 注册表在册即在线
            {
                int sh = 1;
                auto it = relay_->caps_shell.find(c->devid);
                if (it != relay_->caps_shell.end()) sh = it->second;
                j["caps_shell"] = sh;
            }
            fillConnCommon(c, j);
            arr.push_back(j);
        }
        return arr.dump();
    });
    ctx->send(body);
}

void AdminServer::handleAgentDetail(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    int found = 0;
    std::string body = callRelay([this, &devid, &found]() {
        RelayConn* c = relay_->findAgent(devid.c_str());
        if (c == NULL) return std::string("{}");
        found = 1;
        json j;
        j["devid"] = c->devid;
        j["online"] = true;
        fillConnCommon(c, j);
        j["streams"] = json::array();
        for (RelayStream* s = c->streams; s; s = s->next_c) {
            json st;
            st["a_user"] = s->a ? s->a->user : "";
            st["a_sid"] = s->a_sid;
            st["c_sid"] = s->c_sid;
            st["opened"] = (bool)s->opened;
            st["opened_at_ms"] = s->opened_at_ms;
            st["a_closed"] = (bool)s->a_closed;
            st["c_closed"] = (bool)s->c_closed;
            j["streams"].push_back(st);
        }
        return j.dump();
    });
    if (!found) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device not online\"}");
        return;
    }
    ctx->send(body);
}

static std::atomic<uint32_t> g_cfg_sid{0};

void AdminServer::handleAgentCaps(const HttpContextPtr& ctx, const AdminActor& actor) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    auto req = nlohmann::json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    nlohmann::json out = nlohmann::json::object();
    bool has_ports = req.contains("ports");
    bool has_shell = req.contains("shell");
    bool has_dir = req.contains("file_dir");
    if (!has_ports && !has_shell && !has_dir) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"nothing to update\"}");
        return;
    }
    if (has_ports) {
        if (!req["ports"].is_array() || req["ports"].size() > 32) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"ports must be array (max 32)\"}");
            return;
        }
        nlohmann::json arr = nlohmann::json::array();
        std::set<int> seen;
        for (auto& v : req["ports"]) {
            if (!v.is_number_integer()) { ctx->response->status_code = HTTP_STATUS_BAD_REQUEST; ctx->send("{\"error\":\"bad port\"}"); return; }
            int p = v.get<int>();
            if (p <= 0 || p > 65535) { ctx->response->status_code = HTTP_STATUS_BAD_REQUEST; ctx->send("{\"error\":\"port out of range\"}"); return; }
            if (seen.insert(p).second) arr.push_back(p);
        }
        out["ports"] = arr;
    }
    if (has_dir) {
        if (!req["file_dir"].is_string()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"file_dir must be string\"}");
            return;
        }
        std::string v = req["file_dir"].get<std::string>();
        if (v.empty() || v[0] != '/' || v.find("..") != std::string::npos || v.size() > 200) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad file_dir\"}");
            return;
        }
        out["file_dir"] = v;
    }
    if (has_shell) {
        if (actor.role != "admin") {
            ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
            ctx->send("{\"error\":\"shell capability requires admin\"}");
            return;
        }
        if (!req["shell"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"shell must be bool\"}");
            return;
        }
        out["shell"] = req["shell"].get<bool>();
    }
    uint32_t sid = ++g_cfg_sid;
    std::string body = out.dump();
    std::string sent = callRelay([this, &devid, sid, &body]() {
        return relay_->sendConfig(devid.c_str(), sid, body) ? std::string("1") : std::string("0");
    });
    if (sent != "1") {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device offline\"}");
        return;
    }
    std::string ack;
    int waits = settingInt("proxy.config_wait_ms", 2000) / 50;
    for (int i = 0; i < waits && ack.empty(); ++i) {
        usleep(50000);
        ack = callRelay([this, &devid, sid]() {
            return relay_->configAck(devid.c_str(), sid, nullptr);
        });
    }
    if (ack.empty()) {
        ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
        ctx->send("{\"error\":\"device did not respond\"}");
        return;
    }
    auto a = nlohmann::json::parse(ack, nullptr, false);
    if (a.is_discarded() || !a.value("ok", false)) {
        ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
        ctx->send("{\"error\":\"" + a.value("error", std::string("agent rejected")) + "\"}");
        return;
    }
    audit(actor.username, "agent.caps", devid + " " + body);
    ctx->send(ack);
}

static std::string fileSha256(const std::string& path) {
#if defined(WITH_OPENSSL)
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return std::string();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) EVP_DigestUpdate(ctx, buf, n);
    unsigned char md[32];
    unsigned int mdlen = 0;
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    EVP_MD_CTX_free(ctx);
    fclose(fp);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < mdlen; ++i) { out += hex[md[i] >> 4]; out += hex[md[i] & 0xF]; }
    return out;
#else
    (void)path;
    return std::string();
#endif
}

void AdminServer::handleAgentFilePush(const HttpContextPtr& ctx, const AdminActor& actor) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    auto req = nlohmann::json::parse(ctx->body(), nullptr, false);
    std::string name = req.is_discarded() ? "" : req.value("name", "");
    std::string dir = req.is_discarded() ? "" : req.value("dir", "");
    if (dir.size() > 128 || dir.find("..") != std::string::npos || (!dir.empty() && dir[0] == '/')) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad target dir\"}");
        return;
    }
    for (char c : dir) {
        if (!(isalnum((unsigned char)c) || c == '/' || c == '.' || c == '_' || c == '-')) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad target dir\"}");
            return;
        }
    }
    if (cfg_.file_dir.empty() || name.empty() || name.find("..") != std::string::npos ||
        name.find('/') != std::string::npos) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad file name\"}");
        return;
    }
    std::string path = cfg_.file_dir + "/" + name;
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 || st.st_size <= 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"file not found\"}");
        return;
    }
    std::string sha = fileSha256(path);
    if (sha.empty()) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"sha256 unavailable\"}");
        return;
    }
    uint32_t tid = 0;
    std::string err;
    std::string started = callRelay([this, &devid, &path, &dir, &name, &st, &sha, &err, &tid]() {
        return relay_->startFilePush(devid.c_str(), path.c_str(), dir.c_str(), name.c_str(),
                                     (uint64_t)st.st_size, sha, err, tid)
            ? std::string("1") : std::string("0");
    });
    if (started != "1") {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    audit(actor.username, "agent.file_push_start", devid + " " + name + " -> " + dir);
    nlohmann::json j;
    j["tid"] = tid;
    j["name"] = name;
    j["size"] = (uint64_t)st.st_size;
    ctx->response->status_code = HTTP_STATUS_ACCEPTED;
    ctx->send(j.dump());
}

void AdminServer::handleAgentFileStatus(const HttpContextPtr& ctx, const AdminActor& actor) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    uint32_t tid = (uint32_t)strtoul(ctx->param("tid").c_str(), NULL, 10);
    if (!tid) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad tid\"}");
        return;
    }
    std::string p = callRelay([this, tid]() { return relay_->pushProgress(tid); });
    auto j = nlohmann::json::parse(p, nullptr, false);
    if (j.is_discarded()) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"bad state\"}");
        return;
    }
    if (j.value("done", false) && j.value("ok", false)) {
        std::lock_guard<std::mutex> lk(push_mu_);
        if (pushed_done.insert(tid).second) {
            audit(actor.username, "agent.file_push_done", devid + " tid=" + std::to_string(tid));
        }
    }
    ctx->send(p);
}

void AdminServer::handleAgentSysinfo(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    callRelay([this, &devid]() {
        return relay_->sendSysinfoReq(devid.c_str()) ? std::string("1") : std::string("0");
    });
    std::string js;
    uint64_t ts = 0;
    int waits = settingInt("proxy.sysinfo_wait_ms", 2000) / 50;
    for (int i = 0; i < waits && js.empty(); ++i) {
        usleep(50000);
        js = callRelay([this, &devid, &ts]() {
            return relay_->sysinfoOf(devid.c_str(), &ts);
        });
    }
    if (js.empty()) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device offline or no response\"}");
        return;
    }
    std::string out = "{\"ts\":" + std::to_string(ts) + ",\"sysinfo\":" + js + "}";
    ctx->send(out);
}

void AdminServer::handleClients(const HttpContextPtr& ctx) {
    auto actor = currentActor();
    std::string body = callRelay([this, actor]() {
        json arr = json::array();
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (c->role != rtnp::ROLE_CLIENT || !c->authed || c->internal) continue;
            if ((actor.role == "operator" || actor.role == "viewer") && actor.username != c->user) continue;
            json j;
            j["user"] = c->user;
            fillConnCommon(c, j);
            arr.push_back(j);
        }
        return arr.dump();
    });
    ctx->send(body);
}
