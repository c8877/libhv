// 系统配置与个人偏好
#include "admin_server.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <map>
#include <string>
#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <thread>
#include <chrono>

namespace {

struct Def { const char* key; const char* val; int lo; int hi;
             const char* label; const char* unit; const char* group; };
const Def kDefs[] = {
    {"file.max_upload_mb",   "32",  1, 512,      "单文件上传上限", "MiB", "文件"},
    {"file.max_download_mb", "32",  1, 512,      "单文件下载上限", "MiB", "文件"},
    {"proxy.max_concurrent", "32",  1, 256,      "并发上限",       "条",  "代理"},
    {"proxy.sysinfo_wait_ms","2000",500, 10000,  "SYSINFO 等待",   "ms",  "代理"},
    {"proxy.config_wait_ms", "2000",500, 10000,  "CONFIG_ACK 等待","ms",  "代理"},
    {"auth.session_hours",   "12",  1, 168,      "会话有效期",     "小时","账号安全"},
    {"auth.password_min",    "8",   6, 32,       "密码最小长度",   "位",  "账号安全"},
    {"auth.lock_fails",      "5",   1, 20,       "登录失败锁定",   "次",  "账号安全"},
    {"auth.lock_minutes",    "5",   1, 120,      "锁定时长",       "分钟","账号安全"},
    {"term.max_sessions",    "4",   1, 32,       "单账号最大终端数","个", "终端"},
    {"audit.retention_days", "30",  7, 365,      "审计保留",       "天",  "审计"},
    {"ui.site_title",        "rtunnel", 0, 0,    "站点标题",       "",    "展示"},
    {"ui.refresh_seconds",   "0",   0, 300,      "详情默认刷新",   "秒（0=关闭）","展示"},
};

const Def* findDef(const std::string& k) {
    for (auto& d : kDefs) if (k == d.key) return &d;
    return nullptr;
}

bool validate(const std::string& key, const std::string& val, std::string& err) {
    const Def* d = findDef(key);
    if (!d) { err = "unknown key"; return false; }
    if (d->lo == 0 && d->hi == 0) {
        if (val.size() > 64) { err = "value too long"; return false; }
        return true;
    }
    if (val.empty()) { err = "empty value"; return false; }
    for (char c : val) if (c < '0' || c > '9') { err = "not a number"; return false; }
    long n = atol(val.c_str());
    if (n < d->lo || n > d->hi) { err = "out of range"; return false; }
    return true;
}

const char* kPrefs[] = {"display_name", "avatar_color", "default_page", "time_format",
                       "term_font", "theme", "theme_color"};

} // namespace

void AdminServer::loadSettings() {
    std::map<std::string, std::string> dbv;
    db_.getSettings(dbv);
    std::lock_guard<std::mutex> lk(settings_mu_);
    settings_.clear();
    for (auto& d : kDefs) {
        auto it = dbv.find(d.key);
        settings_[d.key] = (it != dbv.end()) ? it->second : d.val;
    }
}

int AdminServer::settingInt(const std::string& key, int def) {
    std::lock_guard<std::mutex> lk(settings_mu_);
    auto it = settings_.find(key);
    if (it == settings_.end()) return def;
    return atoi(it->second.c_str());
}

void AdminServer::handleSettingsGet(const HttpContextPtr& ctx) {
    std::lock_guard<std::mutex> lk(settings_mu_);
    nlohmann::json j = nlohmann::json::object();
    for (auto& kv : settings_) j[kv.first] = kv.second;
    nlohmann::json schema = nlohmann::json::array();
    for (auto& d : kDefs) {
        schema.push_back({{"key", d.key}, {"label", d.label}, {"unit", d.unit},
                          {"group", d.group}, {"min", d.lo}, {"max", d.hi}});
    }
    j["_schema"] = schema;
    ctx->send(j.dump());
}

void AdminServer::handleSettingsSet(const HttpContextPtr& ctx) {
    auto req = nlohmann::json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::map<std::string, std::string> next;
    for (auto it = req.begin(); it != req.end(); ++it) {
        std::string k = it.key();
        std::string v = it.value().is_string() ? it.value().get<std::string>() : nlohmann::to_string(it.value());
        std::string err;
        if (!validate(k, v, err)) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"" + k + ": " + err + "\"}");
            return;
        }
        next[k] = v;
    }
    if (next.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"nothing to update\"}");
        return;
    }
    std::string by = currentActor().username;
    for (auto& kv : next) {
        if (!db_.setSetting(kv.first, kv.second, by)) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"db error\"}");
            return;
        }
        audit(by, "settings.update", kv.first + " = " + kv.second);
    }
    {
        std::lock_guard<std::mutex> lk(settings_mu_);
        for (auto& kv : next) settings_[kv.first] = kv.second;
    }
    handleSettingsGet(ctx);
}

