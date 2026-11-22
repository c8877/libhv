#include "svc_service.hpp"
#include "cpputil/hstring.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace rtvs {

namespace {

struct CmdResult {
    int code = -1;
    bool timeout = false;
    std::string out;
};

long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

// 后台线程里同步执行命令；stdout/stderr 合并读取，超时 kill 整个进程组。
CmdResult runCmd(const std::vector<std::string>& args, int timeout_ms, size_t max_out) {
    CmdResult r;
    int pfd[2];
    if (pipe(pfd) != 0) return r;
    pid_t pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return r;
    }
    if (pid == 0) {
        setsid();
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) {
            dup2(nul, STDIN_FILENO);
            close(nul);
        }
        setenv("LC_ALL", "C", 1);
        setenv("LANG", "C", 1);
        setenv("SYSTEMD_PAGER", "cat", 1);
        setenv("SYSTEMD_COLORS", "0", 1);
        std::vector<char*> av;
        av.reserve(args.size() + 1);
        for (const auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        execvp(av[0], av.data());
        _exit(127);
    }

    close(pfd[1]);
    long long deadline = nowMs() + timeout_ms;
    char buf[8192];
    bool eof = false;
    while (!eof) {
        long long remain = deadline - nowMs();
        if (remain <= 0) {
            r.timeout = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
        struct pollfd p = {pfd[0], POLLIN, 0};
        int pr = poll(&p, 1, (int)std::min<long long>(remain, 200));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        ssize_t n = read(pfd[0], buf, sizeof(buf));
        if (n > 0) {
            if (r.out.size() < max_out) r.out.append(buf, std::min((size_t)n, max_out - r.out.size()));
        }
        else if (n == 0) {
            eof = true;
        }
        else if (errno != EINTR) {
            break;
        }
    }
    close(pfd[0]);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(st)) r.code = WEXITSTATUS(st);
    else if (WIFSIGNALED(st)) r.code = 128 + WTERMSIG(st);
    return r;
}

std::string firstLine(const std::string& s) {
    size_t i = s.find('\n');
    std::string line = i == std::string::npos ? s : s.substr(0, i);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    return line;
}

double readUptimeSec() {
    FILE* fp = fopen("/proc/uptime", "r");
    if (!fp) return 0;
    double up = 0;
    if (fscanf(fp, "%lf", &up) != 1) up = 0;
    fclose(fp);
    return up;
}

bool parseLongLong(const std::string& v, long long& out) {
    if (v.empty() || v[0] == '[') return false;
    errno = 0;
    char* end = nullptr;
    long long n = strtoll(v.c_str(), &end, 10);
    if (errno != 0 || end == v.c_str()) return false;
    out = n;
    return true;
}

int unitRank(const std::string& active) {
    if (active == "failed") return 0;
    if (active == "activating" || active == "deactivating" || active == "reloading") return 1;
    if (active == "active") return 2;
    if (active == "inactive") return 3;
    return 4;
}

int fileRank(const std::string& st) {
    if (st == "enabled") return 0;
    if (st == "enabled-runtime") return 1;
    if (st == "static") return 2;
    if (st == "indirect") return 3;
    if (st == "disabled") return 4;
    if (st == "masked-runtime") return 5;
    if (st == "masked") return 6;
    return 9;
}

std::string lowerStr(const std::string& s) {
    std::string o = s;
    hv::tolower(o);
    return o;
}

int cmpText(const std::string& a, const std::string& b) {
    std::string x = lowerStr(a), y = lowerStr(b);
    return x < y ? -1 : x > y ? 1 : 0;
}

// 空值/负值始终排在最后，其余按 dir 排序
int cmpMissing(long long a, long long b, int dir) {
    if (a < 0 && b < 0) return 0;
    if (a < 0) return 1;
    if (b < 0) return -1;
    long long d = a - b;
    return d < 0 ? -dir : d > 0 ? dir : 0;
}

