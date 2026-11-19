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

std::string dbColText(sqlite3_stmt* st, int col) {
    const unsigned char* p = sqlite3_column_text(st, col);
    return p ? (const char*)p : "";
}

long long dbColInt(sqlite3_stmt* st, int col) { return sqlite3_column_int64(st, col); }

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
    if (!exec(
        "CREATE TABLE IF NOT EXISTS users("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " username TEXT NOT NULL UNIQUE,"
        " pass_hash TEXT NOT NULL,"
        " role TEXT NOT NULL,"
        " status TEXT NOT NULL DEFAULT 'active',"
        " perm_explicit INTEGER NOT NULL DEFAULT 0,"
        " created_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sessions("
        " sid_hash TEXT PRIMARY KEY,"
        " user_id INTEGER NOT NULL,"
        " expire_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS settings("
        " key TEXT PRIMARY KEY,"
        " value TEXT NOT NULL DEFAULT '',"
        " updated_ms INTEGER NOT NULL DEFAULT 0,"
        " updated_by TEXT NOT NULL DEFAULT '');"
        "CREATE TABLE IF NOT EXISTS distribution_tasks("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " batch_id TEXT NOT NULL,"
        " devid TEXT NOT NULL,"
        " kind TEXT NOT NULL DEFAULT 'file',"
        " target_version TEXT NOT NULL DEFAULT '',"
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
        "CREATE TABLE IF NOT EXISTS metric_series_config("
        " devid TEXT NOT NULL,"
        " series TEXT NOT NULL,"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " interval_sec INTEGER NOT NULL DEFAULT 5,"
        " points INTEGER NOT NULL DEFAULT 720,"
        " revision INTEGER NOT NULL DEFAULT 0,"
        " sync_state INTEGER NOT NULL DEFAULT 0,"
        " updated_by TEXT NOT NULL DEFAULT '',"
        " updated_ms INTEGER NOT NULL DEFAULT 0,"
        " synced_ms INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(devid,series));"
        "CREATE TABLE IF NOT EXISTS metric_series_config("
        " devid TEXT NOT NULL,"
        " series TEXT NOT NULL,"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " interval_sec INTEGER NOT NULL DEFAULT 5,"
        " points INTEGER NOT NULL DEFAULT 720,"
        " revision INTEGER NOT NULL DEFAULT 0,"
        " sync_state INTEGER NOT NULL DEFAULT 0,"
        " updated_by TEXT NOT NULL DEFAULT '',"
        " updated_ms INTEGER NOT NULL DEFAULT 0,"
        " synced_ms INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(devid,series));"
        "CREATE TABLE IF NOT EXISTS tty_config("
        " devid TEXT NOT NULL,"
        " path TEXT NOT NULL,"
        " alias TEXT NOT NULL DEFAULT '',"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " source TEXT NOT NULL DEFAULT 'discovered',"
        " baud INTEGER NOT NULL DEFAULT 115200,"
        " data_bits INTEGER NOT NULL DEFAULT 8,"
        " parity TEXT NOT NULL DEFAULT 'N',"
        " stop_bits INTEGER NOT NULL DEFAULT 1,"
        " roles TEXT NOT NULL DEFAULT 'operator,manager,admin',"
        " updated_by TEXT NOT NULL DEFAULT '',"
        " updated_ms INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(devid,path));"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_tty_alias ON tty_config(devid,alias) WHERE alias != '';"
        "CREATE TABLE IF NOT EXISTS port_dict("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " port_start INTEGER NOT NULL,"
        " port_end INTEGER NOT NULL,"
        " name TEXT NOT NULL DEFAULT '',"
        " description TEXT NOT NULL DEFAULT '',"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " created_ms INTEGER NOT NULL,"
        " updated_ms INTEGER NOT NULL,"
        " UNIQUE(port_start, port_end));"
        "CREATE TABLE IF NOT EXISTS tty_dict("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " pattern TEXT NOT NULL UNIQUE,"
        " name TEXT NOT NULL DEFAULT '',"
        " description TEXT NOT NULL DEFAULT '',"
        " kind TEXT NOT NULL DEFAULT 'serial',"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " created_ms INTEGER NOT NULL,"
        " updated_ms INTEGER NOT NULL);"
        // 资源权限：资源字典 + 权限组 + 组成员。管理台所有动作按资源键鉴权。
        "CREATE TABLE IF NOT EXISTS perm_resources("
        " key TEXT PRIMARY KEY,"
        " category TEXT NOT NULL DEFAULT '',"
        " name TEXT NOT NULL DEFAULT '',"
        " description TEXT NOT NULL DEFAULT '',"
        " sort INTEGER NOT NULL DEFAULT 0,"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " updated_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS perm_groups("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL UNIQUE,"
        " description TEXT NOT NULL DEFAULT '',"
        " builtin_key TEXT NOT NULL DEFAULT '',"
        " super INTEGER NOT NULL DEFAULT 0,"
        " seeded INTEGER NOT NULL DEFAULT 0,"
        " all_devices INTEGER NOT NULL DEFAULT 0,"
        " ports TEXT NOT NULL DEFAULT '*',"
        " created_ms INTEGER NOT NULL,"
        " updated_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS perm_group_resources("
        " group_id INTEGER NOT NULL,"
        " resource_key TEXT NOT NULL,"
        " allowed INTEGER NOT NULL DEFAULT 1,"
        " PRIMARY KEY(group_id,resource_key));"
        "CREATE TABLE IF NOT EXISTS perm_group_members("
        " group_id INTEGER NOT NULL,"
        " subject_type TEXT NOT NULL DEFAULT 'user',"
        " subject_id TEXT NOT NULL,"
        " added_ms INTEGER NOT NULL,"
        " PRIMARY KEY(group_id,subject_type,subject_id));"
        "CREATE INDEX IF NOT EXISTS idx_perm_member_subject ON perm_group_members(subject_type,subject_id);"
        // 设备资源：agent 上线时自动 upsert，权限组按 devid 授予
        "CREATE TABLE IF NOT EXISTS res_devices("
        " devid TEXT PRIMARY KEY,"
        " name TEXT NOT NULL DEFAULT '',"
        " version TEXT NOT NULL DEFAULT '',"
        " online INTEGER NOT NULL DEFAULT 0,"
        " last_seen_ms INTEGER NOT NULL DEFAULT 0,"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " source TEXT NOT NULL DEFAULT 'agent',"
        " updated_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS perm_group_devices("
        " group_id INTEGER NOT NULL,"
        " devid TEXT NOT NULL,"
        " added_ms INTEGER NOT NULL,"
        " PRIMARY KEY(group_id,devid));"
        "CREATE INDEX IF NOT EXISTS idx_perm_gdev ON perm_group_devices(devid);"
        "CREATE TABLE IF NOT EXISTS user_prefs("
        " user_id INTEGER NOT NULL,"
        " key TEXT NOT NULL,"
        " value TEXT NOT NULL DEFAULT '',"
        " PRIMARY KEY(user_id,key));"
        // 接入账号（A 侧身份主体）与 Token（临时凭据，可独立限额）
        "CREATE TABLE IF NOT EXISTS client_accounts("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " username TEXT NOT NULL UNIQUE,"
        " display_name TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'active',"
        " note TEXT NOT NULL DEFAULT '',"
        " created_by TEXT NOT NULL DEFAULT '',"
        " created_ms INTEGER NOT NULL,"
        " updated_ms INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS client_tokens("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " account_id INTEGER NOT NULL,"
        " name TEXT NOT NULL DEFAULT '',"
        " token_hash TEXT NOT NULL UNIQUE,"
        " token_prefix TEXT NOT NULL,"
        " status TEXT NOT NULL DEFAULT 'active',"
        " expires_at_ms INTEGER NOT NULL DEFAULT 0,"
        " quota_bytes INTEGER NOT NULL DEFAULT 0,"
        " quota_period TEXT NOT NULL DEFAULT 'monthly',"
        " period_start_ms INTEGER NOT NULL DEFAULT 0,"
        " used_bytes INTEGER NOT NULL DEFAULT 0,"
        " rate_per_min INTEGER NOT NULL DEFAULT 0,"
        " burst INTEGER NOT NULL DEFAULT 0,"
        " max_sessions INTEGER NOT NULL DEFAULT 0,"
        " quota_action TEXT NOT NULL DEFAULT 'cut',"
        " created_by TEXT NOT NULL DEFAULT '',"
        " created_ms INTEGER NOT NULL,"
        " last_used_ms INTEGER NOT NULL DEFAULT 0,"
        " last_used_ip TEXT NOT NULL DEFAULT '',"
        " revoked_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_client_token_account ON client_tokens(account_id);"
        // 管理台账号 API Token（atk-）。归属账号，最终角色取 users.role；
        "CREATE TABLE IF NOT EXISTS admin_tokens("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " user_id INTEGER NOT NULL,"
        " username TEXT NOT NULL,"
        " name TEXT NOT NULL DEFAULT '',"
        " token_hash TEXT NOT NULL UNIQUE,"
        " token_prefix TEXT NOT NULL,"
        " status TEXT NOT NULL DEFAULT 'active',"
        " expires_at_ms INTEGER NOT NULL DEFAULT 0,"
        " created_by TEXT NOT NULL DEFAULT '',"
        " created_ms INTEGER NOT NULL,"
        " last_used_ms INTEGER NOT NULL DEFAULT 0,"
        " last_used_ip TEXT NOT NULL DEFAULT '',"
        " revoked_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_admin_token_user ON admin_tokens(user_id);"
        "UPDATE users SET role='admin' WHERE role='superadmin';"
        "UPDATE users SET role='viewer' WHERE role='user';",
        err)) return false;

    // 增量迁移：distribution_tasks 增加 OTA 字段（老库没有）
    auto hasColumn = [&](const char* table, const char* col) {
        std::string sql = std::string("PRAGMA table_info(") + table + ")";
        sqlite3_stmt* st = nullptr;
        bool found = false;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                const char* n = (const char*)sqlite3_column_text(st, 1);
                if (n && strcmp(n, col) == 0) { found = true; break; }
            }
            sqlite3_finalize(st);
        }
        return found;
    };
    if (!hasColumn("distribution_tasks", "kind")) {
        if (!exec("ALTER TABLE distribution_tasks ADD COLUMN kind TEXT NOT NULL DEFAULT 'file'", err)) return false;
    }
    if (!hasColumn("distribution_tasks", "target_version")) {
        if (!exec("ALTER TABLE distribution_tasks ADD COLUMN target_version TEXT NOT NULL DEFAULT ''", err)) return false;
    }
    exec("CREATE INDEX IF NOT EXISTS idx_dist_kind ON distribution_tasks(kind, status)", err);
    if (!hasColumn("users", "perm_explicit")) {
        if (!exec("ALTER TABLE users ADD COLUMN perm_explicit INTEGER NOT NULL DEFAULT 0", err)) return false;
    }
    if (!hasColumn("perm_groups", "seeded")) {
        if (!exec("ALTER TABLE perm_groups ADD COLUMN seeded INTEGER NOT NULL DEFAULT 0", err)) return false;
    }
    if (!hasColumn("perm_groups", "all_devices")) {
        if (!exec("ALTER TABLE perm_groups ADD COLUMN all_devices INTEGER NOT NULL DEFAULT 0", err)) return false;
    }
    if (!hasColumn("perm_groups", "ports")) {
        if (!exec("ALTER TABLE perm_groups ADD COLUMN ports TEXT NOT NULL DEFAULT '*'", err)) return false;
    }
    return true;
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
    long long uid = sqlite3_last_insert_rowid(db_);
    if (out_id) *out_id = uid;
    // 不自动入组：未分配权限组前按 role 预置组 + 归属设备回落，
    // 管理员显式分配权限组后（perm_explicit=1）只认组。
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
    if (rc != SQLITE_DONE) return false;
    // 角色只决定未显式分配权限组时的回落基线；已入组的账号不受影响
    return true;
}

