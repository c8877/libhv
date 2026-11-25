#include "audit_log.hpp"
#include "cpputil/json.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

namespace {

// actor 形如 client:user#3/token7/operator/10.0.0.2 或 alice#1/admin/10.0.0.2。
// actor_type 只做粗分类，完整身份仍在 actor 字段里。
std::string classifyActor(const std::string& who) {
    if (who.rfind("ai:", 0) == 0) return "ai";
    if (who == "anonymous" || who.empty()) return "anonymous";
    if (who.rfind("client:", 0) == 0) return "client";
    if (who.find("/token") != std::string::npos) return "token";
    return "user";
}

std::string utcNow() {
    char ts[32];
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
    return ts;
}

} // namespace

void AuditLog::init(const std::string& path) {
    path_ = path;
}

void AuditLog::write(const std::string& role, const std::string& action, const std::string& detail) {
    AuditEntry e;
    e.at = utcNow();
    e.actor = role.empty() ? "anonymous" : role;
    e.actor_type = classifyActor(e.actor);
    e.action = action;
    e.result = "ok";
    e.detail = detail;
    writeEntry(e);
}

void AuditLog::writeEntry(const AuditEntry& in) {
    AuditEntry e = in;
    if (e.at.empty()) e.at = utcNow();
    if (e.actor.empty()) e.actor = "anonymous";
    if (e.actor_type.empty()) e.actor_type = classifyActor(e.actor);
    if (e.result.empty()) e.result = "ok";

    // 文本行保持原有格式，便于既有 grep 习惯与 tail
    char line[1024];
    snprintf(line, sizeof(line), "[%s] actor=%s action=%s detail=%s", e.at.c_str(), e.actor.c_str(),
             e.action.c_str(), e.detail.c_str());

    nlohmann::json j;
    j["at"] = e.at;
    j["actor_type"] = e.actor_type;
    j["actor"] = e.actor;
    j["action"] = e.action;
    if (!e.target_type.empty()) j["target_type"] = e.target_type;
    if (!e.target_id.empty()) j["target_id"] = e.target_id;
    j["result"] = e.result;
    j["detail"] = e.detail;
    if (!e.op_id.empty()) j["op_id"] = e.op_id;
    std::string js = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);

    std::lock_guard<std::mutex> lk(mu_);
    if (!path_.empty()) {
        FILE* fp = fopen(path_.c_str(), "a");
        if (fp) {
            fprintf(fp, "%s\n", line);
            fclose(fp);
        }
        FILE* jf = fopen((path_ + ".jsonl").c_str(), "a");
        if (jf) {
            fprintf(jf, "%s\n", js.c_str());
            fclose(jf);
        }
    }
    ring_.push_back(std::move(js));
    if (ring_.size() > 1000) ring_.erase(ring_.begin());
}

void AuditLog::rotate(size_t max_bytes) {
    std::lock_guard<std::mutex> lk(mu_);
    if (path_.empty()) return;
    const std::string files[2] = {path_, path_ + ".jsonl"};
    for (const auto& f : files) {
        struct stat st{};
        if (stat(f.c_str(), &st) != 0 || (size_t)st.st_size < max_bytes) continue;
        rename(f.c_str(), (f + ".1").c_str());
    }
}

std::string AuditLog::tailJson(int n) const {
    std::lock_guard<std::mutex> lk(mu_);
    nlohmann::json arr = nlohmann::json::array();
    int count = 0;
    for (auto it = ring_.rbegin(); it != ring_.rend() && count < n; ++it, ++count) {
        nlohmann::json e = nlohmann::json::parse(*it, nullptr, false);
        if (!e.is_discarded()) arr.push_back(std::move(e));
    }
    return arr.dump();
}
