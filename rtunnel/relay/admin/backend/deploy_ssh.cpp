#include "deploy_ssh.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <vector>

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

namespace {

// libssh2 全局初始化只能做一次
void ensureInit() {
    static std::once_flag once;
    std::call_once(once, [] { libssh2_init(0); });
}

std::string sha256B64(const char* data, size_t len) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)data, len, md);
    // base64（不带换行）
    std::string b64;
    static const char* tb = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    b64.reserve(48);
    for (size_t i = 0; i < sizeof(md); i += 3) {
        unsigned v = md[i] << 16;
        if (i + 1 < sizeof(md)) v |= md[i + 1] << 8;
        if (i + 2 < sizeof(md)) v |= md[i + 2];
        b64 += tb[(v >> 18) & 63];
        b64 += tb[(v >> 12) & 63];
        b64 += (i + 1 < sizeof(md)) ? tb[(v >> 6) & 63] : '=';
        b64 += (i + 2 < sizeof(md)) ? tb[v & 63] : '=';
    }
    return b64;
}

bool writeAll(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = write(fd, p, n);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

}  // namespace

DeploySsh::~DeploySsh() { close(); }

void DeploySsh::close() {
    if (sftp_) {
        libssh2_sftp_shutdown((LIBSSH2_SFTP*)sftp_);
        sftp_ = nullptr;
    }
    if (session_) {
        libssh2_session_disconnect((LIBSSH2_SESSION*)session_, "bye");
        libssh2_session_free((LIBSSH2_SESSION*)session_);
        session_ = nullptr;
    }
    if (sock_ >= 0) {
        ::close(sock_);
        sock_ = -1;
    }
}

bool DeploySsh::waitSocket(long ms) {
    LIBSSH2_SESSION* s = (LIBSSH2_SESSION*)session_;
    if (!s || sock_ < 0) return false;
    struct pollfd p{};
    p.fd = sock_;
    int dir = libssh2_session_block_directions(s);
    if (dir & LIBSSH2_SESSION_BLOCK_INBOUND) p.events |= POLLIN;
    if (dir & LIBSSH2_SESSION_BLOCK_OUTBOUND) p.events |= POLLOUT;
    if (p.events == 0) p.events = POLLIN;
    int rc = poll(&p, 1, (int)ms);
    if (rc > 0) return true;
    return false;
}

// 200ms 没动静是正常现象（Dropbear 之类的服务端要等几百毫秒才发 banner），
// 只按总 deadline 判定超时，避免把“慢一点”当成“连不上”。
bool DeploySsh::waitUntil(Clock::time_point deadline, long slice_ms) {
    auto now = Clock::now();
    if (now >= deadline) return false;
    long left = (long)std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (left <= 0) return false;
    waitSocket(std::min(left, slice_ms));
    return Clock::now() < deadline;
}

