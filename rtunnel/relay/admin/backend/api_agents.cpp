// A/C 连接列表与设备详情（在 relay 线程内取快照，JSON 序列化后回 admin 线程）
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/hasync.h"
#include "cpputil/json.hpp"
#include "common/gzip_util.hpp"

#include <atomic>
#include <sys/stat.h>
#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#endif
#include <set>
#include <ctype.h>

#include <algorithm>
#include <thread>
#include <unistd.h>

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
                j["caps_ports"] = relay_->agentPorts(c->devid);
                j["caps_local_web"] = relay_->agentLocalWeb(c->devid);
                j["caps_net"] = relay_->agentNetCap(c->devid);
                j["caps_proc"] = relay_->agentProcCap(c->devid);
                auto vt = relay_->agent_versions.find(c->devid);
                j["version"] = vt == relay_->agent_versions.end() ? "" : vt->second;
            }
            j["boot_ms"] = relay_->agentBootMs(c->devid);
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
        // 详情页（含 5s 轮询）打开期间让 agent 预热进程与服务缓存
        relay_->warmDevice(c->devid);
        j["devid"] = c->devid;
        j["online"] = true;
        fillConnCommon(c, j);
        j["boot_ms"] = relay_->agentBootMs(c->devid);
        j["caps_ports"] = relay_->agentPorts(c->devid);
        j["caps_local_web"] = relay_->agentLocalWeb(c->devid);
        j["caps_net"] = relay_->agentNetCap(c->devid);
        j["caps_proc"] = relay_->agentProcCap(c->devid);
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
    bool has_reboot = req.contains("reboot");
    bool has_poweroff = req.contains("poweroff");
    bool has_tty = req.contains("allow_tty");
    if (!has_ports && !has_shell && !has_dir && !has_fs && !has_fs_root && !has_fs_rw && !has_fs_max &&
        !has_svc && !has_svc_rw && !has_ota && !has_reboot && !has_poweroff && !has_tty) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"nothing to update\"}");
        return;
    }
    // 每个字段组独立资源：能力配置 / OTA 开关 / 串口白名单
    if ((has_ports || has_shell || has_dir || has_fs || has_fs_root || has_fs_rw || has_fs_max ||
         has_svc || has_svc_rw || has_reboot || has_poweroff) &&
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
    if (has_reboot) {
        if (!req["reboot"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"reboot must be bool\"}");
            return;
        }
        out["reboot"] = req["reboot"].get<bool>();
    }
    if (has_poweroff) {
        if (!req["poweroff"].is_boolean()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"poweroff must be bool\"}");
            return;
        }
        out["poweroff"] = req["poweroff"].get<bool>();
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

int AdminServer::handleAgentSysinfo(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return HTTP_STATUS_FORBIDDEN;
    }
    bool inc = ctx->param("mode") == "inc";
    std::string cached;
    uint64_t cache_ts = 0;
    cached = callRelay([this, &devid, &cache_ts]() { return relay_->sysinfoOf(devid.c_str(), &cache_ts); });
    if (cached.empty()) inc = false;
    bool online = callRelay([this, &devid]() {
                      return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
                  }) == "1";
    if (!online) {
        // 设备离线：直接回缓存，不为一个不可能到达的请求等待
        if (cached.empty()) {
            ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
            ctx->send("{\"error\":\"device offline or no response\"}");
            return HTTP_STATUS_NOT_FOUND;
        }
        ctx->send("{\"ts\":" + std::to_string(cache_ts) + ",\"mode\":\"full\",\"cached\":true,\"sysinfo\":" + cached + "}");
        return HTTP_STATUS_OK;
    }

    uint64_t start_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    bool sent = callRelay([this, &devid, inc]() {
                    return relay_->sendSysinfoReq(devid.c_str(), inc ? 1 : 0) ? std::string("1") : std::string("0");
                }) == "1";
    if (!sent) {
        if (cached.empty()) {
            ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
            ctx->send("{\"error\":\"device offline or no response\"}");
            return HTTP_STATUS_NOT_FOUND;
        }
        ctx->send("{\"ts\":" + std::to_string(cache_ts) + ",\"mode\":\"full\",\"cached\":true,\"sysinfo\":" + cached + "}");
        return HTTP_STATUS_OK;
    }

    // 等 agent 回帧最多 sysinfo_wait_ms。管理台只有一个 HTTP 线程，这里不能阻塞，
    // 否则一个慢设备会卡住整个管理台；放到线程池等，回帧后直接写响应。
    int wait_ms = settingInt("proxy.sysinfo_wait_ms", 2000);
    if (wait_ms < 0) wait_ms = 0;
    hv::async([this, ctx, devid, inc, cached, cache_ts, start_ms, wait_ms]() {
        std::string js;
        uint64_t ts = 0;
        int waits = wait_ms / 50;
        for (int i = 0; i < waits; ++i) {
            usleep(50000);
            js = callRelay([this, &devid, &ts]() { return relay_->sysinfoOf(devid.c_str(), &ts); });
            if (!js.empty() && ts >= start_ms) break;
            js.clear();
        }
        bool stale = false;
        if (js.empty()) {
            js = cached;
            ts = cache_ts;
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
        ctx->send("{\"ts\":" + std::to_string(ts) + ",\"mode\":\"full\"" + (stale ? ",\"cached\":true" : "") +
                  ",\"sysinfo\":" + js + "}");
    });
    return HTTP_STATUS_NEXT;
}

int AdminServer::handleAgentNet(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return HTTP_STATUS_FORBIDDEN;
    }
    std::string scope = ctx->param("scope");
    if (scope.empty()) scope = "tcp";
    if (scope != "tcp" && scope != "udp" && scope != "unix" && scope != "ipc" && scope != "all") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad scope\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    bool with_proc = ctx->param("proc") != "0";
    bool refresh = ctx->param("refresh") == "1";
    int page = atoi(ctx->param("page").c_str());
    if (page <= 0) page = 1;
    int page_size = atoi(ctx->param("page_size").c_str());
    if (page_size <= 0) page_size = 10;
    if (page_size > 50) page_size = 50;
    std::string q = ctx->param("q");
    bool listen_only = ctx->param("listen") == "1";
    std::string utype = ctx->param("utype");
    if (utype.empty()) utype = "all";
    std::string ipc_kind = ctx->param("ipc_kind");
    if (ipc_kind != "msg" && ipc_kind != "sem" && ipc_kind != "posix") ipc_kind = "shm";
    std::string sort = ctx->param("sort");
    if (sort.size() > 32) sort.clear();
    int dir = atoi(ctx->param("dir").c_str());
    dir = dir > 0 ? 1 : (dir < 0 ? -1 : 0);
    std::string key = devid + "|" + scope + "|" + (with_proc ? "1" : "0") + "|" + std::to_string(page) +
                      "|" + std::to_string(page_size) + "|" + q + "|" + (listen_only ? "1" : "0") + "|" +
                      utype + "|" + ipc_kind + "|" + sort + "|" + std::to_string(dir);

    if (!refresh) {
        std::string cj;
        uint64_t cts = 0;
        bool hit = callRelay([this, &key, &cj, &cts]() {
                       return relay_->netCacheGet(key, 3000, cj, cts) ? std::string("1") : std::string("0");
                   }) == "1";
        if (hit) {
            json j = json::parse(cj, nullptr, false);
            if (!j.is_discarded()) {
                j["cached"] = true;
                j["cache_age_ms"] = (uint64_t)time(NULL) * 1000 - cts;
                ctx->send(j.dump());
            } else {
                ctx->send(cj);
            }
            return HTTP_STATUS_OK;
        }
    }

    bool online = callRelay([this, &devid]() {
                      return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
                  }) == "1";
    if (!online) {
        // 离线时给最后一次快照，不限制时长，前端会标出数据时间
        std::string cj;
        uint64_t cts = 0;
        bool hit = callRelay([this, &key, &cj, &cts]() {
                       return relay_->netCacheGet(key, ~0ULL, cj, cts) ? std::string("1") : std::string("0");
                   }) == "1";
        if (hit) {
            json j = json::parse(cj, nullptr, false);
            if (!j.is_discarded()) {
                j["cached"] = true;
                j["offline"] = true;
            }
            ctx->send(j.is_discarded() ? cj : j.dump());
            return HTTP_STATUS_OK;
        }
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device offline or no response\"}");
        return HTTP_STATUS_NOT_FOUND;
    }

    int cap = -1;
    callRelay([this, &devid, &cap]() {
        cap = relay_->agentNetCap(devid.c_str());
        return std::string("1");
    });
    if (cap != 1) {
        ctx->response->status_code = HTTP_STATUS_NOT_IMPLEMENTED;
        ctx->send("{\"error\":\"agent does not support net snapshot\",\"caps_net\":" +
                  std::to_string(cap) + "}");
        return HTTP_STATUS_NOT_IMPLEMENTED;
    }

    json req;
    req["scope"] = scope;
    req["proc"] = with_proc;
    req["page"] = page;
    req["page_size"] = page_size;
    req["q"] = q;
    req["listen"] = listen_only;
    req["utype"] = utype;
    req["ipc_kind"] = ipc_kind;
    req["sort"] = sort;
    req["dir"] = dir;
    req["refresh"] = refresh;
    std::string payload = req.dump();
    uint32_t rid = 0;
    std::string err;
    bool sent = callRelay([this, &devid, &payload, &rid, &err]() {
                    return relay_->startNetQuery(devid.c_str(), payload, rid, err) ? std::string("1")
                                                                                   : std::string("0");
                }) == "1";
    if (!sent) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + (err.empty() ? std::string("device offline") : err) + "\"}");
        return HTTP_STATUS_NOT_FOUND;
    }

    int wait_ms = settingInt("proxy.net_wait_ms", 3000);
    if (wait_ms < 0) wait_ms = 0;
    hv::async([this, ctx, key, rid, wait_ms]() {
        std::string js;
        int waits = wait_ms / 50;
        for (int i = 0; i < waits; ++i) {
            usleep(50000);
            js = callRelay([this, rid]() { return relay_->takeNetResult(rid); });
            if (!js.empty()) break;
        }
        if (js.empty()) {
            std::string cj;
            uint64_t cts = 0;
            bool hit = callRelay([this, &key, &cj, &cts]() {
                           return relay_->netCacheGet(key, ~0ULL, cj, cts) ? std::string("1") : std::string("0");
                       }) == "1";
            if (hit) {
                json j = json::parse(cj, nullptr, false);
                if (!j.is_discarded()) {
                    j["cached"] = true;
                    j["stale"] = true;
                }
                ctx->send(j.is_discarded() ? cj : j.dump());
                return;
            }
            ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
            ctx->send("{\"error\":\"timeout\"}");
            return;
        }
        callRelay([this, &key, &js]() {
            relay_->netCachePut(key, js);
            return std::string("1");
        });
        json j = json::parse(js, nullptr, false);
        if (!j.is_discarded()) j["cached"] = false;
        ctx->send(j.is_discarded() ? js : j.dump());
    });
    return HTTP_STATUS_NEXT;
}

