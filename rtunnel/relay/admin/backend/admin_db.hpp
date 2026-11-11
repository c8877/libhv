#pragma once
// 管理台账号/会话/设备归属（SQLite）。仅供 admin worker 线程访问，内部加锁。
#include <string>
#include <vector>
#include <map>
#include <set>
#include <string>
#include <mutex>

struct sqlite3;

struct DbPermAcl {
    long long agent_group_id = 0;
    std::string tcp_ports;
    std::string tty;
    std::string file = "none";
    bool shell = false;
    bool sysinfo = false;
    bool metrics = false;
    bool config = false;
    bool web = false;
};

struct DbGroupAclRule {
    std::string user;
    std::string devid;
    std::string ports;
    std::string tty;
    bool shell = false;
};

// Token 的可配置限额（创建与更新共用）
struct DbTokenSpec {
    std::string name;
    long long   expires_at_ms = 0;      // 0 = 永久
    long long   quota_bytes = 0;        // 0 = 不限
    std::string quota_period = "monthly";
    int         rate_per_min = 0;
    int         burst = 0;
    int         max_sessions = 0;
    std::string quota_action = "cut";
};

struct DbUser {
    long long   id = 0;
    std::string username;
    std::string role;        // superadmin / admin / user
    std::string status;      // active / disabled
    long long   created_ms = 0;
};

class AdminDb {
public:
    ~AdminDb();

    bool open(const std::string& path, std::string& err);
    bool ok() const { return db_ != nullptr; }

    bool getSettings(std::map<std::string, std::string>& out);
    bool setSetting(const std::string& key, const std::string& value, const std::string& by);
    bool getUserPrefs(long long uid, std::map<std::string, std::string>& out);
    bool setUserPref(long long uid, const std::string& key, const std::string& value);
    bool initialized();                       // users 非空
    bool createUser(const std::string& user, const std::string& pass,
                    const std::string& role, std::string& err, long long* out_id = nullptr);
    bool findUser(const std::string& name, DbUser& out, std::string* pass_hash = nullptr);
    bool findUserById(long long id, DbUser& out);
    bool listUsers(std::vector<DbUser>& out, const std::string& only_role = "");
    bool countUsers(int& n);
    bool setPassword(long long id, const std::string& pass, std::string& err);
    bool setStatus(long long id, const std::string& status);
    bool setRole(long long id, const std::string& role);
    bool removeUser(long long id);

    bool createSession(const std::string& sid_hash, long long user_id, long long expire_ms);
    bool getSession(const std::string& sid_hash, DbUser& out, long long now_ms);
    bool dropSession(const std::string& sid_hash);
    bool dropUserSessions(long long user_id);
    bool purgeExpiredSessions(long long now_ms);
    bool distInsert(const std::string& batch, const std::string& devid, const std::string& file,
                    long long size, const std::string& sha, const std::string& sub,
                    const std::string& target, const std::string& status,
                    long long start_ms, long long deadline_ms, int max_retries,
                    const std::string& actor, long long* id);
    bool distUpdate(long long id, const std::string& status, const std::string& error,
                    long long sent, long long relay_tid, long long finished_ms,
                    long long next_retry_ms, int retry_count);
    bool distProgress(long long id, long long sent);
    bool distExtend(long long id, long long deadline_ms);
    bool distList(const std::string& status, const std::string& q, const std::string& batch,
                  int page, int per_page, std::string& json_out, int& total);
    bool distGet(long long id, std::string& json_out);
    bool distDue(std::string& json_out);   // status/start/deadline/retry 字段列表
    bool distSummary(std::string& json_out);
    bool fileMetaGet(const std::string& name, long long size, long long mtime_ms, std::string& sha);
    bool fileMetaPut(const std::string& name, long long size, long long mtime_ms, const std::string& sha);
    bool distAlreadyPushed(const std::string& devid, const std::string& sha);

    // 设备分组映射与账号范围
    bool groupGet(long long id, std::string& json_out, std::string& err);
    bool groupDelete(long long id, std::string& err);
    bool groupDeviceMap(std::string& json_out);
    bool groupSubjectDevids(const std::string& type, const std::string& subject,
                            std::vector<std::string>& out, bool& has_groups);
    bool userGroupList(const std::string& username, std::string& json_out);
    bool userSetGroups(const std::string& username, const std::vector<long long>& ids, std::string& err);
    bool permOverview(std::string& json_out);
    bool permClientGet(long long id, std::string& json_out, std::string& err);
    bool permClientSave(long long id, const std::string& name, const std::string& desc,
                        const std::vector<std::string>& users,
                        const std::vector<DbPermAcl>& acl, long long& out_id, std::string& err);
    bool permClientDelete(long long id, std::string& err);
    bool permAgentGet(long long id, std::string& json_out, std::string& err);
    bool permAgentSave(long long id, const std::string& name, const std::string& desc,
                       const std::vector<std::string>& devices,
                       const std::map<std::string, std::string>& caps,
                       long long& out_id, std::string& err);
    bool permAgentDelete(long long id, std::string& err);
    bool permEffectiveClient(const std::string& user, std::string& json_out, std::string& err);
    bool permImportAcl(const std::string& content, const std::vector<std::string>& extra_devices,
                       std::string& json_out, std::string& err);
    bool permExportAcl(std::string& json_out);
    bool permMigrateLegacy();
    bool permClientRules(std::vector<DbGroupAclRule>& out, std::set<std::string>& users);
    bool permSyncDictIds();
    bool permAgentCandidates(long long group_id, std::string& json_out);
    bool permAgentAddDevices(long long group_id, const std::vector<std::string>& devids, std::string& err);

