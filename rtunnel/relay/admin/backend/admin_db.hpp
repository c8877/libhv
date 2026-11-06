#pragma once
// 管理台账号/会话/设备归属（SQLite）。仅供 admin worker 线程访问，内部加锁。
#include <string>
#include <vector>
#include <mutex>

struct sqlite3;

struct DbUser {
    long long   id = 0;
    std::string username;
    std::string role;        // superadmin / admin / user
    std::string status;      // active / disabled
    int         can_shell = 1;
    std::string client_token;
    long long   created_ms = 0;
};

class AdminDb {
public:
    ~AdminDb();

    bool open(const std::string& path, std::string& err);
    bool ok() const { return db_ != nullptr; }

    bool initialized();                       // users 非空
    bool createUser(const std::string& user, const std::string& pass,
                    const std::string& role, std::string& err, long long* out_id = nullptr);
    bool findUser(const std::string& name, DbUser& out, std::string* pass_hash = nullptr);
    bool findUserById(long long id, DbUser& out);
    bool listUsers(std::vector<DbUser>& out, const std::string& only_role = "");
    bool countUsers(int& n);
    bool setPassword(long long id, const std::string& pass, std::string& err);
    bool setStatus(long long id, const std::string& status);
    bool setCanShell(long long id, int can);
    bool setClientToken(long long id, const std::string& token);
    bool removeUser(long long id);

    bool createSession(const std::string& sid_hash, long long user_id, long long expire_ms);
    bool getSession(const std::string& sid_hash, DbUser& out, long long now_ms);
    bool dropSession(const std::string& sid_hash);
    bool dropUserSessions(long long user_id);

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
