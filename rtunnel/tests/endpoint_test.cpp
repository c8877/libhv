// --server 地址列表解析的单测（无需网络）
#include "common/tunnel_link.hpp"

#include <stdio.h>
#include <string>

static int g_fail = 0;

static void expect(bool ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        g_fail++;
    }
}

static bool parse(const std::string& spec, int def, std::vector<TunnelEndpoint>& out, std::string& err) {
    return tunnel_parse_endpoints(spec, def, out, err);
}

static void one(const std::string& spec, int def,
                const std::string& host, int port, const char* what) {
    std::vector<TunnelEndpoint> eps;
    std::string err;
    bool ok = parse(spec, def, eps, err);
    if (!ok || eps.size() != 1 || eps[0].host != host || eps[0].port != port) {
        printf("FAIL: %s spec=%s -> %s size=%zu first=%s:%d\n", what, spec.c_str(),
               ok ? "ok" : err.c_str(), eps.size(),
               eps.empty() ? "" : eps[0].host.c_str(), eps.empty() ? 0 : eps[0].port);
        g_fail++;
    }
}

int main() {
    std::vector<TunnelEndpoint> eps;
    std::string err;

    one("relay.example.com", 8443, "relay.example.com", 8443, "bare host uses default port");
    one("relay.example.com:9000", 8443, "relay.example.com", 9000, "explicit port");
    one("[2001:db8::1]:9443", 8443, "2001:db8::1", 9443, "bracketed ipv6 with port");
    one("[::1]", 8443, "::1", 8443, "bracketed ipv6 default port");
    one("2001:db8::1", 8443, "2001:db8::1", 8443, "bare ipv6 default port");
    one(" 10.0.0.1 ", 7000, "10.0.0.1", 7000, "trim spaces + custom default");

    expect(parse("a:1, b:2 ,c", 3, eps, err), "comma list parses");
    expect(eps.size() == 3, "comma list size");
    expect(eps.size() == 3 && eps[0].host == "a" && eps[0].port == 1, "list item 0");
    expect(eps.size() == 3 && eps[1].host == "b" && eps[1].port == 2, "list item 1");
    expect(eps.size() == 3 && eps[2].host == "c" && eps[2].port == 3, "list item 2 default port");

    expect(parse("a,,b", 8443, eps, err) && eps.size() == 2, "empty item skipped");
    expect(parse("1.1.1.1,2.2.2.2,3.3.3.3,4.4.4.4,5.5.5.5,6.6.6.6,7.7.7.7,8.8.8.8,9.9.9.9,10.10.10.10",
                 8443, eps, err) && eps.size() == 8, "cap at 8 endpoints");

    expect(!parse("", 8443, eps, err), "empty spec rejected");
    expect(!parse("h:0", 8443, eps, err), "port 0 rejected");
    expect(!parse("h:70000", 8443, eps, err), "port >65535 rejected");
    expect(!parse("h:abc", 8443, eps, err), "non-numeric port rejected");
    expect(!parse("[::1:8443", 8443, eps, err), "unterminated bracket rejected");

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("endpoint_test: all checks passed\n");
    return 0;
}