void AdminServer::handleProfileGet(const HttpContextPtr& ctx) {
    AdminActor actor = currentActor();
    std::map<std::string, std::string> p;
    db_.getUserPrefs(actor.id, p);
    nlohmann::json j;
    j["username"] = actor.username;
    j["role"] = actor.role;
    j["display_name"] = p.count("display_name") ? p["display_name"] : actor.username;
    j["avatar_color"] = p.count("avatar_color") ? p["avatar_color"] : "";
    j["default_page"] = p.count("default_page") ? p["default_page"] : "overview";
    j["time_format"] = p.count("time_format") ? p["time_format"] : "local";
    j["term_font"] = p.count("term_font") ? p["term_font"] : "14";
    j["theme"] = p.count("theme") ? p["theme"] : "light";
    j["theme_color"] = p.count("theme_color") ? p["theme_color"] : "";
    ctx->send(j.dump());
}

void AdminServer::handleProfileSet(const HttpContextPtr& ctx) {
    AdminActor actor = currentActor();
    auto req = nlohmann::json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    for (auto it = req.begin(); it != req.end(); ++it) {
        std::string k = it.key();
        bool ok = false;
        for (auto* p : kPrefs) if (k == p) ok = true;
        if (!ok) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"unknown key " + k + "\"}");
            return;
        }
        std::string v = it.value().is_string() ? it.value().get<std::string>() : nlohmann::to_string(it.value());
        if (v.size() > 64) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"value too long\"}");
            return;
        }
        if (k == "term_font") {
            int f = atoi(v.c_str());
            if (f < 10 || f > 24) { ctx->response->status_code = HTTP_STATUS_BAD_REQUEST; ctx->send("{\"error\":\"term_font 10-24\"}"); return; }
        }
        if (k == "default_page" && v != "overview" && v != "agents") {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad default_page\"}");
            return;
        }
        if (k == "time_format" && v != "local" && v != "relative") {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad time_format\"}");
            return;
        }
        if (k == "theme" && v != "light" && v != "dark" && v != "color") {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad theme\"}");
            return;
        }
        if (k == "theme_color" && !v.empty() && (v.size() != 7 || v[0] != '#')) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad theme_color\"}");
            return;
        }
        if (k == "avatar_color" && v.size() != 7 && !v.empty()) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"bad color\"}");
            return;
        }
        if (!db_.setUserPref(actor.id, k, v)) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"db error\"}");
            return;
        }
    }
    handleProfileGet(ctx);
}

void AdminServer::cleanupAudit(int days) {
    if (cfg_.cert_dir.empty()) return;
    std::string path = cfg_.cert_dir + "/audit.log";
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) return;
    time_t cutoff = time(NULL) - (time_t)days * 86400;
    std::string keep;
    char line[4096];
    while (fgets(line, sizeof(line), fp)) {
        char ts[32] = {0};
        struct tm tm{};
        bool old = false;
        if (sscanf(line, "[%31[^]]]", ts) == 1 &&
            strptime(ts, "%Y-%m-%dT%H:%M:%SZ", &tm) != NULL) {
            time_t t = timegm(&tm);
            old = (t < cutoff);
        }
        if (!old) keep += line;
    }
    fclose(fp);
    std::string tmp = path + ".tmp";
    FILE* out = fopen(tmp.c_str(), "w");
    if (!out) return;
    fwrite(keep.data(), 1, keep.size(), out);
    fclose(out);
    rename(tmp.c_str(), path.c_str());
}

void AdminServer::cleanupUploads() {
    if (cfg_.file_dir.empty()) return;
    DIR* d = opendir(cfg_.file_dir.c_str());
    if (!d) return;
    time_t cutoff = time(NULL) - 3600;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        std::string n = e->d_name;
        if (n.size() < 11 || n.compare(n.size() - 10, 10, ".uploading") != 0) continue;
        std::string path = cfg_.file_dir + "/" + n;
        struct stat st{};
        if (stat(path.c_str(), &st) == 0 && st.st_mtime < cutoff) unlink(path.c_str());
    }
    closedir(d);
}

void AdminServer::handleLogoutAll(const HttpContextPtr& ctx) {
    AdminActor actor = currentActor();
    db_.dropUserSessions(actor.id);
    audit(actor.username, "account.logout_all", "");
    ctx->response->headers["Set-Cookie"] = "rt_sid=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict";
    ctx->send("{\"ok\":true}");
}
