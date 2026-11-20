#pragma once
// 设备进程快照与进程控制。
//
// 数据来源：
//   /proc/<pid>/{stat,status,cmdline}
//   /etc/passwd            uid -> 用户名
//   /proc/loadavg          负载
//   /proc/meminfo          物理内存占用
//
// CPU% 用两次采样的 utime+stime 差值计算，为单核百分比，多线程进程可能
// 超过 100。首次采样用 300ms 间隔；已有旧快照时改用两次快照的间隔，
// 避免把本次扫描自身的开销算进 agent 的 CPU。列表按条目数与 26KB 预算双重截断。

#include "cpputil/json.hpp"

#include <map>
#include <string>
#include <utility>

namespace rtproc {

struct Options {
    std::string sort = "cpu"; // cpu|mem|pid|start
    std::string q;            // 匹配 pid / 名称 / 命令行 / 用户
    bool kernel = false;      // 是否包含内核线程
    int page = 1;             // 从 1 开始
    int page_size = 10;       // 1..200
};

// 一次采样得到的全量进程行与系统摘要；可按需缓存后反复渲染分页结果。
struct Snapshot {
    nlohmann::json rows = nlohmann::json::array();
    nlohmann::json summary = nlohmann::json::object();
    uint64_t ts = 0;
    int sample_ms = 300; // 本次 CPU% 的统计窗口
    // pid -> {累计 CPU 毫秒, 启动时间}，用于跨快照算 CPU%
    std::map<int, std::pair<uint64_t, uint64_t>> ticks;
};

// 采样：两次读 /proc 算 CPU%，不做过滤/排序/分页。
// window_ms > 0 时在两次采样间等待该时长；0 表示只采一次，cpu_pct 置 0，
// 之后由 diffCpu 用旧快照补齐。默认 300ms。
// err 只在读不到 /proc 时非空。
bool sample(Snapshot& out, std::string& err, int window_ms = 300);

// 用上一份快照的累计 CPU 时间计算本份快照的 cpu_pct，
// 统计窗口为两份快照的时间差；新出现或已重启的进程记为 0。
void diffCpu(Snapshot& cur, const Snapshot& prev);

// 按选项过滤、排序、分页并限制单帧预算；age_ms > 0 表示来自缓存。
nlohmann::json render(const Snapshot& snap, const Options& opt, uint64_t age_ms);

// {"ts","sample_ms","summary":{...},"entries":[...],"truncated":bool}
// 等价于 sample + render(age_ms=0)。
nlohmann::json collect(const Options& opt, std::string& err);

// 单进程详情：命令行、exe、cwd、内存明细、CPU 时间、启动时间。
nlohmann::json detail(int pid, std::string& err);

// 发送信号；拒绝 pid<=1、agent 自身与非法信号号。
bool sendSignal(int pid, int sig, std::string& err);

} // namespace rtproc
