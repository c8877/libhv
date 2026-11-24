#pragma once
// 管理台账号/会话/设备归属（SQLite）。仅供 admin worker 线程访问，内部加锁。
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>
#include <atomic>

struct sqlite3;
struct sqlite3_stmt;

// SQLite 列读取（定义在 admin_db.cpp）：NULL 文本按空串处理。
std::string dbColText(sqlite3_stmt* st, int col);
long long   dbColInt(sqlite3_stmt* st, int col);

// 端口范围包含判断：target 的每个端口都必须被 allowed 覆盖（"*" 覆盖全部）。
bool adminPortsCovered(const std::string& allowed, const std::string& target);

// 全局资源键（relay/dict/权限组/账号/令牌/证书/审计/设置/仓库）：
// 不受设备范围约束，只有 all_devices 或 super 才能生效和授予。
bool permIsGlobal(const std::string& key);

struct DbGroupAclRule {
    std::string user;
    std::string devid;
    std::string ports;
    std::string tty;        // 可读的串口（* = 全部）
    bool tty_write = false; // 是否允许写串口 / 强制接管
    bool shell = false;
    int  file = 0;      // 0 无 / 1 只读 / 2 读写
    bool svc = false;
    long long token_id = 0;  // 0 = 账号级；>0 = 仅该 rtk 令牌
};

// 一次请求的有效授权：功能资源 + 设备范围
struct DbPermScope {
    bool super = false;         // 超级组：全部资源、全部设备
    bool all_devices = false;   // 组勾了「全部设备」
    bool global_scope = false;  // 组勾了「全局权限」：全局资源键（仓库/设置/审计等）生效
    bool has_groups = false;    // 是否加入过权限组
    bool token_groups = false;   // 令牌自身是否配置了权限组（true 时取交集）
    std::set<std::string> resources;
    std::set<std::string> devices;
    std::string ports;           // 组级端口范围（逗号分隔，空 = 无端口权限）
};

// a 的权限是否覆盖 b（资源/设备/端口都不小于 b）；用于判断变更是否收窄
bool dbPermCovers(const DbPermScope& a, const DbPermScope& b);

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

