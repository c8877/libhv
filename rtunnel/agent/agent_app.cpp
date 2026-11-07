#include "agent_app.hpp"
#include "hversion.h"
#include "cpputil/json.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"
#include "common/shell_spawn.hpp"

#include <stdio.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/sysinfo.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <string.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <vector>
#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#include "openssl/x509.h"
#include "openssl/pem.h"
#include "openssl/bio.h"
#endif
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <pty.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/ioctl.h>

// ---------------------------------------------------------------- 回调薄封装

void AgentApp::localReadCb(hio_t* io, void* buf, int n) {
    auto* s = (LocalStream*)hio_context(io);
    if (s) s->app->onLocalRead(s, buf, n);
}

void AgentApp::localWriteCb(hio_t* io, const void* buf, int len) {
    (void)buf; (void)len;
    auto* s = (LocalStream*)hio_context(io);
    if (s) s->app->onLocalWrite(s);
}

void AgentApp::localCloseCb(hio_t* io) {
    auto* s = (LocalStream*)hio_context(io);
    if (s) s->app->onLocalClose(s);
}

void AgentApp::localConnectCb(hio_t* io) {
    auto* s = (LocalStream*)hio_context(io);
    if (s) s->app->onLocalConnect(s);
}

void AgentApp::closeTimeoutCb(htimer_t* t) {
    auto* s = (LocalStream*)hevent_userdata(t);
    if (s) s->app->onCloseTimeout(s);
}

// ---------------------------------------------------------------- 会话管理

int AgentApp::portAllowed(int port) {
    if (allow_all) return 1;
    for (int i = 0; i < 32 && allow_ports[i] > 0; ++i) {
        if (allow_ports[i] == port) return 1;
    }
    return 0;
}

LocalStream* AgentApp::streamFind(uint32_t sid) {
    for (LocalStream* s = streams; s; s = s->next) {
        if (s->sid == sid) return s;
    }
    return NULL;
}

void AgentApp::streamFree(LocalStream* s) {
    if (s == NULL || s->freed) return;
    s->freed = 1;
    LocalStream** pp = &streams;
    while (*pp) {
        if (*pp == s) { *pp = s->next; break; }
        pp = &(*pp)->next;
    }
    if (s->close_timer) { htimer_del(s->close_timer); s->close_timer = NULL; }
    killShellChild(s);
    if (s->local && !hio_is_closed(s->local)) {
        hio_set_context(s->local, NULL);
        hio_close(s->local);
    }
    delete s;
}

// 半关闭 60s 未收到对端 CLOSE：RESET(CLOSE_TIMEOUT) 并回收（设计 §7.5）
void AgentApp::onCloseTimeout(LocalStream* s) {
    s->close_timer = NULL;
    if (io && state == 2 && !hio_is_closed(io)) {
        rtnp::Writer w;
        w.u16(rtnp::CLOSE_TIMEOUT);
        rtnp::send(io, rtnp::RESET, s->sid, w.data, (uint16_t)w.len);
    }
    streamFree(s);
}

void AgentApp::armCloseTimer(LocalStream* s) {
    if (s->close_timer) return;
    s->close_timer = htimer_add(loop, closeTimeoutCb, 60000, 1);
    hevent_set_userdata(s->close_timer, s);
}

void AgentApp::sendOpenFail(uint32_t sid, uint16_t code, const char* msg) {
    rtnp::Writer w;
    w.u16(code);
    w.str(msg ? msg : "");
    rtnp::send(io, rtnp::OPEN_FAIL, sid, w.data, (uint16_t)w.len);
}

void AgentApp::flushPending(LocalStream* s) {
    // remote_closed 只表示 A→C 方向 EOF，不影响 C→A 的数据回传
    if (!s->connected) return;
    while (s->pending.len > 0 && s->credit > 0) {
        size_t n = s->pending.len;
        if (n > s->credit) n = s->credit;
        if (n > rtnp::MAX_PAYLOAD) n = rtnp::MAX_PAYLOAD;
        if (rtnp::send(io, rtnp::DATA, s->sid, s->pending.data, (uint16_t)n) != 0) {
            return;
        }
        s->credit -= (uint32_t)n;
        s->pending.consume(n);
    }
    if (s->pending.len > 0) {
        if (!s->paused) { hio_read_stop(s->local); s->paused = 1; }
    } else if (s->paused) {
        s->paused = 0;
        hio_read(s->local);
    }
}

void AgentApp::maybeGrantWindow(LocalStream* s) {
    if (s->local == NULL || hio_is_closed(s->local) || s->unacked == 0) return;
    if (hio_write_bufsize(s->local) > rtnp::LOW_WATER) {
        hio_setcb_write(s->local, localWriteCb);
        return;
    }
    if (io) {
        rtnp::Writer w;
        w.u32(s->unacked);
        rtnp::send(io, rtnp::WINDOW, s->sid, w.data, (uint16_t)w.len);
    }
    s->unacked = 0;
    if (!s->want_shutdown) hio_setcb_write(s->local, NULL);
}

