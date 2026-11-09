// 设备分组与授权预览 API
#include "admin_server.hpp"
#include "cpputil/json.hpp"

using nlohmann::json;

namespace {
void bad(const HttpContextPtr& ctx, const std::string& msg) {
    ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
    json j;
    j["error"] = msg;
    ctx->send(j.dump());
}
void fail(const HttpContextPtr& ctx, const std::string& msg) {
    ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
    json j;
    j["error"] = msg;
    ctx->send(j.dump());
}
} // namespace

void AdminServer::refreshGroupAcl() {
    if (!db_.ok()) return;
    std::vector<DbGroupAclRule> rules;
    if (!db_.groupClientRules(rules)) return;
    std::vector<GroupAclRule> out;
    std::set<std::string> users;
    db_.groupClientUsers(users);
    for (auto& r : rules) {
        GroupAclRule g;
        g.user = r.user;
        g.devid = r.devid;
        g.ports = r.ports;
        g.tty = r.tty;
        g.shell = r.shell;
        out.push_back(g);
        users.insert(r.user);
    }
    callRelay([this, out, users]() {
        relay_->setGroupAcl(out, users);
        return std::string();
    });
}

void AdminServer::handleGroups(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.groupList(out)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleGroupCreate(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::string name = req.value("name", "");
    std::string desc = req.value("description", "");
    long long id = 0;
    std::string err;
    if (!db_.groupCreate(name, desc, id, err)) { bad(ctx, err); return; }
    audit(role, "groups.create", name);
    refreshGroupAcl();
    json out;
    out["id"] = id;
    out["ok"] = true;
    ctx->send(out.dump());
}

void AdminServer::handleGroupGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out, err;
    if (!db_.groupGet(id, out, err)) { fail(ctx, err); return; }
    ctx->send(out);
}

void AdminServer::handleGroupUpdate(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::string err;
    if (req.contains("name")) {
        if (!db_.groupUpdate(id, req.value("name", ""), req.value("description", ""), err)) {
            bad(ctx, err);
            return;
        }
        audit(role, "groups.update", std::to_string(id) + " name=" + req.value("name", ""));
    }
    if (req.contains("caps")) {
        std::map<std::string, std::string> caps;
        if (req["caps"].is_object()) {
            for (auto it = req["caps"].begin(); it != req["caps"].end(); ++it) {
                if (it.value().is_boolean()) caps[it.key()] = it.value().get<bool>() ? "1" : "0";
                else if (it.value().is_string()) caps[it.key()] = it.value().get<std::string>();
                else if (it.value().is_number_integer()) caps[it.key()] = std::to_string(it.value().get<long long>());
            }
        }
        if (!db_.groupSetCaps(id, caps, err)) { bad(ctx, err); return; }
        audit(role, "groups.caps", std::to_string(id));
    }
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleGroupDelete(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string err;
    if (!db_.groupDelete(id, err)) { bad(ctx, err); return; }
    audit(role, "groups.delete", std::to_string(id));
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleGroupAddDevices(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::vector<std::string> devids;
    if (req.contains("devids") && req["devids"].is_array()) {
        for (auto& v : req["devids"]) if (v.is_string()) devids.push_back(v.get<std::string>());
    } else if (req.contains("devid") && req["devid"].is_string()) {
        devids.push_back(req["devid"].get<std::string>());
    }
    if (devids.empty()) { bad(ctx, "empty devids"); return; }
    std::string err;
    for (auto& d : devids) {
        if (!db_.groupAddDevice(id, d, err)) { bad(ctx, err); return; }
    }
    audit(role, "groups.devices.add", std::to_string(id) + " n=" + std::to_string(devids.size()));
    refreshGroupAcl();
    ctx->send("{\"ok\":true,\"added\":" + std::to_string(devids.size()) + "}");
}

void AdminServer::handleGroupRemoveDevice(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string devid = ctx->param("devid");
    if (devid.empty()) { bad(ctx, "empty devid"); return; }
    if (!db_.groupRemoveDevice(id, devid)) { bad(ctx, "remove failed"); return; }
    audit(role, "groups.devices.remove", std::to_string(id) + " " + devid);
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleGroupAddSubject(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { bad(ctx, "bad json"); return; }
    std::string type = req.value("type", "");
    std::string subject = req.value("id", "");
    if (type == "admin" && currentActor().role != "admin") { bad(ctx, "admin only"); return; }
    std::string err;
    if (!db_.groupAddSubject(id, type, subject, err)) { bad(ctx, err); return; }
    audit(role, "groups.subjects.add", type + ":" + subject);
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleGroupRemoveSubject(const HttpContextPtr& ctx, const std::string& role) {
    long long id = atoll(ctx->param("id").c_str());
    std::string type = ctx->param("type");
    std::string subject = ctx->param("subject");
    if (!db_.groupRemoveSubject(id, type, subject)) { bad(ctx, "remove failed"); return; }
    audit(role, "groups.subjects.remove", type + ":" + subject);
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleGroupDeviceMap(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.groupDeviceMap(out)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleAccessEffective(const HttpContextPtr& ctx) {
    std::string type = ctx->param("type");
    std::string id = ctx->param("id");
    std::string out, err;
    if (!db_.groupEffective(type, id, out, err)) { bad(ctx, err); return; }
    ctx->send(out);
}