int AdminServer::proxyAgentProc(const HttpContextPtr& ctx, const std::string& devid, const json& req) {
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return HTTP_STATUS_FORBIDDEN;
    }
    bool online = callRelay([this, &devid]() {
                      return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
                  }) == "1";
    if (!online) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device offline\"}");
        return HTTP_STATUS_NOT_FOUND;
    }
    int cap = -1;
    callRelay([this, &devid, &cap]() {
        cap = relay_->agentProcCap(devid.c_str());
        return std::string("1");
    });
    if (cap != 1) {
        ctx->response->status_code = HTTP_STATUS_NOT_IMPLEMENTED;
        ctx->send("{\"error\":\"agent does not support process snapshot\",\"caps_proc\":" +
                  std::to_string(cap) + "}");
        return HTTP_STATUS_NOT_IMPLEMENTED;
    }

    std::string payload = req.dump();
    uint32_t rid = 0;
    std::string err;
    bool sent = callRelay([this, &devid, &payload, &rid, &err]() {
                    return relay_->startProcQuery(devid.c_str(), payload, rid, err) ? std::string("1")
                                                                                     : std::string("0");
                }) == "1";
    if (!sent) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + (err.empty() ? std::string("device offline") : err) + "\"}");
        return HTTP_STATUS_NOT_FOUND;
    }

    bool signal = req.value("op", std::string()) == "signal";
    int wait_ms = settingInt(signal ? "proxy.proc_signal_wait_ms" : "proxy.proc_wait_ms", signal ? 2000 : 4000);
    if (wait_ms < 0) wait_ms = 0;
    hv::async([this, ctx, rid, wait_ms]() {
        std::string js;
        int waits = wait_ms / 50;
        for (int i = 0; i < waits; ++i) {
            usleep(50000);
            js = callRelay([this, rid]() { return relay_->takeProcResult(rid); });
            if (!js.empty()) break;
        }
        if (js.empty()) {
            ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
            ctx->send("{\"error\":\"timeout\"}");
            return;
        }
        ctx->send(js);
    });
    return HTTP_STATUS_NEXT;
}