bool AdminDb::removeUser(long long id) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM perm_group_members WHERE subject_type='user' AND subject_id=?", -1, &st, nullptr) ==
        SQLITE_OK) {
        std::string sid = std::to_string(id);
        sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    if (sqlite3_prepare_v2(db_, "DELETE FROM users WHERE id=?", -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) return false;
    sqlite3_prepare_v2(db_, "DELETE FROM sessions WHERE user_id=?", -1, &st, nullptr);
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);

    // 删除账号时一并清理其 API Token 与分组绑定，避免留下可用凭据
    std::vector<long long> tids;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM admin_tokens WHERE user_id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) tids.push_back(sqlite3_column_int64(st, 0));
    }
    sqlite3_finalize(st);
    for (long long tid : tids) {
        std::string sid = std::to_string(tid);
        if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_members WHERE subject_type='atk' AND subject_id=?",
                               -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    if (sqlite3_prepare_v2(db_, "DELETE FROM admin_tokens WHERE user_id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        sqlite3_step(st);
    }
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

bool AdminDb::deviceList(std::vector<std::string>& out) {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT devid FROM res_devices ORDER BY devid", -1, &st, nullptr) != SQLITE_OK)
        return false;
    while (sqlite3_step(st) == SQLITE_ROW) out.push_back((const char*)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return true;
}

