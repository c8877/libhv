#include "endpoint.hpp"

#include <stdio.h>
#include <stdlib.h>

static bool parsePort(const std::string& s, int& port) {
    if (s.empty()) return false;
    char* end = NULL;
    long v = strtol(s.c_str(), &end, 10);
    if (!end || *end != '\0' || v <= 0 || v > 65535) return false;
    port = (int)v;
    return true;
}

bool tunnel_parse_endpoints(const std::string& spec, int default_port,
                            std::vector<TunnelEndpoint>& out, std::string& err) {
    out.clear();
    err.clear();
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t comma = spec.find(',', pos);
        std::string item = spec.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        pos = (comma == std::string::npos) ? spec.size() + 1 : comma + 1;

        size_t b = item.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        size_t e = item.find_last_not_of(" \t");
        item = item.substr(b, e - b + 1);
        if (item.empty()) continue;

        TunnelEndpoint ep;
        ep.port = default_port;
        if (item[0] == '[') {
            size_t rb = item.find(']');
            if (rb == std::string::npos) { err = "invalid IPv6 endpoint: " + item; return false; }
            ep.host = item.substr(1, rb - 1);
            if (rb + 1 < item.size()) {
                if (item[rb + 1] != ':' || !parsePort(item.substr(rb + 2), ep.port)) {
                    err = "invalid port in endpoint: " + item;
                    return false;
                }
            }
        } else {
            size_t c1 = item.find(':');
            // 单个冒号才当 host:port；多个冒号视为裸 IPv6，用默认端口
            if (c1 != std::string::npos && item.find(':', c1 + 1) == std::string::npos) {
                ep.host = item.substr(0, c1);
                if (!parsePort(item.substr(c1 + 1), ep.port)) {
                    err = "invalid port in endpoint: " + item;
                    return false;
                }
            } else {
                ep.host = item;
            }
        }
        if (ep.host.empty()) { err = "empty host in endpoint: " + item; return false; }
        if (ep.port <= 0 || ep.port > 65535) { err = "invalid default port"; return false; }
        if (out.size() >= 8) break;   // 超出上限的部分忽略
        out.push_back(ep);
    }
    if (out.empty()) { err = "no endpoint in: " + spec; return false; }
    return true;
}

std::string tunnel_format_endpoints(const std::vector<TunnelEndpoint>& eps) {
    std::string s;
    char tail[16];
    for (size_t i = 0; i < eps.size(); ++i) {
        if (i) s += ",";
        if (eps[i].host.find(':') != std::string::npos) {
            s += "[" + eps[i].host + "]";
        } else {
            s += eps[i].host;
        }
        snprintf(tail, sizeof(tail), ":%d", eps[i].port);
        s += tail;
    }
    return s;
}

size_t tunnel_endpoint_index(const std::vector<TunnelEndpoint>& eps, const TunnelEndpoint& ep) {
    for (size_t i = 0; i < eps.size(); ++i) {
        if (eps[i].host == ep.host && eps[i].port == ep.port) return i;
    }
    return eps.size();
}
