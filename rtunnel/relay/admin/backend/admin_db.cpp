#include "admin_db.hpp"
#include "cpputil/json.hpp"
#include "common/logging.hpp"

#include "sqlite3.h"

#include <time.h>
#include <string.h>
#include <stdio.h>

#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#include "openssl/rand.h"
#include "openssl/sha.h"
#endif

namespace {

std::string hexEncode(const unsigned char* p, size_t n) {
    static const char* h = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = h[p[i] >> 4];
        s[2 * i + 1] = h[p[i] & 0xF];
    }
    return s;
}

int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hexDecode(const std::string& s, unsigned char* out, size_t n) {
    if (s.size() != n * 2) return false;
    for (size_t i = 0; i < n; ++i) {
        int a = hexVal(s[2 * i]), b = hexVal(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = (unsigned char)((a << 4) | b);
    }
    return true;
}

const int PBKDF2_ITER = 120000;
const int SALT_LEN = 16;
const int KEY_LEN = 32;

} // namespace

AdminDb::~AdminDb() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

long long AdminDb::nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::string AdminDb::randomHex(int bytes) {
#if defined(WITH_OPENSSL)
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), bytes) != 1) return "";
    return hexEncode(buf.data(), buf.size());
#else
    (void)bytes;
    return "";
#endif
}

std::string AdminDb::sha256Hex(const std::string& data) {
#if defined(WITH_OPENSSL)
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)data.data(), data.size(), md);
    return hexEncode(md, sizeof(md));
#else
    (void)data;
    return "";
#endif
}

std::string AdminDb::hashPassword(const std::string& pass, std::string& err) {
#if defined(WITH_OPENSSL)
    unsigned char salt[SALT_LEN], key[KEY_LEN];
    if (RAND_bytes(salt, sizeof(salt)) != 1) { err = "rand failed"; return ""; }
    if (PKCS5_PBKDF2_HMAC(pass.data(), (int)pass.size(), salt, sizeof(salt),
                          PBKDF2_ITER, EVP_sha256(), sizeof(key), key) != 1) {
        err = "pbkdf2 failed";
        return "";
    }
    char head[64];
    snprintf(head, sizeof(head), "pbkdf2$%d$", PBKDF2_ITER);
    return std::string(head) + hexEncode(salt, sizeof(salt)) + "$" + hexEncode(key, sizeof(key));
#else
    (void)pass;
    err = "built without OpenSSL";
    return "";
#endif
}

bool AdminDb::verifyPassword(const std::string& pass, const std::string& stored) {
#if defined(WITH_OPENSSL)
    int iter = 0;
    char salt_hex[64] = {0}, key_hex[128] = {0};
    if (sscanf(stored.c_str(), "pbkdf2$%d$%63[0-9a-f]$%127[0-9a-f]", &iter, salt_hex, key_hex) != 3) return false;
    unsigned char salt[SALT_LEN], want[KEY_LEN], got[KEY_LEN];
    if (!hexDecode(salt_hex, salt, sizeof(salt))) return false;
    if (!hexDecode(key_hex, want, sizeof(want))) return false;
    if (PKCS5_PBKDF2_HMAC(pass.data(), (int)pass.size(), salt, sizeof(salt),
                          iter, EVP_sha256(), sizeof(got), got) != 1) return false;
    unsigned char diff = 0;
    for (int i = 0; i < KEY_LEN; ++i) diff |= (unsigned char)(got[i] ^ want[i]);
    return diff == 0;
#else
    (void)pass; (void)stored;
    return false;
#endif
}

bool AdminDb::exec(const char* sql, std::string& err) {
    char* e = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        err = e ? e : "sqlite error";
        if (e) sqlite3_free(e);
        return false;
    }
    return true;
}

bool AdminDb::open(const std::string& path, std::string& err) {
    path_ = path;
    if (sqlite3_open(path.c_str(), &db_) != SQLITE_OK) {
        err = db_ ? sqlite3_errmsg(db_) : "open failed";
        if (db_) { sqlite3_close(db_); db_ = nullptr; }
        return false;
    }
    sqlite3_busy_timeout(db_, 3000);
    std::string e;
    if (!exec("PRAGMA journal_mode=WAL;", e) || !migrate(e)) {
        err = e;
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }
    return true;
}

