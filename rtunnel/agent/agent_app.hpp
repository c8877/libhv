#pragma once
// C：设备侧 agent。连 B、认证、执行 OPEN、把字节透传给本地端口。
//
// 用法：rtunnel-agent --server HOST --port 8443 --devid PILE-000123 --allow 22 [--verbose]
// 长连接（HELLO/AUTH/心跳/重连/收帧）由 common/TunnelLink 承担。

#include "common/tunnel_link.hpp"
#include "fs_service.hpp"
#include "svc_session.hpp"
#include "proc_service.hpp"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>
#include <cstdio>

#include "cpputil/json.hpp"
#include <ctime>
#include <deque>
#include <map>
#include <set>

#include "cpputil/json.hpp"

class AgentApp;

namespace rtfs {
class Session;
}

struct LocalStream {
    AgentApp* app = nullptr;
    uint32_t sid = 0;
    uint64_t opened_ms = 0; // 会话建立时间，本地页「会话」卡片用
    int target_port = 0;     // 端口转发流的目标端口（本地页会话过滤用）
    hio_t* local = nullptr;
    int connected = 0;
    int local_eof = 0;
    int remote_closed = 0;
    int paused = 0;
    int want_shutdown = 0;
    uint32_t credit = 0;
    uint32_t unacked = 0;
    rtnp::Buffer pending;
    htimer_t* close_timer = nullptr; // 半关闭 60s 超时
    pid_t child_pid = -1;            // shell 子进程（is_shell 时）
    int is_shell = 0;                // OPEN_SHELL：pty 而非 TCP socket
    int is_fs = 0;                   // OPEN_FS：文件管理协议流
    rtfs::Session* fs_session = nullptr;
    int is_svc = 0;                  // OPEN_SVC：服务管理协议流
    rtvs::Session* svc_session = nullptr;
    std::string tty_path;    // TTY 流对应设备路径
    int is_monitor = 0;      // 只读监听流
    uint32_t monitor_of = 0; // 监听目标 sid
    int freed = 0;           // 防重复释放（close 回调与超时/断链并发）
    LocalStream* next = nullptr;
};

// 一次 OTA 安装的临时状态由函数栈持有；.new 文件仅用于原子替换

class AgentApp : public TunnelLink {
public:
    int run(int argc, char** argv);

    char devid[128] = {0};
    int allow_ports[32] = {0};
    int nallow = 0;
    int allow_all = 0;
    int allow_shell = 0;
    char allow_tty[512] = {0}; // 逗号分隔的串口白名单   // 允许 OPEN_SHELL（forkpty 起 shell，默认禁用）
    std::string config_file = "/opt/data/etc/rtunnel-agent.json"; // --config-file 可覆盖
    bool config_loaded = false;                          // 本次启动是否成功读取配置文件
    std::string file_dir = "/opt/data/files";
    double cpu_usage = 0;
    std::vector<double> cpu_cores;
    std::vector<unsigned long long> cpu_last_tot, cpu_last_idle;
    std::string args_line;               // 启动参数（展示/排障）
    std::vector<std::string> saved_argv; // 原样 argv，OTA 重启时 execv 使用
    int allow_ota = 0;                   // 允许 relay 通知安装 file_dir 里的新版本（默认关）
    std::string self_path;               // /proc/self/exe
    int max_idle = 0;                    // 无会话空闲多少秒后退出（0=常驻，按需 agent 用）
    uint64_t last_active_ms = 0;
    int had_stream = 0; // 是否用过（用过后无会话快速退出）
    uint64_t idle_since_ms = 0;
    LocalStream* streams = nullptr;
    std::string web_listen; // --local-web 监听地址（空=不启用本地 Web）
    int web_enabled = 0;
    std::string file_root;  // --file-root（空则用 file_dir）
    int file_rw = 0;        // --file-rw
    int file_max_mb = 512;  // --file-max-mb
    int file_enabled = 1;   // 文件服务开关（可由 relay 配置关闭）
    std::string file_token; // --file-token
    int svc_rw = 0;         // --svc-rw：允许启停/屏蔽服务
    int svc_enabled = 1;    // --no-svc 关闭
    std::string svc_token;  // --svc-token

    rtfs::Service& fsService() { return fs_; }
    bool fsReady() const { return fs_.ready(); }
    rtvs::Service& svcService() { return svc_; }
    bool svcReady() const { return svc_.ready(); }

    // 设备详情快照：只在 loop 线程采集，HTTP 线程读缓存字符串
    nlohmann::json buildSysinfo();
    void refreshWebSnapshot();
    std::string webSnapshot();

    // 本地页与 relay 共用的同步查询；会阻塞（双采样 / fork lsipc），
    // 必须在工作线程调用，不能跑在 loop 上。
    std::string procsListJson(const nlohmann::json& req);
    std::string netSnapshotJson(const nlohmann::json& req);
    std::string metricPointsJson(const nlohmann::json& q);

