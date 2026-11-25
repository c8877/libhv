#pragma once
// B：中继服务器。设备注册表 + ACL + c_sid 分配 + 帧转发。
//
// 用法：rtunnel-relay --port 8443 [--host 0.0.0.0] [--acl user=devid1,devid2] [--allow-all]

#include "rtnp/codec.hpp"

#include <string>
#include <unordered_map>
#include <vector>
#include <set>

class RelayServer;
struct RelayConn;

struct SysinfoEntry {
    std::string json;
    uint64_t    ts = 0;
};

struct FilePush {
    FILE*       fp = nullptr;
    uint64_t    size = 0;
    uint64_t    sent = 0;
    uint64_t    received = 0;
    int         phase = 0;         // 0=等 BEGIN ACK，1=传数据，2=等 END ACK
    std::string devid;
    std::string name;
    std::string sha;
};

struct ConfigAck {
    uint32_t    sid = 0;
    std::string json;
    uint64_t    ts = 0;
};

// agent OTA：文件已由分发通道落到设备 file_dir，这里只下发安装指令并跟踪阶段
struct OtaApply {
    std::string  devid;
    std::string  version;
    std::string  phase;      // verifying / installing / installed
    std::string  error;
    int          done = 0;
    int          ok = 0;
    uint64_t     ts = 0;
};

struct RelayStream {
    uint32_t     a_sid = 0;
    uint32_t     c_sid = 0;
    RelayConn*   a = nullptr;
    RelayConn*   c = nullptr;
    int          a_closed = 0;
    int          c_closed = 0;
    int          opened = 0;             // OPEN_OK 经过 relay 时置位
    int          a_readonly = 0;         // 只读串口流：丢弃 client 方向的 DATA（兼容旧 agent）
    uint16_t     port = 0;               // OPEN 目标端口，用量上报用
    uint64_t     opened_at_ms = 0;       // 建流时间（管理台展示）
    uint64_t     last_active_ms = 0;     // 最近一次转发帧，空闲回收用
    uint64_t     bytes_up = 0;           // A→C 载荷字节（用量）
    uint64_t     bytes_down = 0;         // C→A 载荷字节（用量）
    RelayStream* next_a = nullptr;   // a->streams
    RelayStream* next_c = nullptr;   // c->streams
};

struct RelayConn {
    RelayServer* server = nullptr;
    hio_t*       io = nullptr;
    int          role = 0;           // rtnp::ROLE_*
    int          authed = 0;
    int          internal = 0;   // 管理台内部链路（终端/代理），不在客户端列表展示
    int          admin_proxy = 0; // 通过进程内代理密钥认证，跳过分组鉴权
    char         devid[128] = {0};
    char         user[128] = {0};
    rtnp::Decoder in;
    RelayStream* streams = nullptr;
    uint32_t     next_c_sid = 0;
    RelayConn*   next_all = nullptr; // 全局链表
    RelayConn*   reg_next = nullptr; // 注册表链表（仅 agent）
    uint64_t     bytes_in = 0;
    uint64_t     bytes_out = 0;
    htimer_t*    hs_timer = nullptr; // 握手+认证超时（AUTH_OK 后摘除）
    uint64_t     connected_at_ms = 0;
    uint64_t     last_seen_ms = 0;
    char         remote_addr[64] = {0};
    long long    token_id = 0;           // auth_type=2 通过后填
    long long    account_id = 0;         // 令牌归属账号（用量归属）
    char         token_prefix[24] = {0};
    int          peer_proto = 0;         // HELLO 里的语义版本，0 表示未握手
    uint64_t     usage_online_ms = 0;    // 上次计入在线时长的单调时钟
};

// OPEN_SHELL 到 a:<user> 但 agent 尚未注册时挂起，等 A 侧 agent 上线后继续
struct PendingOpen {
    RelayConn*  a = nullptr;
    uint32_t    a_sid = 0;
    char        devid[128] = {0};
    uint16_t    port = 0;
    uint16_t    flags = 0;
    uint64_t    deadline_ms = 0;
    PendingOpen* next = nullptr;
};