nlohmann::json applyListPage(const nlohmann::json& full, const ListOptions& opt) {
    using nlohmann::json;
    json out = full;
    json base = full.contains("services") && full["services"].is_array() ? full["services"] : json::array();
    int stopped = 0;
    for (auto& s : base) {
        if (s.value("active", "") == "inactive") ++stopped;
    }
    out["total"] = full.value("total", (int)base.size());
    out["running"] = full.value("running", 0);
    out["failed"] = full.value("failed", 0);
    out["stopped"] = stopped;

    std::string tab = opt.tab.empty() ? "all" : opt.tab;
    std::string q = lowerStr(opt.q);
    std::vector<json> rows;
    for (auto& s : base) {
        std::string active = s.value("active", "");
        if (tab == "running" && active != "active") continue;
        if (tab == "failed" && active != "failed") continue;
        if (tab == "stopped" && active != "inactive") continue;
        if (!q.empty()) {
            std::string hay = lowerStr(s.value("name", "") + " " + s.value("description", ""));
            if (hay.find(q) == std::string::npos) continue;
        }
        rows.push_back(s);
    }

    std::string key = opt.sort;
    int dir = opt.dir < 0 ? -1 : 1;
    std::stable_sort(rows.begin(), rows.end(), [&](const json& a, const json& b) {
        int c = 0;
        if (key == "mem") {
            c = cmpMissing(a.value("mem", -1LL), b.value("mem", -1LL), dir);
        } else if (key == "restarts") {
            c = cmpMissing(a.value("restarts", 0LL), b.value("restarts", 0LL), dir);
        } else if (key == "uptime") {
            long long ua = a.value("active", "") == "active" ? a.value("active_ms", -1LL) : -1;
            long long ub = b.value("active", "") == "active" ? b.value("active_ms", -1LL) : -1;
            c = cmpMissing(ua, ub, dir);
        } else if (key == "name") {
            c = cmpText(a.value("name", ""), b.value("name", "")) * dir;
        } else if (key == "start") {
            c = (fileRank(a.value("file_state", "")) - fileRank(b.value("file_state", ""))) * dir;
        } else if (key == "status") {
            c = (unitRank(a.value("active", "")) - unitRank(b.value("active", ""))) * dir;
        } else {
            c = unitRank(a.value("active", "")) - unitRank(b.value("active", ""));
        }
        if (c == 0) c = cmpText(a.value("name", ""), b.value("name", ""));
        return c < 0;
    });

    int matched = (int)rows.size();
    int page_size = opt.page_size > 0 ? opt.page_size : 10;
    if (page_size > 200) page_size = 200;
    int pages = matched > 0 ? (matched + page_size - 1) / page_size : 1;
    int page = opt.page > 0 ? opt.page : 1;
    if (page > pages) page = pages;

    json pagearr = json::array();
    size_t begin = (size_t)(page - 1) * (size_t)page_size;
    size_t end = begin + (size_t)page_size;
    if (begin > rows.size()) begin = rows.size();
    if (end > rows.size()) end = rows.size();
    for (size_t i = begin; i < end; ++i) pagearr.push_back(rows[i]);

    out["services"] = pagearr;
    out["matched"] = matched;
    out["page"] = page;
    out["page_size"] = page_size;
    out["pages"] = pages;
    out["sort"] = key;
    out["dir"] = dir;
    out["tab"] = tab;
    return out;
}

