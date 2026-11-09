#pragma once
// 管理台账号/会话/设备归属（SQLite）。仅供 admin worker 线程访问，内部加锁。
#include <string>
#include <vector>
#include <map>
#include <string>
#include <mutex>

struct sqlite3;

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

    // 设备分组
    bool groupList(std::string& json_out);
    bool groupGet(long long id, std::string& json_out, std::string& err);
    bool groupCreate(const std::string& name, const std::string& desc,
                     long long& out_id, std::string& err);
    bool groupUpdate(long long id, const std::string& name, const std::string& desc,
                     std::string& err);
    bool groupDelete(long long id, std::string& err);
    bool groupAddDevice(long long id, const std::string& devid, std::string& err);
    bool groupRemoveDevice(long long id, const std::string& devid);
    bool groupSetCaps(long long id, const std::map<std::string, std::string>& caps,
                      std::string& err);
    bool groupAddSubject(long long id, const std::string& type, const std::string& subject,
                         std::string& err);
    bool groupRemoveSubject(long long id, const std::string& type, const std::string& subject);
    bool groupDeviceMap(std::string& json_out);
    bool groupEffective(const std::string& type, const std::string& subject,
                        std::string& json_out, std::string& err);    bool groupSubjectDevids(const std::string& type, const std::string& subject,
                            std::vector<std::string>& out, bool& has_groups);


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
