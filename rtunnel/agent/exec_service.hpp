#pragma once
// 设备侧受控命令执行。
//
// 与 OPEN_SHELL 的区别：不走 PTY、不经过 /bin/sh、一次性执行返回退出码，
// 输出有字节上限、执行有超时，命令必须命中启动参数里的白名单模板。
//
// 白名单模板是空格分隔的 argv 片段，逐段做通配匹配（`*` 任意长度，`?` 单字符），
// 段数必须与 argv 相等。例如 "systemctl status *" 只放行 3 段且首段为 systemctl。
// 匹配是逐段的，不做前缀匹配，避免 "systemctl status *" 放行
// "systemctl status foo; rm -rf /" 这类拼接（本实现也不经过 shell）。

#include <string>
#include <vector>

namespace rtexec {

struct Options {
    std::vector<std::string> allow;   // 每项一个模板，如 "systemctl status *"
    int max_timeout_ms = 30000;       // 请求超时的上限
    int max_bytes = 262144;           // 两路输出合计上限
};

struct Result {
    bool ok = false;                  // 是否成功启动并等待到退出
    std::string error;                // ok=false 时的原因
    int exit_code = -1;               // 正常退出码；被信号杀死时 -1
    int signal = 0;                   // 终止信号（0 表示不是被信号杀死）
    long long duration_ms = 0;
    size_t stdout_bytes = 0;
    size_t stderr_bytes = 0;
    bool truncated = false;           // 输出超过上限被截断
    bool timed_out = false;           // 超时被强杀
    std::string out;                  // stdout（最多 max_bytes）
    std::string err;                  // stderr（最多 max_bytes）
};

// 单段通配匹配：`*` 任意长度，`?` 单字符。
bool matchSegment(const std::string& pattern, const std::string& text);

// argv 是否命中任一模板。命中返回 true；否则 reason 给出 "未授权: <argv[0]>" 之类的说明。
bool allowed(const Options& opt, const std::vector<std::string>& argv, std::string& reason);

// 解析 "cmd a *;cmd b" 形式的白名单（分号分隔，忽略空白项）。
std::vector<std::string> parseAllow(const std::string& spec);

// 同步执行：fork + execv，父进程 poll 两路管道，超时或超字节数即终止。
// 必须在工作线程调用（会阻塞到命令结束或超时）。
void run(const Options& opt, const std::vector<std::string>& argv, const std::string& cwd,
         int timeout_ms, size_t max_bytes, Result& r);

} // namespace rtexec