void parseShow(const std::string& text, double uptime_sec, std::vector<nlohmann::json>& out,
               UnitStat& stat) {
    std::map<std::string, std::string> kv;
    auto flush = [&]() {
        if (kv.empty()) return;
        std::string id = kv["Id"];
        if (id.empty() || kv["LoadState"] == "not-found") {
            kv.clear();
            return;
        }
        nlohmann::json s;
        s["name"] = id;
        s["description"] = kv["Description"];
        s["load"] = kv["LoadState"];
        s["active"] = kv["ActiveState"];
        s["sub"] = kv["SubState"];
        s["file_state"] = kv["UnitFileState"];
        s["result"] = kv["Result"];
        long long n = 0;
        s["pid"] = parseLongLong(kv["MainPID"], n) ? n : 0;
        s["mem"] = parseLongLong(kv["MemoryCurrent"], n) ? n : -1;
        s["exit"] = parseLongLong(kv["ExecMainStatus"], n) ? n : 0;
        s["restarts"] = parseLongLong(kv["NRestarts"], n) ? n : 0;
        long long mono_us = 0;
        long long active_ms = -1;
        if (parseLongLong(kv["ActiveEnterTimestampMonotonic"], mono_us) && mono_us > 0 &&
            kv["ActiveState"] == "active") {
            long long up_ms = (long long)(uptime_sec * 1000);
            active_ms = up_ms - mono_us / 1000;
            if (active_ms < 0) active_ms = 0;
        }
        s["active_ms"] = active_ms;
        out.push_back(std::move(s));

        stat.total++;
        if (kv["ActiveState"] == "active") stat.running++;
        else if (kv["ActiveState"] == "failed") stat.failed++;
        kv.clear();
    };

    std::istringstream is(text);
    std::string line;
    while (std::getline(is, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) {
            flush();
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    flush();
}

std::string errText(const CmdResult& r) {
    if (r.timeout) return "systemctl 超时";
    std::string line = firstLine(r.out);
    if (!line.empty()) return line;
    if (r.code == 127) return "systemctl 不可用";
    return "systemctl 退出码 " + std::to_string(r.code);
}

} // namespace

bool validUnitName(const std::string& unit) {
    if (unit.size() < 9 || unit.size() > 255) return false;
    if (unit.compare(unit.size() - 8, 8, ".service") != 0) return false;
    if (unit[0] == '-' || unit[0] == '.') return false;
    for (char c : unit) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) continue;
        if (c == '_' || c == '.' || c == '-' || c == ':' || c == '@' || c == '\\') continue;
        return false;
    }
    return true;
}

bool validVerb(const std::string& verb) {
    static const char* kVerbs[] = {"start", "stop", "restart", "enable", "disable", "mask", "unmask"};
    for (const char* v : kVerbs) {
        if (verb == v) return true;
    }
    return false;
}

bool Service::systemdAvailable() {
    struct stat st;
    return stat("/run/systemd/system", &st) == 0 && S_ISDIR(st.st_mode);
}

Service::Service() {
    systemd_ = systemdAvailable();
    enabled_ = systemd_;
}

void Service::start() {
    std::lock_guard<std::mutex> lk(th_mu_);
    if (!systemd_ || !enabled_.load(std::memory_order_relaxed) || th_.joinable()) return;
    th_ = std::thread([this]() { workerLoop(); });
}

Service::~Service() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        stop_ = true;
    }
    cv_.notify_all();
    std::lock_guard<std::mutex> lk(th_mu_);
    if (th_.joinable()) th_.join();
}

void Service::post(std::function<void()> fn) {
    start();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (stop_) return;
        jobs_.push_back(std::move(fn));
    }
    cv_.notify_one();
}

void Service::workerLoop() {
    for (;;) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this]() { return stop_ || !jobs_.empty(); });
            if (stop_ && jobs_.empty()) return;
            fn = std::move(jobs_.front());
            jobs_.pop_front();
        }
        fn();
    }
}

