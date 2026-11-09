// 分组 ACL 刷新与设备分组映射 API
#include "admin_server.hpp"
#include "cpputil/json.hpp"

using nlohmann::json;

void AdminServer::refreshGroupAcl() {
    if (!db_.ok()) return;
    std::vector<DbGroupAclRule> rules;
    std::set<std::string> users;
    if (!db_.permClientRules(rules, users)) return;
    std::vector<GroupAclRule> out;
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

void AdminServer::handleGroupDeviceMap(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.groupDeviceMap(out)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send(out);
}
