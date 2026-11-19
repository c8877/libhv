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
    bool all = !scopeRestricted();
    std::vector<std::string> mine = myDevids();
    std::vector<std::tuple<std::string, std::string>> seen;
    std::string body = callRelay([this, all, mine, &seen]() {
        json arr = json::array();
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (c->role != rtnp::ROLE_AGENT || !c->authed) continue;
            if (!all && std::find(mine.begin(), mine.end(), std::string(c->devid)) == mine.end()) continue;
            json j;
            j["devid"] = c->devid;
            j["online"] = true; // 注册表在册即在线
            {
                int sh = 1;
                auto it = relay_->caps_shell.find(c->devid);
                if (it != relay_->caps_shell.end()) sh = it->second;
                j["caps_shell"] = sh;
            }
            {
                auto it = relay_->caps_ota.find(c->devid);
                j["caps_ota"] = it == relay_->caps_ota.end() ? -1 : it->second;
                j["caps_arch"] = relay_->agentArch(c->devid);
                auto vt = relay_->agent_versions.find(c->devid);
                j["version"] = vt == relay_->agent_versions.end() ? "" : vt->second;
            }
            fillConnCommon(c, j);
            seen.emplace_back(c->devid, j.value("version", std::string()));
            arr.push_back(j);
        }
        return arr.dump();
    });
    // 设备资源表跟随全量在线列表刷新；受限账号只看到部分设备，不能据此标记其它设备离线
    if (all) {
        std::vector<std::string> online;
        long long now = AdminDb::nowMs();
        for (auto& s : seen) {
            db_.resDeviceSync(std::get<0>(s), "", std::get<1>(s), 1, now);
            online.push_back(std::get<0>(s));
        }
        db_.resDevicesMarkOffline(online);
    }
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
    bool has_fs = req.contains("fs");
    bool has_fs_root = req.contains("fs_root");
    bool has_fs_rw = req.contains("fs_rw");
    bool has_fs_max = req.contains("fs_max_mb");
    bool has_svc = req.contains("svc");
    bool has_svc_rw = req.contains("svc_rw");
    bool has_ota = req.contains("ota");
    bool has_tty = req.contains("allow_tty");
    if (!has_ports && !has_shell && !has_dir && !has_fs && !has_fs_root && !has_fs_rw && !has_fs_max &&
        !has_svc && !has_svc_rw && !has_ota && !has_tty) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"nothing to update\"}");
        return;
    }
    // 每个字段组独立资源：能力配置 / OTA 开关 / 串口白名单
    if ((has_ports || has_shell || has_dir || has_fs || has_fs_root || has_fs_rw || has_fs_max ||
         has_svc || has_svc_rw) &&
        !hasPerm("device.caps.write")) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\",\"need\":\"device.caps.write\"}");
        return;
    }
    if (has_ota && !hasPerm("device.ota.write")) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\",\"need\":\"device.ota.write\"}");
        return;
    }
    if (has_tty && !hasPerm("device.tty.write")) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\",\"need\":\"device.tty.write\"}");
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
            if (!v.is_number_integer()) {
                ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
                ctx->send("{\"error\":\"bad port\"}");
                return;
            }
            int p = v.get<int>();
            if (p <= 0 || p > 65535) {
                ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
                ctx->send("{\"error\":\"port out of range\"}");
                return;
            }
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
    if (has_fs) {
        if (!req["fs"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"fs must be bool\"}");
            return;
        }
        out["fs"] = req["fs"].get<bool>();
    }
    if (has_fs_root) {
        if (!req["fs_root"].is_string()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"fs_root must be string\"}");
            return;
        }
        std::string v = req["fs_root"].get<std::string>();
        if (v.empty() || v[0] != '/' || v.find("..") != std::string::npos || v.size() > 512) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad fs_root\"}");
            return;
        }
        out["fs_root"] = v;
    }
    if (has_fs_rw) {
        if (!req["fs_rw"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"fs_rw must be bool\"}");
            return;
        }
        out["fs_rw"] = req["fs_rw"].get<bool>();
    }
    if (has_fs_max) {
        if (!req["fs_max_mb"].is_number_integer()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"fs_max_mb must be integer\"}");
            return;
        }
        int v = req["fs_max_mb"].get<int>();
        if (v <= 0 || v > 10240) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"fs_max_mb must be 1..10240\"}");
            return;
        }
        out["fs_max_mb"] = v;
    }
    if (has_svc) {
        if (!req["svc"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"svc must be bool\"}");
            return;
        }
        out["svc"] = req["svc"].get<bool>();
    }
    if (has_svc_rw) {
        if (!req["svc_rw"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"svc_rw must be bool\"}");
            return;
        }
        out["svc_rw"] = req["svc_rw"].get<bool>();
    }
    if (has_ota) {
        if (!req["ota"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"ota must be bool\"}");
            return;
        }
        out["ota"] = req["ota"].get<bool>();
    }
    if (has_tty) {
        if (!req["allow_tty"].is_array() || req["allow_tty"].size() > 16) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"allow_tty must be array (max 16)\"}");
            return;
        }
        nlohmann::json arr = nlohmann::json::array();
        size_t total = 0;
        for (auto& v : req["allow_tty"]) {
            if (!v.is_string()) {
                ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
                ctx->send("{\"error\":\"bad tty path\"}");
                return;
            }
            std::string p = v.get<std::string>();
            if (p.empty() || p[0] != '/' || p.size() > 200 || p.find("..") != std::string::npos) {
                ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
                ctx->send("{\"error\":\"bad tty path\"}");
                return;
            }
            total += p.size() + 1;
            arr.push_back(p);
        }
        if (total > 500) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"allow_tty too long\"}");
            return;
        }
        out["allow_tty"] = arr;
    }
    if (has_shell) {
        if (!req["shell"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"shell must be bool\"}");
            return;
        }
        out["shell"] = req["shell"].get<bool>();
    }
    uint32_t sid = ++g_cfg_sid;
    std::string body = out.dump();
    std::string sent = callRelay([this, &devid, sid, &body]() { return relay_->sendConfig(devid.c_str(), sid, body) ? std::string("1") : std::string("0"); });
    if (sent != "1") {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device offline\"}");
        return;
    }
    std::string ack;
    int waits = settingInt("proxy.config_wait_ms", 2000) / 50;
    for (int i = 0; i < waits && ack.empty(); ++i) {
        usleep(50000);
        ack = callRelay([this, &devid, sid]() { return relay_->configAck(devid.c_str(), sid, nullptr); });
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

void AdminServer::handleAgentSysinfo(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    bool inc = ctx->param("mode") == "inc";
    std::string cached;
    uint64_t ts = 0;
    cached = callRelay([this, &devid, &ts]() { return relay_->sysinfoOf(devid.c_str(), &ts); });
    if (cached.empty()) inc = false;
    bool online = callRelay([this, &devid]() {
                      return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
                  }) == "1";
    if (!online) {
        // 设备离线：直接回缓存，不为一个不可能到达的请求阻塞 2s
        if (cached.empty()) {
            ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
            ctx->send("{\"error\":\"device offline or no response\"}");
            return;
        }
        ctx->send("{\"ts\":0,\"mode\":\"full\",\"cached\":true,\"sysinfo\":" + cached + "}");
        return;
    }
    uint64_t start_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    callRelay([this, &devid, inc]() { return relay_->sendSysinfoReq(devid.c_str(), inc ? 1 : 0) ? std::string("1") : std::string("0"); });
    std::string js;
    int waits = settingInt("proxy.sysinfo_wait_ms", 2000) / 50;
    for (int i = 0; i < waits; ++i) {
        usleep(50000);
        js = callRelay([this, &devid, &ts]() { return relay_->sysinfoOf(devid.c_str(), &ts); });
        if (!js.empty() && ts >= start_ms) break;
        js.clear();
    }
    bool stale = false;
    if (js.empty()) {
        js = cached;
        ts = 0;
        stale = !js.empty();
    }
    if (js.empty()) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device offline or no response\"}");
        return;
    }
    bool last_full = true;
    uint64_t last_ts = 0;
    std::string last = callRelay([this, &devid, &last_full, &last_ts]() { return relay_->sysinfoLastOf(devid.c_str(), &last_full, &last_ts); });
    if (inc && !stale && !last.empty() && !last_full) {
        ctx->send("{\"ts\":" + std::to_string(last_ts) + ",\"mode\":\"inc\",\"sysinfo\":" + last + "}");
        return;
    }
    std::string out = "{\"ts\":" + std::to_string(ts) + ",\"mode\":\"full\"" + (stale ? ",\"cached\":true" : "") +
                      ",\"sysinfo\":" + js + "}";
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
