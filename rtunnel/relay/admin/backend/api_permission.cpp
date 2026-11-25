// 权限 API：资源/设备/端口/串口字典、权限组与授权对象
#include "admin_server.hpp"
#include "api_reply.hpp"
#include "cpputil/json.hpp"

#include <algorithm>

using nlohmann::json;

extern thread_local AdminActor tl_actor;

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
    // 变更前的管理账号有效范围：保存后只断开权限变小的账号
    auto scope_before = snapshotUserScopes();
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
    int global_scope = req.value("global_scope", false) ? 1 : 0;
    std::string ports = req.value("ports", std::string("*"));
    // 授予边界：只能授予自己有效权限的子集，super 组只有 super 能改
    DbPermScope want;
    want.resources.insert(resources.begin(), resources.end());
    want.all_devices = all_devices != 0;
    want.global_scope = global_scope != 0;
    if (!want.all_devices) want.devices.insert(devices.begin(), devices.end());
    // 没有端口转发能力的组不参与端口范围比较（与 permGroupScope / subjectPortsLocked 一致）
    if (want.resources.count("device.port")) want.ports = ports;
    DbPermScope old;
    if (id) {
        if (!db_.permGroupScope(id, old)) { apiNotFound(ctx, "权限组不存在"); return; }
        // 不能改比自己权限更高的组（改名、降级也不行）；内置组同样按这条边界判断
        std::string why = grantDenied(old);
        if (!why.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, why); return; }
        want.super = old.super;
    }
    // 组里带全局键却没开全局权限 = 静默无效，直接拒绝并提示
    if (!want.global_scope && !want.all_devices && !want.super) {
        for (const auto& r : want.resources) {
            if (permIsGlobal(r)) {
                apiError(ctx, HTTP_STATUS_BAD_REQUEST,
                         "资源 " + r + " 是全局资源，需要同时开启「全局权限」");
                return;
            }
        }
    }
    std::string denied = grantDenied(want);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    if (set_members && !hasPerm("group.member")) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "需要 group.member 才能调整组成员");
        return;
    }
    long long out_id = id;
    // 组成员：保存前抓旧成员（校验管辖范围、保存后按收窄/移除逐个踢线）
    std::vector<std::pair<std::pair<std::string, std::string>, DbPermScope>> old_members;
    if (set_members && id > 0) {
        std::vector<std::pair<std::string, std::string>> old_list;
        if (db_.permGroupMemberList(id, old_list)) {
            for (auto& m : old_list) {
                DbPermScope ms;
                if (!subjectScope(m.first, m.second, ms)) continue;
                std::string why = manageDenied(ms);
                if (!why.empty()) {
                    apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能修改权限高于你的成员: " + m.first + ":" + m.second);
                    return;
                }
                old_members.push_back({m, ms});
            }
        }
    }
    // 新加入的成员当前权限也不能高于自己：否则可以把高权令牌“关进”窄组
    if (set_members) {
        std::set<std::string> in_old;
        for (auto& om : old_members) in_old.insert(om.first.first + ":" + om.first.second);
        for (auto& m : members) {
            if (in_old.count(m.first + ":" + m.second)) continue;
            DbPermScope ms;
            if (!subjectScope(m.first, m.second, ms)) continue;
            std::string why = manageDenied(ms);
            if (!why.empty()) {
                apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能操作权限高于你的成员: " + m.first + ":" + m.second);
                return;
            }
        }
    }
    std::string err;
    if (!db_.permGroupSaveAll(id, req.value("name", ""), req.value("description", ""), resources, devices,
                              all_devices, global_scope, ports, set_members, members, out_id, err)) {
        apiBad(ctx, err);
        return;
    }
    audit(role, id ? "permission.group.update" : "permission.group.create",
          std::to_string(out_id) + " " + req.value("name", "") + " res=" + std::to_string(resources.size()) +
              " dev=" + std::to_string(all_devices ? -1 : (long long)devices.size()) +
              " member=" + std::to_string(members.size()));
    refreshGroupAcl();
    // 逐个成员判断：被移出组或组权限收窄才断开，扩容/改名不打断在线用户
    for (auto& om : old_members)
        kickSubjectShrunk(om.first.first, om.first.second, om.second, "group changed");
    kickConsoleWsShrunk(scope_before, "group changed");
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
    std::string builtin;
    if (db_.permGroupBuiltin(id, builtin) && !builtin.empty()) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "内置角色组不能删除，可以修改其资源与设备范围");
        return;
    }
    DbPermScope gs;
    if (!db_.permGroupScope(id, gs)) { apiNotFound(ctx, "权限组不存在"); return; }
    std::string denied = grantDenied(gs);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    auto scope_before = snapshotUserScopes();
    // 删组前先抓成员：删完就查不到了；成员当前权限也不能高于自己
    std::vector<std::pair<std::pair<std::string, std::string>, DbPermScope>> members;
    {
        std::vector<std::pair<std::string, std::string>> list;
        if (db_.permGroupMemberList(id, list)) {
            for (auto& m : list) {
                DbPermScope ms;
                if (!subjectScope(m.first, m.second, ms)) continue;
                std::string why = manageDenied(ms);
                if (!why.empty()) {
                    apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能删除包含更高权限成员的组: " + m.first + ":" + m.second);
                    return;
                }
                members.push_back({m, ms});
            }
        }
    }
    std::string err;
    if (!db_.permGroupDelete(id, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.group.delete", std::to_string(id));
    refreshGroupAcl();
    // 删组等于所有成员失去该组权限：按主体逐个断开（扩容不涉及的场景）
    for (auto& m : members) kickSubjectShrunk(m.first.first, m.first.second, m.second, "group deleted");
    kickConsoleWsShrunk(scope_before, "group deleted");
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
    // 组成员变更会直接改变成员的授权面：组的 scope 必须在自己范围内
    DbPermScope gs;
    if (!db_.permGroupScope(id, gs)) { apiNotFound(ctx, "权限组不存在"); return; }
    std::string denied = grantDenied(gs);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    // 现有成员的权限也不能高于自己：否则可以借“移出组”停用同级账号
    std::vector<std::pair<std::pair<std::string, std::string>, DbPermScope>> old_scopes;
    {
        std::vector<std::pair<std::string, std::string>> old_list;
        if (db_.permGroupMemberList(id, old_list)) {
            for (auto& m : old_list) {
                DbPermScope ms;
                if (!subjectScope(m.first, m.second, ms)) continue;
                std::string why = manageDenied(ms);
                if (!why.empty()) {
                    apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能修改权限高于你的成员: " + m.first + ":" + m.second);
                    return;
                }
                old_scopes.push_back({m, ms});
            }
        }
    }
    // 新加入的成员同样不能高于自己：否则可以把高权账号/令牌“关进”窄组
    for (auto& m : members) {
        bool is_old = false;
        for (auto& os : old_scopes)
            if (os.first == m) { is_old = true; break; }
        if (is_old) continue;
        DbPermScope ms;
        if (!subjectScope(m.first, m.second, ms)) continue;
        std::string why = manageDenied(ms);
        if (!why.empty()) {
            apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能操作权限高于你的成员: " + m.first + ":" + m.second);
            return;
        }
    }
    auto scope_before = snapshotUserScopes();
    if (!db_.permGroupSetMembers(id, members, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.group.members", std::to_string(id) + " n=" + std::to_string(members.size()));
    refreshGroupAcl();
    // 只处理被移出组的 atk/rtk：仍在组里的令牌按组内规则继续使用
    auto inNew = [&](const std::string& ty, const std::string& sid) {
        for (auto& m : members)
            if (m.first == ty && m.second == sid) return true;
        return false;
    };
    for (auto& os : old_scopes) {
        if (inNew(os.first.first, os.first.second)) continue;
        if (os.first.first == "atk" || os.first.first == "rtk" || os.first.first == "client")
            kickSubjectShrunk(os.first.first, os.first.second, os.second, "group members changed");
    }
    kickConsoleWsShrunk(scope_before, "group members changed");
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
    // 不允许改自己的组：一次误操作就能把自己（含唯一的超级管理员）锁在门外
    if (id == currentActor().id) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能修改自己的权限组");
        return;
    }
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
    std::vector<long long> cur;
    db_.permGroupsOfUser(id, cur);
    // 目标当前有效权限必须在自己范围内：不能停用权限比自己高的账号
    DbPermScope before;
    if (!db_.permScopeOfUser(id, before)) { apiNotFound(ctx, "账号不存在"); return; }
    std::string manage = manageDenied(before);
    if (!manage.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, manage); return; }
    std::vector<long long> check = cur;
    for (long long g : ids)
        if (std::find(check.begin(), check.end(), g) == check.end()) check.push_back(g);
    std::string denied = grantGroupsDenied(check);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    auto scope_before = snapshotUserScopes();
    std::string err;
    if (!db_.permSetUserGroups(id, ids, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.user.groups", u.username + " n=" + std::to_string(ids.size()));
    refreshGroupAcl();
    kickConsoleWsShrunk(scope_before, "user groups narrowed");
    ctx->send("{\"ok\":true}");
}

void AdminServer::handlePermSubjects(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.permSubjects(out)) { apiBad(ctx, "db error"); return; }
    if (tl_actor.is_client) {
        // 接入令牌只看本租户的接入账号与其令牌；控制台账号/atk 不对其暴露
        json arr = json::parse(out, nullptr, false);
        if (!arr.is_discarded() && arr.is_array()) {
            json keep = json::array();
            for (auto& s : arr) {
                std::string type = s.value("type", std::string());
                long long acc = 0;
                if (type == "client") {
                    acc = atoll(s.value("id", std::string()).c_str());
                } else if (type == "rtk") {
                    DbTokenSpec spec;
                    long long tid = atoll(s.value("id", std::string()).c_str());
                    std::string err;
                    if (!db_.clientTokenSpec(tid, spec, acc, nullptr, err)) continue;
                } else {
                    continue;
                }
                long long t = 0;
                if (db_.tenantOfClient(acc, t) && t == tl_actor.tenant_id) keep.push_back(s);
            }
            ctx->send(keep.dump());
            return;
        }
    }
    ctx->send(out);
}

void AdminServer::handleResDeviceUpdate(const HttpContextPtr& ctx, const std::string& role) {
    std::string devid = ctx->param("devid");
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) { apiBad(ctx, "bad json"); return; }
    // 停用设备资源会把它从所有非 super 组的授权面里去掉，必须限定在自己范围内
    if (!ownsDevid(devid)) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "设备不在授权范围内");
        return;
    }
    std::string err;
    if (!db_.resDeviceUpdate(devid, req.value("name", std::string()), req.value("enabled", true) ? 1 : 0, err)) {
        apiBad(ctx, err);
        return;
    }
    // 归属租户：把设备划给某个租户后，只有该租户的接入账号能访问
    if (req.contains("tenant_id") && req["tenant_id"].is_number()) {
        long long tenant_id = req["tenant_id"].get<long long>();
        if (!db_.setDeviceTenant(devid, tenant_id, err)) {
            apiBad(ctx, err);
            return;
        }
    }
    audit(role, "permission.device.update", devid);
    refreshGroupAcl();
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleClientPermGroups(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string td = accountTenantDenied(id);
    if (!td.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, td); return; }
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
    std::string td = accountTenantDenied(id);
    if (!td.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, td); return; }
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
    // 现有组 + 目标组都要在自己范围内：既不能授予更多，也不能摘掉比自己高的组
    std::vector<long long> cur;
    db_.permGroupsOfClient(id, cur);
    DbPermScope before;
    db_.permScopeOfClient(id, before);
    std::vector<long long> check = cur;
    for (long long g : ids)
        if (std::find(check.begin(), check.end(), g) == check.end()) check.push_back(g);
    std::string denied = grantGroupsDenied(check);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    if (!db_.permSetClientGroups(id, ids, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.client.groups", std::to_string(id) + " n=" + std::to_string(ids.size()));
    refreshGroupAcl();
    DbPermScope after;
    db_.permScopeOfClient(id, after);
    if (!dbPermCovers(after, before)) kickClientsById(id, "account groups narrowed");
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleSubjectGroups(const HttpContextPtr& ctx) {
    std::string type = ctx->param("type");
    std::string id = ctx->param("id");
    std::string td = subjectTenantDenied(type, id);
    if (!td.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, td); return; }
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
    std::string td = subjectTenantDenied(type, id);
    if (!td.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, td); return; }
    // 不允许改自己的组：避免误操作把自己（尤其是唯一的超级管理员）锁在门外
    if (type == "user" && atoll(id.c_str()) == currentActor().id) {
        apiError(ctx, HTTP_STATUS_FORBIDDEN, "不能修改自己的权限组");
        return;
    }
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object() || !req.contains("ids") || !req["ids"].is_array()) {
        apiBad(ctx, "bad json");
        return;
    }
    if (type != "user" && type != "atk" && type != "client" && type != "rtk") {
        apiBad(ctx, "bad subject type");
        return;
    }
    std::vector<long long> ids;
    for (auto& v : req["ids"])
        if (v.is_number_integer()) ids.push_back(v.get<long long>());
    std::vector<long long> cur;
    db_.permGroupsOfSubject(type, id, cur);
    // 目标当前有效权限必须在自己范围内：防止横向降权（清空同事的组）和越权改组
    DbPermScope before;
    if (!subjectScope(type, id, before)) { apiNotFound(ctx, "授权对象不存在"); return; }
    std::string manage = manageDenied(before);
    if (!manage.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, manage); return; }
    std::vector<long long> check = cur;
    for (long long g : ids)
        if (std::find(check.begin(), check.end(), g) == check.end()) check.push_back(g);
    std::string denied = grantGroupsDenied(check);
    if (!denied.empty()) { apiError(ctx, HTTP_STATUS_FORBIDDEN, denied); return; }
    auto scope_before = (type == "user") ? snapshotUserScopes() : std::map<long long, DbPermScope>();
    std::string err;
    if (!db_.permSetSubjectGroups(type, id, ids, err)) { apiBad(ctx, err); return; }
    audit(role, "permission.subject.groups", type + ":" + id + " n=" + std::to_string(ids.size()));
    refreshGroupAcl();
    if (type == "user") kickConsoleWsShrunk(scope_before, "subject groups narrowed");
    else kickSubjectShrunk(type, id, before, "subject groups narrowed");
    ctx->send("{\"ok\":true}");
}