// 接入账号 Token 的运行时策略（admin 推送，relay 线程只读）
struct TokenPolicy {
    long long   id = 0;
    long long   account_id = 0;
    std::string username;
    std::string hash;
    std::string prefix;
    std::string name;
    int         status = 0;              // 0 active, 1 disabled, 2 revoked
    int         account_active = 1;
    int64_t     expires_at_ms = 0;       // 0 = 永久
    int64_t     quota_bytes = 0;         // 0 = 不限
    int         quota_period = 0;        // 0 total, 1 daily, 2 monthly
    int64_t     period_start_ms = 0;
    int64_t     used_base = 0;           // DB 中已落库用量
    int         rate_per_min = 0;
    int         burst = 0;
    int         max_sessions = 0;
    int         quota_action = 0;        // 0 cut, 1 block
};

struct TokenUsage {
    int64_t  used_bytes = 0;
    int64_t  period_start_ms = 0;
    double   bucket = 0;
    uint64_t refill_ms = 0;
    int      active_sessions = 0;
    uint64_t last_used_ms = 0;
    int64_t  reported_bytes = 0;   // 已推给管理面的字节，增量上报用
    int64_t  last_report_ms = 0;   // 上次上报时间（墙钟）：按量/按时节流
    char     last_ip[64] = {0};
};

// relay 自身累计计数（只在 relay 线程读写，管理台经 callRelay 取快照）
struct RelayCounters {
    uint64_t conns_agent_total = 0;
    uint64_t conns_client_total = 0;
    uint64_t auth_failed_total = 0;
    uint64_t frames_in_total = 0;          // 从对端收到的完整帧
    uint64_t frames_forwarded_total = 0;   // 成功转发给对端的帧
    uint64_t frame_errors_total = 0;       // 坏帧 / 未知 sid / 协议错
    uint64_t bytes_in_total = 0;
    uint64_t bytes_forwarded_total = 0;
    uint64_t streams_opened_total = 0;
    uint64_t streams_open_failed_total = 0;
    uint64_t streams_idle_reset_total = 0;
};

struct GroupAclRule {
    std::string user;
    std::string devid;
    std::string ports;
    std::string tty;         // 可读的串口（* = 全部）
    bool        tty_write = false; // 是否允许写串口 / 强制接管
    bool        shell = false;
    int         file = 0;    // 0 无 / 1 只读 / 2 读写
    bool        svc = false;
    long long   token_id = 0; // 0 = 账号级规则；>0 = 仅该 rtk 令牌
};

class RelayServer {
    friend class AdminServer;

public:
    int run(int argc, char** argv);
    void setGroupAcl(std::vector<GroupAclRule> rules, std::set<std::string> users,
                     std::set<long long> scoped_tokens);
    // 认证策略全量替换（relay 线程）；JSON 由 admin 的 clientAuthPolicies 生成
    void setAuthPolicies(const std::string& json);
    // 用量快照（relay 线程），JSON 数组
    std::string tokenUsageSnapshot();
    // 取走待落库的用量事件（relay 线程），JSON 数组；含 token 字节增量与设备在线时长
    std::string takeUsageEvents();

    // 更新下发给 agent 的地址列表（relay 线程）。servers 为空表示停用下发。
    // 内容变化时更新 rev 并清空旧 ACK；只改期望状态，设备上线在 AUTH_OK 后自动对齐。
    bool setRelayList(const std::string& servers, bool allow_leave, std::string& err);
    std::string relayListStatus();    // JSON：配置 + 在线 agent 的采纳情况
    // 按任务快照下行（同步任务用，内容可与当前运行时列表不同）
    bool sendRelayListJson(const char* devid, const std::string& json);
    std::string relayListAcksJson();                 // JSON 对象：devid -> 最近 ACK
    std::string onlineAgentDevids();                 // JSON 数组，relay 线程