void AgentApp::maybeShutdownWrite(LocalStream* s) {
    if (!s->want_shutdown || s->local == NULL || hio_is_closed(s->local)) return;
    if (hio_write_is_complete(s->local)) {
        shutdown(hio_fd(s->local), SHUT_WR);
        s->want_shutdown = 0;
        hio_setcb_write(s->local, NULL);
    }
}

void AgentApp::onLocalWrite(LocalStream* s) {
    maybeShutdownWrite(s);
    maybeGrantWindow(s);
}

void AgentApp::onLocalClose(LocalStream* s) {
    if (verbose) logf_("[agent] local close sid=%u", s->sid);
    killShellChild(s);
    if (!s->connected) {
        // 本地 connect 失败
        sendOpenFail(s->sid, rtnp::TARGET_REFUSED, "connect refused");
        s->local = NULL;
        streamFree(s);
        return;
    }
    if (!s->local_eof) {
        s->local_eof = 1;
        if (state == 2 && io && !hio_is_closed(io)) {
            rtnp::send(io, rtnp::CLOSE, s->sid, NULL, 0);
        }
    }
    s->local = NULL;
    if (s->remote_closed) {
        streamFree(s);
    } else {
        armCloseTimer(s);
    }
}

void AgentApp::onLocalRead(LocalStream* s, void* buf, int readbytes) {
    if (verbose) logf_("[agent] local read sid=%u len=%d", s->sid, readbytes);
    if (readbytes <= 0) {
        // pty 的 io_type 不是 socket，libhv 不会自动 close；read==0 视为 EOF
        if (s->local && !hio_is_closed(s->local)) hio_close(s->local);
        return;
    }
    // 注意：remote_closed 只表示 A→C 方向已 EOF，C→A 方向仍要把数据发回去。
    if (s->pending.append(buf, (size_t)readbytes) != 0) {
        rtnp::send(io, rtnp::RESET, s->sid, NULL, 0);
        streamFree(s);
        return;
    }
    flushPending(s);
}

void AgentApp::onLocalConnect(LocalStream* s) {
    int err = hio_error(s->local);
    if (err != 0) {
        sendOpenFail(s->sid, rtnp::TARGET_REFUSED, "connect failed");
        s->local = NULL;
        streamFree(s);
        return;
    }
    s->connected = 1;
    rtnp::Writer w;
    w.str("127.0.0.1");
    w.u16(0);
    rtnp::send(io, rtnp::OPEN_OK, s->sid, w.data, (uint16_t)w.len);
    hio_setcb_read(s->local, localReadCb);
    hio_read(s->local);
    if (verbose) logf_("[agent] stream %u -> local fd=%d ok", s->sid, hio_fd(s->local));
}

void AgentApp::handleOpen(const rtnp::Frame* f) {
    if (verbose) logf_("[agent] OPEN sid=%u flags=0x%04x", f->sid, f->flags);
    // OPEN_SHELL：设备侧直接起 shell（forkpty），独立于端口白名单，须 --allow-shell 显式开启
    if (f->flags & rtnp::OPEN_SHELL) {
        if (!allow_shell) {
            sendOpenFail(f->sid, rtnp::TARGET_NOT_ALLOWED, "shell disabled");
            return;
        }
        auto* s = new LocalStream{};
        s->app = this;
        s->sid = f->sid;
        s->credit = rtnp::DEFAULT_CREDIT;
        s->is_shell = 1;
        had_stream = 1;
        s->next = streams;
        streams = s;
        if (openShellStream(s) != 0) {
            sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "shell spawn failed");
            streamFree(s);
        }
        return;
    }

    rtnp::Reader r(f->payload, f->len);
    uint16_t port = r.u16();
    if (r.err || port == 0) {
        sendOpenFail(f->sid, rtnp::PROTOCOL_ERROR, "bad open");
        return;
    }
    if (!portAllowed(port)) {
        sendOpenFail(f->sid, rtnp::TARGET_NOT_ALLOWED, "port not allowed");
        return;
    }
    if (streamFind(f->sid) != NULL) {
        sendOpenFail(f->sid, rtnp::PROTOCOL_ERROR, "sid in use");
        return;
    }

    auto* s = new LocalStream{};
    s->app = this;
    s->sid = f->sid;
    had_stream = 1;
    s->credit = rtnp::DEFAULT_CREDIT;
    s->next = streams;
    streams = s;

    hio_t* io = hio_create_socket(loop, "127.0.0.1", port, HIO_TYPE_TCP, HIO_CLIENT_SIDE);
    if (io == NULL) {
        sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "socket failed");
        streamFree(s);
        return;
    }
    s->local = io;
    hio_set_context(io, s);
    hio_setcb_connect(io, localConnectCb);
    hio_setcb_close(io, localCloseCb);
    hio_set_max_write_bufsize(io, rtnp::CONN_INFLIGHT + rtnp::MAX_PAYLOAD);
    if (hio_connect(io) != 0) {
        sendOpenFail(f->sid, rtnp::TARGET_REFUSED, "connect failed");
        s->local = NULL;
        streamFree(s);
    }
}