// 后台刷新 unit-file 清单。只持有 shared_ptr，不引用 Service。
void Service::unitFilesRefresh(std::shared_ptr<UnitFiles> uf, uint64_t gen) {
    CmdResult f = runCmd({"systemctl", "list-unit-files", "--type=service", "--no-legend",
                          "--no-pager"},
                         15000, 1024 * 1024);
    std::map<std::string, std::string> m;
    if (f.code == 0 && !f.timeout) {
        std::istringstream is(f.out);
        std::string line;
        while (std::getline(is, line)) {
            std::istringstream ls(line);
            std::string name, state;
            ls >> name >> state;
            if (name.size() < 9 || name.compare(name.size() - 8, 8, ".service") != 0) continue;
            if (name.find('@') != std::string::npos) continue;   // 模板单元
            if (state == "static" || state == "alias" || state == "generated" ||
                state == "transient" || state == "bad")
                continue;
            m[name] = state;
        }
    }
    std::lock_guard<std::mutex> lk(uf->mu);
    // 期间发生过 enable/disable/mask，结果可能已过期，丢弃等下一次刷新
    if (!m.empty() && uf->gen == gen) {
        uf->files = std::move(m);
        uf->ms = nowMs();
        uf->ver++;
    }
    uf->refreshing = false;
}

void Service::invalidateUnitFiles() {
    std::lock_guard<std::mutex> lk(unit_files_->mu);
    unit_files_->gen++;
    unit_files_->ms = 0;
}

void Service::listAsync(const ListOptions& opt, Callback cb) {
    post([this, opt, cb]() {
        nlohmann::json r;
        if (!enabled_) {
            r["ok"] = false;
            r["error"] = "服务管理已关闭";
            cb(r);
            return;
        }
        if (!systemdAvailable()) {
            r["ok"] = false;
            r["error"] = "此设备未使用 systemd";
            cb(r);
            return;
        }
        long long now = nowMs();
        uint64_t uf_ver = 0;
        {
            std::lock_guard<std::mutex> lk(unit_files_->mu);
            uf_ver = unit_files_->ver;
        }
        // 详情页预热每 30s 刷新一次，缓存留 40s 保证期间的请求都能直接命中；
        // unit-file 清单在后台更新后版本会变，缓存随之失效，下一次请求带上新单元
        if (!list_cache_.is_null() && now - list_cache_ms_ < 40000 &&
            list_files_ver_ == uf_ver) {
            cb(applyListPage(list_cache_, opt));
            return;
        }
        double up = readUptimeSec();
        CmdResult c = runCmd({"systemctl", "show", "*.service", "-p",
                              "Id,Description,LoadState,ActiveState,SubState,UnitFileState,MainPID,"
                              "MemoryCurrent,Result,ExecMainStatus,NRestarts,ActiveEnterTimestampMonotonic"},
                             15000, 4 * 1024 * 1024);
        if (c.code != 0 || c.timeout) {
            r["ok"] = false;
            r["error"] = errText(c);
            cb(r);
            return;
        }
        std::vector<nlohmann::json> units;
        UnitStat stat;
        parseShow(c.out, up, units, stat);
        std::map<std::string, size_t> index;
        for (size_t i = 0; i < units.size(); ++i) index[units[i].value("name", "")] = i;

        // 停止后的 disabled/masked 单元会被 systemd 卸载，show '*.service' 不再列出，
        // 补一份 unit-file 清单，保证还能从界面启用/禁用/屏蔽。
        // 该命令在单核设备上约 2.8s，放后台刷新：本次先用已有清单，
        // 完成后下一次请求自然带上新装的单元或新的启禁用状态。
        std::map<std::string, std::string> files;
        bool kick_files = false;
        uint64_t files_gen = 0;
        {
            std::lock_guard<std::mutex> lk(unit_files_->mu);
            files = unit_files_->files;
            uf_ver = unit_files_->ver;
            if (!unit_files_->refreshing &&
                (unit_files_->ms == 0 || now - unit_files_->ms > 300000)) {
                unit_files_->refreshing = true;
                files_gen = unit_files_->gen;
                kick_files = true;
            }
        }
        if (kick_files) {
            std::thread(Service::unitFilesRefresh, unit_files_, files_gen).detach();
        }
        for (const auto& kv : files) {
            auto it = index.find(kv.first);
            if (it != index.end()) {
                units[it->second]["file_state"] = kv.second;   // unit-file 状态更完整
                continue;
            }
            nlohmann::json s;
            s["name"] = kv.first;
            s["description"] = "";
            s["load"] = "unloaded";
            s["active"] = "inactive";
            s["sub"] = "dead";
            s["file_state"] = kv.second;
            s["result"] = "success";
            s["pid"] = 0;
            s["mem"] = -1;
            s["exit"] = 0;
            s["active_ms"] = -1;
            units.push_back(std::move(s));
            stat.total++;
        }
        std::sort(units.begin(), units.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            int ra = unitRank(a.value("active", ""));
            int rb = unitRank(b.value("active", ""));
            if (ra != rb) return ra < rb;
            return a.value("name", "") < b.value("name", "");
        });
        r["ok"] = true;
        r["services"] = units;
        r["total"] = stat.total;
        r["running"] = stat.running;
        r["failed"] = stat.failed;
        list_cache_ = r;
        list_cache_ms_ = now;
        list_files_ver_ = uf_ver;
        cb(applyListPage(r, opt));
    });
}

