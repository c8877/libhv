#include "proc_service.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace rtproc {

namespace {

const int kSampleMs = 300;
const size_t kMaxCmd = 1024;
const size_t kListCmd = 256; // 列表只回传命令前缀，详情页再读完整命令行
const size_t kBudget = 26000; // 单帧 payload 上限 32KB，留出 JSON 外壳余量

uint64_t wallMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool readFile(const char* path, std::string& out, size_t max_len = 256 * 1024) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        size_t take = (size_t)n;
        if (out.size() + take > max_len) take = max_len - out.size();
        out.append(buf, take);
        if (out.size() >= max_len) break;
    }
    close(fd);
    return true;
}

struct Stat {
    int pid = 0;
    int ppid = 0;
    int threads = 0;
    long long utime = 0;
    long long stime = 0;
    long long starttime = 0;
    std::string comm;
    std::string state;
};

struct Status {
    int uid = -1;
    long long rss_kb = 0;
    long long vsz_kb = 0;
    long long vmpeak_kb = 0;
    long long vmswap_kb = 0;
};

bool readStat(int pid, Stat& s) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    std::string line;
    if (!readFile(path, line, 8192) || line.empty()) return false;
    size_t l = line.find('(');
    size_t r = line.rfind(')');
    if (l == std::string::npos || r == std::string::npos || r <= l) return false;
    s.pid = atoi(line.c_str());
    s.comm = line.substr(l + 1, r - l - 1);
    std::istringstream is(line.substr(r + 1));
    std::string t[20];
    int n = 0;
    while (n < 20 && (is >> t[n])) ++n;
    if (n < 20) return false;
    s.state = t[0];
    s.ppid = atoi(t[1].c_str());
    s.utime = atoll(t[11].c_str());
    s.stime = atoll(t[12].c_str());
    s.threads = atoi(t[17].c_str());
    s.starttime = atoll(t[19].c_str());
    return true;
}

void readStatus(int pid, Status& st) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    std::string txt;
    if (!readFile(path, txt, 64 * 1024)) return;
    std::istringstream is(txt);
    std::string line;
    while (std::getline(is, line)) {
        if (line.compare(0, 4, "Uid:") == 0) {
            int u = -1;
            if (sscanf(line.c_str() + 4, "%d", &u) == 1) st.uid = u;
        } else if (line.compare(0, 6, "VmRSS:") == 0) {
            sscanf(line.c_str() + 6, "%lld", &st.rss_kb);
        } else if (line.compare(0, 7, "VmSize:") == 0) {
            sscanf(line.c_str() + 7, "%lld", &st.vsz_kb);
        } else if (line.compare(0, 7, "VmPeak:") == 0) {
            sscanf(line.c_str() + 7, "%lld", &st.vmpeak_kb);
        } else if (line.compare(0, 7, "VmSwap:") == 0) {
            sscanf(line.c_str() + 7, "%lld", &st.vmswap_kb);
        }
    }
}

std::string readCmdline(int pid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    std::string raw;
    if (!readFile(path, raw, kMaxCmd)) return std::string();
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\0') raw[i] = ' ';
    }
    while (!raw.empty() && raw.back() == ' ') raw.pop_back();
    return raw;
}

std::string readLink(const char* path) {
    char buf[1024];
    ssize_t n = readlink(path, buf, sizeof buf - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    return buf;
}

void loadUsers(std::map<int, std::string>& out) {
    std::ifstream f("/etc/passwd");
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find(':');
        if (a == std::string::npos) continue;
        size_t b = line.find(':', a + 1);
        if (b == std::string::npos) continue;
        size_t c = line.find(':', b + 1);
        if (c == std::string::npos) continue;
        int uid = atoi(line.substr(b + 1, c - b - 1).c_str());
        if (!out.count(uid)) out[uid] = line.substr(0, a);
    }
}

