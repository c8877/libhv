// 设备 TTY 配置
#include "admin_server.hpp"
#include "cpputil/json.hpp"

using nlohmann::json;

static bool validAlias(const std::string& a) {
    if (a.empty()) return true;
    if (a.size() > 32) return false;
    for (unsigned char c : a) {
        if (!(isalnum(c) || c == '_' || c == '-')) return false;
    }
    return true;
}

static bool validBaud(int b) {
    static const int ok[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600};
    for (int v : ok) if (v == b) return true;
    return false;
}

void AdminServer::handleTtyList(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    std::string out;
    if (!db_.ttyList(devid, out)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db\"}");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleTtyPut(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    json body;
    try { body = json::parse(ctx->body()); } catch (...) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::string path = body.value("path", "");
    std::string alias = body.value("alias", "");
    std::string parity = body.value("parity", "N");
    std::string roles = body.value("roles", "operator,manager,admin");
    int enabled = body.value("enabled", true) ? 1 : 0;
    int baud = body.value("baud", 115200);
    int data_bits = body.value("data_bits", 8);
    int stop_bits = body.value("stop_bits", 1);

    if (path.empty() || path[0] != '/' || path.size() > 255 || !validAlias(alias) ||
        !validBaud(baud) || data_bits < 5 || data_bits > 8 ||
        (parity != "N" && parity != "E" && parity != "O") ||
        (stop_bits != 1 && stop_bits != 2)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad tty config\"}");
        return;
    }
    AdminActor actor = currentActor();
    std::string err;
    if (!db_.ttyUpsert(devid, path, alias, enabled, baud, data_bits, parity, stop_bits,
                       roles, actor.username, err)) {
        ctx->response->status_code = HTTP_STATUS_CONFLICT;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    audit(actor.username, "tty.config", devid + " " + path + " alias=" + alias);
    std::string out;
    db_.ttyList(devid, out);
    ctx->send(out);
}

void AdminServer::handleTtyDelete(const HttpContextPtr& ctx) {
    std::string devid = ctx->param("devid");
    if (!ownsDevid(devid)) {
        ctx->response->status_code = HTTP_STATUS_FORBIDDEN;
        ctx->send("{\"error\":\"forbidden\"}");
        return;
    }
    std::string path = ctx->param("path");
    if (path.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"path required\"}");
        return;
    }
    std::string err;
    if (!db_.ttyDelete(devid, path, err)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"" + err + "\"}");
        return;
    }
    AdminActor actor = currentActor();
    audit(actor.username, "tty.delete", devid + " " + path);
    ctx->send("{\"ok\":true}");
}
