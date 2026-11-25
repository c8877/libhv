#pragma once
// relay 地址的表示与解析。agent/client 的 --server、relay 下发的列表、
// 管理台「运维 → 中继列表」共用这一套规则。

#include <string>
#include <vector>

struct TunnelEndpoint {
    std::string host;
    int         port = 8443;
};

// 解析地址列表：
//   "h"               -> h:default_port
//   "h:8443"          -> h:8443
//   "[::1]:8443"      -> ::1:8443
//   "h1,h2:9000"      -> 两个地址
// 最多保留 8 个，多余的丢弃。格式错误返回 false 并填充 err。
bool tunnel_parse_endpoints(const std::string& spec, int default_port,
                            std::vector<TunnelEndpoint>& out, std::string& err);

// 规范化为 "h:p,h2:p2"（IPv6 自动加方括号），用于落盘与展示。
std::string tunnel_format_endpoints(const std::vector<TunnelEndpoint>& eps);

// 在列表中查找 host+port 相同的项；不存在返回 eps.size()。
size_t tunnel_endpoint_index(const std::vector<TunnelEndpoint>& eps, const TunnelEndpoint& ep);

// 按优先级挑选下一个连接地址：下标即优先级（0 最高）。
// 跳过永久认证失败（auth_bad=1）的地址；处于失败冷却期的地址（fail_until_ms > now_ms）
// 先跳过，只有在没有可用地址时才返回冷却最早到期的那个用于探测。
// 全部被认证拒绝时返回 eps 个数，调用方应放弃重连。
// auth_bad 与 fail_until_ms 需与地址列表等长（缺项按 0 处理）。
size_t tunnel_pick_endpoint(const std::vector<char>& auth_bad,
                            const std::vector<long long>& fail_until_ms, long long now_ms);
