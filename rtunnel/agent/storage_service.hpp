#pragma once
// 分区目录占用统计：一次遍历挂载点，返回扁平目录列表（路径/字节/文件数/子目录数）。
//
// 扫描规则与 du -x 一致：不跨设备、不跟随符号链接、不进入伪文件系统；
// 单个分区的目录节点通常只有几百到几千条，整棵树一次建好后前端下钻不再扫描。

#include "cpputil/json.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace rtstorage {

struct Options {
    std::string mount = "/";
    int max_depth = 32;          // 统计深度，超过后不再累加（防深目录爆栈）
    uint64_t max_entries = 200000; // 单次扫描的目录项上限
    int max_seconds = 20;        // 单次扫描的时间上限
    int tree_depth = 16;         // 回传给前端的层级上限
    size_t max_nodes = 5000;     // 回传的目录节点上限
};

struct Result {
    bool ok = false;
    std::string error;
    std::string mount;
    uint64_t total_kb = 0;
    uint64_t used_kb = 0;
    uint64_t free_kb = 0;     // 非特权用户可用
    uint64_t reserved_kb = 0; // 保留块
    uint64_t bytes = 0;
    uint64_t files = 0;
    uint64_t dirs = 0;
    uint64_t entries = 0;
    int scan_ms = 0;
    bool truncated = false;
    std::string truncated_reason;
    std::vector<std::string> skipped; // 读不到的目录（最多 8 个）
    nlohmann::json nodes = nlohmann::json::array();
};

// 扫描挂载点。cancel 置位时尽快返回，ok=false、error="cancelled"。
bool scan(const Options& opt, const std::atomic<bool>& cancel, Result& out);

// 结果转 JSON（STATIC_INFO 与本地 HTTP 共用）。
std::string toJson(const Result& r);

} // namespace rtstorage
