// 权限页 API：客户端分组 / 设备分组 / ACL 矩阵
#include "admin_server.hpp"
#include "cpputil/json.hpp"

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
