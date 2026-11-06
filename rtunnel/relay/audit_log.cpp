#include "audit_log.hpp"
#include "cpputil/json.hpp"

#include <stdio.h>
#include <time.h>

void AuditLog::init(const std::string& path) {
    path_ = path;
}

void AuditLog::write(const std::string& role, const std::string& action, const std::string& detail) {
    char ts[32];
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);

    char line[1024];
    snprintf(line, sizeof(line), "[%s] role=%s action=%s detail=%s",
             ts, role.c_str(), action.c_str(), detail.c_str());

    if (!path_.empty()) {
        FILE* fp = fopen(path_.c_str(), "a");
        if (fp) {
            fprintf(fp, "%s\n", line);
            fclose(fp);
        }
    }
    ring_.push_back(line);
    if (ring_.size() > 1000) ring_.erase(ring_.begin());
}

std::string AuditLog::tailJson(int n) const {
    nlohmann::json arr = nlohmann::json::array();
    int count = 0;
    for (auto it = ring_.rbegin(); it != ring_.rend() && count < n; ++it, ++count) {
        arr.push_back(*it);
    }
    return arr.dump();
}