std::vector<int> listPids() {
    std::vector<int> out;
    DIR* d = opendir("/proc");
    if (!d) return out;
    struct dirent* de = nullptr;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] < '0' || de->d_name[0] > '9') continue;
        int pid = atoi(de->d_name);
        if (pid > 0) out.push_back(pid);
    }
    closedir(d);
    return out;
}

const char* stateText(const std::string& s) {
    if (s.empty()) return "未知";
    switch (s[0]) {
        case 'R': return "运行";
        case 'S': return "睡眠";
        case 'D': return "不可中断";
        case 'Z': return "僵尸";
        case 'T':
        case 't': return "停止";
        case 'X':
        case 'x': return "退出";
        case 'I': return "空闲";
        case 'W': return "换页";
        case 'P': return "停放";
        default: return "其他";
    }
}

double loadOne(const char* path, int idx) {
    std::string t;
    if (!readFile(path, t, 512)) return 0;
    std::istringstream is(t);
    double v = 0;
    for (int i = 0; i <= idx; ++i) {
        if (!(is >> v)) return 0;
    }
    return v;
}

std::string lower(const std::string& s) {
    std::string o = s;
    for (size_t i = 0; i < o.size(); ++i) o[i] = (char)tolower((unsigned char)o[i]);
    return o;
}

} // namespace

bool sample(Snapshot& out, std::string& err) {
    using nlohmann::json;
    err.clear();
    out.rows = json::array();
    out.summary = json::object();
    out.ts = 0;

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;

    double uptime = 0;
    {
        std::string t;
        if (readFile("/proc/uptime", t, 256)) uptime = atof(t.c_str());
    }
    uint64_t now = wallMs();
    uint64_t up_ms = (uint64_t)(uptime * 1000);
    uint64_t boot_ms = now > up_ms ? now - up_ms : 0;

    std::map<int, std::string> users;
    loadUsers(users);

    std::vector<int> pids = listPids();
    if (pids.empty()) {
        err = "read /proc failed";
        return false;
    }

    struct Row {
        Stat st;
        Status ss;
        std::string cmd;
        std::string user;
        bool kernel = false;
        double cpu = 0;
    };
    std::vector<Row> rows;
    rows.reserve(pids.size());
    for (size_t i = 0; i < pids.size(); ++i) {
        Row r;
        if (!readStat(pids[i], r.st)) continue;
        readStatus(pids[i], r.ss);
        r.cmd = readCmdline(pids[i]);
        r.kernel = r.cmd.empty();
        if (r.kernel) {
            r.cmd = "[" + r.st.comm + "]";
        } else if (r.cmd.size() > kListCmd) {
            size_t cut = kListCmd;
            while (cut > 0 && ((unsigned char)r.cmd[cut] & 0xC0) == 0x80) --cut;
            r.cmd = r.cmd.substr(0, cut) + "...";
        }
        auto u = users.find(r.ss.uid);
        if (u != users.end()) r.user = u->second;
        else if (r.ss.uid >= 0) r.user = std::to_string(r.ss.uid);
        rows.push_back(std::move(r));
    }

    // 第二次采样算 CPU%
    usleep(kSampleMs * 1000);
    for (size_t i = 0; i < rows.size(); ++i) {
        Stat s2;
        if (!readStat(rows[i].st.pid, s2)) continue;
        long long d = (s2.utime + s2.stime) - (rows[i].st.utime + rows[i].st.stime);
        if (d < 0) d = 0;
        rows[i].cpu = (double)d * 100.0 / (double)hz / ((double)kSampleMs / 1000.0);
    }

    int running = 0, sleeping = 0, zombie = 0, stopped = 0, kernel = 0;
    long long threads = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        threads += rows[i].st.threads;
        if (rows[i].kernel) ++kernel;
        const std::string& s = rows[i].st.state;
        if (s == "R") ++running;
        else if (s == "Z") ++zombie;
        else if (s == "T" || s == "t") ++stopped;
        else ++sleeping;
    }

    json jr = json::array();
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        json e;
        e["pid"] = r.st.pid;
        e["ppid"] = r.st.ppid;
        e["uid"] = r.ss.uid;
        e["user"] = r.user;
        e["comm"] = r.st.comm;
        e["state"] = r.st.state;
        e["state_text"] = stateText(r.st.state);
        e["cpu_pct"] = (double)((long long)(r.cpu * 10)) / 10.0;
        e["rss_kb"] = r.ss.rss_kb;
        e["threads"] = r.st.threads;
        e["cmd"] = r.cmd;
        e["kernel"] = r.kernel;
        e["start_ms"] = boot_ms + (uint64_t)(r.st.starttime * 1000 / hz);
        jr.push_back(std::move(e));
    }

    long long mem_total = 0, mem_avail = 0;
    {
        std::string mi;
        if (readFile("/proc/meminfo", mi, 64 * 1024)) {
            std::istringstream is(mi);
            std::string line;
            while (std::getline(is, line)) {
                if (line.compare(0, 9, "MemTotal:") == 0) sscanf(line.c_str() + 9, "%lld", &mem_total);
                else if (line.compare(0, 13, "MemAvailable:") == 0) sscanf(line.c_str() + 13, "%lld", &mem_avail);
            }
        }
    }

    json summary;
    summary["total"] = (int)rows.size();
    summary["running"] = running;
    summary["sleeping"] = sleeping;
    summary["zombie"] = zombie;
    summary["stopped"] = stopped;
    summary["kernel"] = kernel;
    summary["threads"] = threads;
    summary["load1"] = loadOne("/proc/loadavg", 0);
    summary["load5"] = loadOne("/proc/loadavg", 1);
    summary["load15"] = loadOne("/proc/loadavg", 2);
    summary["mem_total_kb"] = mem_total;
    summary["mem_used_kb"] = mem_total > mem_avail ? mem_total - mem_avail : 0;
    summary["uptime_sec"] = (long long)uptime;

    out.rows = std::move(jr);
    out.summary = std::move(summary);
    out.ts = wallMs();
    return true;
}