bool AdminDb::migrate(std::string& err) {
    return exec(
        "CREATE TABLE IF NOT EXISTS users("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " username TEXT NOT NULL UNIQUE,"
        " pass_hash TEXT NOT NULL,"
        " role TEXT NOT NULL,"
        " status TEXT NOT NULL DEFAULT 'active',"
        " created_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sessions("
        " sid_hash TEXT PRIMARY KEY,"
        " user_id INTEGER NOT NULL,"
        " expire_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS devices("
        " devid TEXT PRIMARY KEY,"
        " owner_id INTEGER,"
        " bound_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_dev_owner ON devices(owner_id);"
        "CREATE TABLE IF NOT EXISTS settings("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL DEFAULT '',"
        " updated_ms INTEGER NOT NULL DEFAULT 0,"
        " updated_by TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE IF NOT EXISTS distribution_tasks("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " batch_id TEXT NOT NULL,"
        " devid TEXT NOT NULL,"
        " file_name TEXT NOT NULL,"
        " file_size INTEGER NOT NULL,"
        " file_sha TEXT NOT NULL DEFAULT '',"
        " sub_dir TEXT NOT NULL DEFAULT '',"
        " target_path TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'scheduled',"
        " start_at_ms INTEGER NOT NULL DEFAULT 0,"
        " deadline_at_ms INTEGER NOT NULL DEFAULT 0,"
        " retry_count INTEGER NOT NULL DEFAULT 0,"
        " max_retries INTEGER NOT NULL DEFAULT 3,"
        " next_retry_ms INTEGER NOT NULL DEFAULT 0,"
        " extend_count INTEGER NOT NULL DEFAULT 0,"
        " sent INTEGER NOT NULL DEFAULT 0,"
        " error TEXT NOT NULL DEFAULT '',"
        " relay_tid INTEGER NOT NULL DEFAULT 0,"
        " actor TEXT NOT NULL DEFAULT '',"
        " created_ms INTEGER NOT NULL DEFAULT 0,"
        " started_ms INTEGER NOT NULL DEFAULT 0,"
        " finished_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_dist_due ON distribution_tasks(status, start_at_ms);"
        "CREATE INDEX IF NOT EXISTS idx_dist_dead ON distribution_tasks(status, deadline_at_ms);"
        "CREATE TABLE IF NOT EXISTS file_meta("
        " name TEXT PRIMARY KEY,"
        " size INTEGER NOT NULL DEFAULT 0,"
        " mtime_ms INTEGER NOT NULL DEFAULT 0,"
        " sha256 TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE IF NOT EXISTS user_prefs("
        " user_id INTEGER NOT NULL,"
        " key TEXT NOT NULL,"
        " value TEXT NOT NULL DEFAULT '',"
        " PRIMARY KEY(user_id,key));"
        "UPDATE users SET role='admin' WHERE role='superadmin';"
        "UPDATE users SET role='viewer' WHERE role='user';",
        err);
}

bool AdminDb::getSettings(std::map<std::string, std::string>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT key,value FROM settings", -1, &st, nullptr) != SQLITE_OK) return false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        out[(const char*)sqlite3_column_text(st, 0)] = (const char*)sqlite3_column_text(st, 1);
    }
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::setSetting(const std::string& key, const std::string& value, const std::string& by) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO settings(key,value,updated_ms,updated_by) VALUES(?,?,?,?) "
                      "ON CONFLICT(key) DO UPDATE SET value=excluded.value,"
                      " updated_ms=excluded.updated_ms, updated_by=excluded.updated_by";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, nowMs());
    sqlite3_bind_text(st, 4, by.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::getUserPrefs(long long uid, std::map<std::string, std::string>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT key,value FROM user_prefs WHERE user_id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, uid);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out[(const char*)sqlite3_column_text(st, 0)] = (const char*)sqlite3_column_text(st, 1);
    }
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::setUserPref(long long uid, const std::string& key, const std::string& value) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO user_prefs(user_id,key,value) VALUES(?,?,?) "
                      "ON CONFLICT(user_id,key) DO UPDATE SET value=excluded.value";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, uid);
    sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, value.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::initialized() {
    int n = 0;
    return countUsers(n) && n > 0;
}