// 同步执行并等待结果：exec 接口与 operations.apply/verify 共用。
// 必须在工作线程调用（最多阻塞 timeout_ms + 3s）。
bool AdminServer::execOnDevice(const std::string& devid, const std::string& payload, int timeout_ms,
                               json& out, std::string& err) {
    uint32_t rid = 0;
    bool sent = callRelay([this, &devid, &payload, &rid, &err]() {
                    return relay_->startExec(devid.c_str(), payload, rid, err) ? std::string("1")
                                                                              : std::string("0");
                }) == "1";
    if (!sent) {
        if (err.empty()) err = "device offline";
        return false;
    }
    std::string meta, raw;
    bool got = false;
    int waits = (timeout_ms + 3000) / 50;
    for (int i = 0; i < waits; ++i) {
        usleep(50000);
        got = callRelay([this, rid, &meta, &raw]() {
            return relay_->takeExecResult(rid, meta, raw) ? std::string("1") : std::string("0");
        }) == "1";
        if (got) break;
    }
    if (!got) {
        err = "timeout waiting for device";
        return false;
    }
    json r = json::parse(meta, nullptr, false);
    if (r.is_discarded() || !r.is_object()) r = json::object();

    // 输出块格式：u8 tag + u32 大端长度 + 数据；按序拼成 stdout/stderr
    std::string outbuf, errbuf;
    size_t i = 0;
    while (i + 5 <= raw.size()) {
        unsigned char tag = (unsigned char)raw[i];
        size_t n = ((size_t)(unsigned char)raw[i + 1] << 24) |
                   ((size_t)(unsigned char)raw[i + 2] << 16) |
                   ((size_t)(unsigned char)raw[i + 3] << 8) |
                   (size_t)(unsigned char)raw[i + 4];
        i += 5;
        if (n > raw.size() - i) n = raw.size() - i;
        (tag == 1 ? errbuf : outbuf).append(raw, i, n);
        i += n;
    }
    out = json::object();
    out["ok"] = r.value("ok", false);
    out["exit_code"] = r.value("exit_code", -1);
    out["signal"] = r.value("signal", 0);
    out["duration_ms"] = r.value("duration_ms", (long long)0);
    out["truncated"] = r.value("truncated", false);
    out["timed_out"] = r.value("timed_out", false);
    out["stdout_bytes"] = r.value("stdout_bytes", (long long)outbuf.size());
    out["stderr_bytes"] = r.value("stderr_bytes", (long long)errbuf.size());
    if (r.contains("error")) out["error"] = r["error"];
    // 设备输出可能不是合法 UTF-8：替换而不是抛异常
    out["stdout"] = json::parse(json(outbuf).dump(-1, ' ', false, json::error_handler_t::replace));
    out["stderr"] = json::parse(json(errbuf).dump(-1, ' ', false, json::error_handler_t::replace));
    return true;
}

