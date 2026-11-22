#include "token_util.hpp"
#include "sha256.hpp"
#include "util/base64.h"

#include "openssl/rand.h"

#include <string.h>
#include <time.h>

namespace rtutil {

TokenPeriod tokenPeriodFromName(const std::string& name) {
    if (name == "daily") return PERIOD_DAILY;
    if (name == "monthly") return PERIOD_MONTHLY;
    return PERIOD_TOTAL;
}

const char* tokenPeriodName(TokenPeriod p) {
    switch (p) {
    case PERIOD_DAILY:   return "daily";
    case PERIOD_MONTHLY: return "monthly";
    default:             return "total";
    }
}

namespace {

int64_t utcOf(int year, int mon, int day) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = year - 1900;
    t.tm_mon = mon - 1;
    t.tm_mday = day;
    return (int64_t)timegm(&t) * 1000;
}

int64_t monthStart(int64_t ms) {
    time_t sec = (time_t)(ms / 1000);
    struct tm t;
    gmtime_r(&sec, &t);
    return utcOf(t.tm_year + 1900, t.tm_mon + 1, 1);
}

bool randomBytes(unsigned char* out, size_t len) {
    return RAND_bytes(out, (int)len) == 1;
}

} // namespace

int64_t tokenPeriodStart(TokenPeriod p, int64_t now_ms) {
    switch (p) {
    case PERIOD_DAILY:   return now_ms / 86400000 * 86400000;
    case PERIOD_MONTHLY: return monthStart(now_ms);
    default:             return 0;
    }
}

int64_t tokenPeriodNext(TokenPeriod p, int64_t period_start_ms) {
    if (period_start_ms <= 0) return 0;
    if (p == PERIOD_DAILY) return period_start_ms + 86400000;
    if (p == PERIOD_MONTHLY) {
        time_t sec = (time_t)(period_start_ms / 1000);
        struct tm t;
        gmtime_r(&sec, &t);
        int y = t.tm_year + 1900;
        int m = t.tm_mon + 1;
        if (++m > 12) { m = 1; y++; }
        return utcOf(y, m, 1);
    }
    return 0;
}

int64_t tokenPeriodNextFrom(TokenPeriod p, int64_t period_start_ms) {
    if (p == PERIOD_TOTAL) return 0;
    if (period_start_ms <= 0) return tokenPeriodNext(p, tokenPeriodStart(p, (int64_t)time(NULL) * 1000));
    return tokenPeriodNext(p, period_start_ms);
}

bool isClientToken(const std::string& s) {
    return s.size() > 4 && s.compare(0, 4, "rtk_") == 0;
}

std::string newClientToken() {
    unsigned char raw[32];
    if (!randomBytes(raw, sizeof(raw))) return std::string();
    std::string b64 = hv::Base64Encode(raw, sizeof(raw));
    while (!b64.empty() && b64.back() == '=') b64.pop_back();
    for (char& c : b64) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    return "rtk_" + b64;
}

std::string tokenPrefix(const std::string& token) {
    if (token.size() <= 12) return token;
    return token.substr(0, 12) + "…";
}

bool isAdminToken(const std::string& s) {
    return s.size() > 4 && s.compare(0, 4, "atk-") == 0;
}

std::string newAdminToken() {
    unsigned char raw[32];
    if (!randomBytes(raw, sizeof(raw))) return std::string();
    return "atk-" + sha256Hex(raw, sizeof(raw));
}

} // namespace rtutil