bool DeploySsh::connect(const DeploySshTarget& t, std::string& err) {
    ensureInit();
    close();
    last_error_.clear();

    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* res = nullptr;
    std::string port = std::to_string(t.port);
    int gai = getaddrinfo(t.host.c_str(), port.c_str(), &hints, &res);
    if (gai != 0 || !res) {
        err = "解析地址失败: " + std::string(gai_strerror(gai));
        return false;
    }

    // 逐个地址试，10 秒总超时
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string last_err = "连接超时";
    for (struct addrinfo* ai = res; ai; ai = ai->ai_next) {
        int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = ::connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno != EINPROGRESS) {
            last_err = std::string("connect: ") + strerror(errno);
            ::close(fd);
            continue;
        }
        bool connected = (rc == 0);
        while (!connected) {
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now()).count();
            if (left <= 0) break;
            struct pollfd p{};
            p.fd = fd;
            p.events = POLLOUT;
            int pr = poll(&p, 1, (int)std::min<long long>(left, 500));
            if (pr > 0) {
                int so = 0;
                socklen_t sl = sizeof(so);
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so, &sl);
                if (so == 0) {
                    connected = true;
                } else {
                    last_err = std::string("connect: ") + strerror(so);
                    break;
                }
            }
        }
        if (!connected) {
            ::close(fd);
            continue;
        }
        // 打开 TCP_NODELAY 与 keepalive：长安装过程不因为空闲被判死
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        sock_ = fd;
        break;
    }
    freeaddrinfo(res);
    if (sock_ < 0) {
        err = last_err;
        return false;
    }

    LIBSSH2_SESSION* s = libssh2_session_init();
    if (!s) {
        err = "初始化 SSH 会话失败";
        close();
        return false;
    }
    session_ = s;
    libssh2_session_set_blocking(s, 0);
    // 单次等待上限；外层循环还有总超时
    libssh2_session_set_timeout(s, 15000);

    // 握手（非阻塞）：有的服务端（Dropbear）连上后要等几百毫秒才发 banner，
    // 所以这里按总超时判断，单次 200ms 没动静就继续等。
    auto hs_deadline = Clock::now() + std::chrono::seconds(20);
    for (;;) {
        int rc = libssh2_session_handshake(s, sock_);
        if (rc == 0) break;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (!waitUntil(hs_deadline)) {
                err = "SSH 握手超时：目标 20 秒内没完成密钥交换（端口可能不是 SSH，或被中间设备拦了）";
                close();
                return false;
            }
            continue;
        }
        char* msg = nullptr;
        libssh2_session_last_error(s, &msg, nullptr, 0);
        err = std::string("SSH 握手失败: ") + (msg ? msg : "unknown");
        close();
        return false;
    }

    // 主机密钥：首次接受并记录，之后指纹必须一致
    // 握手刚结束时非阻塞模式下主机密钥可能还没就绪，稍等再取
    const char* hk = nullptr;
    size_t hklen = 0;
    int hktype = 0;
    auto hk_deadline = Clock::now() + std::chrono::seconds(5);
    while (!hk) {
        hk = libssh2_session_hostkey(s, &hklen, &hktype);
        if (hk) break;
        int rc2 = libssh2_session_last_errno(s);
        if (rc2 != LIBSSH2_ERROR_EAGAIN) break;
        if (!waitUntil(hk_deadline, 100)) break;
    }
    if (!hk) {
        char* msg = nullptr;
        libssh2_session_last_error(s, &msg, nullptr, 0);
        err = std::string("目标没有提供主机密钥") + (msg ? (": " + std::string(msg)) : "");
        close();
        return false;
    }
    std::string fp = "sha256:" + sha256B64(hk, hklen);
    std::string key = t.host + ":" + std::to_string(t.port);
    std::string known;
    if (!t.known_hosts.empty()) {
        FILE* f = fopen(t.known_hosts.c_str(), "r");
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                std::string l(line);
                while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
                if (l.empty() || l[0] == '#') continue;
                size_t sp = l.find(' ');
                if (sp == std::string::npos) continue;
                if (l.substr(0, sp) != key) continue;
                known = l.substr(sp + 1);
                break;
            }
            fclose(f);
        }
        if (!known.empty() && known != fp) {
            err = "主机密钥变了：" + key + " 现在是 " + fp + "，记录里是 " + known +
                  "（确认换机后删掉 known_hosts 对应行）";
            close();
            return false;
        }
        if (known.empty()) {
            int fd = open(t.known_hosts.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (fd >= 0) {
                std::string line = key + " " + fp + "\n";
                writeAll(fd, line.data(), line.size());
                ::close(fd);
            }
        }
    }

    // 认证（非阻塞）
    std::string u = t.user;
    int rc = LIBSSH2_ERROR_EAGAIN;
    auto auth_deadline = Clock::now() + std::chrono::seconds(20);
    for (;;) {
        if (t.auth == "key") {
            rc = libssh2_userauth_publickey_frommemory(
                s, u.c_str(), u.size(), nullptr, 0, t.secret.c_str(), t.secret.size(), nullptr);
        } else {
            rc = libssh2_userauth_password_ex(s, u.c_str(), u.size(), t.secret.c_str(),
                                             t.secret.size(), nullptr);
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (!waitUntil(auth_deadline)) {
                err = "SSH 认证超时（20 秒）";
                close();
                return false;
            }
            continue;
        }
        break;
    }
    if (rc != 0) {
        char* msg = nullptr;
        libssh2_session_last_error(s, &msg, nullptr, 0);
        err = std::string(t.auth == "key" ? "私钥认证失败: " : "密码认证失败: ") +
              (msg ? msg : "unknown");
        close();
        return false;
    }
    return true;
}

