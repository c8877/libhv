// 操作面：踢会话 / 设备下线 / ACL 读写与热加载 / 审计查询
#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <stdio.h>
#include <signal.h>

using nlohmann::json;

void AdminServer::handleKickStream(const HttpContextPtr& ctx, const std::string& role) {
    uint32_t a_sid = (uint32_t)atoi(ctx->param("a_sid").c_str());
    bool all = currentActor().role != "user";
    std::vector<std::string> mine = myDevids();
    int found = 0;
    callRelay([this, a_sid, &found, all, mine]() {
        for (RelayConn* ag = relay_->agents; ag; ag = ag->reg_next) {
            for (RelayStream* s = ag->streams; s; s = s->next_c) {
                if (s->a_sid != a_sid) continue;
                if (!all && std::find(mine.begin(), mine.end(), std::string(ag->devid)) == mine.end()) {
                    found = 2;
                    return std::string();
                }
                found = relay_->kickStream(a_sid) ? 1 : 0;
                return std::string();
            }
        }
        return std::string();
    });
    if (found == 2) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    if (!found) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"stream not found\"}");
        return;
    }
    audit(role, "streams.kick", "a_sid=" + std::to_string(a_sid));
    json j;
    j["a_sid"] = a_sid;
    j["kicked"] = true;
    ctx->send(j.dump());
}

void AdminServer::handleKickAgent(const HttpContextPtr& ctx, const std::string& role) {
    std::string devid = ctx->param("devid");
    int found = 0;
    callRelay([this, &devid, &found]() {
        found = relay_->kickAgent(devid.c_str()) ? 1 : 0;
        return std::string();
    });
    if (!found) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"device not online\"}");
        return;
    }
    audit(role, "agents.kick", devid);
    json j;
    j["devid"] = devid;
    j["kicked"] = true;
    ctx->send(j.dump());
}

void AdminServer::handleAclGet(const HttpContextPtr& ctx) {
    std::string body = callRelay([this]() {
        json j;
        j["path"] = relay_->acl_file_path;
        std::string content;
        if (relay_->acl_file_path[0]) {
            FILE* fp = fopen(relay_->acl_file_path, "r");
            if (fp) {
                char buf[512];
                while (fgets(buf, sizeof(buf), fp)) content += buf;
                fclose(fp);
            }
        }
        j["content"] = content;
        j["cli"] = json::array();
        for (AclEntry* e = relay_->acl_cli; e; e = e->next) {
            j["cli"].push_back(std::string(e->user) + "=" + e->pattern);
        }
        return j.dump();
    });
    ctx->send(body);
}

void AdminServer::handleAclPut(const HttpContextPtr& ctx, const std::string& role) {
    json req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.contains("content")) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"expect {\\\"content\\\":...}\"}");
        return;
    }
    std::string content = req["content"].get<std::string>();

    int rc = 0, entries = 0;
    std::string err;
    callRelay([this, &content, &rc, &entries, &err]() {
        if (!relay_->acl_file_path[0]) { rc = 400; err = "no --acl-file configured"; return std::string(); }
        std::string path = relay_->acl_file_path;
        std::string tmp = path + ".tmp";
        FILE* fp = fopen(tmp.c_str(), "w");
        if (!fp) { rc = 500; err = "write failed"; return std::string(); }
        fputs(content.c_str(), fp);
        fclose(fp);
        if (rename(tmp.c_str(), path.c_str()) != 0) { rc = 500; err = "rename failed"; return std::string(); }
        if (relay_->aclFileLoad() != 0) { rc = 400; err = "parse failed, old acl kept"; return std::string(); }
        for (AclEntry* e = relay_->acl_file; e; e = e->next) entries++;
        return std::string();
    });
    if (rc != 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        json j;
        j["error"] = err;
        ctx->send(j.dump());
        return;
    }
    audit(role, "acl.put", std::to_string(entries) + " entries");
    json j;
    j["entries"] = entries;
    ctx->send(j.dump());
}

void AdminServer::handleAclReload(const HttpContextPtr& ctx, const std::string& role) {
    int rc = 0;
    callRelay([this, &rc]() {
        rc = relay_->aclFileLoad();
        return std::string();
    });
    audit(role, "acl.reload", rc == 0 ? "ok" : "failed");
    json j;
    j["reloaded"] = (rc == 0);
    ctx->send(j.dump());
}

void AdminServer::handleAudit(const HttpContextPtr& ctx) {
    (void)ctx;
    ctx->send(audit_.tailJson(200));
}

void AdminServer::handleShutdown(const HttpContextPtr& ctx, const std::string& role) {
    audit(role, "shutdown", "requested");
    ctx->send("{\"draining\":true}");
    // 在 relay 线程复用 SIGTERM 的 drain 路径；drain 期间 admin 线程仍可写回响应
    callRelay([this]() {
        relay_->onShutdown(SIGTERM);
        return std::string();
    });
}
