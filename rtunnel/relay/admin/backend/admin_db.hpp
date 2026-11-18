#pragma once
// 管理台账号/会话/设备归属（SQLite）。仅供 admin worker 线程访问，内部加锁。
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>

struct sqlite3;
struct sqlite3_stmt;

// SQLite 列读取（定义在 admin_db.cpp）：NULL 文本按空串处理。
std::string dbColText(sqlite3_stmt* st, int col);
long long   dbColInt(sqlite3_stmt* st, int col);

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
    int  file = 0;      // 0 无 / 1 只读 / 2 读写
    bool svc = false;
    long long token_id = 0;  // 0 = 账号级；>0 = 仅该 rtk 令牌
};

// 一次请求的有效授权：功能资源 + 设备范围
struct DbPermScope {
    bool super = false;         // 超级组：全部资源、全部设备
    bool all_devices = false;   // 组勾了「全部设备」
    bool has_groups = false;    // 是否加入过权限组
    bool explicit_perm = false; // 是否被显式分配过（false 才按 role 回落）
    bool token_groups = false;   // 令牌自身是否配置了权限组（true 时取交集）
    std::set<std::string> resources;
    std::set<std::string> devices;
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
    bool permClientRules(std::vector<DbGroupAclRule>& out, std::set<std::string>& users,
                         std::set<long long>& scoped_tokens);
    bool permSyncDictIds();
    bool permAgentCandidates(long long group_id, std::string& json_out);
    bool permAgentAddDevices(long long group_id, const std::vector<std::string>& devids, std::string& err);

    // ---- 资源权限：资源字典 + 权限组 ----
    bool permSeed();                        // 幂等：补资源、预置权限组
    bool permMigrateGroups();               // 一次性：旧客户端分组/设备分组 -> 权限组
    bool permResources(std::string& json_out);
    bool permResourceUpdate(const std::string& key, const std::string& name,
                            const std::string& desc, int enabled, std::string& err);
    bool permGroups(std::string& json_out);
    bool permGroupDetail(long long id, std::string& json_out, std::string& err);
    // resources/devices/all_devices/ports 任一为空表示不改；members 为空表示不改成员
    bool permGroupSave(long long id, const std::string& name, const std::string& desc,
                       const std::vector<std::string>& resources,
                       const std::vector<std::string>& devices,
                       int all_devices, const std::string& ports,
                       long long& out_id, std::string& err);
    bool permGroupSaveAll(long long id, const std::string& name, const std::string& desc,
                          const std::vector<std::string>& resources,
                          const std::vector<std::string>& devices,
                          int all_devices, const std::string& ports, bool set_members,
                          const std::vector<std::pair<std::string, std::string>>& members,
                          long long& out_id, std::string& err);
    bool permGroupDelete(long long id, std::string& err);
    bool permGroupMembers(long long id, std::string& json_out, std::string& err);
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
    bool permSetUserGroups(long long user_id, const std::string& role,
                           const std::vector<long long>& ids, std::string& err);
    // 有效授权：账号（role 用于未显式分配时的回落）
    bool permsOf(long long user_id, const std::string& role,
                 std::set<std::string>& out, bool& super);
    bool permScopeOfUser(long long user_id, const std::string& role, DbPermScope& out);
    // 令牌：归属主体授权 ∩ 令牌组授权（令牌组只能收窄）
    bool permScopeOfAdminToken(long long token_id, long long user_id, const std::string& role,
                               DbPermScope& out);
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
    // 令牌绑定的设备分组（复用 subject_groups，subject_type='atk'）
    bool adminTokenGroups(long long token_id, std::string& json_out);
    bool adminTokenSetGroups(long long token_id, const std::vector<long long>& ids,
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
