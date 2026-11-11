#include "token_util.hpp"

#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(WITH_OPENSSL)
#include "openssl/rand.h"
#endif

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
#if defined(WITH_OPENSSL)
    if (RAND_bytes(raw, sizeof(raw)) != 1) return std::string();
#else
    FILE* f = fopen("/dev/urandom", "rb");
    if (f == NULL) return std::string();
    size_t n = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (n != sizeof(raw)) return std::string();
#endif
    // base64url（无填充），自实现以免依赖 libhv 的 util 头路径
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out = "rtk_";
    out.reserve(47);
    size_t i = 0;
    while (i + 3 <= sizeof(raw)) {
        uint32_t v = ((uint32_t)raw[i] << 16) | ((uint32_t)raw[i + 1] << 8) | raw[i + 2];
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6) & 0x3F]);
        out.push_back(tbl[v & 0x3F]);
        i += 3;
    }
    size_t rem = sizeof(raw) - i;
    if (rem == 1) {
        uint32_t v = (uint32_t)raw[i] << 16;
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
    } else if (rem == 2) {
        uint32_t v = ((uint32_t)raw[i] << 16) | ((uint32_t)raw[i + 1] << 8);
        out.push_back(tbl[(v >> 18) & 0x3F]);
        out.push_back(tbl[(v >> 12) & 0x3F]);
        out.push_back(tbl[(v >> 6) & 0x3F]);
    }
    return out;
}

std::string clientTokenPrefix(const std::string& token) {
    if (token.size() <= 12) return token;
    return token.substr(0, 12) + "…";
}

} // namespace rtutil