    hloop_t*    loop = nullptr;
    void*       admin = nullptr;      // AdminServer*（--admin 启用时创建）
    uint64_t    started_at_ms = 0;
    RelayCounters stats;              // 自身累计指标（relay 线程）
    std::string cert_dir;             // 证书台账目录（--admin-cert-dir）
    std::vector<std::string> revoked_sans;  // 已吊销身份（cert_dir/revoked.txt 持久化）
    RelayConn*  conns = nullptr;      // 全局连接表
    RelayConn*  agents = nullptr;     // devid 注册表
    std::unordered_map<std::string, SysinfoEntry> sysinfo_cache;
    struct SysinfoLast { std::string json; bool full = true; uint64_t ts = 0; };
    std::unordered_map<std::string, SysinfoLast> sysinfo_last;
    std::unordered_map<std::string, ConfigAck> config_acks;
    std::unordered_map<std::string, int> caps_shell;   // devid -> 0/1，用于设备列表隐藏 Shell
    std::unordered_map<std::string, std::string> caps_file_dir;
    std::unordered_map<std::string, int> caps_ota;      // devid -> 0/1（未知时无条目）
    std::unordered_map<std::string, std::string> caps_arch; // devid -> uname -m
    std::unordered_map<std::string, std::vector<int>> caps_ports; // devid -> 允许转发端口
    std::unordered_map<std::string, std::string> caps_local_web;  // devid -> 本地页监听地址
    std::unordered_map<std::string, int> caps_net;                 // devid -> 支持 NET_REQ
    std::unordered_map<std::string, int> caps_proc;                // devid -> 支持 PROC_REQ
    std::string admin_proxy_secret;   // 进程内生成的随机密钥，仅供 AdminServer 代理连接 relay
    std::unordered_map<std::string, std::string> agent_versions;
    std::unordered_map<std::string, uint64_t> agent_boot_ms;   // devid -> 系统开机时间（epoch ms）
    std::unordered_map<uint32_t, FilePush> pushes;
    std::unordered_map<uint32_t, std::string> push_results;
    uint32_t push_seq = 0;
    std::unordered_map<uint32_t, OtaApply> ota_applies;
    uint32_t ota_seq = 0;
    std::unordered_map<uint32_t, std::string> check_results;
    uint32_t check_seq = 0;
    std::unordered_map<uint32_t, std::string> tty_list_results;
    std::unordered_map<uint32_t, std::string> net_results;
    uint32_t net_seq = 0;
    std::unordered_map<uint32_t, std::string> proc_results;
    uint32_t proc_seq = 0;
    std::unordered_map<uint32_t, std::string> power_results;
    uint32_t power_seq = 0;
    // 分块结果（诊断包 / 目录树）：meta 是 INFO 帧的 json，body 是原始字节
    struct ChunkAcc {
        std::string meta;
        std::string body;
    };
    std::unordered_map<uint32_t, ChunkAcc> diag_acc;      // 分块接收中
    std::unordered_map<uint32_t, ChunkAcc> diag_results;  // 已收完，等管理台取走
    uint32_t diag_seq = 0;
    std::unordered_map<uint32_t, ChunkAcc> storage_acc;
    std::unordered_map<uint32_t, ChunkAcc> storage_results;
    uint32_t storage_seq = 0;
    std::unordered_map<std::string, uint64_t> warm_sent_ms; // devid -> 上次下发预热的时间
    struct NetCache { std::string json; uint64_t ts = 0; };
    std::unordered_map<std::string, NetCache> net_cache;
    std::unordered_map<uint32_t, std::string> metric_data_results;
    std::unordered_map<std::string, std::string> metric_status;
    uint32_t metric_query_seq = 0;
    uint32_t tty_list_seq = 0;
    int         allow_all = 0;   // --allow-all 测试开关：跳过分组鉴权
    int         require_mtls = 0;
    int         verbose = 0;
    std::vector<GroupAclRule> acl_groups;       // 管理台分组授权（relay 线程只读）
    std::set<std::string>     acl_grouped_users;
    std::set<long long>       acl_scoped_tokens; // 配了 rtk 权限组的令牌：只认令牌级规则
    std::unordered_map<std::string, TokenPolicy> token_cache;  // sha256 hex -> policy
    std::unordered_map<std::string, int> account_active;       // username -> 1/0
    std::unordered_map<long long, TokenUsage>    token_usage;  // token id -> usage
    // 待管理面取走的用量事件（JSON 行）与设备在线时长累加（relay 线程）
    std::vector<std::string> usage_pending;
    struct OnlineAcc { std::string day; long long ms = 0; };
    std::unordered_map<std::string, OnlineAcc> usage_online;
    uint64_t usage_dropped = 0;      // 队列满丢弃的事件数（管理面长期不可用时）
    std::set<long long> pending_token_cuts;       // 流量到顶待断开（帧处理内不能直接 close）
    PendingOpen* pending = nullptr;   // 等待 A 侧 agent 上线的 OPEN_SHELL
    // relay 下发的地址列表（只在 relay 线程读写）
    std::string relay_list_json;      // 空 = 不下发
    std::string relay_list_servers;   // 规范化后的 "h:p,..."
    bool        relay_list_allow_leave = false;
    uint32_t    relay_list_rev = 0;
    struct RelayListAck { std::string json; uint64_t ts = 0; };
    std::unordered_map<std::string, RelayListAck> relay_list_acks;  // devid -> 最近一次 ACK
    hio_t*      listenio = nullptr;
    int         draining = 0;
    uint64_t    drain_deadline = 0;
    uint32_t    drain_timeout_ms = 10000;
    uint32_t    stream_idle_timeout_ms = 1800000;  // 流空闲回收（0 = 关闭）

private:
    bool         groupAclDevice(const char* user, long long token_id, const char* devid);
    bool         groupAclCheck(const char* user, long long token_id, const char* devid, uint16_t port,
                               int is_shell, const char* tty_path, int tty_write, int is_fs, int fs_rw,
                               int is_svc);
    bool         groupAclApplies(const GroupAclRule& r, const char* user, long long token_id) const;
    RelayConn*   findClientByUser(const char* user);
    void         addPending(RelayConn* a, uint32_t a_sid, const char* devid, uint16_t port, uint16_t flags);
    void         completePending(const char* devid);
    void         sweepPending();
    void         sweepStreams();          // 回收长时间无数据的已建流
    void         sweepIdleAgents();       // 回收长时间没有帧的 agent 控制连接
    void         dropPendingFor(RelayConn* a);
    RelayConn*   findAgent(const char* devid);
    RelayStream* findStreamByA(RelayConn* a, uint32_t sid, RelayStream** prev);
    RelayStream* findStreamByC(RelayConn* c, uint32_t sid, RelayStream** prev);
    void         streamRemove(RelayStream* s);
    void         sendErr(hio_t* io, uint8_t type, uint32_t sid, uint16_t code, const char* msg);
    void         forward(RelayConn* to, uint8_t type, uint32_t sid, const uint8_t* payload, uint16_t len, uint16_t flags = 0);
    int          checkPeerSan(RelayConn* c, const char* claimed, const char* what, const char* id_name);
    void         registryRemove(RelayConn* c);
    void         connCloseAllStreams(RelayConn* c, uint16_t code);
    // 管理台写操作（均在 relay 线程内调用）
    bool         kickStream(uint32_t a_sid);          // 双侧 RESET + 回收映射
    bool         kickAgent(const char* devid);        // 强制下线（agent 会退避重连）
    // 撤权联动：关闭接入账号的活跃会话（含控制连接）。
    // token_id>0 时只关该令牌建立的连接；返回关闭的连接数。
    int          kickClientSessions(const std::vector<std::string>& users, long long token_id,
                                    const char* reason);
    bool         isRevoked(const std::string& san) const;
    bool         revokeSan(const std::string& san);   // 写黑名单 + 踢在线连接
    void         loadRevoked();                       // 启动时读 revoked.txt
    bool         sendSysinfoReq(const char* devid, int mode = 0);   // 要求 agent 立即上报
    bool         sendRelayList(RelayConn* c);                       // 给单条 agent 连接下发
    std::string  sysinfoOf(const char* devid, uint64_t* ts);
    std::string  sysinfoLastOf(const char* devid, bool* full, uint64_t* ts);
    bool         sendConfig(const char* devid, uint32_t sid, const std::string& json);
    std::string  configAck(const char* devid, uint32_t sid, uint64_t* ts);
    bool         startFilePush(const char* devid, const char* path, const char* dir,
                               const char* name, uint64_t size, const std::string& sha,
                               std::string& err, uint32_t& tid);
    std::string  pushProgress(uint32_t tid);
    bool         cancelFilePush(uint32_t tid);
    bool         startOtaApply(const char* devid, const char* path, const std::string& version,
                               uint64_t size, const std::string& sha, std::string& err, uint32_t& tid);
    std::string  otaApplyProgress(uint32_t tid);
    bool         cancelOtaApply(uint32_t tid);
    std::string  agentVersion(const char* devid);
    std::string  agentArch(const char* devid);
    std::vector<int> agentPorts(const char* devid); // 允许转发的端口，未知时为空
    std::string  agentLocalWeb(const char* devid);
    int          agentOtaCap(const char* devid);   // -1 未知 0 不支持 1 支持
    int          agentNetCap(const char* devid);   // -1 未知 0 不支持 1 支持
    int          agentProcCap(const char* devid);  // -1 未知 0 不支持 1 支持
    bool         startPowerAction(const char* devid, const std::string& json, uint32_t& rid, std::string& err);
    std::string  takePowerResult(uint32_t rid);
    bool         startDiag(const char* devid, const std::string& json, uint32_t& rid, std::string& err);
    // 取走诊断包；meta 是 DIAG_INFO 的 json，body 是 tar.gz 内容。没有结果返回 false。
    bool         takeDiag(uint32_t rid, std::string& meta, std::string& body);
    bool         startStorageQuery(const char* devid, const std::string& json, uint32_t& rid,
                                   std::string& err);
    // 取走目录树；meta 是 STORAGE_INFO 的 json，body 是 nodes 数组 JSON。
    bool         takeStorageResult(uint32_t rid, std::string& meta, std::string& body);
    uint64_t     agentBootMs(const char* devid);   // 系统开机时间（epoch ms），0 未知
    bool         startFileCheck(const char* devid, const char* dir, const char* name,
                                uint32_t& cid, std::string& err);
    std::string  takeCheckResult(uint32_t cid);
    bool         startTtyList(const char* devid, uint32_t& rid, std::string& err);
    std::string  takeTtyListResult(uint32_t rid);
    bool         startNetQuery(const char* devid, const std::string& json, uint32_t& rid, std::string& err);
    std::string  takeNetResult(uint32_t rid);
    bool         netCacheGet(const std::string& key, uint64_t max_age_ms, std::string& out, uint64_t& ts);
    void         netCachePut(const std::string& key, const std::string& json);
    bool         startProcQuery(const char* devid, const std::string& json, uint32_t& rid, std::string& err);
    std::string  takeProcResult(uint32_t rid);
    // 详情页打开期间让 agent 每 30s 预采样（同一设备 15s 内只下发一次）
    void         warmDevice(const char* devid, uint32_t ttl_ms = 120000);
    bool         pushTtyCfg(const char* devid, const std::string& json);
    bool         pushMetricCfg(const char* devid, const std::string& json);
    bool         startMetricQuery(const char* devid, const std::string& json, uint32_t& rid, std::string& err);
    std::string  takeMetricData(uint32_t rid);
    std::string  takeMetricStatus(const std::string& devid);
    bool         hasAgent(const char* devid);
    bool         agentOnline(const char* devid) { return findAgent(devid) != nullptr; }
    std::string  fileDirOf(const char* devid);
    void         onFileAck(RelayConn* c, const rtnp::Frame* f);
    void         onOtaAck(RelayConn* c, const rtnp::Frame* f);    void         onHsTimeout(RelayConn* c);
    void         handleFrame(RelayConn* c, rtnp::Frame* f);
    void         onRead(RelayConn* c, void* buf, int readbytes);
    void         onClose(RelayConn* c);
    void         onAccept(hio_t* io);
    void         onStats();
    void         onDrain(htimer_t* timer);
    void         onShutdown(int signo);
    void         tokenSweep();
    const TokenPolicy* tokenById(long long id) const;
    TokenPolicy* tokenById(long long id);
    void         tokenAddBytes(RelayConn* c, uint32_t n);
    bool         tokenRateAllow(RelayConn* c, uint32_t* retry_after_ms);
    void         tokenDropConnections(long long token_id, uint16_t code, const char* msg);
    void         tokenApplyCuts();
    void         tokenSessionAdd(RelayConn* c);
    void         tokenSessionRemove(RelayConn* c);
    // 用量（relay 线程）：流关闭记一次会话，设备断开冲掉剩余在线时长
    void         usagePushRaw(const char* subject_type, const std::string& subject_id,
                              const char* metric, long long qty, const std::string& meta,
                              const std::string& day);
    void         usageFlushTokenBytes(long long token_id, bool force);
    void         usageOnStreamClose(RelayStream* s);
    void         usageOnAgentClose(RelayConn* c);
    void         usageFlushOnline(const std::string& devid, bool erase);
    std::string  usageLocalDay(long long ts_ms) const;

    // libhv C 回调的薄封装
    static void readCb(hio_t* io, void* buf, int n);
    static void closeCb(hio_t* io);
    static void acceptCb(hio_t* io);
    static void hsTimeoutCb(htimer_t* t);
    static void statsCb(htimer_t* t);
    static void tokenCb(htimer_t* t);
    static void tokenCutCb(htimer_t* t);
    static void drainCb(htimer_t* t);
    static void shutdownCb(hsignal_t* sig);
};
