#include "deploy_scan.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

bool privateV4(uint32_t ip) {
    // 10/8, 172.16/12, 192.168/16, 127/8, 169.254/16, 100.64/10
    if ((ip & 0xFF000000u) == 0x0A000000u) return true;
    if ((ip & 0xFFF00000u) == 0xAC100000u) return true;
    if ((ip & 0xFFFF0000u) == 0xC0A80000u) return true;
    if ((ip & 0xFF000000u) == 0x7F000000u) return true;
    if ((ip & 0xFFFF0000u) == 0xA9FE0000u) return true;
    if ((ip & 0xFFC00000u) == 0x64400000u) return true;
    return false;
}

std::string ipStr(uint32_t ip) {
    in_addr a{};
    a.s_addr = htonl(ip);
    char buf[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &a, buf, sizeof(buf));
    return std::string(buf);
}

bool hasNmap() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    const char* paths[] = {"/usr/bin/nmap", "/usr/local/bin/nmap", "/bin/nmap", "/opt/bin/nmap"};
    cached = 0;
    for (const char* p : paths) {
        if (access(p, X_OK) == 0) {
            cached = 1;
            break;
        }
    }
    return cached == 1;
}

// 跑一条命令并抓 stdout，带墙钟上限；超时杀进程组
std::string runCapture(const std::string& cmd, int timeout_s, bool& timed_out) {
    timed_out = false;
    int p[2];
    if (pipe(p) != 0) return std::string();
    pid_t pid = fork();
    if (pid < 0) {
        close(p[0]);
        close(p[1]);
        return std::string();
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(p[1], 1);
        dup2(p[1], 2);
        close(p[0]);
        close(p[1]);
        execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(p[1]);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    std::string out;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    int status = 0;
    for (;;) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) break;
        if (std::chrono::steady_clock::now() > deadline) {
            timed_out = true;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        struct pollfd pfd{};
        pfd.fd = p[0];
        pfd.events = POLLIN;
        if (poll(&pfd, 1, 100) > 0) {
            char buf[4096];
            for (;;) {
                ssize_t n = read(p[0], buf, sizeof(buf));
                if (n <= 0) break;
                if (out.size() < 512 * 1024) out.append(buf, (size_t)n);
            }
        }
    }
    char buf[4096];
    for (;;) {
        ssize_t n = read(p[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    close(p[0]);
    return out;
}

// nmap -oG 的可解析行：Host: 10.0.0.5 (name)\tPorts: 22/open/tcp//ssh///
void parseNmap(const std::string& out, int port, std::vector<DeployScanHost>& items) {
    size_t pos = 0;
    std::string porttag = std::to_string(port) + "/open/";
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        std::string line = out.substr(pos, (nl == std::string::npos ? out.size() : nl) - pos);
        pos = (nl == std::string::npos) ? out.size() : nl + 1;
        size_t hp = line.find("Host: ");
        size_t pp = line.find("Ports: ");
        if (hp == std::string::npos || pp == std::string::npos) continue;
        hp += 6;
        size_t sp = line.find(' ', hp);
        std::string ip = line.substr(hp, (sp == std::string::npos ? line.size() : sp) - hp);
        std::string ports = line.substr(pp + 7);
        if (ports.find(porttag) == std::string::npos) continue;
        DeployScanHost h;
        h.host = ip;
        h.port = port;
        // 服务名在第 5 段：22/open/tcp//ssh///
        size_t a = ports.find(porttag);
        std::string rest = ports.substr(a);
        std::vector<std::string> seg;
        size_t s = 0;
        while (s <= rest.size() && seg.size() < 6) {
            size_t slash = rest.find('/', s);
            seg.push_back(rest.substr(s, (slash == std::string::npos ? rest.size() : slash) - s));
            if (slash == std::string::npos) break;
            s = slash + 1;
        }
        h.service = (seg.size() > 4 && !seg[4].empty()) ? seg[4] : "tcp";
        items.push_back(h);
    }
}

DeployScanResult scanWithNmap(const std::string& cidr, int port, int scanned) {
    DeployScanResult r;
    r.engine = "nmap";
    r.scanned = scanned;
    std::string cmd = "nmap -Pn -n -p" + std::to_string(port) +
                      " --open -T4 --max-retries 1 --host-timeout 4s -oG - " + cidr + " 2>/dev/null";
    bool timed_out = false;
    auto t0 = std::chrono::steady_clock::now();
    std::string out = runCapture(cmd, 90, timed_out);
    r.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    if (timed_out) {
        r.error = "nmap 扫描超时（网段太大或目标不响应），缩小网段再试";
        return r;
    }
    parseNmap(out, port, r.items);
    if (out.find("Host: ") == std::string::npos && out.empty()) {
        r.error = "nmap 没有输出（未安装或参数不被支持）";
        return r;
    }
    r.ok = true;
    return r;
}

int readBanner(int fd) {
    char buf[256];
    fcntl(fd, F_SETFL, O_NONBLOCK);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    std::string all;
    while (std::chrono::steady_clock::now() < deadline && all.size() < 200) {
        struct pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 50);
        if (pr <= 0) continue;
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        all.append(buf, (size_t)n);
        if (all.find('\n') != std::string::npos) break;
    }
    return all.compare(0, 4, "SSH-") == 0 ? 1 : 0;
}

// 没有 nmap 时的兜底：非阻塞 connect + 并发
DeployScanResult scanWithTcp(const std::vector<std::string>& ips, int port, int timeout_ms) {
    DeployScanResult r;
    r.engine = "tcp";
    r.scanned = (int)ips.size();
    auto t0 = std::chrono::steady_clock::now();
    const size_t kConc = 128;
    struct Pending {
        int fd = -1;
        uint32_t ip = 0;
        std::chrono::steady_clock::time_point deadline;
    };
    std::vector<Pending> inflight;
    size_t next = 0;
    auto timeout = std::chrono::milliseconds(std::max(200, std::min(timeout_ms, 3000)));
    while (next < ips.size() || !inflight.empty()) {
        while (inflight.size() < kConc && next < ips.size()) {
            uint32_t ip = ntohl(inet_addr(ips[next].c_str()));
            ++next;
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            int fl = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port = htons((uint16_t)port);
            sa.sin_addr.s_addr = htonl(ip);
            int rc = connect(fd, (sockaddr*)&sa, sizeof(sa));
            if (rc != 0 && errno != EINPROGRESS) {
                close(fd);
                continue;
            }
            Pending pd;
            pd.fd = fd;
            pd.ip = ip;
            pd.deadline = std::chrono::steady_clock::now() + timeout;
            inflight.push_back(pd);
        }
        if (inflight.empty()) break;
        std::vector<struct pollfd> pfds;
        pfds.reserve(inflight.size());
        for (auto& p : inflight) {
            struct pollfd pfd{};
            pfd.fd = p.fd;
            pfd.events = POLLOUT;
            pfds.push_back(pfd);
        }
        poll(pfds.data(), pfds.size(), 50);
        auto now = std::chrono::steady_clock::now();
        for (size_t i = 0; i < inflight.size();) {
            bool done = false, ok = false;
            if (pfds[i].revents) {
                int so = 0;
                socklen_t sl = sizeof(so);
                getsockopt(inflight[i].fd, SOL_SOCKET, SO_ERROR, &so, &sl);
                done = true;
                ok = (so == 0);
            } else if (now > inflight[i].deadline) {
                done = true;
            }
            if (!done) {
                ++i;
                continue;
            }
            if (ok) {
                DeployScanHost h;
                h.host = ipStr(inflight[i].ip);
                h.port = port;
                h.service = readBanner(inflight[i].fd) ? "ssh" : "tcp";
                r.items.push_back(h);
            }
            close(inflight[i].fd);
            inflight.erase(inflight.begin() + i);
            pfds.erase(pfds.begin() + i);
        }
    }
    r.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    std::sort(r.items.begin(), r.items.end(),
              [](const DeployScanHost& a, const DeployScanHost& b) {
                  return ntohl(inet_addr(a.host.c_str())) < ntohl(inet_addr(b.host.c_str()));
              });
    r.ok = true;
    return r;
}

}  // namespace

