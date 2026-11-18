// 分组 ACL 刷新 API
#include "admin_server.hpp"
#include "cpputil/json.hpp"

using nlohmann::json;

void AdminServer::refreshGroupAcl() {
    if (!db_.ok()) return;
    std::vector<DbGroupAclRule> rules;
    std::set<std::string> users;
    std::set<long long> scoped_tokens;
    if (!db_.permClientRules(rules, users, scoped_tokens)) return;
    std::vector<GroupAclRule> out;
    for (auto& r : rules) {
        GroupAclRule g;
        g.user = r.user;
        g.devid = r.devid;
        g.ports = r.ports;
        g.tty = r.tty;
        g.shell = r.shell;
        g.file = r.file;
        g.svc = r.svc;
        g.token_id = r.token_id;
        out.push_back(g);
        users.insert(r.user);
    }
    callRelay([this, out, users, scoped_tokens]() {
        relay_->setGroupAcl(out, users, scoped_tokens);
        return std::string();
    });
}

void AdminServer::refreshTokenCache() {
    if (!db_.ok()) return;
    std::string policies;
    if (!db_.clientAuthPolicies(policies)) return;
    callRelay([this, policies]() {
        relay_->setAuthPolicies(policies);
        return std::string();
    });
}