// 受控命令执行。请求体 {"argv":["df","-h"],"cwd":"/tmp","timeout_ms":5000,"max_bytes":65536}，
// 不接受 shell 字符串：agent 侧按白名单逐段匹配 argv，再做 fork/exec。
int AdminServer::handleAgentExec(const HttpContextPtr& ctx, const std::string& role) {
    (void)role;   // 审计主体由 actorLabel 自动带上，这里只需要权限已通过
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return HTTP_STATUS_FORBIDDEN;
    }
    json body = json::parse(ctx->body(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        apiError(ctx, HTTP_STATUS_BAD_REQUEST, "bad json");
        return HTTP_STATUS_BAD_REQUEST;
    }
    if (body.contains("shell") && body["shell"].is_boolean() && body["shell"].get<bool>()) {
        apiError(ctx, HTTP_STATUS_BAD_REQUEST, "不支持 shell 模式，请传 argv 数组");
        return HTTP_STATUS_BAD_REQUEST;
    }
    std::vector<std::string> argv;
    if (body.contains("argv") && body["argv"].is_array()) {
        for (auto& v : body["argv"]) {
            if (!v.is_string()) {
                apiError(ctx, HTTP_STATUS_BAD_REQUEST, "argv 必须是字符串数组");
                return HTTP_STATUS_BAD_REQUEST;
            }
            argv.push_back(v.get<std::string>());
        }
    }
    if (argv.empty() || argv.size() > 64) {
        apiError(ctx, HTTP_STATUS_BAD_REQUEST, "argv 不能为空且最多 64 段");
        return HTTP_STATUS_BAD_REQUEST;
    }
    size_t total = 0;
    for (auto& a : argv) total += a.size() + 1;
    if (total > 8192) {
        apiError(ctx, HTTP_STATUS_BAD_REQUEST, "argv 过长");
        return HTTP_STATUS_BAD_REQUEST;
    }
    int timeout_ms = body.value("timeout_ms", 30000);
    if (timeout_ms < 1000) timeout_ms = 1000;
    // 直接执行接口是同步等待的，上限压到 30s；长任务走终端或 operations
    if (timeout_ms > 30000) {
        apiError(ctx, HTTP_STATUS_BAD_REQUEST, "timeout_ms 上限 30000，长任务请用终端");
        return HTTP_STATUS_BAD_REQUEST;
    }
    long long max_bytes = body.value("max_bytes", (long long)65536);
    if (max_bytes < 1024) max_bytes = 1024;
    if (max_bytes > 1048576) max_bytes = 1048576;

    json payload;
    payload["argv"] = argv;
    payload["timeout_ms"] = timeout_ms;
    payload["max_bytes"] = max_bytes;
    if (body.contains("cwd") && body["cwd"].is_string()) payload["cwd"] = body["cwd"];
    std::string js = payload.dump();

    bool online = callRelay([this, &devid]() {
                      return relay_->agentOnline(devid.c_str()) ? std::string("1") : std::string("0");
                  }) == "1";
    if (!online) {
        apiError(ctx, HTTP_STATUS_NOT_FOUND, "device offline");
        return HTTP_STATUS_NOT_FOUND;
    }
    int cap = -1;
    callRelay([this, &devid, &cap]() {
        cap = relay_->agentExecCap(devid.c_str());
        return std::string("1");
    });
    if (cap != 1) {
        apiError(ctx, HTTP_STATUS_NOT_IMPLEMENTED, "agent 未启用命令执行（需要 --allow-exec）");
        return HTTP_STATUS_NOT_IMPLEMENTED;
    }

    // 异步等待前先固化身份：审计线程里没有请求上下文，actorLabel() 会是空
    std::string who = actorLabel();
    if (who.empty()) who = "anonymous";

    // 单独起线程等待：命令可能跑满 30s，不能占 hv 的公共异步池
    std::thread([this, ctx, devid, js, timeout_ms, argv, who]() {
        json resp;
        std::string err;
        if (!execOnDevice(devid, js, timeout_ms, resp, err)) {
            ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
            apiError(ctx, HTTP_STATUS_GATEWAY_TIMEOUT, err);
            return;
        }
        std::string cmdline;
        for (size_t k = 0; k < argv.size(); ++k) {
            if (k) cmdline += ' ';
            cmdline += argv[k];
        }
        if (cmdline.size() > 300) cmdline = cmdline.substr(0, 300) + "…";
        std::string detail = devid + " `" + cmdline + "` exit=" +
                             std::to_string(resp["exit_code"].get<int>()) +
                             (resp["timed_out"].get<bool>() ? " timeout" : "");

        AuditEntry ae;
        ae.actor = who;
        ae.action = "device.exec";
        ae.target_type = "device";
        ae.target_id = devid;
        ae.result = resp["ok"].get<bool>() ? "ok" : "error";
        ae.detail = detail;
        audit_.writeEntry(ae);
        ctx->send(resp.dump(-1, ' ', false, json::error_handler_t::replace));
    }).detach();
    return HTTP_STATUS_NEXT;
}