// SQLite 语句执行统计（sqlite3_profile 回调），供 /metrics 使用
struct DbStats {
    unsigned long long queries = 0;
    unsigned long long total_us = 0;
    unsigned long long max_us = 0;
    unsigned long long slow_queries = 0;   // >= 100ms
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
                    const std::string& actor, const std::string& kind,
                    const std::string& version, long long* id);
    bool distUpdate(long long id, const std::string& status, const std::string& error,
                    long long sent, long long relay_tid, long long finished_ms,
                    long long next_retry_ms, int retry_count);
    bool distProgress(long long id, long long sent);
    bool distExtend(long long id, long long deadline_ms);
    // devids 为空表示不按设备过滤；受限账号必须传入自己的设备集
    bool distList(const std::string& status, const std::string& q, const std::string& batch,
                  const std::string& kind, const std::vector<std::string>& devids,
                  const std::string& sort, int dir,
                  int page, int per_page, std::string& json_out, int& total);
    bool distGet(long long id, std::string& json_out);
    bool distDue(std::string& json_out);   // status/start/deadline/retry 字段列表
    bool distSummary(const std::vector<std::string>& devids, std::string& json_out);
    bool fileMetaGet(const std::string& name, long long size, long long mtime_ms, std::string& sha);
    bool fileMetaPut(const std::string& name, long long size, long long mtime_ms, const std::string& sha);
    bool distAlreadyPushed(const std::string& devid, const std::string& sha);
    bool distSetPhase(long long id, const std::string& status, const std::string& error,
                      long long sent, long long relay_tid, long long started_ms);
    bool distOtaActive(const std::string& devid);
    bool distOtaSucceeded(const std::string& devid, const std::string& version);
    bool distRecover();   // relay 重启后把中断的传输/安装阶段重新入队

    // ---- 资源权限：资源字典 + 权限组 ----
    bool permSeed();                        // 幂等：补资源、预置权限组
    // 数据面规则：账号规则 + 令牌级收窄规则
    bool permClientRules(std::vector<DbGroupAclRule>& out, std::set<std::string>& users,
                         std::set<long long>& scoped_tokens);
    bool permEffectiveClient(const std::string& user, std::string& json_out, std::string& err);
    bool permResources(std::string& json_out);
    bool permResourceUpdate(const std::string& key, const std::string& name,
                            const std::string& desc, int enabled, std::string& err);
    bool permGroups(std::string& json_out);
    bool permGroupDetail(long long id, std::string& json_out, std::string& err);
    // 组的授权面（不含成员），用于服务端校验「只能授予自己权限的子集」；
    // 组不存在返回 false。
    bool permGroupScope(long long id, DbPermScope& out);
    // 角色对应内置组的授权面（角色只是内置组的别名）
    bool permRoleScope(const std::string& role, DbPermScope& out);
    // 组的 builtin_key（非内置组返回空），用于保护内置角色组
    bool permGroupBuiltin(long long id, std::string& key);
    // resources/devices/all_devices/ports 任一为空表示不改；members 为空表示不改成员
    bool permGroupSaveAll(long long id, const std::string& name, const std::string& desc,
                          const std::vector<std::string>& resources,
                          const std::vector<std::string>& devices,
                          int all_devices, int global_scope, const std::string& ports, bool set_members,
                          const std::vector<std::pair<std::string, std::string>>& members,
                          long long& out_id, std::string& err);
    bool permGroupDelete(long long id, std::string& err);
    bool permGroupMembers(long long id, std::string& json_out, std::string& err);
    // 只列组内实际成员（type,id），用于管辖范围校验与撤权踢线
    bool permGroupMemberList(long long id, std::vector<std::pair<std::string, std::string>>& out);
    bool permGroupSetMembers(long long id,
                             const std::vector<std::pair<std::string, std::string>>& members,
                             std::string& err);
    bool permGroupsOfUser(long long user_id, std::vector<long long>& ids);
    bool permGroupsOfSubject(const std::string& type, const std::string& id, std::vector<long long>& ids);
    bool permSetSubjectGroups(const std::string& type, const std::string& id,
                              const std::vector<long long>& ids, std::string& err);
    // 接入账号（subject_type='client'）的权限组
    bool permGroupsOfClient(long long account_id, std::vector<long long>& ids);
    bool permSetClientGroups(long long account_id, const std::vector<long long>& ids, std::string& err);
    bool clientPermsOf(long long account_id, std::set<std::string>& out, bool& super);
    bool permSetUserGroups(long long user_id, const std::vector<long long>& ids, std::string& err);
    // 有效授权：账号只由组成员关系求值，角色不参与
    bool permScopeOfUser(long long user_id, DbPermScope& out);
    // 令牌：归属主体授权 ∩ 令牌组授权（令牌组只能收窄）
    bool permScopeOfAdminToken(long long token_id, long long user_id, DbPermScope& out);
    bool permScopeOfClientToken(long long token_id, long long account_id, DbPermScope& out);
    bool permScopeOfClient(long long account_id, DbPermScope& out);
    // 字典/授权对象页面用
    bool permResourcesPage(const std::string& q, int page, int page_size, std::string& json_out);
    bool resDevicesPage(const std::string& q, int page, int page_size, std::string& json_out);
    bool resDeviceUpdate(const std::string& devid, const std::string& name, int enabled, std::string& err);
    bool resDeviceSync(const std::string& devid, const std::string& name, const std::string& version,
                       int online, long long seen_ms);
    bool resDevicesMarkOffline(const std::vector<std::string>& online);
    bool permSubjects(std::string& json_out);
    bool permMembersOfGroup(long long gid, std::string& json_out);
    std::string permMembersOfGroupLocked(long long gid);

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

    // 管理台账号 API Token（atk-）
    bool adminTokenList(long long user_id, const std::string& q, std::string& json_out);
    bool adminTokenCreate(long long user_id, const std::string& username, const DbTokenSpec& spec,
                          const std::string& by, int max_per_user, std::string& plain_out,
                          std::string& json_out, std::string& err);
    bool adminTokenSpec(long long id, DbTokenSpec& spec, long long& user_id, std::string& username,
                        std::string* status, std::string& err);
    bool adminTokenUpdate(long long id, const DbTokenSpec& spec, const std::string& status,
                          std::string& err);
    bool adminTokenRotate(long long id, std::string& plain_out, std::string& json_out,
                          std::string& err);
    bool adminTokenDelete(long long id, std::string& err);
    // Bearer atk- 鉴权：校验 Token + 归属账号状态，更新最近使用
    bool adminTokenAuth(const std::string& hash, long long now_ms, const std::string& ip,
                        DbUser& user, long long& token_id, std::string& err);
    // Bearer rtk_ 鉴权：接入账号 Token，返回账号与 Token id
    bool clientTokenAuth(const std::string& hash, long long now_ms, const std::string& ip,
                         long long& account_id, std::string& username, long long& token_id,
                         std::string& err);
    bool dictTtys(const std::string& q, std::string& json_out);
    // 字典分页：kind = ports | ttys
    bool dictPage(const std::string& kind, const std::string& q, int page, int page_size,
                  std::string& json_out);
    bool dictTtyAdd(const std::string& pattern, const std::string& name, const std::string& desc,
                    long long& out_id, std::string& err);
    bool dictBatchAdd(const std::string& kind, const std::vector<std::string>& values,
                      const std::string& name_prefix, std::string& json_out, std::string& err);
    bool dictUpdate(const std::string& kind, long long id, const std::string& name,
                    const std::string& desc, int enabled, std::string& err);
    bool dictDelete(const std::string& kind, long long id, std::string& err);
    bool dictBatchDelete(const std::string& kind, const std::vector<long long>& ids,
                         std::string& json_out);

    // ---- relay 地址字典与下发列表 ----
    bool relayDictPage(const std::string& q, int page, int page_size, std::string& json_out, int& total);
    bool relayDictAdd(const std::string& host, int port, const std::string& name,
                      const std::string& desc, long long& out_id, std::string& err);
    bool relayDictUpdate(long long id, const std::string& host, int port, const std::string& name,
                         const std::string& desc, int enabled, std::string& err);
    bool relayDictDelete(long long id, std::string& err);   // 被下发列表引用时拒绝
    // 有序下发列表：[{position,endpoint_id,host,port,name,enabled}]
    bool relayListJson(std::string& json_out);
    bool relayListSet(const std::vector<long long>& ids, const std::string& by, std::string& err);
    // 同步任务（任务级 meta；逐设备行复用 distribution_tasks，kind='relay'）
    bool relayTaskCreate(const std::string& batch, const std::string& name,
                         const std::string& list_json, long long list_rev, int allow_leave,
                         const std::string& mode, int wave_size, int wave_interval_s,
                         int fail_threshold_pct, const std::vector<std::string>& devids,
                         long long start_ms, long long deadline_ms, int max_retries,
                         const std::string& actor, std::string& err);
    bool relayTaskActive(std::string& json_out);   // scheduled/running 的任务
    bool relayTaskList(const std::string& status, const std::string& q, int page, int page_size,
                       const std::vector<std::string>& devids, bool scoped, std::string& json_out,
                       int& total);
    bool relayTaskGet(const std::string& batch, std::string& json_out, std::string& err);
    bool relayTaskItems(const std::string& batch, std::string& json_out);
    bool relayTaskDue(std::string& json_out);   // 待执行的逐设备行
    bool relayTaskPayload(const std::string& batch, std::string& json_out, long long& rev);
    bool relayTaskSetStatus(const std::string& batch, const std::string& status, long long finished_ms);
    bool relayTaskPauseStale(long long rev, int& n);   // 列表已变更：暂停快照过期的任务
    bool relayTaskSetWave(const std::string& batch, int wave, long long wave_done_ms);
    bool relayTaskReleaseWave(const std::string& batch, int wave, long long now_ms, std::string& err);
    bool relayTaskWaveStats(const std::string& batch, int wave, int& success, int& failed,
                            int& timeout, int& pending, int& active, int& total);
    bool relayTaskOverall(const std::string& batch, int& success, int& failed, int& timeout,
                          int& pending, int& active, int& total);
    bool relayTaskCancelPending(const std::string& batch, int& n);
    bool relayTaskRetryFailed(const std::string& batch, int& n, int& min_wave);
    bool relayItemMarkRunning(long long id, long long started_ms);




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

    bool deviceList(std::vector<std::string>& out);

    // 工具
    static std::string hashPassword(const std::string& pass, std::string& err);
    static bool verifyPassword(const std::string& pass, const std::string& stored);
    static std::string randomHex(int bytes);
    static std::string sha256Hex(const std::string& data);
    static long long nowMs();

    // sqlite3_profile 回调入口 + 快照
    void recordQuery(unsigned long long ns);
    void stats(DbStats& out) const;

private:
    bool exec(const char* sql, std::string& err);
    bool migrate(std::string& err);

    sqlite3*   db_ = nullptr;
    std::mutex mu_;
    std::string path_;
    std::atomic<unsigned long long> q_total_{0};
    std::atomic<unsigned long long> q_us_{0};
    std::atomic<unsigned long long> q_max_us_{0};
    std::atomic<unsigned long long> q_slow_{0};
};
