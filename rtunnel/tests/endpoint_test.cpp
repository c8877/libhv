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

    // 下发列表的规范化与查找：写盘用规范化字符串，切换判断用 host+port 精确匹配
    expect(parse("a:1, [2001:db8::1]:2 ,c", 3, eps, err), "format source parses");
    expect(tunnel_format_endpoints(eps) == "a:1,[2001:db8::1]:2,c:3", "format canonical");
    {
        std::vector<TunnelEndpoint> back;
        expect(tunnel_parse_endpoints(tunnel_format_endpoints(eps), 8443, back, err), "format round trip parses");
        expect(back.size() == eps.size() && back[1].host == "2001:db8::1" && back[1].port == 2,
               "format round trip values");
    }
    {
        TunnelEndpoint cur;
        cur.host = "c"; cur.port = 3;
        expect(tunnel_endpoint_index(eps, cur) == 2, "find existing endpoint");
        cur.port = 4;
        expect(tunnel_endpoint_index(eps, cur) == eps.size(), "missing endpoint");
        expect(tunnel_endpoint_index(std::vector<TunnelEndpoint>(), cur) == 0, "empty list index");
    }

    // 优先级选择：下标 0 最高；跳过认证失败与失败冷却中的地址
    {
        std::vector<char> bad = {0, 0, 0};
        std::vector<long long> until = {0, 0, 0};
        expect(tunnel_pick_endpoint(bad, until, 1000) == 0, "pick top priority");
        bad = {1, 0, 0};
        expect(tunnel_pick_endpoint(bad, until, 1000) == 1, "skip auth rejected");
        bad = {0, 0, 0};
        until = {2000, 0, 0};
        expect(tunnel_pick_endpoint(bad, until, 1000) == 1, "skip cooling endpoint");
        until = {1000, 0, 0};
        expect(tunnel_pick_endpoint(bad, until, 1000) == 0, "cooldown expired");
        // 全都在冷却：挑最早解冻的探测，而不是死等当前地址
        until = {2000, 1500, 3000};
        expect(tunnel_pick_endpoint(bad, until, 1000) == 1, "all cooling -> earliest");
        bad = {1, 1, 1};
        until = {0, 0, 0};
        expect(tunnel_pick_endpoint(bad, until, 1000) == 3, "all auth rejected -> none");
        bad = {1, 0, 0};
        until = {0, 2000, 0};
        expect(tunnel_pick_endpoint(bad, until, 1000) == 2, "skip bad and cooling");
        expect(tunnel_pick_endpoint({}, {}, 1000) == 0, "empty pick");
    }

    if (g_fail) {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("endpoint_test: all checks passed\n");
    return 0;
}