// OPEN_SHELL：forkpty 起交互 shell，pty 挂进事件循环，复用现有 DATA/流控逻辑
int AgentApp::openShellStream(LocalStream* s) {
    pid_t pid = -1;
    hio_t* pty = shellSpawnPty(loop, s, localReadCb, localCloseCb, &pid);
    if (pty == NULL) return -1;
    s->child_pid = pid;
    s->local = pty;
    s->connected = 1;
    rtnp::Writer w;
    w.str("127.0.0.1");
    w.u16(0);
    rtnp::send(io, rtnp::OPEN_OK, s->sid, w.data, (uint16_t)w.len);
    if (verbose) logf_("[agent] shell stream %u spawned pid=%d", s->sid, (int)pid);
    return 0;
}

void AgentApp::killShellChild(LocalStream* s) {
    if (s->child_pid > 0) {
        kill(s->child_pid, SIGHUP);
        waitpid(s->child_pid, NULL, WNOHANG);
        s->child_pid = -1;
    }
}

// READY 后的会话级帧（连接级 HELLO/AUTH/PING 由 TunnelLink 处理）
static void agentSampleCpu(AgentApp* app) {
    std::ifstream f("/proc/stat");
    if (!f) return;
    std::string line;
    std::vector<unsigned long long> tot, idl;
    while (std::getline(f, line)) {
        if (line.rfind("cpu", 0) != 0) break;
        unsigned long long v[8] = {0};
        int n = sscanf(line.c_str(), "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
                       &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        if (n < 4) continue;
        unsigned long long t = 0;
        for (int i = 0; i < 8; ++i) t += v[i];
        tot.push_back(t);
        idl.push_back(v[3] + v[4]);
    }
    if (tot.empty()) return;
    if (app->cpu_last_tot.size() == tot.size()) {
        app->cpu_usage = 0;
        app->cpu_cores.clear();
        for (size_t i = 0; i < tot.size(); ++i) {
            unsigned long long dt = tot[i] - app->cpu_last_tot[i];
            unsigned long long di = idl[i] - app->cpu_last_idle[i];
            double u = dt > 0 ? 100.0 * (double)(dt - di) / (double)dt : 0.0;
            if (i == 0) app->cpu_usage = u;
            else app->cpu_cores.push_back(u);
        }
    }
    app->cpu_last_tot = tot;
    app->cpu_last_idle = idl;
}

void AgentApp::loadConfig() {
    if (config_file.empty()) return;
    FILE* fp = fopen(config_file.c_str(), "r");
    if (!fp) return;
    std::string js;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) js.append(buf, n);
    fclose(fp);
    auto j = nlohmann::json::parse(js, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    if (j.contains("allow") && j["allow"].is_array()) {
        int idx = 0;
        for (auto& v : j["allow"]) {
            if (!v.is_number_integer()) continue;
            int p = v.get<int>();
            if (p > 0 && p < 65536 && idx < 31) allow_ports[idx++] = p;
        }
        allow_ports[idx] = 0;
        nallow = idx;
    }
    if (j.contains("shell") && j["shell"].is_boolean()) allow_shell = j["shell"].get<bool>() ? 1 : 0;
    if (j.contains("file_dir") && j["file_dir"].is_string()) {
        std::string v = j["file_dir"].get<std::string>();
        if (!v.empty() && v[0] == '/' && v.find("..") == std::string::npos) file_dir = v;
    }
    logf_("[agent] config loaded from %s (allow=%d shell=%d)", config_file.c_str(),
          allow_ports[0], allow_shell);
}

bool AgentApp::saveConfig() {
    if (config_file.empty()) return false;
    std::string dir = config_file.substr(0, config_file.find_last_of('/'));
    if (!dir.empty()) {
        std::string cmd = "mkdir -p '" + dir + "'";
        if (system(cmd.c_str()) != 0) { /* 目录可能已存在 */ }
    }
    nlohmann::json j;
    j["allow"] = nlohmann::json::array();
    for (int i = 0; i < 32 && allow_ports[i] > 0; ++i) j["allow"].push_back(allow_ports[i]);
    j["shell"] = allow_shell != 0;
    j["file_dir"] = file_dir;
    std::string tmp = config_file + ".tmp";
    FILE* fp = fopen(tmp.c_str(), "w");
    if (!fp) return false;
    std::string body = j.dump(2);
    bool ok = fwrite(body.data(), 1, body.size(), fp) == body.size();
    fclose(fp);
    if (!ok) { unlink(tmp.c_str()); return false; }
    return rename(tmp.c_str(), config_file.c_str()) == 0;
}

void AgentApp::handleConfigSet(const rtnp::Frame* f) {
    rtnp::Reader rr(f->payload, f->len);
    static char cfg[8192];
    rr.str(cfg, sizeof(cfg));
    auto r = nlohmann::json::parse(rr.err ? std::string() : std::string(cfg), nullptr, false);
    if (r.is_discarded() || !r.is_object()) {
        std::string e = "{\"ok\":false,\"error\":\"bad json\"}";
        rtnp::Writer w; w.str(e.c_str());
        rtnp::send(io, rtnp::CONFIG_ACK, f->sid, w.data, (uint16_t)w.len);
        return;
    }
    int new_ports[32] = {0};
    bool ports_set = false, shell_set = false;
    int new_shell = allow_shell;
    if (r.contains("ports")) {
        if (!r["ports"].is_array()) {
            std::string e = "{\"ok\":false,\"error\":\"ports must be array\"}";
            rtnp::Writer w; w.str(e.c_str());
            rtnp::send(io, rtnp::CONFIG_ACK, f->sid, w.data, (uint16_t)w.len);
            return;
        }
        int idx = 0;
        for (auto& v : r["ports"]) {
            if (!v.is_number_integer()) {
                std::string e = "{\"ok\":false,\"error\":\"bad port\"}";
                rtnp::Writer w; w.str(e.c_str());
                rtnp::send(io, rtnp::CONFIG_ACK, f->sid, w.data, (uint16_t)w.len);
                return;
            }
            int p = v.get<int>();
            if (p <= 0 || p > 65535 || idx >= 32) {
                std::string e = "{\"ok\":false,\"error\":\"port out of range or too many\"}";
                rtnp::Writer w; w.str(e.c_str());
                rtnp::send(io, rtnp::CONFIG_ACK, f->sid, w.data, (uint16_t)w.len);
                return;
            }
            bool dup = false;
            for (int k = 0; k < idx; ++k) if (new_ports[k] == p) dup = true;
            if (!dup) new_ports[idx++] = p;
        }
        ports_set = true;
        new_ports[idx] = 0;
    }
    if (r.contains("shell")) {
        if (!r["shell"].is_boolean()) {
            std::string e = "{\"ok\":false,\"error\":\"shell must be bool\"}";
            rtnp::Writer w; w.str(e.c_str());
            rtnp::send(io, rtnp::CONFIG_ACK, f->sid, w.data, (uint16_t)w.len);
            return;
        }
        new_shell = r["shell"].get<bool>() ? 1 : 0;
        shell_set = true;
    }
    std::string new_file_dir = file_dir;
    if (r.contains("file_dir")) {
        if (!r["file_dir"].is_string()) {
            std::string e = "{\"ok\":false,\"error\":\"file_dir must be string\"}";
            rtnp::Writer w; w.str(e.c_str());
            rtnp::send(io, rtnp::CONFIG_ACK, f->sid, w.data, (uint16_t)w.len);
            return;
        }
        std::string v = r["file_dir"].get<std::string>();
        if (v.empty() || v[0] != '/' || v.find("..") != std::string::npos || v.size() > 200) {
            std::string e = "{\"ok\":false,\"error\":\"bad file_dir\"}";
            rtnp::Writer w; w.str(e.c_str());
            rtnp::send(io, rtnp::CONFIG_ACK, f->sid, w.data, (uint16_t)w.len);
            return;
        }
        new_file_dir = v;
    }
    if (ports_set) {
        memcpy(allow_ports, new_ports, sizeof(allow_ports));
        nallow = 0;
        while (nallow < 32 && allow_ports[nallow] > 0) nallow++;
    }
    if (shell_set) allow_shell = new_shell;
    file_dir = new_file_dir;
    bool persisted = saveConfig();
    nlohmann::json ack;
    ack["ok"] = true;
    ack["persisted"] = persisted;
    ack["ports"] = nlohmann::json::array();
    for (int i = 0; i < 32 && allow_ports[i] > 0; ++i) ack["ports"].push_back(allow_ports[i]);
    ack["shell"] = allow_shell != 0;
    ack["file_dir"] = file_dir;
    std::string body = ack.dump();
    logf_("[agent] config set: ports=%d shell=%d persisted=%d", allow_ports[0], allow_shell, (int)persisted);
    rtnp::Writer w; w.str(body.c_str());
    rtnp::send(io, rtnp::CONFIG_ACK, f->sid, w.data, (uint16_t)w.len);
}

struct FileRx {
    FILE* fp = nullptr;
    uint64_t expect = 0;
    uint64_t written = 0;
    std::string name;
    std::string dest;
};

static std::map<uint32_t, FileRx> g_file_rx;


// 自带 SHA-256（不依赖 OpenSSL，板子可能按无 TLS 构建）
struct Sha256 {
    uint32_t h[8];
    uint64_t total;
    unsigned char buf[64];
    size_t n;
    void init() {
        static const uint32_t H[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                      0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
        for (int i = 0; i < 8; ++i) h[i] = H[i];
        total = 0; n = 0;
    }
    static uint32_t rotr(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }
    void block(const unsigned char* p) {
        static const uint32_t K[64] = {
            0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
            0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
            0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
            0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
            0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
                   ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6)^rotr(e,11)^rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2)^rotr(a,13)^rotr(a,22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    void update(const void* data, size_t len) {
        const unsigned char* p = (const unsigned char*)data;
        total += len;
        while (len > 0) {
            size_t take = 64 - n;
            if (take > len) take = len;
            memcpy(buf + n, p, take);
            n += take; p += take; len -= take;
            if (n == 64) { block(buf); n = 0; }
        }
    }
    std::string hex() {
        uint64_t bits = total * 8;
        unsigned char pad = 0x80;
        update(&pad, 1);
        unsigned char zero = 0;
        while (n != 56) update(&zero, 1);
        unsigned char lenb[8];
        for (int i = 0; i < 8; ++i) lenb[i] = (unsigned char)(bits >> (56 - i*8));
        update(lenb, 8);
        static const char* hx = "0123456789abcdef";
        std::string out;
        for (int i = 0; i < 8; ++i) {
            for (int j = 3; j >= 0; --j) {
                unsigned char c = (h[i] >> (j*8)) & 0xFF;
                out += hx[c >> 4]; out += hx[c & 0xF];
            }
        }
        return out;
    }
};

std::string AgentApp::sha256File(const std::string& path) {
    FILE* fp0 = fopen(path.c_str(), "rb");
    if (!fp0) return std::string();
    Sha256 ctx; ctx.init();
    char b[64 * 1024];
    size_t n;
    while ((n = fread(b, 1, sizeof(b), fp0)) > 0) ctx.update(b, n);
    fclose(fp0);
    return ctx.hex();
}


void AgentApp::handleFileFrame(const rtnp::Frame* f) {
    auto sendAck = [&](bool ok, uint64_t received, const std::string& err) {
        nlohmann::json j;
        j["ok"] = ok;
        j["received"] = received;
        if (!err.empty()) j["error"] = err;
        std::string s = j.dump();
        rtnp::Writer w; w.str(s.c_str());
        rtnp::send(io, rtnp::FILE_ACK, f->sid, w.data, (uint16_t)w.len);
    };
    if (f->type == rtnp::FILE_BEGIN) {
        rtnp::Reader r(f->payload, f->len);
        char dir[256] = {0};
        r.str(dir, sizeof(dir));
        char name[256] = {0};
        r.str(name, sizeof(name));
        uint64_t size = r.u64();
        char sha[80] = {0};
        r.str(sha, sizeof(sha));
        bool bad_dir = false;
        for (const char* p = dir; *p; ++p) {
            if (!(isalnum((unsigned char)*p) || *p == '/' || *p == '.' || *p == '_' || *p == '-')) {
                bad_dir = true; break;
            }
        }
        if (strstr(dir, "..") || dir[0] == '/') bad_dir = true;
        if (r.err || name[0] == '\0' || strstr(name, "..") || strchr(name, '/') ||
            size == 0 || bad_dir) {
            sendAck(false, 0, "bad begin");
            return;
        }
        std::string target = file_dir;
        if (dir[0]) target += "/" + std::string(dir);
        if (!target.empty()) {
            std::string cmd = "mkdir -p '" + target + "'";
            system(cmd.c_str());
        }
        std::string dest = target + "/" + name;
        std::string part = dest + ".part";
        uint64_t have = 0;
        struct stat st{};
        if (stat(part.c_str(), &st) == 0 && st.st_size > 0 && (uint64_t)st.st_size <= size) {
            have = (uint64_t)st.st_size;
        }
        FILE* fp = fopen(part.c_str(), have ? "ab" : "wb");
        if (!fp) { sendAck(false, 0, "open failed"); return; }
        FileRx rx;
        rx.fp = fp; rx.expect = size; rx.written = have; rx.name = name; rx.dest = dest;
        g_file_rx[f->sid] = rx;
        if (have) logf_("[agent] resume %s at %llu", name, (unsigned long long)have);
        sendAck(true, have, "");
        return;
    }
    auto it = g_file_rx.find(f->sid);
    if (it == g_file_rx.end()) { sendAck(false, 0, "no transfer"); return; }
    FileRx& rx = it->second;
    if (f->type == rtnp::FILE_DATA) {
        if (f->len > 0 && fwrite(f->payload, 1, f->len, rx.fp) != f->len) {
            sendAck(false, rx.written, "write failed");
            return;
        }
        rx.written += f->len;
        sendAck(true, rx.written, "");
        return;
    }
    // FILE_END
    fflush(rx.fp);
    fsync(fileno(rx.fp));
    fclose(rx.fp);
    rx.fp = nullptr;
    std::string part = rx.dest + ".part";
    std::string want = sha256File(part);
    rtnp::Reader r(f->payload, f->len);
    char expect[80] = {0};
    r.str(expect, sizeof(expect));
    if (want.empty() || strcmp(want.c_str(), expect) != 0) {
        unlink(part.c_str());
        g_file_rx.erase(it);
        sendAck(false, rx.written, "sha256 mismatch");
        return;
    }
    if (rename(part.c_str(), rx.dest.c_str()) != 0) {
        unlink(part.c_str());
        g_file_rx.erase(it);
        sendAck(false, rx.written, "rename failed");
        return;
    }
    logf_("[agent] file received: %s (%llu bytes)", rx.dest.c_str(), (unsigned long long)rx.written);
    g_file_rx.erase(it);
    sendAck(true, rx.written, "");
}

void AgentApp::cleanupParts(const std::string& dir, time_t cutoff) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        std::string p = dir + "/" + e->d_name;
        struct stat st{};
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            cleanupParts(p, cutoff);
        } else if (st.st_mtime < cutoff) {
            size_t n = strlen(e->d_name);
            if (n > 5 && strcmp(e->d_name + n - 5, ".part") == 0) unlink(p.c_str());
        }
    }
    closedir(d);
}

