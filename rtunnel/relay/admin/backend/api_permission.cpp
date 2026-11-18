// 权限页 API：客户端分组 / 设备分组 / ACL 矩阵
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/json.hpp"
#include <algorithm>
#include <map>

using nlohmann::json;

void AdminServer::handlePermOverview(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.permOverview(out)) { ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR; ctx->send("{\"error\":\"db error\"}"); return; }
    ctx->send(out);
}

void AdminServer::handlePermClientGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.permClientGet(id, out, err)) { apiNotFound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handlePermClientSave(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
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
        apiBad(ctx, err);
        return;
    }
    audit(role, id ? "permission.client.update" : "permission.client.create",
          std::to_string(out_id) + " " + req.value("name", "") +
              " users=" + std::to_string(users.size()) + " acl=" + std::to_string(acl.size()));
    refreshGroupAcl();
    json out; out["ok"] = true; out["id"] = out_id; ctx->send(out.dump());
}

void AdminServer::handlePermClientDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.permClientDelete(id, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.client.delete", std::to_string(id));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePermAgentGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.permAgentGet(id, out, err)) { apiNotFound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handlePermAgentSave(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
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
        apiBad(ctx, err);
        return;
    }
    audit(role, id ? "permission.agent.update" : "permission.agent.create",
          std::to_string(out_id) + " " + req.value("name", "") +
              " devices=" + std::to_string(devices.size()) + " caps=" + std::to_string(caps.size()));
    refreshGroupAcl();
    json out; out["ok"] = true; out["id"] = out_id; ctx->send(out.dump());
}