int AdminServer::handleAgentPower(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    json body = json::parse(ctx->body(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    std::string action = body.value("action", std::string());
    if (action != "reboot" && action != "poweroff") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"action must be reboot or poweroff\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    std::string password = body.value("password", std::string());
    if (password.empty() || password.size() > 256) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"root password required\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    int delay = body.value("delay_ms", 1000);
    if (delay < 0) delay = 0;
    if (delay > 60000) delay = 60000;
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return HTTP_STATUS_FORBIDDEN;
    }
    audit(currentActor().username, "agent.power", devid + " " + action);
    json req;
    req["action"] = action;
    req["delay_ms"] = delay;
    req["password"] = password; // 只透传给 agent 校验，不落审计/日志
    std::string payload = req.dump();
    uint32_t rid = 0;
    std::string err;
    bool sent = callRelay([this, &devid, &payload, &rid, &err]() {
                    return relay_->startPowerAction(devid.c_str(), payload, rid, err) ? std::string("1")
                                                                                        : std::string("0");
                }) == "1";
    if (!sent) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + (err.empty() ? std::string("device offline") : err) + "\"}");
        return HTTP_STATUS_NOT_FOUND;
    }
    hv::async([this, ctx, rid]() {
        std::string js;
        for (int i = 0; i < 40; ++i) { // 最多 2s，agent 收到即回，实际远小于此
            usleep(50000);
            js = callRelay([this, rid]() { return relay_->takePowerResult(rid); });
            if (!js.empty()) break;
        }
        if (js.empty()) {
            ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
            ctx->send("{\"error\":\"timeout\"}");
            return;
        }
        ctx->send(js);
    });
    return HTTP_STATUS_NEXT;
}