    bool dictPorts(const std::string& q, std::string& json_out);
    bool dictPortAdd(const std::string& value, const std::string& name, const std::string& desc,
                     long long& out_id, std::string& err);
    // 接入账号与 Token
    bool clientAccountList(const std::string& q, std::string& json_out);
    bool clientAccountGet(long long id, std::string& json_out, std::string& err);
    bool clientAccountCreate(const std::string& username, const std::string& display_name,
                             const std::string& note, const std::string& by,
                             long long& out_id, std::string& err);
    bool clientAccountUpdate(long long id, const std::string& display_name, const std::string& note,
                             const std::string& status, std::string& err);
    bool clientAccountDelete(long long id, std::string& err);
    bool clientTokenList(long long account_id, std::string& json_out, std::string& err);
    bool clientTokenCreate(long long account_id, const DbTokenSpec& spec, const std::string& by,
                           int max_per_account, std::string& plain_out, std::string& json_out,
                           std::string& err);
    bool clientTokenSpec(long long id, DbTokenSpec& spec, long long& account_id,
                         std::string* status, std::string& err);
    bool clientTokenUpdate(long long id, const DbTokenSpec& spec, const std::string& status,
                           std::string& err);
    bool clientTokenRotate(long long id, std::string& plain_out, std::string& json_out,
                           std::string& err);
    bool clientTokenDelete(long long id, std::string& err);
    // 推给 relay 的认证策略：账号状态 + 全部未吊销 Token
    bool clientAuthPolicies(std::string& json_out);
    // relay 用量快照落库，period_start_ms 作为乐观锁
    bool clientTokenUsageApply(const std::string& json, std::string& err);

    bool dictTtys(const std::string& q, std::string& json_out);
    bool dictTtyAdd(const std::string& pattern, const std::string& name, const std::string& desc,
                    long long& out_id, std::string& err);
    bool dictBatchAdd(const std::string& kind, const std::vector<std::string>& values,
                      const std::string& name_prefix, std::string& json_out, std::string& err);
    bool dictUpdate(const std::string& kind, long long id, const std::string& name,
                    const std::string& desc, int enabled, std::string& err);
    bool dictDelete(const std::string& kind, long long id, std::string& err);
    bool dictBatchDelete(const std::string& kind, const std::vector<long long>& ids,
                         std::string& json_out);




    bool metricSeriesList(const std::string& devid, std::string& json_out);
    bool metricSeriesPut(const std::string& devid, const std::string& rows_json,
                         const std::string& actor, long long& revision, std::string& err);
    bool metricSeriesDevids(std::vector<std::string>& out);
    bool metricSeriesAck(const std::string& devid, long long revision);
    bool ttyList(const std::string& devid, std::string& json_out);
    bool ttyDevids(std::vector<std::string>& out);
    bool ttyUpsert(const std::string& devid, const std::string& path, const std::string& alias,
                   int enabled, int baud, int data_bits, const std::string& parity, int stop_bits,
                   const std::string& roles, const std::string& actor, std::string& err);
    bool ttyDelete(const std::string& devid, const std::string& path, std::string& err);
    bool ttyResolve(const std::string& devid, const std::string& alias_or_path,
                    std::string& path, int& baud, int& data_bits,
                    std::string& parity, int& stop_bits, bool& enabled);

    bool bindDevice(const std::string& devid, long long owner_id);
    bool unbindDevice(const std::string& devid);
    bool deviceOwner(const std::string& devid, long long& owner_id);  // 0=未绑定
    bool ownedDevids(long long owner_id, std::vector<std::string>& out);
    bool deviceList(std::vector<std::pair<std::string, long long>>& out);

    // 工具
    static std::string hashPassword(const std::string& pass, std::string& err);
    static bool verifyPassword(const std::string& pass, const std::string& stored);
    static std::string randomHex(int bytes);
    static std::string sha256Hex(const std::string& data);
    static long long nowMs();

private:
    bool exec(const char* sql, std::string& err);
    bool migrate(std::string& err);

    sqlite3*   db_ = nullptr;
    std::mutex mu_;
    std::string path_;
};