bool AdminDb::countUsers(int& n) {
    std::lock_guard<std::mutex> lk(mu_);
    n = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM users", -1, &st, nullptr) != SQLITE_OK) return false;
    if (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return true;
}

static void fillUser(sqlite3_stmt* st, DbUser& u) {
    u.id = sqlite3_column_int64(st, 0);
    u.username = (const char*)sqlite3_column_text(st, 1);
    u.role = (const char*)sqlite3_column_text(st, 2);
    u.status = (const char*)sqlite3_column_text(st, 3);
    u.created_ms = sqlite3_column_int64(st, 4);
}

bool AdminDb::createUser(const std::string& user, const std::string& pass,
                         const std::string& role, std::string& err, long long* out_id) {
    if (user.empty() || pass.size() < 6) { err = "username empty or password < 8 chars"; return false; }
    if (role != "admin" && role != "manager" && role != "operator" && role != "viewer") {
        err = "bad role";
        return false;
    }
    {
        int n = 0;
        if (!countUsers(n)) { err = "db error"; return false; }
        if (role == "admin" && n > 0) { err = "admin already exists"; return false; }
    }
    std::string hash = hashPassword(pass, err);
    if (hash.empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO users(username,pass_hash,role,status,created_ms)"
                      " VALUES(?,?,?,'active',?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) { err = sqlite3_errmsg(db_); return false; }
    sqlite3_bind_text(st, 1, user.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, nowMs());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) { err = (rc == SQLITE_CONSTRAINT) ? "username exists" : sqlite3_errmsg(db_); return false; }
    if (out_id) *out_id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::findUser(const std::string& name, DbUser& out, std::string* pass_hash) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,username,role,status,created_ms,pass_hash"
                                " FROM users WHERE username=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) {
        fillUser(st, out);
        if (pass_hash) *pass_hash = (const char*)sqlite3_column_text(st, 5);
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::findUserById(long long id, DbUser& out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,username,role,status,created_ms"
                                " FROM users WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) fillUser(st, out);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::listUsers(std::vector<DbUser>& out, const std::string& only_role) {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    sqlite3_stmt* st = nullptr;
    const char* sql = only_role.empty()
        ? "SELECT id,username,role,status,created_ms FROM users ORDER BY id"
        : "SELECT id,username,role,status,created_ms FROM users WHERE role=? ORDER BY id";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    if (!only_role.empty()) sqlite3_bind_text(st, 1, only_role.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        DbUser u; fillUser(st, u); out.push_back(u);
    }
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::setPassword(long long id, const std::string& pass, std::string& err) {
    if (pass.size() < 6) { err = "password < 8 chars"; return false; }
    std::string hash = hashPassword(pass, err);
    if (hash.empty()) return false;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE users SET pass_hash=? WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_); return false;
    }
    sqlite3_bind_text(st, 1, hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::setStatus(long long id, const std::string& status) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE users SET status=? WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::setRole(long long id, const std::string& role) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE users SET role=? WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::removeUser(long long id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM users WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return false;
    sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE user_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    sqlite3_prepare_v2(db_, "UPDATE devices SET owner_id=NULL,bound_ms=0 WHERE owner_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::createSession(const std::string& sid_hash, long long user_id, long long expire_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT OR REPLACE INTO sessions(sid_hash,user_id,expire_ms) VALUES(?,?,?)",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, sid_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, user_id);
    sqlite3_bind_int64(st, 3, expire_ms);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::getSession(const std::string& sid_hash, DbUser& out, long long now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT u.id,u.username,u.role,u.status,u.created_ms"
                      " FROM sessions s JOIN users u ON u.id=s.user_id"
                      " WHERE s.sid_hash=? AND s.expire_ms>? AND u.status='active'";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, sid_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, now_ms);
    bool ok = sqlite3_step(st) == SQLITE_ROW;
    if (ok) fillUser(st, out);
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::dropSession(const std::string& sid_hash) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE sid_hash=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, sid_hash.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::dropUserSessions(long long user_id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE user_id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, user_id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::purgeExpiredSessions(long long now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE expire_ms <= ?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, now_ms);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

static void fillDistRow(sqlite3_stmt* st, nlohmann::json& j) {
    j["id"] = sqlite3_column_int64(st, 0);
    j["batch_id"] = (const char*)sqlite3_column_text(st, 1);
    j["devid"] = (const char*)sqlite3_column_text(st, 2);
    j["file_name"] = (const char*)sqlite3_column_text(st, 3);
    j["file_size"] = sqlite3_column_int64(st, 4);
    j["sub_dir"] = (const char*)sqlite3_column_text(st, 5);
    j["target_path"] = (const char*)sqlite3_column_text(st, 6);
    j["status"] = (const char*)sqlite3_column_text(st, 7);
    j["start_at_ms"] = sqlite3_column_int64(st, 8);
    j["deadline_at_ms"] = sqlite3_column_int64(st, 9);
    j["retry_count"] = sqlite3_column_int(st, 10);
    j["max_retries"] = sqlite3_column_int(st, 11);
    j["next_retry_ms"] = sqlite3_column_int64(st, 12);
    j["extend_count"] = sqlite3_column_int(st, 13);
    j["sent"] = sqlite3_column_int64(st, 14);
    j["error"] = (const char*)sqlite3_column_text(st, 15);
    j["relay_tid"] = sqlite3_column_int64(st, 16);
    j["actor"] = (const char*)sqlite3_column_text(st, 17);
    j["created_ms"] = sqlite3_column_int64(st, 18);
    j["started_ms"] = sqlite3_column_int64(st, 19);
    j["finished_ms"] = sqlite3_column_int64(st, 20);
    j["file_sha"] = (const char*)sqlite3_column_text(st, 21);
}

bool AdminDb::distInsert(const std::string& batch, const std::string& devid, const std::string& file,
                         long long size, const std::string& sha, const std::string& sub,
                         const std::string& target, const std::string& status,
                         long long start_ms, long long deadline_ms, int max_retries,
                         const std::string& actor, long long* id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO distribution_tasks(batch_id,devid,file_name,file_size,file_sha,"
        "sub_dir,target_path,status,start_at_ms,deadline_at_ms,max_retries,actor,created_ms)"
        " VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, batch.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, file.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, size);
    sqlite3_bind_text(st, 5, sha.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, sub.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, target.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, start_ms);
    sqlite3_bind_int64(st, 10, deadline_ms);
    sqlite3_bind_int(st, 11, max_retries);
    sqlite3_bind_text(st, 12, actor.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 13, nowMs());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return false;
    if (id) *id = sqlite3_last_insert_rowid(db_);
    return true;
}

bool AdminDb::distUpdate(long long id, const std::string& status, const std::string& error,
                         long long sent, long long relay_tid, long long finished_ms,
                         long long next_retry_ms, int retry_count) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE distribution_tasks SET status=?, error=?, sent=?, relay_tid=?,"
        " finished_ms=?, next_retry_ms=?, retry_count=? WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, status.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, error.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, sent);
    sqlite3_bind_int64(st, 4, relay_tid);
    sqlite3_bind_int64(st, 5, finished_ms);
    sqlite3_bind_int64(st, 6, next_retry_ms);
    sqlite3_bind_int(st, 7, retry_count);
    sqlite3_bind_int64(st, 8, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::distProgress(long long id, long long sent) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE distribution_tasks SET sent=? WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, sent);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::distExtend(long long id, long long deadline_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "UPDATE distribution_tasks SET status='queued', deadline_at_ms=?,"
        " extend_count=extend_count+1, next_retry_ms=0 WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, deadline_ms);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::distGet(long long id, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id,batch_id,devid,file_name,file_size,sub_dir,target_path,status,"
        "start_at_ms,deadline_at_ms,retry_count,max_retries,next_retry_ms,extend_count,sent,error,"
        "relay_tid,actor,created_ms,started_ms,finished_ms,file_sha FROM distribution_tasks WHERE id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW) {
        nlohmann::json j;
        fillDistRow(st, j);
        json_out = j.dump();
        ok = true;
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::distList(const std::string& status, const std::string& q, const std::string& batch,
                       int page, int per_page, std::string& json_out, int& total) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string where = " WHERE 1=1";
    std::vector<std::string> args;
    if (status == "active") {
        where += " AND status IN ('scheduled','queued','running')";
    } else if (status == "done") {
        where += " AND status='success'";
    } else if (status == "failed_group") {
        where += " AND status IN ('failed','timeout')";
    } else if (!status.empty()) {
        where += " AND status=?";
        args.push_back(status);
    }
    if (!batch.empty()) {
        where += " AND batch_id=?";
        args.push_back(batch);
    }
    if (!q.empty()) {
        where += " AND (devid LIKE ? OR file_name LIKE ?)";
        args.push_back("%" + q + "%");
        args.push_back("%" + q + "%");
    }
    std::string countSql = "SELECT COUNT(*) FROM distribution_tasks" + where;
    sqlite3_stmt* st = nullptr;
    total = 0;
    if (sqlite3_prepare_v2(db_, countSql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    for (size_t i = 0; i < args.size(); ++i)
        sqlite3_bind_text(st, (int)i + 1, args[i].c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);

    std::string sql = "SELECT id,batch_id,devid,file_name,file_size,sub_dir,target_path,status,"
        "start_at_ms,deadline_at_ms,retry_count,max_retries,next_retry_ms,extend_count,sent,error,"
        "relay_tid,actor,created_ms,started_ms,finished_ms,file_sha FROM distribution_tasks" + where +
        " ORDER BY id DESC LIMIT ? OFFSET ?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    for (size_t i = 0; i < args.size(); ++i)
        sqlite3_bind_text(st, idx++, args[i].c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, idx++, per_page);
    sqlite3_bind_int(st, idx++, (page - 1) * per_page);
    nlohmann::json arr = nlohmann::json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        nlohmann::json j;
        fillDistRow(st, j);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::distDue(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT id,batch_id,devid,file_name,file_size,sub_dir,target_path,status,"
        "start_at_ms,deadline_at_ms,retry_count,max_retries,next_retry_ms,extend_count,sent,error,"
        "relay_tid,actor,created_ms,started_ms,finished_ms,file_sha FROM distribution_tasks"
        " WHERE status IN ('scheduled','queued','running') ORDER BY id";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    nlohmann::json arr = nlohmann::json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        nlohmann::json j;
        fillDistRow(st, j);
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::distSummary(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT status,COUNT(*) FROM distribution_tasks GROUP BY status",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    long long all = 0, active = 0, done = 0, failed = 0, conflict = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        std::string s = (const char*)sqlite3_column_text(st, 0);
        long long n = sqlite3_column_int64(st, 1);
        all += n;
        if (s == "success") done += n;
        else if (s == "failed" || s == "timeout") failed += n;
        else if (s == "conflict") conflict += n;
        else if (s == "canceled") { /* 归入全部，不计进行中 */ }
        else active += n;
    }
    sqlite3_finalize(st);
    nlohmann::json j;
    j["all"] = all;
    j["active"] = active;
    j["done"] = done;
    j["failed"] = failed;
    j["conflict"] = conflict;
    json_out = j.dump();
    return true;
}

bool AdminDb::fileMetaGet(const std::string& name, long long size, long long mtime_ms,
                          std::string& sha) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT size,mtime_ms,sha256 FROM file_meta WHERE name=?",
                           -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = false;
    if (sqlite3_step(st) == SQLITE_ROW &&
        sqlite3_column_int64(st, 0) == size && sqlite3_column_int64(st, 1) == mtime_ms) {
        sha = (const char*)sqlite3_column_text(st, 2);
        ok = !sha.empty();
    }
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::fileMetaPut(const std::string& name, long long size, long long mtime_ms,
                          const std::string& sha) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO file_meta(name,size,mtime_ms,sha256) VALUES(?,?,?,?) "
                      "ON CONFLICT(name) DO UPDATE SET size=excluded.size,"
                      " mtime_ms=excluded.mtime_ms, sha256=excluded.sha256";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, size);
    sqlite3_bind_int64(st, 3, mtime_ms);
    sqlite3_bind_text(st, 4, sha.c_str(), -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::distAlreadyPushed(const std::string& devid, const std::string& sha) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "SELECT 1 FROM distribution_tasks WHERE devid=? AND file_sha=?"
                      " AND status IN ('success','conflict') LIMIT 1";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, sha.c_str(), -1, SQLITE_TRANSIENT);
    bool found = (sqlite3_step(st) == SQLITE_ROW);
    sqlite3_finalize(st);
    return found;
}

bool AdminDb::bindDevice(const std::string& devid, long long owner_id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql = "INSERT INTO devices(devid,owner_id,bound_ms) VALUES(?,?,?)"
                      " ON CONFLICT(devid) DO UPDATE SET owner_id=excluded.owner_id,bound_ms=excluded.bound_ms";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, owner_id);
    sqlite3_bind_int64(st, 3, nowMs());
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE;
}

bool AdminDb::unbindDevice(const std::string& devid) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM devices WHERE devid=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::deviceOwner(const std::string& devid, long long& owner_id) {
    std::lock_guard<std::mutex> lk(mu_);
    owner_id = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT owner_id FROM devices WHERE devid=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        if (sqlite3_column_type(st, 0) != SQLITE_NULL) owner_id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::ownedDevids(long long owner_id, std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT devid FROM devices WHERE owner_id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, owner_id);
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back((const char*)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::deviceList(std::vector<std::pair<std::string, long long>>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT devid,COALESCE(owner_id,0) FROM devices ORDER BY devid", -1, &st, nullptr) != SQLITE_OK) return false;
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.push_back({(const char*)sqlite3_column_text(st, 0), sqlite3_column_int64(st, 1)});
    }
    sqlite3_finalize(st);
    return true;
}