int AdminServer::handleAgentDiag(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return HTTP_STATUS_FORBIDDEN;
    }
    audit(currentActor().username, "agent.diag", devid);
    json req;
    req["max_mb"] = 8;
    std::string payload = req.dump();
    uint32_t rid = 0;
    std::string err;
    bool sent = callRelay([this, &devid, &payload, &rid, &err]() {
                    return relay_->startDiag(devid.c_str(), payload, rid, err) ? std::string("1")
                                                                               : std::string("0");
                }) == "1";
    if (!sent) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + (err.empty() ? std::string("device offline") : err) + "\"}");
        return HTTP_STATUS_NOT_FOUND;
    }
    hv::async([this, ctx, rid]() {
        std::string meta, body;
        bool got = false;
        for (int i = 0; i < 600; ++i) { // 最多 30s：journal/tar 在单核设备上要几秒
            usleep(50000);
            got = callRelay([this, rid, &meta, &body]() { return relay_->takeDiag(rid, meta, body) ? std::string("1") : std::string("0"); }) == "1";
            if (got) break;
        }
        if (!got) {
            ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
            ctx->send("{\"error\":\"timeout\"}");
            return;
        }
        json m = json::parse(meta, nullptr, false);
        if (m.is_discarded() || !m.value("ok", false)) {
            ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
            ctx->send("{\"error\":\"" +
                      (m.is_discarded() ? std::string("bad diag response") : m.value("error", std::string("diag failed"))) +
                      "\"}");
            return;
        }
        std::string name = m.value("name", std::string("diag.tar.gz"));
        // 文件名只保留安全字符，防止头注入
        std::string safe;
        for (char c : name) {
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') safe += c;
        }
        if (safe.empty()) safe = "diag.tar.gz";
        ctx->response->status_code = HTTP_STATUS_OK;
        ctx->response->SetHeader("Content-Type", "application/gzip");
        ctx->response->SetHeader("Content-Disposition", "attachment; filename=\"" + safe + "\"");
        ctx->response->SetHeader("X-Diag-Sha256", m.value("sha256", std::string()));
        ctx->send(body);
    });
    return HTTP_STATUS_NEXT;
}