bool deployParseCidr(const std::string& cidr, std::vector<std::string>& ips, std::string& err,
                     int max_hosts) {
    std::string s = cidr;
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (s.empty()) {
        err = "网段不能为空";
        return false;
    }
    std::string ip_part = s, mask_part;
    size_t slash = s.find('/');
    if (slash != std::string::npos) {
        ip_part = s.substr(0, slash);
        mask_part = s.substr(slash + 1);
    }
    struct in_addr a{};
    if (inet_pton(AF_INET, ip_part.c_str(), &a) != 1) {
        err = "只支持 IPv4 地址，例如 192.168.1.0/24";
        return false;
    }
    int len = 32;
    if (!mask_part.empty()) {
        if (mask_part.find_first_not_of("0123456789") != std::string::npos) {
            err = "掩码只能是 0-32 的数字";
            return false;
        }
        len = atoi(mask_part.c_str());
        if (len < 16 || len > 32) {
            err = "掩码限定 /16 ~ /32";
            return false;
        }
    }
    uint32_t ip = ntohl(a.s_addr);
    uint32_t mask = (len == 0) ? 0u : (0xFFFFFFFFu << (32 - len));
    uint32_t net = ip & mask;
    uint64_t count = (len == 32) ? 1 : (1ull << (32 - len));
    if (count > (uint64_t)max_hosts) {
        err = "网段太大（" + std::to_string(count) + " 个地址，上限 " +
              std::to_string(max_hosts) + "），请用更小的前缀";
        return false;
    }
    if (!privateV4(net)) {
        err = "只允许扫描私有网段（10/8、172.16/12、192.168/16、127/8）";
        return false;
    }
    ips.clear();
    ips.reserve((size_t)count);
    for (uint64_t i = 0; i < count; ++i) ips.push_back(ipStr(net + (uint32_t)i));
    return true;
}

DeployScanResult deployScanSubnet(const std::string& cidr, int port, int timeout_ms,
                                  const std::string& engine) {
    DeployScanResult r;
    if (port < 1 || port > 65535) port = 22;
    std::vector<std::string> ips;
    std::string err;
    if (!deployParseCidr(cidr, ips, err, 1024)) {
        r.error = err;
        return r;
    }
    // 只用校验后的网络地址拼 nmap 参数，原始输入不直接进 shell
    int bits = 32;
    while (bits > 0 && (1ull << (32 - bits)) < (uint64_t)ips.size()) --bits;
    std::string target = ips.front() + "/" + std::to_string(bits);

    bool want_nmap = (engine == "auto" || engine == "nmap");
    if (want_nmap && hasNmap()) {
        auto nr = scanWithNmap(target, port, (int)ips.size());
        if (nr.ok) return nr;
        // 强制 nmap 时把失败原因说清楚，别悄悄换成内置扫描
        if (engine == "nmap") return nr;
        // auto：nmap 跑失败（输出为空/超时）就退回内置扫描
    } else if (engine == "nmap") {
        r.error = "这台 relay 上没有 nmap（/usr/bin/nmap），改用内置 TCP 扫描或装一个 nmap";
        return r;
    }
    return scanWithTcp(ips, port, timeout_ms);
}