void AgentApp::sendSysinfo() {
    using nlohmann::json;
    agentSampleCpu(this);
    json j;
    j["version"] = hv_version();
    j["args"] = args_line;

    struct utsname u;
    if (uname(&u) == 0) {
        j["kernel"] = u.release;
        j["machine"] = u.machine;
    }
    {
        std::ifstream f("/proc/device-tree/model");
        if (f) { std::string m; std::getline(f, m); j["board"] = m; }
    }
    {
        std::ifstream f("/proc/cpuinfo");
        std::string line, model;
        int cores = 0;
        while (std::getline(f, line)) {
            if (line.rfind("processor", 0) == 0) cores++;
            if (model.empty() && (line.rfind("model name", 0) == 0 || line.rfind("Processor", 0) == 0)) {
                size_t p = line.find(':');
                if (p != std::string::npos) model = line.substr(p + 1);
            }
        }
        while (!model.empty() && (model[0] == ' ' || model[0] == '\t')) model.erase(0, 1);
        j["cpu_model"] = model;
    }
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        unsigned long long total = 0, avail = 0, swap_total = 0, swap_free = 0;
        while (std::getline(f, line)) {
            unsigned long long v = 0;
            if (sscanf(line.c_str(), "MemTotal: %llu kB", &v) == 1) total = v;
            else if (sscanf(line.c_str(), "MemAvailable: %llu kB", &v) == 1) avail = v;
            else if (sscanf(line.c_str(), "SwapTotal: %llu kB", &v) == 1) swap_total = v;
            else if (sscanf(line.c_str(), "SwapFree: %llu kB", &v) == 1) swap_free = v;
        }
        j["mem_total_kb"] = total;
        j["mem_available_kb"] = avail;
        j["swap_total_kb"] = swap_total;
        j["swap_free_kb"] = swap_free;
    }
    {
        nlohmann::json mounts = nlohmann::json::array();
        std::ifstream mf("/proc/mounts");
        std::string line;
        std::string last_dev;
        while (std::getline(mf, line)) {
            std::istringstream is(line);
            std::string dev, mnt, fstype;
            if (!(is >> dev >> mnt >> fstype)) continue;
            if (fstype == "squashfs" || (fstype == "overlay" && mnt != "/") ||
                mnt.rfind("/run", 0) == 0 || mnt.rfind("/dev/shm", 0) == 0 ||
                mnt.rfind("/sys", 0) == 0 ||
                fstype == "proc" || fstype == "sysfs" || fstype == "devtmpfs" ||
                fstype == "devpts" || fstype.rfind("cgroup", 0) == 0 ||
                fstype == "pstore" || fstype == "debugfs" || fstype == "securityfs" ||
                fstype == "tracefs" || fstype == "configfs" || fstype == "mqueue" ||
                fstype == "hugetlbfs" || fstype == "fusectl" || fstype == "binfmt_misc") continue;
            struct statvfs vfs;
            if (statvfs(mnt.c_str(), &vfs) != 0) continue;
            unsigned long long total = (unsigned long long)vfs.f_blocks * vfs.f_frsize / 1024;
            if (total == 0) continue;
            nlohmann::json m;
            m["mount"] = mnt;
            m["fstype"] = fstype;
            m["total_kb"] = total;
            m["free_kb"] = (unsigned long long)vfs.f_bavail * vfs.f_frsize / 1024;
            mounts.push_back(m);
        }
        j["mounts"] = mounts;
    }
    {
        nlohmann::json cpu;
        cpu["usage"] = cpu_usage;
        cpu["cores"] = cpu_cores;
        j["cpu"] = cpu;
    }
    {
        double la[3] = {0, 0, 0};
        if (getloadavg(la, 3) == 3) {
            j["load1"] = la[0]; j["load5"] = la[1]; j["load15"] = la[2];
        }
    }
    {
        json nics = json::array();
        struct ifaddrs* ifs = NULL;
        if (getifaddrs(&ifs) == 0) {
            for (struct ifaddrs* p = ifs; p; p = p->ifa_next) {
                if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET) continue;
                if (strcmp(p->ifa_name, "lo") == 0) continue;
                char ip[64] = {0}, mask[64] = {0};
                struct sockaddr_in* sin = (struct sockaddr_in*)p->ifa_addr;
                inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
                if (p->ifa_netmask) {
                    struct sockaddr_in* m = (struct sockaddr_in*)p->ifa_netmask;
                    inet_ntop(AF_INET, &m->sin_addr, mask, sizeof(mask));
                }
                json n;
                n["name"] = p->ifa_name;
                n["ip"] = ip;
                n["netmask"] = mask;
                n["up"] = (p->ifa_flags & IFF_UP) ? true : false;
                nics.push_back(n);
            }
            freeifaddrs(ifs);
        }
        j["nics"] = nics;
    }
    j["mtls"] = (tls_cert[0] != '\0');
    {
        nlohmann::json caps;
        caps["ports"] = nlohmann::json::array();
        for (int i = 0; i < 32 && allow_ports[i] > 0; ++i) caps["ports"].push_back(allow_ports[i]);
        caps["shell"] = allow_shell != 0;
        caps["sysinfo"] = true;
        caps["mtls"] = (tls_cert[0] != '\0');
        caps["file_dir"] = file_dir;
        j["caps"] = caps;
    }

