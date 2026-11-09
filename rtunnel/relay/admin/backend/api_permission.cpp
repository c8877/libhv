// 权限页 API：客户端分组 / 设备分组 / ACL 矩阵
#include "admin_server.hpp"
#include "cpputil/json.hpp"
#include <algorithm>
#include <map>

using nlohmann::json;

namespace {
void bad(const HttpContextPtr& ctx, const std::string& msg) {
    ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
    json j; j["error"] = msg; ctx->send(j.dump());
}
void notfound(const HttpContextPtr& ctx, const std::string& msg) {
    ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
    json j; j["error"] = msg; ctx->send(j.dump());
}
} // namespace

void AdminServer::handlePermOverview(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.permOverview(out)) { ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR; ctx->send("{\"error\":\"db error\"}"); return; }
    ctx->send(out);
}

void AdminServer::handlePermClientGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.permClientGet(id, out, err)) { notfound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handlePermClientSave(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::vector<std::string> users;
    if (req.contains("users")) for (auto& v : req["users"]) if (v.is_string()) users.push_back(v.get<std::string>());
    std::vector<DbPermAcl> acl;
    if (req.contains("acl")) {
        for (auto& v : req["acl"]) {
            if (!v.is_object()) continue;
            DbPermAcl a;
            a.agent_group_id = v.value("agent_group_id", 0LL);
            a.tcp_ports = v.value("tcp_ports", "");
            a.tty = v.value("tty", "");
            a.file = v.value("file", "none");
            a.shell = v.value("shell", false);
            a.sysinfo = v.value("sysinfo", false);
            a.metrics = v.value("metrics", false);
            a.config = v.value("config", false);
            a.web = v.value("web", false);
            acl.push_back(a);
        }
    }
    long long out_id = id;
    std::string err;
    if (!db_.permClientSave(id, req.value("name", ""), req.value("description", ""), users, acl, out_id, err)) {
        bad(ctx, err);
        return;
    }
    audit(role, id ? "permission.client.update" : "permission.client.create",
          std::to_string(out_id) + " " + req.value("name", ""));
    refreshGroupAcl();
    json out; out["ok"] = true; out["id"] = out_id; ctx->send(out.dump());
}

void AdminServer::handlePermClientDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.permClientDelete(id, err)) { bad(ctx, err); return; }
    audit(role, "permission.client.delete", std::to_string(id));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePermAgentGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.permAgentGet(id, out, err)) { notfound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handlePermAgentSave(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::vector<std::string> devices;
    if (req.contains("devices")) for (auto& v : req["devices"]) if (v.is_string()) devices.push_back(v.get<std::string>());
    std::map<std::string, std::string> caps;
    if (req.contains("caps") && req["caps"].is_object()) {
        for (auto it = req["caps"].begin(); it != req["caps"].end(); ++it) {
            if (it.value().is_boolean()) caps[it.key()] = it.value().get<bool>() ? "1" : "0";
            else if (it.value().is_string()) caps[it.key()] = it.value().get<std::string>();
            else if (it.value().is_number_integer()) caps[it.key()] = std::to_string(it.value().get<long long>());
        }
    }
    long long out_id = id;
    std::string err;
    if (!db_.permAgentSave(id, req.value("name", ""), req.value("description", ""), devices, caps, out_id, err)) {
        bad(ctx, err);
        return;
    }
    audit(role, id ? "permission.agent.update" : "permission.agent.create",
          std::to_string(out_id) + " " + req.value("name", ""));
    refreshGroupAcl();
    json out; out["ok"] = true; out["id"] = out_id; ctx->send(out.dump());
}

void AdminServer::handlePermAgentDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.permAgentDelete(id, err)) { bad(ctx, err); return; }
    audit(role, "permission.agent.delete", std::to_string(id));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePermEffective(const HttpContextPtr& ctx) {
    std::string user = ctx->param("client");
    std::string out, err;
    if (!db_.permEffectiveClient(user, out, err)) { bad(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handlePermAgentCandidates(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out;
    if (!db_.permAgentCandidates(id, out)) { bad(ctx, "db error"); return; }
    json base = json::parse(out, nullptr, false);
    if (base.is_discarded()) { bad(ctx, "db error"); return; }
    std::map<std::string, json> map;
    for (auto& d : base["devices"]) map[d.value("devid", "")] = d;
    std::string online = callRelay([this]() {
        json arr = json::array();
        for (RelayConn* c = relay_->agents; c; c = c->reg_next) {
            json j;
            j["devid"] = c->devid;
            j["online"] = true;
            j["last_seen_ms"] = c->last_seen_ms;
            j["groups"] = json::array();
            arr.push_back(j);
        }
        return arr.dump();
    });
    json oa = json::parse(online, nullptr, false);
    if (!oa.is_discarded() && oa.is_array()) {
        for (auto& o : oa) {
            std::string devid = o.value("devid", "");
            if (map.count(devid)) {
                map[devid]["online"] = true;
                map[devid]["source"] = "online";
                map[devid]["last_seen_ms"] = o.value("last_seen_ms", 0LL);
            } else {
                map[devid] = o;
                map[devid]["source"] = "online";
            }
        }
    }
    std::string q = ctx->param("q");
    bool online_only = ctx->param("online") == "1";
    json arr = json::array();
    for (auto& kv : map) {
        auto& d = kv.second;
        if (online_only && !d.value("online", false)) continue;
        if (!q.empty() && kv.first.find(q) == std::string::npos) continue;
        arr.push_back(d);
    }
    std::sort(arr.begin(), arr.end(), [](const json& a, const json& b) {
        if (a.value("online", false) != b.value("online", false)) return a.value("online", false);
        return a.value("devid", "") < b.value("devid", "");
    });
    json outj;
    outj["devices"] = arr;
    ctx->send(outj.dump());
}

void AdminServer::handlePermAgentAddDevices(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::vector<std::string> devids;
    if (req.contains("devids")) for (auto& v : req["devids"]) if (v.is_string()) devids.push_back(v.get<std::string>());
    std::string err;
    if (!db_.permAgentAddDevices(id, devids, err)) { bad(ctx, err); return; }
    audit(role, "permission.agent.devices.add", std::to_string(id) + " n=" + std::to_string(devids.size()));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDictPorts(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.dictPorts(ctx->param("q"), out)) { bad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handleDictPortAdd(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    long long id = 0; std::string err;
    if (!db_.dictPortAdd(req.value("value", ""), req.value("name", ""), req.value("description", ""), id, err)) {
        bad(ctx, err); return;
    }
    audit(role, "permission.dict.port.add", req.value("value", ""));
    json out; out["ok"] = true; out["id"] = id; ctx->send(out.dump());
}

void AdminServer::handleDictTtys(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.dictTtys(ctx->param("q"), out)) { bad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handleDictTtyAdd(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    long long id = 0; std::string err;
    if (!db_.dictTtyAdd(req.value("value", ""), req.value("name", ""), req.value("description", ""), id, err)) {
        bad(ctx, err); return;
    }
    audit(role, "permission.dict.tty.add", req.value("value", ""));
    json out; out["ok"] = true; out["id"] = id; ctx->send(out.dump());
}

void AdminServer::handleDictBatchAdd(const HttpContextPtr& ctx, const std::string& role, const std::string& kind) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::vector<std::string> values;
    if (req.contains("values")) for (auto& v : req["values"]) if (v.is_string()) values.push_back(v.get<std::string>());
    std::string out, err;
    if (!db_.dictBatchAdd(kind, values, req.value("name_prefix", ""), out, err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send(out.empty() ? "{\"error\":\"" + err + "\"}" : out);
        return;
    }
    audit(role, "permission.dict." + kind + ".batch_add", "n=" + std::to_string(values.size()));
    ctx->send(out);
}

void AdminServer::handleDictUpdate(const HttpContextPtr& ctx, const std::string& role, const std::string& kind) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::string err;
    if (!db_.dictUpdate(kind, id, req.value("name", ""), req.value("description", ""),
                        req.value("enabled", true) ? 1 : 0, err)) { bad(ctx, err); return; }
    audit(role, "permission.dict." + kind + ".update", std::to_string(id));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDictDelete(const HttpContextPtr& ctx, const std::string& role, const std::string& kind) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.dictDelete(kind, id, err)) {
        ctx->response->status_code = err == "referenced" ? HTTP_STATUS_CONFLICT : HTTP_STATUS_BAD_REQUEST;
        json j; j["error"] = err; ctx->send(j.dump());
        return;
    }
    audit(role, "permission.dict." + kind + ".delete", std::to_string(id));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDictBatchDelete(const HttpContextPtr& ctx, const std::string& role, const std::string& kind) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::vector<long long> ids;
    if (req.contains("ids")) for (auto& v : req["ids"]) if (v.is_number_integer()) ids.push_back(v.get<long long>());
    std::string out;
    db_.dictBatchDelete(kind, ids, out);
    audit(role, "permission.dict." + kind + ".batch_delete", "n=" + std::to_string(ids.size()));
    ctx->send(out);
}
