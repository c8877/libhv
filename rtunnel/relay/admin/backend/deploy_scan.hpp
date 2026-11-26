#pragma once
// 子网扫描：找出网段里开了 SSH 的主机，给「部署」页直接选。
//
// 优先用系统 nmap（-Pn 跳过 ping 探测，避免屏蔽 ICMP 的主机被漏掉）；
// 没有 nmap 的精简镜像/静态部署退回到内置 TCP connect 扫描，行为一致：
// 只做 TCP 连接 + 读 SSH 横幅，不发任何包构造，不需要 root。

#include <string>
#include <vector>

struct DeployScanHost {
    std::string host;
    int         port = 22;
    std::string service;   // ssh / tcp
    int         rtt_ms = 0;
};

struct DeployScanResult {
    bool                     ok = false;
    std::string              error;
    std::string              engine;      // nmap / tcp
    int                      scanned = 0; // 网段内地址数
    long long                duration_ms = 0;
    std::vector<DeployScanHost> items;
};

// 只接受 IPv4 的 a.b.c.d 或 a.b.c.d/len；限制在私有/回环/CGNAT 段，
// 且地址数不超过 max_hosts。返回展开后的地址列表（网络地址与广播地址也包含，
// 真实网络里这两台常常就是网关/设备）。
bool deployParseCidr(const std::string& cidr, std::vector<std::string>& ips,
                     std::string& err, int max_hosts);

// port 默认 22；timeout_ms 是单个地址的连接超时（内含上限）；
// engine: auto（有 nmap 就用）/ nmap（没有就报错）/ tcp（强制内置扫描）
DeployScanResult deployScanSubnet(const std::string& cidr, int port, int timeout_ms,
                                  const std::string& engine = "auto");