int AdminServer::handleAgentStorage(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return HTTP_STATUS_FORBIDDEN;
    }
    std::string mount = ctx->param("mount", "/");
    if (mount.empty() || mount[0] != '/' || mount.size() > 256) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad mount\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    // path 为空表示列分区根；depth 是相对 path 的回传层级
    std::string path = ctx->param("path");
    if (!path.empty()) {
        bool outside = mount != "/" && path != mount && path.rfind(mount + "/", 0) != 0;
        if (path[0] != '/' || path.size() > 1024 || outside ||
            path.find("/../") != std::string::npos ||
            (path.size() >= 3 && path.compare(path.size() - 3, 3, "/..") == 0)) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad path\"}");
            return HTTP_STATUS_BAD_REQUEST;
        }
    }
    int depth = atoi(ctx->param("depth", "1").c_str());
    if (depth < 0) depth = 0;
    if (depth > 64) depth = 64;
    json req;
    req["mount"] = mount;
    if (!path.empty()) req["path"] = path;
    req["depth"] = depth;
    if (ctx->param("force") == "1") req["force"] = true;
    std::string payload = req.dump();
    uint32_t rid = 0;
    std::string err;
    bool sent = callRelay([this, &devid, &payload, &rid, &err]() {
                    return relay_->startStorageQuery(devid.c_str(), payload, rid, err) ? std::string("1")
                                                                                       : std::string("0");
                }) == "1";
    if (!sent) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"" + (err.empty() ? std::string("device offline") : err) + "\"}");
        return HTTP_STATUS_NOT_FOUND;
    }
    hv::async([this, ctx, rid]() {
        std::string meta, body;
        bool got = false;
        // 单核设备整盘遍历约 1-2s，入口上限给 30s
        for (int i = 0; i < 600; ++i) {
            usleep(50000);
            got = callRelay([this, rid, &meta, &body]() {
                      return relay_->takeStorageResult(rid, meta, body) ? std::string("1") : std::string("0");
                  }) == "1";
            if (got) break;
        }
        if (!got) {
            ctx->response->status_code = HTTP_STATUS_GATEWAY_TIMEOUT;
            ctx->send("{\"error\":\"timeout\"}");
            return;
        }
        json m = json::parse(meta, nullptr, false);
        if (m.is_discarded() || !m.value("ok", false)) {
            ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
            ctx->send("{\"error\":\"" +
                      (m.is_discarded() ? std::string("bad storage response")
                                        : m.value("error", std::string("scan failed"))) +
                      "\"}");
            return;
        }
        // 新 agent 把整份响应压成 gzip 上传（首字节标记），这里不再解析 JSON，
        // 支持 gzip 的客户端直接原样转发压缩流，由浏览器解压。
        if (!body.empty() && body[0] == '\1') {
            std::string ae = ctx->header("Accept-Encoding");
            if (ae.find("gzip") != std::string::npos) {
                ctx->response->headers["Content-Encoding"] = "gzip";
                ctx->response->headers["Vary"] = "Accept-Encoding";
                ctx->response->content_type = APPLICATION_JSON;
                ctx->send(body.substr(1));
                return;
            }
            std::string plain;
            if (!gzipDecompress(body.data() + 1, body.size() - 1, plain)) {
                ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
                ctx->send("{\"error\":\"gunzip failed\"}");
                return;
            }
            ctx->response->content_type = APPLICATION_JSON;
            ctx->send(plain);
            return;
        }
        if (!body.empty() && body[0] == '\0') {
            ctx->response->content_type = APPLICATION_JSON;
            ctx->send(body.substr(1));
            return;
        }
        // 首字节只可能是 '\1'（gzip）或 '\0'（明文），其它都是协议错误
        ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
        ctx->send("{\"error\":\"bad storage response\"}");
    });
    return HTTP_STATUS_NEXT;
}