#if defined(WITH_OPENSSL)
    if (tls_cert[0]) {
        FILE* fp = fopen(tls_cert, "r");
        if (fp) {
            X509* x = PEM_read_X509(fp, NULL, NULL, NULL);
            fclose(fp);
            if (x) {
                char subj[256] = {0}, iss[256] = {0};
                X509_NAME_oneline(X509_get_subject_name(x), subj, sizeof(subj));
                X509_NAME_oneline(X509_get_issuer_name(x), iss, sizeof(iss));
                j["cert_subject"] = subj;
                j["cert_issuer"] = iss;
                j["cert_serial"] = std::to_string((long long)ASN1_INTEGER_get(X509_get_serialNumber(x)));
                j["cert_valid"] = (X509_cmp_current_time(X509_get0_notAfter(x)) < 0) ? 0 : 1;
                X509_free(x);
            }
        }
    }
#endif

    std::string body = j.dump();
    rtnp::Writer w;
    w.str(body.c_str());
    rtnp::send(io, rtnp::SYSINFO, 0, w.data, (uint16_t)w.len);
}

void AgentApp::onFrame(rtnp::Frame* f) {
    switch (f->type) {
    case rtnp::SYSINFO_REQ:
        sendSysinfo();
        return;
    case rtnp::CONFIG_SET:
        handleConfigSet(f);
        return;
    case rtnp::FILE_BEGIN:
    case rtnp::FILE_DATA:
    case rtnp::FILE_END:
        handleFileFrame(f);
        return;
    case rtnp::OPEN:
        handleOpen(f);
        return;
    case rtnp::DATA: {
        LocalStream* s = streamFind(f->sid);
        if (s == NULL || s->local == NULL) return;
        if (f->len > 0) {
            hio_write(s->local, f->payload, f->len);
            s->unacked += f->len;
        }
        maybeGrantWindow(s);
        return;
    }
    case rtnp::WINDOW: {
        LocalStream* s = streamFind(f->sid);
        if (s == NULL) return;
        rtnp::Reader r(f->payload, f->len);
        uint32_t inc = r.u32();
        if (r.err) return;
        s->credit += inc;
        if (s->credit > rtnp::DEFAULT_CREDIT) s->credit = rtnp::DEFAULT_CREDIT;
        flushPending(s);
        return;
    }
    case rtnp::CLOSE: {
        LocalStream* s = streamFind(f->sid);
        if (s == NULL) return;
        s->remote_closed = 1;
        if (s->is_shell) {
            // pty 无 SHUT_WR 语义：对端 EOF = 结束 shell 子进程
            killShellChild(s);
        } else if (s->local && hio_write_is_complete(s->local)) {
            shutdown(hio_fd(s->local), SHUT_WR);
            s->want_shutdown = 0;
        } else {
            s->want_shutdown = 1;
            if (s->local) hio_setcb_write(s->local, localWriteCb);
        }
        if (s->local_eof) streamFree(s);
        else armCloseTimer(s);
        return;
    }
    case rtnp::RESIZE: {
        LocalStream* s = streamFind(f->sid);
        if (s == NULL || !s->is_shell || s->local == NULL) return;
        rtnp::Reader r(f->payload, f->len);
        uint16_t cols = r.u16();
        uint16_t rows = r.u16();
        if (r.err || cols == 0 || rows == 0) return;
        struct winsize wsz = {(unsigned short)rows, (unsigned short)cols, 0, 0};
        ioctl(hio_fd(s->local), TIOCSWINSZ, &wsz);
        return;
    }
    case rtnp::RESET: {
        LocalStream* s = streamFind(f->sid);
        if (s) streamFree(s);
        return;
    }
    default:
        return;
    }
}