void AdminServer::handlePermAgentDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.permAgentDelete(id, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.agent.delete", std::to_string(id));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePermEffective(const HttpContextPtr& ctx) {
    std::string user = ctx->param("client");
    std::string out, err;
    if (!db_.permEffectiveClient(user, out, err)) { apiBad(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handlePermImportAcl(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string content = req.value("content", "");
    std::string online = callRelay([this]() {
        json arr = json::array();
        for (RelayConn* c = relay_->agents; c; c = c->reg_next) arr.push_back(c->devid);
        return arr.dump();
    });
    std::vector<std::string> extra;
    json oa = json::parse(online, nullptr, false);
    if (!oa.is_discarded() && oa.is_array()) {
        for (auto& v : oa) if (v.is_string()) extra.push_back(v.get<std::string>());
    }
    std::string out, err;
    if (!db_.permImportAcl(content, extra, out, err)) { apiBad(ctx, err); return; }
    json res = json::parse(out, nullptr, false);
    int n = res.is_object() ? res.value("imported", 0) : 0;
    audit(role, "permission.acl.import", "imported=" + std::to_string(n));
    refreshGroupAcl();
    ctx->send(out);
}

void AdminServer::handlePermExportAcl(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.permExportAcl(out)) { apiBad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handlePermAgentCandidates(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out;
    if (!db_.permAgentCandidates(id, out)) { apiBad(ctx, "db error"); return; }
    json base = json::parse(out, nullptr, false);
    if (base.is_discarded()) { apiBad(ctx, "db error"); return; }
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
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::vector<std::string> devids;
    if (req.contains("devids")) for (auto& v : req["devids"]) if (v.is_string()) devids.push_back(v.get<std::string>());
    std::string err;
    if (!db_.permAgentAddDevices(id, devids, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.agent.devices.add", std::to_string(id) + " n=" + std::to_string(devids.size()));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDictPorts(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.dictPorts(ctx->param("q"), out)) { apiBad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handleDictPortAdd(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    long long id = 0; std::string err;
    if (!db_.dictPortAdd(req.value("value", ""), req.value("name", ""), req.value("description", ""), id, err)) {
        apiBad(ctx, err); return;
    }
    audit(role, "permission.dict.port.add", req.value("value", ""));
    json out; out["ok"] = true; out["id"] = id; ctx->send(out.dump());
}

void AdminServer::handleDictTtys(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.dictTtys(ctx->param("q"), out)) { apiBad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handleDictTtyAdd(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    long long id = 0; std::string err;
    if (!db_.dictTtyAdd(req.value("value", ""), req.value("name", ""), req.value("description", ""), id, err)) {
        apiBad(ctx, err); return;
    }
    audit(role, "permission.dict.tty.add", req.value("value", ""));
    json out; out["ok"] = true; out["id"] = id; ctx->send(out.dump());
}

void AdminServer::handleDictBatchAdd(const HttpContextPtr& ctx, const std::string& role, const std::string& kind) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
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
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string err;
    if (!db_.dictUpdate(kind, id, req.value("name", ""), req.value("description", ""),
                        req.value("enabled", true) ? 1 : 0, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.dict." + kind + ".update", std::to_string(id));
    refreshGroupAcl();
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
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::vector<long long> ids;
    if (req.contains("ids")) for (auto& v : req["ids"]) if (v.is_number_integer()) ids.push_back(v.get<long long>());
    std::string out;
    db_.dictBatchDelete(kind, ids, out);
    audit(role, "permission.dict." + kind + ".batch_delete", "n=" + std::to_string(ids.size()));
    ctx->send(out);
}

// ---------------------------------------------------------------- 资源字典与权限组

// 字典页：资源 / 端口 / 串口统一分页入口
void AdminServer::handlePermissionDicts(const HttpContextPtr& ctx) {
    std::string kind = ctx->param("kind", "resources");
    std::string q = ctx->param("q");
    int page = atoi(ctx->param("page", "1").c_str());
    int page_size = atoi(ctx->param("page_size", "10").c_str());
    std::string out;
    bool ok = kind == "resources" ? db_.permResourcesPage(q, page, page_size, out)
              : kind == "devices" ? db_.resDevicesPage(q, page, page_size, out)
                                  : db_.dictPage(kind, q, page, page_size, out);
    if (!ok) {
        apiBad(ctx, "bad kind");
        return;
    }
    ctx->send(out);
}

void AdminServer::handlePermResources(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.permResources(out)) { apiBad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handlePermResourceUpdate(const HttpContextPtr& ctx, const std::string& role) {
    std::string key = ctx->param("key");
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string name = req.value("name", "");
    std::string desc = req.value("description", "");
    int enabled = req.contains("enabled") ? (req.value("enabled", true) ? 1 : 0) : 1;
    std::string err;
    if (!db_.permResourceUpdate(key, name, desc, enabled, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.resource.update", key);
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePermGroups(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.permGroups(out)) { apiBad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handlePermGroupSave(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::vector<std::string> resources, devices;
    if (req.contains("resources") && req["resources"].is_array()) {
        for (auto& v : req["resources"])
            if (v.is_string()) resources.push_back(v.get<std::string>());
    }
    if (req.contains("devices") && req["devices"].is_array()) {
        for (auto& v : req["devices"])
            if (v.is_string()) devices.push_back(v.get<std::string>());
    }
    std::vector<std::pair<std::string, std::string>> members;
    bool set_members = false;
    if (req.contains("members") && req["members"].is_array()) {
        set_members = true;
        for (auto& v : req["members"]) {
            if (!v.is_object()) continue;
            std::string ty = v.value("type", std::string());
            std::string sid = v.value("id", std::string());
            if (ty.empty() && v.contains("id") && v["id"].is_number_integer())
                sid = std::to_string(v["id"].get<long long>());
            if (!ty.empty() && !sid.empty()) members.emplace_back(ty, sid);
        }
    }
    int all_devices = req.value("all_devices", false) ? 1 : 0;
    std::string ports = req.value("ports", std::string("*"));
    long long out_id = id;
    std::string err;
    if (!db_.permGroupSaveAll(id, req.value("name", ""), req.value("description", ""), resources, devices,
                              all_devices, ports, set_members, members, out_id, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, id ? "permission.group.update" : "permission.group.create",
          std::to_string(out_id) + " " + req.value("name", "") + " res=" + std::to_string(resources.size()) +
              " dev=" + std::to_string(all_devices ? -1 : (long long)devices.size()) +
              " member=" + std::to_string(members.size()));
    refreshGroupAcl();
    json out;
    out["ok"] = true;
    out["id"] = out_id;
    ctx->send(out.dump());
}

void AdminServer::handlePermGroupDetail(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.permGroupDetail(id, out, err)) { apiNotFound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handlePermGroupDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.permGroupDelete(id, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.group.delete", std::to_string(id));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePermGroupMembers(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.permGroupMembers(id, out, err)) { apiNotFound(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handlePermGroupMembersSave(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::vector<std::pair<std::string, std::string>> members;
    if (req.contains("members") && req["members"].is_array()) {
        for (auto& v : req["members"]) {
            if (!v.is_object()) continue;
            std::string ty = v.value("type", std::string());
            std::string sid = v.value("id", std::string());
            if (ty.empty() && v.contains("id") && v["id"].is_number_integer())
                sid = std::to_string(v["id"].get<long long>());
            if (!ty.empty() && !sid.empty()) members.emplace_back(ty, sid);
        }
    }
    std::string err;
    if (!db_.permGroupSetMembers(id, members, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.group.members", std::to_string(id) + " n=" + std::to_string(members.size()));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleUserPermGroups(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    DbUser u;
    if (!db_.findUserById(id, u)) { apiNotFound(ctx, "账号不存在"); return; }
    std::vector<long long> ids;
    db_.permGroupsOfUser(id, ids);
    std::string all;
    db_.permGroups(all);
    json out;
    out["ids"] = ids;
    out["groups"] = json::parse(all, nullptr, false);
    ctx->send(out.dump());
}

void AdminServer::handleUserPermGroupsSave(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    DbUser u;
    if (!db_.findUserById(id, u)) { apiNotFound(ctx, "账号不存在"); return; }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("ids") || !req["ids"].is_array()) {
        apiBad(ctx, "bad json");
        return;
    }
    std::vector<long long> ids;
    for (auto& v : req["ids"])
        if (v.is_number_integer()) ids.push_back(v.get<long long>());
    std::string err;
    if (!db_.permSetUserGroups(id, u.role, ids, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.user.groups", u.username + " n=" + std::to_string(ids.size()));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePermSubjects(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.permSubjects(out)) { apiBad(ctx, "db error"); return; }
    ctx->send(out);
}

void AdminServer::handleResDeviceUpdate(const HttpContextPtr& ctx, const std::string& role) {
    std::string devid = ctx->param("devid");
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    std::string err;
    if (!db_.resDeviceUpdate(devid, req.value("name", std::string()), req.value("enabled", true) ? 1 : 0, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, "permission.device.update", devid);
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientPermGroups(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string acc, err;
    if (!db_.clientAccountGet(id, acc, err)) { apiNotFound(ctx, "接入账号不存在"); return; }
    std::vector<long long> ids;
    db_.permGroupsOfClient(id, ids);
    std::string all;
    db_.permGroups(all);
    json out;
    out["ids"] = ids;
    out["groups"] = json::parse(all, nullptr, false);
    ctx->send(out.dump());
}

void AdminServer::handleClientPermGroupsSave(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string acc, err;
    if (!db_.clientAccountGet(id, acc, err)) { apiNotFound(ctx, "接入账号不存在"); return; }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("ids") || !req["ids"].is_array()) {
        apiBad(ctx, "bad json");
        return;
    }
    std::vector<long long> ids;
    for (auto& v : req["ids"])
        if (v.is_number_integer()) ids.push_back(v.get<long long>());
    if (!db_.permSetClientGroups(id, ids, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.client.groups", std::to_string(id) + " n=" + std::to_string(ids.size()));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleSubjectGroups(const HttpContextPtr& ctx) {
    std::string type = ctx->param("type");
    std::string id = ctx->param("id");
    std::vector<long long> ids;
    db_.permGroupsOfSubject(type, id, ids);
    std::string all;
    db_.permGroups(all);
    json out;
    out["ids"] = ids;
    out["groups"] = json::parse(all, nullptr, false);
    ctx->send(out.dump());
}

void AdminServer::handleSubjectGroupsSave(const HttpContextPtr& ctx, const std::string& role) {
    std::string type = ctx->param("type");
    std::string id = ctx->param("id");
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("ids") || !req["ids"].is_array()) {
        apiBad(ctx, "bad json");
        return;
    }
    std::vector<long long> ids;
    for (auto& v : req["ids"])
        if (v.is_number_integer()) ids.push_back(v.get<long long>());
    std::string err;
    if (!db_.permSetSubjectGroups(type, id, ids, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.subject.groups", type + ":" + id + " n=" + std::to_string(ids.size()));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}