int AdminServer::handleAgentProcs(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    callRelay([this, &devid]() {
        relay_->warmDevice(devid.c_str());
        return std::string("1");
    });
    json req;
    req["op"] = "list";
    std::string sort = ctx->param("sort");
    if (sort != "mem" && sort != "pid" && sort != "start" && sort != "threads" && sort != "user" &&
        sort != "comm")
        sort = "cpu";
    req["sort"] = sort;
    int dir = atoi(ctx->param("dir").c_str());
    req["dir"] = (dir == 1 || dir == -1) ? dir : 0;
    req["q"] = ctx->param("q");
    req["kernel"] = ctx->param("kernel") == "1";
    int page = atoi(ctx->param("page").c_str());
    if (page <= 0) page = 1;
    int page_size = atoi(ctx->param("page_size").c_str());
    if (page_size <= 0) page_size = 10;
    if (page_size > 50) page_size = 50;
    req["page"] = page;
    req["page_size"] = page_size;
    req["refresh"] = ctx->param("refresh") == "1";
    return proxyAgentProc(ctx, devid, req);
}

int AdminServer::handleAgentProcDetail(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    int pid = atoi(ctx->param("pid").c_str());
    if (pid <= 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"invalid pid\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    json req;
    req["op"] = "detail";
    req["pid"] = pid;
    return proxyAgentProc(ctx, devid, req);
}

int AdminServer::handleAgentProcSignal(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    json body = json::parse(ctx->body(), nullptr, false);
    if (body.is_discarded() || !body.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    int pid = body.value("pid", 0);
    int sig = body.value("signal", 15);
    if (pid <= 1) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"invalid pid\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    if (sig < 1 || sig > 64) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"invalid signal\"}");
        return HTTP_STATUS_BAD_REQUEST;
    }
    audit(currentActor().username, "agent.proc.signal",
          devid + " pid=" + std::to_string(pid) + " signal=" + std::to_string(sig));
    json req;
    req["op"] = "signal";
    req["pid"] = pid;
    req["signal"] = sig;
    return proxyAgentProc(ctx, devid, req);
}

void AdminServer::handleClients(const HttpContextPtr& ctx) {
    auto actor = currentActor();
    bool scoped = scopeRestricted();
    std::vector<std::string> mine = scoped ? myDevids() : std::vector<std::string>();
    std::string body = callRelay([this, actor, scoped, mine]() {
        json arr = json::array();
        for (RelayConn* c = relay_->conns; c; c = c->next_all) {
            if (c->role != rtnp::ROLE_CLIENT || !c->authed || c->internal) continue;
            if ((actor.role == "operator" || actor.role == "viewer") && actor.username != c->user) continue;
            if (scoped && actor.username != c->user) {
                // 受限账号只看到与自己设备有活动会话的客户端，避免暴露全局接入账号
                bool relevant = false;
                for (RelayStream* s = c->streams; s && !relevant; s = s->next_c) {
                    if (s->a && std::find(mine.begin(), mine.end(), s->a->devid) != mine.end())
                        relevant = true;
                }
                if (!relevant) continue;
            }
            json j;
            j["user"] = c->user;
            fillConnCommon(c, j);
            arr.push_back(j);
        }
        return arr.dump();
    });
    ctx->send(body);
}
