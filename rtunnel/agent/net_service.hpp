#pragma once
// 设备网络与 IPC 快照采集。
//
// 数据来源：
//   TCP/UDP  —— /proc/net/{tcp,tcp6,udp,udp6}
//   UNIX     —— /proc/net/unix
//   进程归属 —— /proc/<pid>/fd 中的 socket:[inode]
//   IPC      —— lsipc -m|-q|-s -J，缺失时回退 /proc/sysvipc/*
//
// 采集结果直接作为 NET_INFO 的载荷，条目数受 limit 与 26KB 预算双重限制。

#include "cpputil/json.hpp"

#include <string>

namespace rtnet {

struct Options {
    std::string scope = "all"; // tcp|udp|unix|ipc|all
    bool with_proc = true;     // 反查 socket 占用进程
    bool with_ipc = true;      // 是否执行 IPC 采集（需要 fork lsipc，开销较大）
    int page = 1;              // 从 1 开始
    int page_size = 10;        // 1..200
    std::string q;             // 地址/端口/路径/进程关键字
    bool listen_only = false;  // TCP LISTEN / UDP UNCONN
    std::string utype;         // UNIX: STREAM|DGRAM|all
    std::string ipc_kind = "shm"; // shm|msg|sem
};

bool scopeValid(const std::string& s);

// 把完整 IPC 快照按 ipc_kind/q/page 切成一页；其他 kind 的数组会置空，counts 保留。
nlohmann::json pageIpc(nlohmann::json ipc, const Options& opt);

// 返回 {"scope","ts","summary":{tcp,udp,unix,ipc},"tcp":{...},"udp":{...},"unix":{...},"ipc":{...}}
// summary.ipc = -1 表示本次未采集（调用方可用缓存补齐）。
// err 只在整体失败（读不到 /proc/net）时非空。
nlohmann::json collect(const Options& opt, std::string& err);

} // namespace rtnet