    // 本地页打开期间续期进程/服务预热（与 relay 详情的 WARM_REQ 共用缓存）。
    // 可从 HTTP 工作线程调用，事件循环操作会投递回 loop。
    void requestWarm(uint64_t ttl_ms);

protected:
    uint8_t role() const override { return rtnp::ROLE_AGENT; }
    const char* identity() const override { return devid; }
    const char* logTag() const override { return "agent"; }
    void onFrame(rtnp::Frame* f) override;
    void onLinkDown() override;

private:
    std::mutex web_mu_;
    std::string web_body_;    // refreshWebSnapshot 生成，webSnapshot 复制
    std::mutex net_mu_;
    nlohmann::json net_ipc_cache_;   // 最近一次 IPC 采集结果
    uint64_t net_ipc_ts_ = 0;
    std::atomic<int> net_inflight_{0};
    std::atomic<int> proc_inflight_{0};
    // 设备详情页打开期间（WARM_REQ 续期）每 30s 预采样进程与服务，供后续请求直接命中
    std::mutex proc_cache_mu_;
    rtproc::Snapshot proc_cache_;
    std::atomic<uint64_t> warm_until_ms_{0};
    std::atomic<uint64_t> warm_next_ms_{0};
    std::atomic<int> warm_inflight_{0};
    bool warm_timer_started_ = false;
    uint64_t started_ms_ = 0; // loop 启动时刻（单调时钟，算 agent 运行时长）
    rtfs::Service fs_;
    rtvs::Service svc_;

    int portAllowed(int port);
    LocalStream* streamFind(uint32_t sid);
    void streamFree(LocalStream* s);
    void onCloseTimeout(LocalStream* s);
    void armCloseTimer(LocalStream* s);
    void sendOpenFail(uint32_t sid, uint16_t code, const char* msg);
    void flushPending(LocalStream* s);
    void maybeGrantWindow(LocalStream* s);
    void maybeShutdownWrite(LocalStream* s);
    void onLocalWrite(LocalStream* s);
    void onLocalClose(LocalStream* s);
    void onLocalRead(LocalStream* s, void* buf, int readbytes);
    void onLocalConnect(LocalStream* s);
    void handleOpen(const rtnp::Frame* f);
    int openShellStream(LocalStream* s);
    void killShellChild(LocalStream* s);
    void sendSysinfo(int mode = 0);
    std::string sysinfo_sig;
    void onReady() override;
    void loadConfig();
    bool saveConfig();
    int applyFileConfig(const std::string& root, int rw, int max_mb, int enabled, std::string& err);
    void handleConfigSet(const rtnp::Frame* f);
    void handleFileFrame(const rtnp::Frame* f);
    void handleOtaApply(const rtnp::Frame* f);
    void handleNetReq(const rtnp::Frame* f);
    void handleProcReq(const rtnp::Frame* f);
    void handleWarmReq(const rtnp::Frame* f);
    void onWarmTick();
    void ensureWarmTimer();
    static void warmTickCb(htimer_t* t);
    static void warmKickCb(hevent_t* ev);
    static bool otaProbe(const std::string& path, const std::string& expect, std::string& out);
    bool hasOtaPending();
    void clearOtaPending();
    void scheduleAgentRestart(uint32_t sid, const std::string& installed);
    void restartAgentNow();
    std::string app_prefix_;            // /opt/rtunnel/agent（由 /proc/self/exe 推导）
    std::string ota_installed_path_;    // 本次安装后的可执行文件
    uint32_t ota_restart_sid_ = 0;      // 重启失败时补发 OTA_APPLY_ACK 用
    int ota_flush_ticks_ = 0;           // ACK 写完后等 flush 的计时
    bool ota_connected_ = false;        // 本次进程是否已通过 relay 认证
    struct TtyCfg {
        int enabled = 1;
        int baud = 115200;
        int data_bits = 8;
        char parity = 'N';
        int stop_bits = 1;
    };
    std::map<std::string, TtyCfg> tty_cfgs_;
    struct SeriesCfg {
        int enabled = 1;
        int interval = 5;
        int points = 720;
    };
    std::map<std::string, SeriesCfg> series_cfgs;
    std::mutex series_mu_; // loop 写 ring，本地页 HTTP 线程读
    std::map<std::string, std::deque<std::string>> series_rings;
    std::map<std::string, long long> series_last_ms;
    int series_revision = 0;
    void seriesTick();
    void sampleSeries(const std::string& name, const SeriesCfg& cfg);
    std::string sha256File(const std::string& path);
    void cleanupParts(const std::string& dir, time_t cutoff);

    // libhv C 回调的薄封装（对象经 hio_context / hevent_userdata 取回）
    static void localReadCb(hio_t* io, void* buf, int n);
    static void localWriteCb(hio_t* io, const void* buf, int len);
    static void localCloseCb(hio_t* io);
    static void localConnectCb(hio_t* io);
    static void closeTimeoutCb(htimer_t* t);
};