nlohmann::json render(const Snapshot& snap, const Options& opt, uint64_t age_ms) {
    using nlohmann::json;
    const json& rows = snap.rows;

    std::string q = lower(opt.q);
    std::vector<size_t> hits;
    hits.reserve(rows.size());
    for (size_t i = 0; i < rows.size(); ++i) {
        const json& r = rows[i];
        if (!opt.kernel && r.value("kernel", false)) continue;
        if (!q.empty()) {
            std::string hay = std::to_string(r.value("pid", 0)) + " " + r.value("comm", std::string()) + " " +
                              r.value("cmd", std::string()) + " " + r.value("user", std::string());
            if (lower(hay).find(q) == std::string::npos) continue;
        }
        hits.push_back(i);
    }

    std::string sort = opt.sort;
    if (sort != "mem" && sort != "pid" && sort != "start") sort = "cpu";
    if (sort == "mem") {
        std::stable_sort(hits.begin(), hits.end(), [&](size_t a, size_t b) {
            return rows[a].value("rss_kb", 0LL) > rows[b].value("rss_kb", 0LL);
        });
    } else if (sort == "pid") {
        std::stable_sort(hits.begin(), hits.end(), [&](size_t a, size_t b) {
            return rows[a].value("pid", 0) < rows[b].value("pid", 0);
        });
    } else if (sort == "start") {
        std::stable_sort(hits.begin(), hits.end(), [&](size_t a, size_t b) {
            return rows[a].value("start_ms", 0ULL) > rows[b].value("start_ms", 0ULL);
        });
    } else {
        std::stable_sort(hits.begin(), hits.end(), [&](size_t a, size_t b) {
            return rows[a].value("cpu_pct", 0.0) > rows[b].value("cpu_pct", 0.0);
        });
    }

    int page_size = opt.page_size;
    if (page_size < 1) page_size = 10;
    if (page_size > 200) page_size = 200;
    int matched = (int)hits.size();
    int pages = matched > 0 ? (matched + page_size - 1) / page_size : 1;
    int page = opt.page;
    if (page < 1) page = 1;
    if (page > pages) page = pages;
    size_t begin = (size_t)(page - 1) * (size_t)page_size;
    size_t end = begin + (size_t)page_size;
    if (begin > hits.size()) begin = hits.size();
    if (end > hits.size()) end = hits.size();

    json entries = json::array();
    size_t used = 0;
    bool truncated = false;
    for (size_t k = begin; k < end; ++k) {
        const json& r = rows[hits[k]];
        std::string s = r.dump();
        if (used + s.size() > kBudget) {
            truncated = true;
            break;
        }
        used += s.size();
        entries.push_back(r);
    }

    json summary = snap.summary;
    summary["matched"] = matched;

    json out;
    out["ts"] = snap.ts;
    out["sample_ms"] = kSampleMs;
    out["sort"] = sort;
    out["summary"] = std::move(summary);
    out["entries"] = std::move(entries);
    out["truncated"] = truncated;
    out["page"] = page;
    out["page_size"] = page_size;
    out["pages"] = pages;
    out["matched"] = matched;
    out["cached"] = age_ms > 0;
    out["cache_age_ms"] = age_ms;
    return out;
}

