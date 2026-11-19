#pragma once
// 设备进程快照与进程控制。
//
// 数据来源：
//   /proc/<pid>/{stat,status,cmdline}
//   /etc/passwd            uid -> 用户名
//   /proc/loadavg          负载
//   /proc/meminfo          物理内存占用
//
// CPU% 用两次采样（间隔 300ms）的 utime+stime 差值计算，为单核百分比，
// 多线程进程可能超过 100。列表按条目数与 26KB 预算双重截断。

#include "cpputil/json.hpp"

#include <string>

namespace rtproc {

struct Options {
    std::string sort = "cpu"; // cpu|mem|pid|start
    std::string q;            // 匹配 pid / 名称 / 命令行 / 用户
    bool kernel = false;      // 是否包含内核线程
    int page = 1;             // 从 1 开始
    int page_size = 10;       // 1..200
};

// {"ts","sample_ms","summary":{...},"entries":[...],"truncated":bool}
// err 只在读不到 /proc 时非空。
nlohmann::json collect(const Options& opt, std::string& err);

// 单进程详情：命令行、exe、cwd、内存明细、CPU 时间、启动时间。
nlohmann::json detail(int pid, std::string& err);

// 发送信号；拒绝 pid<=1、agent 自身与非法信号号。
bool sendSignal(int pid, int sig, std::string& err);

} // namespace rtproc
