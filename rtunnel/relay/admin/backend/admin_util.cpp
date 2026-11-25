#include "admin_util.hpp"

std::string adminCookieValue(const HttpContextPtr& ctx, const std::string& key) {
    const HttpCookie& ck = ctx->request->GetCookie(key.c_str());
    if (!ck.name.empty()) return ck.value;
    std::string c = ctx->header("Cookie");
    size_t pos = 0;
    while (pos < c.size()) {
        size_t semi = c.find(';', pos);
        std::string item = c.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos);
        size_t eq = item.find('=');
        if (eq != std::string::npos) {
            std::string k = item.substr(0, eq);
            while (!k.empty() && (k[0] == ' ' || k[0] == '\t')) k.erase(0, 1);
            if (k == key) return item.substr(eq + 1);
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return "";
}

bool adminReqIsHttps(const HttpContextPtr& ctx) {
    if (ctx->request && ctx->request->scheme == "https") return true;
    return ctx->header("X-Forwarded-Proto") == "https";
}

std::string adminMaskUrl(const std::string& url) {
    if (url.empty()) return "";
    size_t scheme = url.find("://");
    size_t host_start = scheme == std::string::npos ? 0 : scheme + 3;
    size_t slash = url.find('/', host_start);
    std::string head = url.substr(0, slash == std::string::npos ? url.size() : slash);
    std::string tail = url.size() > 4 ? url.substr(url.size() - 4) : url;
    return head + "/…" + tail;
}
