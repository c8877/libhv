#include "exec_service.hpp"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

namespace rtexec {

namespace {

long long monoMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
        size_t b = i;
        while (i < s.size() && s[i] != ' ' && s[i] != '\t') ++i;
        if (i > b) out.push_back(s.substr(b, i - b));
    }
    return out;
}

// argv[0] 不含 '/' 时按 PATH 查找可执行文件；找不到返回空串。
std::string resolveExe(const std::string& name) {
    if (name.find('/') != std::string::npos) {
        return access(name.c_str(), X_OK) == 0 ? name : std::string();
    }
    const char* path = getenv("PATH");
    std::string dirs = path ? path : "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    size_t pos = 0;
    while (pos <= dirs.size()) {
        size_t colon = dirs.find(':', pos);
        std::string dir = dirs.substr(pos, colon == std::string::npos ? std::string::npos : colon - pos);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            if (access(full.c_str(), X_OK) == 0) return full;
        }
        if (colon == std::string::npos) break;
        pos = colon + 1;
    }
    return std::string();
}

void setNonBlock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

} // namespace

bool matchSegment(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0;
    size_t star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool allowed(const Options& opt, const std::vector<std::string>& argv, std::string& reason) {
    if (opt.allow.empty()) {
        reason = "命令执行未启用（启动参数缺少 --allow-exec）";
        return false;
    }
    if (argv.empty()) {
        reason = "argv 为空";
        return false;
    }
    for (const auto& item : argv) {
        if (item.size() > 4096 || item.find('\0') != std::string::npos) {
            reason = "argv 参数非法";
            return false;
        }
    }
    for (const auto& tmpl : opt.allow) {
        std::vector<std::string> segs = splitWs(tmpl);
        if (segs.size() != argv.size()) continue;
        bool hit = true;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (!matchSegment(segs[i], argv[i])) {
                hit = false;
                break;
            }
        }
        if (hit) return true;
    }
    reason = "命令不在白名单内: " + argv[0];
    return false;
}

std::vector<std::string> parseAllow(const std::string& spec) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= spec.size()) {
        size_t semi = spec.find(';', pos);
        std::string one = trim(spec.substr(pos, semi == std::string::npos ? std::string::npos : semi - pos));
        if (!one.empty()) out.push_back(one);
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    return out;
}

void run(const Options& opt, const std::vector<std::string>& argv, const std::string& cwd,
         int timeout_ms, size_t max_bytes, Result& r) {
    r = Result{};
    if (timeout_ms <= 0) timeout_ms = opt.max_timeout_ms;
    if (timeout_ms > opt.max_timeout_ms) timeout_ms = opt.max_timeout_ms;
    if (max_bytes == 0) max_bytes = (size_t)opt.max_bytes;
    if (max_bytes > (size_t)opt.max_bytes) max_bytes = (size_t)opt.max_bytes;

    std::string exe = resolveExe(argv[0]);
    if (exe.empty()) {
        r.error = "找不到可执行文件: " + argv[0];
        return;
    }

    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    if (pipe(out_pipe) != 0) {
        r.error = std::string("pipe: ") + strerror(errno);
        return;
    }
    if (pipe(err_pipe) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        r.error = std::string("pipe: ") + strerror(errno);
        return;
    }

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    std::vector<std::string> storage = argv;
    storage[0] = exe;
    for (auto& a : storage) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    long long t0 = monoMs();
    pid_t pid = fork();
    if (pid < 0) {
        r.error = std::string("fork: ") + strerror(errno);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        return;
    }
    if (pid == 0) {
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]); close(out_pipe[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        execv(exe.c_str(), cargv.data());
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    setNonBlock(out_pipe[0]);
    setNonBlock(err_pipe[0]);

    const long long deadline = t0 + timeout_ms;
    bool killed = false;
    int open_fds = 2;
    char buf[8192];
    while (open_fds > 0) {
        long long left = deadline - monoMs();
        if (left <= 0) {
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            killed = true;
            break;
        }
        struct pollfd pfd[2];
        pfd[0].fd = out_pipe[0];
        pfd[0].events = POLLIN;
        pfd[1].fd = err_pipe[0];
        pfd[1].events = POLLIN;
        int pr = poll(pfd, 2, (int)std::min<long long>(left, 200));
        if (pr < 0 && errno != EINTR) break;
        for (int i = 0; i < 2; ++i) {
            if (pfd[i].fd < 0 || !(pfd[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(pfd[i].fd, buf, sizeof buf);
            if (n > 0) {
                std::string& dst = (i == 0) ? r.out : r.err;
                size_t used = r.out.size() + r.err.size();
                size_t room = used < max_bytes ? max_bytes - used : 0;
                size_t take = (size_t)n < room ? (size_t)n : room;
                if (take < (size_t)n) r.truncated = true;
                if (take) dst.append(buf, take);
            } else if (n == 0) {
                close(pfd[i].fd);
                pfd[i].fd = -1;
                --open_fds;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                close(pfd[i].fd);
                pfd[i].fd = -1;
                --open_fds;
            }
        }
        if (r.out.size() + r.err.size() >= max_bytes) {
            // 到达上限后不再读，但也不能让子进程卡在写阻塞上
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            killed = true;
            break;
        }
    }
    if (out_pipe[0] >= 0) close(out_pipe[0]);
    if (err_pipe[0] >= 0) close(err_pipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    // 管道被提前关闭时子进程可能还在写；再补一刀，避免留下孤儿
    if (killed) kill(-pid, SIGKILL);

    r.duration_ms = monoMs() - t0;
    r.stdout_bytes = r.out.size();
    r.stderr_bytes = r.err.size();
    r.timed_out = killed && r.out.size() + r.err.size() < max_bytes;
    if (WIFEXITED(status)) {
        r.ok = true;
        r.exit_code = WEXITSTATUS(status);
        if (r.exit_code == 127 && r.out.empty() && r.err.empty()) {
            r.error = "exec 失败（127）";
        }
    } else if (WIFSIGNALED(status)) {
        r.signal = WTERMSIG(status);
        r.ok = !killed;   // 超时/截断导致的强杀不算成功启动失败，但标记出来
        if (killed) r.error = "timeout";
    } else {
        r.error = "waitpid 异常";
    }
}

} // namespace rtexec