void Service::logsAsync(const std::string& unit, int lines, Callback cb) {
    post([this, unit, lines, cb]() {
        nlohmann::json r;
        r["unit"] = unit;
        if (!enabled_) {
            r["ok"] = false;
            r["error"] = "服务管理已关闭";
            cb(r);
            return;
        }
        if (!validUnitName(unit)) {
            r["ok"] = false;
            r["error"] = "服务名不合法";
            cb(r);
            return;
        }
        int n = lines < 20 ? 20 : (lines > 500 ? 500 : lines);
        CmdResult c = runCmd({"journalctl", "-u", unit, "-n", std::to_string(n), "--no-pager",
                              "--no-hostname", "-o", "short-iso"},
                             15000, 256 * 1024);
        if (c.code != 0 || c.timeout) {
            r["ok"] = false;
            r["error"] = errText(c);
            cb(r);
            return;
        }
        r["ok"] = true;
        if (c.out.find("No journal files were found.") != std::string::npos || c.out.find("-- No entries --") != std::string::npos) {
            r["text"] = "";
            r["note"] = "该设备未保留 journal 日志";
        }
        else {
            r["text"] = c.out;
        }
        cb(r);
    });
}

void Service::actionAsync(const std::string& unit, const std::string& verb, Callback cb) {
    post([this, unit, verb, cb]() {
        nlohmann::json r;
        r["unit"] = unit;
        r["verb"] = verb;
        if (!enabled_) {
            r["ok"] = false;
            r["error"] = "服务管理已关闭";
            cb(r);
            return;
        }
        if (!validUnitName(unit) || !validVerb(verb)) {
            r["ok"] = false;
            r["error"] = "参数不合法";
            cb(r);
            return;
        }
        if (!writable_) {
            r["ok"] = false;
            r["error"] = "服务控制为只读，需在 agent 启用 --svc-rw";
            cb(r);
            return;
        }
        std::vector<std::string> cmd = {"systemctl", "--no-pager", "--no-ask-password"};
        // 启停/重启只排队作业，等 systemd 完成会让请求挂住十几秒；界面靠刷新看结果
        if (verb == "start" || verb == "stop" || verb == "restart") cmd.push_back("--no-block");
        cmd.push_back(verb);
        cmd.push_back(unit);
        CmdResult c = runCmd(cmd, 15000, 16 * 1024);
        if (c.code != 0 || c.timeout) {
            r["ok"] = false;
            r["error"] = errText(c);
        }
        else {
            r["ok"] = true;
            list_cache_ms_ = 0;
            if (verb == "enable" || verb == "disable" || verb == "mask" || verb == "unmask") invalidateUnitFiles();
        }
        cb(r);
    });
}

} // namespace rtvs
