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

struct RelayStream {
    uint32_t     a_sid = 0;
    uint32_t     c_sid = 0;
    RelayConn*   a = nullptr;
    RelayConn*   c = nullptr;
    int          a_closed = 0;
    int          c_closed = 0;
    int          opened = 0;             // OPEN_OK 经过 relay 时置位
    uint64_t     opened_at_ms = 0;
    RelayStream* next_a = nullptr;   // a->streams
    RelayStream* next_c = nullptr;   // c->streams
};

struct RelayConn {
    RelayServer* server = nullptr;
    hio_t*       io = nullptr;
    int          role = 0;           // rtnp::ROLE_*
    int          authed = 0;
    int          internal = 0;   // 管理台内部链路（终端/代理），不在客户端列表展示
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
    char         token_prefix[24] = {0};
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
    char     last_ip[64] = {0};
};

struct AclEntry {
    char      user[128] = {0};
    char      pattern[128] = {0};
    AclEntry* next = nullptr;
};

struct GroupAclRule {
    std::string user;
    std::string devid;
    std::string ports;
    std::string tty;
    bool        shell = false;
};

class RelayServer {
    friend class AdminServer;

public:
    int run(int argc, char** argv);
    void setGroupAcl(std::vector<GroupAclRule> rules, std::set<std::string> users);
    // 认证策略全量替换（relay 线程）；JSON 由 admin 的 clientAuthPolicies 生成
    void setAuthPolicies(const std::string& json);
    // 用量快照（relay 线程），JSON 数组
    std::string tokenUsageSnapshot();

    hloop_t*    loop = nullptr;
    void*       admin = nullptr;      // AdminServer*（--admin 启用时创建）
    uint64_t    started_at_ms = 0;
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
    std::unordered_map<uint32_t, FilePush> pushes;
    std::unordered_map<uint32_t, std::string> push_results;
    uint32_t push_seq = 0;
    std::unordered_map<uint32_t, std::string> check_results;
    uint32_t check_seq = 0;
    std::unordered_map<uint32_t, std::string> tty_list_results;
    std::unordered_map<uint32_t, std::string> metric_data_results;
    std::unordered_map<std::string, std::string> metric_status;
    uint32_t metric_query_seq = 0;
    uint32_t tty_list_seq = 0;
    struct ClientCred { std::string user, token; };
    std::vector<ClientCred> client_creds;   // --client-users（user:token）
    std::string client_token;               // --client-token（全局 token）
    std::vector<std::string> shell_users;   // --shell-users（允许 OPEN_SHELL 的用户）
    int         allow_all = 0;
    int         require_mtls = 0;
    int         verbose = 0;
    AclEntry*   acl_cli = nullptr;    // --acl 命令行条目（固定）
    AclEntry*   acl_file = nullptr;   // --acl-file 文件条目（SIGHUP 热加载）
    std::vector<GroupAclRule> acl_groups;       // 管理台分组授权（relay 线程只读）
    std::set<std::string>     acl_grouped_users;
    std::unordered_map<std::string, TokenPolicy> token_cache;  // sha256 hex -> policy
    std::unordered_map<std::string, int> account_active;       // username -> 1/0
    std::unordered_map<long long, TokenUsage>    token_usage;  // token id -> usage
    std::set<long long> pending_token_cuts;       // 流量到顶待断开（帧处理内不能直接 close）
    char        acl_file_path[256] = {0};
    PendingOpen* pending = nullptr;   // 等待 A 侧 agent 上线的 OPEN_SHELL
    hio_t*      listenio = nullptr;
    int         draining = 0;
    uint64_t    drain_deadline = 0;
    uint32_t    drain_timeout_ms = 10000;

private:
    int          aclCheck(const char* user, const char* devid);
    bool         groupAclDevice(const char* user, const char* devid);
    bool         groupAclCheck(const char* user, const char* devid, uint16_t port,
                               int is_shell, const char* tty_path);
    bool         clientAuthEnabled() const { return !client_creds.empty() || !client_token.empty(); }
    bool         shellAllowed(const char* user) const;
    RelayConn*   findClientByUser(const char* user);
    void         addPending(RelayConn* a, uint32_t a_sid, const char* devid, uint16_t port, uint16_t flags);
    void         completePending(const char* devid);
    void         sweepPending();
    void         dropPendingFor(RelayConn* a);
    void         aclAddTo(AclEntry** head, const char* user, const char* patterns);
    void         aclFree(AclEntry** head);
    int          aclFileLoad();
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
    bool         isRevoked(const std::string& san) const;
    bool         revokeSan(const std::string& san);   // 写黑名单 + 踢在线连接
    void         loadRevoked();                       // 启动时读 revoked.txt
    bool         sendSysinfoReq(const char* devid, int mode = 0);   // 要求 agent 立即上报
    std::string  sysinfoOf(const char* devid, uint64_t* ts);
    std::string  sysinfoLastOf(const char* devid, bool* full, uint64_t* ts);
    bool         sendConfig(const char* devid, uint32_t sid, const std::string& json);
    std::string  configAck(const char* devid, uint32_t sid, uint64_t* ts);
    bool         startFilePush(const char* devid, const char* path, const char* dir,
                               const char* name, uint64_t size, const std::string& sha,
                               std::string& err, uint32_t& tid);
    std::string  pushProgress(uint32_t tid);
    bool         cancelFilePush(uint32_t tid);
    bool         startFileCheck(const char* devid, const char* dir, const char* name,
                                uint32_t& cid, std::string& err);
    std::string  takeCheckResult(uint32_t cid);
    bool         startTtyList(const char* devid, uint32_t& rid, std::string& err);
    std::string  takeTtyListResult(uint32_t rid);
    bool         pushTtyCfg(const char* devid, const std::string& json);
    bool         pushMetricCfg(const char* devid, const std::string& json);
    bool         startMetricQuery(const char* devid, const std::string& json, uint32_t& rid, std::string& err);
    std::string  takeMetricData(uint32_t rid);
    std::string  takeMetricStatus(const std::string& devid);
    bool         hasAgent(const char* devid);
    bool         agentOnline(const char* devid) { return findAgent(devid) != nullptr; }
    std::string  fileDirOf(const char* devid);
    void         onFileAck(RelayConn* c, const rtnp::Frame* f);
    void         onHsTimeout(RelayConn* c);
    void         handleFrame(RelayConn* c, rtnp::Frame* f);
    void         onRead(RelayConn* c, void* buf, int readbytes);
    void         onClose(RelayConn* c);
    void         onAccept(hio_t* io);
    void         onSighup();
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

    // libhv C 回调的薄封装
    static void readCb(hio_t* io, void* buf, int n);
    static void closeCb(hio_t* io);
    static void acceptCb(hio_t* io);
    static void hsTimeoutCb(htimer_t* t);
    static void statsCb(htimer_t* t);
    static void tokenCb(htimer_t* t);
    static void tokenCutCb(htimer_t* t);
    static void drainCb(htimer_t* t);
    static void sighupCb(hsignal_t* sig);
    static void shutdownCb(hsignal_t* sig);
};
