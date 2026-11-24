// 设备代理共用的 RTNP 帧工具
#include "proxy_util.hpp"
#include "rtnp/codec.hpp"
#include "cpputil/hstring.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

std::atomic<int> g_proxy_active{0};

ProxyGuard::ProxyGuard() { ++g_proxy_active; }
ProxyGuard::~ProxyGuard() { --g_proxy_active; }





int tcpConnect(const char* host, int port, int timeout_ms) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    // 管理台到本机 relay 的这条连接也是「帧头 + 小 payload」两次写，必须关 Nagle
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &sa.sin_addr) != 1 || connect(fd, (sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool writeAll(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n > 0) {
        ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
        if (w <= 0) return false;
        c += w; n -= (size_t)w;
    }
    return true;
}

bool readAll(int fd, void* p, size_t n) {
    char* c = (char*)p;
    while (n > 0) {
        ssize_t r = recv(fd, c, n, 0);
        if (r <= 0) return false;
        c += r; n -= (size_t)r;
    }
    return true;
}

void put16(std::string& s, uint16_t v) { s.push_back((char)(v >> 8)); s.push_back((char)v); }
void put32(std::string& s, uint32_t v) { for (int i = 3; i >= 0; --i) s.push_back((char)(v >> (8 * i))); }

void sendFrame(int fd, uint8_t type, uint32_t sid, const std::string& payload, uint16_t flags) {
    // RTNP 单帧 payload 上限 32K，超过则拆成多个同类型帧，接收端按序拼接
    size_t off = 0;
    do {
        size_t n = payload.size() - off;
        if (n > rtnp::MAX_PAYLOAD) n = rtnp::MAX_PAYLOAD;
        std::string h = "RTNP";
        h.push_back(1); h.push_back((char)type);
        put16(h, flags);
        put32(h, sid);
        put16(h, (uint16_t)n);
        put16(h, 0);
        writeAll(fd, h.data(), h.size());
        if (n) writeAll(fd, payload.data() + off, n);
        off += n;
    } while (off < payload.size());
}

void sendData(int fd, uint32_t sid, const std::string& p) { sendFrame(fd, 0x13, sid, p); }
void sendWindow(int fd, uint32_t sid, uint32_t n) {
    std::string w; put32(w, n); sendFrame(fd, 0x14, sid, w);
}

bool recvFrame(int fd, uint8_t& type, uint32_t& sid, std::string& payload) {
    char h[16];
    if (!readAll(fd, h, sizeof(h))) return false;
    if (memcmp(h, "RTNP", 4) != 0) return false;
    type = (uint8_t)h[5];
    sid = ((uint32_t)(uint8_t)h[8] << 24) | ((uint32_t)(uint8_t)h[9] << 16) |
          ((uint32_t)(uint8_t)h[10] << 8) | (uint8_t)h[11];
    uint16_t len = (uint16_t)((uint8_t)h[12] << 8 | (uint8_t)h[13]);
    payload.resize(len);
    if (len && !readAll(fd, &payload[0], len)) return false;
    return true;
}

std::string rtnpStr(const std::string& s) {
    std::string o;
    put16(o, (uint16_t)s.size());
    o += s;
    return o;
}

std::string urlDecode(const std::string& in) {
    std::string out;
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() && isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            auto hx = [](char c) { return (c <= '9') ? c - '0' : (tolower((unsigned char)c) - 'a' + 10); };
            out.push_back((char)((hx(in[i + 1]) << 4) | hx(in[i + 2])));
            i += 2;
        } else out.push_back(in[i]);
    }
    return out;
}

std::string lower(std::string s) {
    hv::tolower(s);
    return s;
}

std::string trim(const std::string& s) {
    return hv::trim(s);
}

std::string dechunk(const std::string& in) {
    std::string out;
    size_t p = 0;
    while (p < in.size()) {
        size_t eol = in.find("\r\n", p);
        if (eol == std::string::npos) break;
        long n = strtol(in.substr(p, eol - p).c_str(), nullptr, 16);
        if (n <= 0) break;
        size_t start = eol + 2;
        if (start + (size_t)n > in.size()) { out += in.substr(start); break; }
        out.append(in, start, (size_t)n);
        p = start + (size_t)n + 2;
    }
    return out;
}