void AgentApp::onLinkDown() {
    // 关闭所有本地连接
    while (streams) {
        LocalStream* s = streams;
        streams = s->next;
        if (s->close_timer) htimer_del(s->close_timer);
        if (s->local && !hio_is_closed(s->local)) hio_close(s->local);
        delete s;
    }
}

// ---------------------------------------------------------------- 入口

int AgentApp::run(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) {
        if (i) args_line += " ";
        args_line += argv[i];
    }
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--server") == 0 && i + 1 < argc) {
            host = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--devid") == 0 && i + 1 < argc) {
            snprintf(devid, sizeof(devid), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--allow") == 0 && i + 1 < argc) {
            char buf[256];
            snprintf(buf, sizeof(buf), "%s", argv[++i]);
            char* save = NULL;
            for (char* p = strtok_r(buf, ",", &save); p; p = strtok_r(NULL, ",", &save)) {
                if (nallow < (int)(sizeof(allow_ports) / sizeof(int))) {
                    allow_ports[nallow++] = atoi(p);
                }
            }
        } else if (strcmp(argv[i], "--allow-all") == 0) {
            allow_all = 1;
        } else if (strcmp(argv[i], "--config-file") == 0 && i + 1 < argc) {
            config_file = argv[++i];
        } else if (strcmp(argv[i], "--file-dir") == 0 && i + 1 < argc) {
            file_dir = argv[++i];
        } else if (strcmp(argv[i], "--allow-shell") == 0) {
            allow_shell = 1;
        } else if (strcmp(argv[i], "--max-idle") == 0 && i + 1 < argc) {
            max_idle = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--tls-ca") == 0 && i + 1 < argc) {
            snprintf(tls_ca, sizeof(tls_ca), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-cert") == 0 && i + 1 < argc) {
            snprintf(tls_cert, sizeof(tls_cert), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-key") == 0 && i + 1 < argc) {
            snprintf(tls_key, sizeof(tls_key), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--tls-sni") == 0 && i + 1 < argc) {
            snprintf(tls_sni, sizeof(tls_sni), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else {
            fprintf(stderr,
                "Usage: %s --devid ID [--server H] [--port P] [--allow 22,80] [--allow-all]\n"
                "          [--allow-shell [--max-idle SEC]] [--verbose]\n",
                argv[0]);
            return 1;
        }
    }
    if (devid[0] == '\0') {
        fprintf(stderr, "--devid is required\n");
        return 1;
    }

    hloop_t* lp = hloop_new(0);
    last_active_ms = hloop_now_ms(lp);
    if (max_idle > 0) {
        agentSampleCpu(this);
        htimer_t* cpu_timer = htimer_add(lp, [](htimer_t* t) {
            agentSampleCpu((AgentApp*)hevent_userdata(t));
        }, 1000, INFINITE);
        hevent_set_userdata(cpu_timer, this);
        htimer_t* part_timer = htimer_add(lp, [](htimer_t* t) {
            auto* app = (AgentApp*)hevent_userdata(t);
            app->cleanupParts(app->file_dir, time(NULL) - 24 * 3600);
        }, 3600 * 1000, INFINITE);
        hevent_set_userdata(part_timer, this);
        htimer_t* idle_timer = htimer_add(lp, [](htimer_t* t) {
            auto* app = (AgentApp*)hevent_userdata(t);
            uint64_t now = hloop_now_ms(app->loop);
            if (app->streams != NULL) {
                app->last_active_ms = now;
                app->had_stream = 1;
                app->idle_since_ms = 0;
                return;
            }
            if (app->had_stream) {
                // 按需 agent：最后一个会话结束后 2s 退出
                if (!app->idle_since_ms) app->idle_since_ms = now;
                if (now - app->idle_since_ms >= 2000) {
                    logf_("[agent] no sessions, exiting (on-demand agent)");
                    hloop_stop(app->loop);
                }
                return;
            }
            if (now - app->last_active_ms >= (uint64_t)app->max_idle * 1000) {
                logf_("[agent] idle %ds, exiting (on-demand agent)", app->max_idle);
                hloop_stop(app->loop);
            }
        }, 1000, INFINITE);
        hevent_set_userdata(idle_timer, this);
    }
    loadConfig();
    init(lp);
    hloop_run(lp);
    hloop_free(&lp);
    return 0;
}