DeploySshResult DeploySsh::exec(const std::string& cmd, const std::string& stdin_data,
                                int timeout_s, const CancelFn& cancel) {
    DeploySshResult r;
    LIBSSH2_SESSION* s = (LIBSSH2_SESSION*)session_;
    if (!s) {
        r.err = "SSH 未连接";
        return r;
    }
    LIBSSH2_CHANNEL* ch = nullptr;
    auto open_deadline = Clock::now() + std::chrono::seconds(20);
    for (;;) {
        ch = libssh2_channel_open_session(s);
        if (ch) break;
        int rc = libssh2_session_last_errno(s);
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (!waitUntil(open_deadline)) {
                r.err = "打开 SSH 通道超时";
                return r;
            }
            continue;
        }
        char* msg = nullptr;
        libssh2_session_last_error(s, &msg, nullptr, 0);
        r.err = std::string("打开通道失败: ") + (msg ? msg : "unknown");
        return r;
    }
    auto exec_deadline = Clock::now() + std::chrono::seconds(20);
    for (;;) {
        int rc = libssh2_channel_exec(ch, cmd.c_str());
        if (rc == 0) break;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (!waitUntil(exec_deadline)) {
                r.err = "执行命令超时";
                libssh2_channel_free(ch);
                return r;
            }
            continue;
        }
        r.err = "执行命令失败";
        libssh2_channel_free(ch);
        return r;
    }

    // 先写 stdin（sudo 密码），关掉写方向让远端知道输入结束
    if (!stdin_data.empty()) {
        size_t off = 0;
        auto w_deadline = Clock::now() + std::chrono::seconds(20);
        bool w_ok = true;
        while (off < stdin_data.size()) {
            ssize_t w = libssh2_channel_write_ex(ch, 0, stdin_data.data() + off,
                                                 stdin_data.size() - off);
            if (w == LIBSSH2_ERROR_EAGAIN) {
                if (!waitUntil(w_deadline)) { w_ok = false; break; }
                continue;
            }
            if (w < 0) { w_ok = false; break; }
            off += (size_t)w;
        }
        if (!w_ok) {
            libssh2_channel_close(ch);
            libssh2_channel_free(ch);
            r.err = "写入远端 stdin 超时（sudo 密码没送出去）";
            return r;
        }
    }
    libssh2_channel_send_eof(ch);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    bool canceled = false, timed_out = false;
    char buf[8192];
    for (;;) {
        bool busy = false;
        for (int stream = 0; stream <= 1; ++stream) {
            for (;;) {
                ssize_t n = libssh2_channel_read_ex(ch, stream, buf, sizeof(buf));
                if (n > 0) {
                    if (r.out.size() < 256 * 1024) r.out.append(buf, (size_t)n);
                    continue;
                }
                if (n == LIBSSH2_ERROR_EAGAIN) {
                    busy = true;
                }
                break;
            }
        }
        if (libssh2_channel_eof(ch)) break;
        if (cancel && cancel()) {
            canceled = true;
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            timed_out = true;
            break;
        }
        if (busy) waitSocket(200);
        else usleep(20000);
    }

    if (canceled || timed_out) {
        libssh2_channel_close(ch);
        libssh2_channel_free(ch);
        r.err = canceled ? "已取消" : ("执行超时（" + std::to_string(timeout_s) + "s）");
        return r;
    }

    libssh2_channel_close(ch);
    r.status = libssh2_channel_get_exit_status(ch);
    libssh2_channel_free(ch);
    r.ok = (r.status == 0);
    return r;
}

bool DeploySsh::upload(const std::string& local, const std::string& remote,
                       int timeout_s, const CancelFn& cancel, std::string& err) {
    LIBSSH2_SESSION* s = (LIBSSH2_SESSION*)session_;
    if (!s) {
        err = "SSH 未连接";
        return false;
    }
    if (!sftp_) {
        LIBSSH2_SFTP* sf = nullptr;
        auto sf_deadline = Clock::now() + std::chrono::seconds(20);
        for (;;) {
            sf = libssh2_sftp_init(s);
            if (sf) break;
            if (libssh2_session_last_errno(s) == LIBSSH2_ERROR_EAGAIN) {
                if (!waitUntil(sf_deadline)) {
                    err = "打开 SFTP 超时";
                    return false;
                }
                continue;
            }
            char* msg = nullptr;
            libssh2_session_last_error(s, &msg, nullptr, 0);
            err = std::string("打开 SFTP 失败: ") + (msg ? msg : "unknown");
            return false;
        }
        sftp_ = sf;
    }

    FILE* f = fopen(local.c_str(), "rb");
    if (!f) {
        err = "读取本地安装包失败: " + std::string(strerror(errno));
        return false;
    }
    LIBSSH2_SFTP_HANDLE* h = nullptr;
    auto ro_deadline = Clock::now() + std::chrono::seconds(20);
    for (;;) {
        h = libssh2_sftp_open_ex((LIBSSH2_SFTP*)sftp_, remote.c_str(), remote.size(),
                                 LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                 LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR, 0);
        if (h) break;
        if (libssh2_session_last_errno(s) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitUntil(ro_deadline)) {
                fclose(f);
                err = "创建远端文件超时";
                return false;
            }
            continue;
        }
        char* msg = nullptr;
        libssh2_session_last_error(s, &msg, nullptr, 0);
        fclose(f);
        err = std::string("创建远端文件失败: ") + (msg ? msg : "unknown");
        return false;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    std::vector<char> buf(64 * 1024);
    bool ok = true;
    for (;;) {
        if (cancel && cancel()) {
            err = "已取消";
            ok = false;
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            err = "上传超时";
            ok = false;
            break;
        }
        size_t n = fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        size_t off = 0;
        while (off < n) {
            ssize_t w = libssh2_sftp_write(h, buf.data() + off, n - off);
            if (w == LIBSSH2_ERROR_EAGAIN) {
                if (!waitUntil(deadline)) {
                    err = "上传超时";
                    ok = false;
                    break;
                }
                continue;
            }
            if (w < 0) {
                err = "写入远端失败";
                ok = false;
                break;
            }
            off += (size_t)w;
        }
        if (!ok) break;
    }
    fclose(f);
    auto cl_deadline = Clock::now() + std::chrono::seconds(10);
    for (;;) {
        int rc = libssh2_sftp_close_handle(h);
        if (rc == 0) break;
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (!waitUntil(cl_deadline)) break;
            continue;
        }
        break;
    }
    return ok;
}