// 打开一条到本机 relay 的 RTNP 会话并 OPEN(devid, port)
int openStream(const AdminConfig& cfg, const std::string& devid, int port, std::string& err) {
    int fd = tcpConnect("127.0.0.1", cfg.shell_relay_port, 5000);
    if (fd < 0) { err = "relay connect failed"; return -1; }

    std::string hello;
    hello.push_back(2);            // ROLE_CLIENT
    hello.push_back(1);            // ver
    hello += rtnpStr("admin-proxy");
    put32(hello, 0);
    sendFrame(fd, 0x01, 0, hello);

    uint8_t t; uint32_t sid; std::string p;
    if (!recvFrame(fd, t, sid, p) || t != 0x02) { err = "no HELLO_ACK"; close(fd); return -1; }

    // 管理台内部代理：auth_type=3 + 进程内密钥，relay 校验后按 console 身份放行
    std::string auth = rtnpStr("console");
    auth.push_back(cfg.proxy_secret.empty() ? 0 : 3);
    auth += rtnpStr(cfg.proxy_secret);
    sendFrame(fd, 0x03, 0, auth);
    if (!recvFrame(fd, t, sid, p) || t != 0x04) { err = "auth failed"; close(fd); return -1; }

    std::string open = rtnpStr(devid);
    put16(open, (uint16_t)port);
    sendFrame(fd, 0x10, 1, open);
    if (!recvFrame(fd, t, sid, p) || t != 0x11) {
        if (t == 0x12 && p.size() > 2) {
            // OPEN_FAIL: u16 code + str msg
            err = "open failed: " + p.substr(4);
        } else {
            err = "open failed";
        }
        close(fd);
        return -1;
    }
    return fd;
}


int openTtyStream(const AdminConfig& cfg, const std::string& devid, const std::string& path,
                  int baud, int data_bits, char parity, int stop_bits, uint16_t flags, std::string& err) {
    int fd = tcpConnect("127.0.0.1", cfg.shell_relay_port, 5000);
    if (fd < 0) { err = "relay connect failed"; return -1; }
    std::string hello;
    hello.push_back(2);
    hello.push_back(1);
    hello += rtnpStr("admin-tty");
    put32(hello, 0);
    sendFrame(fd, 0x01, 0, hello);
    uint8_t t; uint32_t sid; std::string p;
    if (!recvFrame(fd, t, sid, p) || t != 0x02) { err = "no HELLO_ACK"; close(fd); return -1; }
    // 管理台内部代理：auth_type=3 + 进程内密钥，relay 校验后按 console 身份放行
    std::string auth = rtnpStr("console");
    auth.push_back(cfg.proxy_secret.empty() ? 0 : 3);
    auth += rtnpStr(cfg.proxy_secret);
    sendFrame(fd, 0x03, 0, auth);
    if (!recvFrame(fd, t, sid, p) || t != 0x04) { err = "auth failed"; close(fd); return -1; }

    std::string op = rtnpStr(devid);
    op += rtnpStr(path);
    put32(op, (uint32_t)baud);
    op.push_back((char)data_bits);
    op.push_back(parity);
    op.push_back((char)stop_bits);
    sendFrame(fd, 0x10, 1, op, flags);
    if (!recvFrame(fd, t, sid, p) || t != 0x11) {
        err = (t == 0x12 && p.size() > 2) ? ("open failed: " + p.substr(4)) : "open failed";
        close(fd);
        return -1;
    }
    return fd;
}

// 打开设备内建服务流（OPEN_FS / OPEN_SVC）。payload 由调用方按各自协议拼好。
static int openBuiltinStream(const AdminConfig& cfg, const std::string& devid, uint16_t flags,
                             const std::string& payload, const char* tag, std::string& err) {
    int fd = tcpConnect("127.0.0.1", cfg.shell_relay_port, 5000);
    if (fd < 0) { err = "relay connect failed"; return -1; }
    std::string hello;
    hello.push_back(2);
    hello.push_back(1);
    hello += rtnpStr(tag);
    put32(hello, 0);
    sendFrame(fd, 0x01, 0, hello);
    uint8_t t; uint32_t sid; std::string p;
    if (!recvFrame(fd, t, sid, p) || t != 0x02) { err = "no HELLO_ACK"; close(fd); return -1; }
    // 管理台内部代理：auth_type=3 + 进程内密钥，relay 校验后按 console 身份放行
    std::string auth = rtnpStr("console");
    auth.push_back(cfg.proxy_secret.empty() ? 0 : 3);
    auth += rtnpStr(cfg.proxy_secret);
    sendFrame(fd, 0x03, 0, auth);
    if (!recvFrame(fd, t, sid, p) || t != 0x04) { err = "auth failed"; close(fd); return -1; }

    std::string op = rtnpStr(devid);
    op += payload;
    sendFrame(fd, 0x10, 1, op, flags);
    if (!recvFrame(fd, t, sid, p) || t != 0x11) {
        err = (t == 0x12 && p.size() > 2) ? ("open failed: " + p.substr(4)) : "open failed";
        close(fd);
        return -1;
    }
    // 会话可能长时间空闲，去掉握手阶段的 5s 读超时
    struct timeval tv = {0, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

int openFsStream(const AdminConfig& cfg, const std::string& devid, bool rw, std::string& err) {
    std::string op;
    op.push_back((char)(rw ? 1 : 0));
    op.push_back((char)1);   // trusted：relay 已完成用户认证
    return openBuiltinStream(cfg, devid, rtnp::OPEN_FS, op, "admin-fs", err);
}

int openSvcStream(const AdminConfig& cfg, const std::string& devid, std::string& err) {
    std::string op;
    op.push_back((char)1);   // trusted：relay 已完成用户认证
    return openBuiltinStream(cfg, devid, rtnp::OPEN_SVC, op, "admin-svc", err);
}