nlohmann::json collect(const Options& opt, std::string& err) {
    Snapshot snap;
    if (!sample(snap, err)) return nlohmann::json::object();
    return render(snap, opt, 0);
}

nlohmann::json detail(int pid, std::string& err) {
    using nlohmann::json;
    err.clear();
    if (pid <= 0) {
        err = "invalid pid";
        return json::object();
    }
    Stat st;
    if (!readStat(pid, st)) {
        err = "process not found";
        return json::object();
    }
    Status ss;
    readStatus(pid, ss);
    std::map<int, std::string> users;
    loadUsers(users);

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    double uptime = 0;
    {
        std::string t;
        if (readFile("/proc/uptime", t, 256)) uptime = atof(t.c_str());
    }
    uint64_t now = wallMs();
    uint64_t up_ms = (uint64_t)(uptime * 1000);
    uint64_t boot_ms = now > up_ms ? now - up_ms : 0;

    char exe_path[64], cwd_path[64];
    snprintf(exe_path, sizeof exe_path, "/proc/%d/exe", pid);
    snprintf(cwd_path, sizeof cwd_path, "/proc/%d/cwd", pid);

    json j;
    j["pid"] = st.pid;
    j["ppid"] = st.ppid;
    j["uid"] = ss.uid;
    auto u = users.find(ss.uid);
    j["user"] = u != users.end() ? u->second : (ss.uid >= 0 ? std::to_string(ss.uid) : std::string());
    j["comm"] = st.comm;
    j["state"] = st.state;
    j["state_text"] = stateText(st.state);
    j["threads"] = st.threads;
    j["cmd"] = readCmdline(pid);
    j["exe"] = readLink(exe_path);
    j["cwd"] = readLink(cwd_path);
    j["rss_kb"] = ss.rss_kb;
    j["vsz_kb"] = ss.vsz_kb;
    j["vmpeak_kb"] = ss.vmpeak_kb;
    j["vmswap_kb"] = ss.vmswap_kb;
    j["cpu_sec"] = (double)(st.utime + st.stime) / (double)hz;
    j["start_ms"] = boot_ms + (uint64_t)(st.starttime * 1000 / hz);
    j["ts"] = now;
    return j;
}

bool sendSignal(int pid, int sig, std::string& err) {
    err.clear();
    if (pid <= 1) {
        err = "refuse to signal pid " + std::to_string(pid);
        return false;
    }
    if (pid == (int)getpid()) {
        err = "refuse to signal agent itself";
        return false;
    }
    if (sig < 1 || sig > 64) {
        err = "invalid signal";
        return false;
    }
    if (kill(pid, sig) != 0) {
        err = strerror(errno);
        return false;
    }
    return true;
}

} // namespace rtproc
