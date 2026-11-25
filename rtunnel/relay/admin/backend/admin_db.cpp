#include "admin_db.hpp"
#include "common/sha256.hpp"
#include "cpputil/json.hpp"
#include "common/logging.hpp"

#include "sqlite3.h"

#include "openssl/evp.h"
#include "openssl/rand.h"

#include <time.h>
#include <string.h>
#include <stdio.h>

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

void dbProfileCb(void* arg, const char* /*sql*/, sqlite3_uint64 ns) {
    static_cast<AdminDb*>(arg)->recordQuery((unsigned long long)ns);
}

} // namespace

void AdminDb::recordQuery(unsigned long long ns) {
    unsigned long long us = ns / 1000;
    q_total_.fetch_add(1, std::memory_order_relaxed);
    q_us_.fetch_add(us, std::memory_order_relaxed);
    if (us >= 100000) q_slow_.fetch_add(1, std::memory_order_relaxed);
    unsigned long long cur = q_max_us_.load(std::memory_order_relaxed);
    while (us > cur && !q_max_us_.compare_exchange_weak(cur, us, std::memory_order_relaxed)) {}
}

void AdminDb::stats(DbStats& out) const {
    out.queries = q_total_.load(std::memory_order_relaxed);
    out.total_us = q_us_.load(std::memory_order_relaxed);
    out.max_us = q_max_us_.load(std::memory_order_relaxed);
    out.slow_queries = q_slow_.load(std::memory_order_relaxed);
}

std::string dbColText(sqlite3_stmt* st, int col) {
    const unsigned char* p = sqlite3_column_text(st, col);
    return p ? (const char*)p : "";
}

long long dbColInt(sqlite3_stmt* st, int col) { return sqlite3_column_int64(st, col); }

bool dbIsDay(const std::string& s) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

AdminDb::~AdminDb() {
    if (db_) { sqlite3_close(db_); db_ = nullptr; }
}

long long AdminDb::nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

std::string AdminDb::randomHex(int bytes) {
    std::vector<unsigned char> buf(bytes);
    if (RAND_bytes(buf.data(), bytes) != 1) return "";
    return hexEncode(buf.data(), buf.size());
}

std::string AdminDb::sha256Hex(const std::string& data) {
    return rtutil::sha256Hex(data);
}

std::string AdminDb::hashPassword(const std::string& pass, std::string& err) {
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
}

bool AdminDb::verifyPassword(const std::string& pass, const std::string& stored) {
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
    // 语句耗时统计：/metrics 的 rtunnel_admin_db_* 由此而来
    sqlite3_profile(db_, dbProfileCb, this);
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
        " global_scope INTEGER NOT NULL DEFAULT 0,"
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
        " tenant_id INTEGER NOT NULL DEFAULT 1,"
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
        // 租户：计费与设备归属边界。默认租户 id=1，升级前的数据都归它
        "CREATE TABLE IF NOT EXISTS tenants("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " name TEXT NOT NULL UNIQUE,"
        " note TEXT NOT NULL DEFAULT '',"
        " status TEXT NOT NULL DEFAULT 'active',"
        // 套餐：0 表示不限。设备数按已登记设备计（默认租户不受限），
        // 并发与月流量由 relay 在数据面强制
        " max_devices INTEGER NOT NULL DEFAULT 0,"
        " max_sessions INTEGER NOT NULL DEFAULT 0,"
        " quota_bytes INTEGER NOT NULL DEFAULT 0,"
        " created_ms INTEGER NOT NULL,"
        " updated_ms INTEGER NOT NULL);"
        "INSERT OR IGNORE INTO tenants(id,name,note,status,created_ms,updated_ms)"
        " VALUES(1,'默认租户','升级前已有的账号与设备','active',0,0);"
        "CREATE TABLE IF NOT EXISTS client_accounts("
        " tenant_id INTEGER NOT NULL DEFAULT 1,"
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
        " rate_kbps INTEGER NOT NULL DEFAULT 0,"
        " groups_explicit INTEGER NOT NULL DEFAULT 0,"
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
        " revoked_ms INTEGER NOT NULL DEFAULT 0,"
        " groups_explicit INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_admin_token_user ON admin_tokens(user_id);"
        // 用量流水：append-only，按事件落一行；day 是事件归属日（relay 本地时区）
        "CREATE TABLE IF NOT EXISTS usage_events("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " ts_ms INTEGER NOT NULL,"
        " tenant_id INTEGER NOT NULL DEFAULT 0,"
        " day TEXT NOT NULL,"
        " subject_type TEXT NOT NULL,"
        " subject_id TEXT NOT NULL,"
        " metric TEXT NOT NULL,"
        " qty INTEGER NOT NULL,"
        " meta TEXT NOT NULL DEFAULT '',"
        " source TEXT NOT NULL DEFAULT 'relay');"
        "CREATE INDEX IF NOT EXISTS idx_usage_events_ts ON usage_events(ts_ms);"
        "CREATE INDEX IF NOT EXISTS idx_usage_events_subject"
        " ON usage_events(subject_type,subject_id,metric,day);"
        // 日聚合：出账/报表直接查这张表，不扫流水
        "CREATE TABLE IF NOT EXISTS usage_daily("
        " day TEXT NOT NULL,"
        " tenant_id INTEGER NOT NULL DEFAULT 0,"
        " subject_type TEXT NOT NULL,"
        " subject_id TEXT NOT NULL,"
        " metric TEXT NOT NULL,"
        " qty INTEGER NOT NULL,"
        " events INTEGER NOT NULL DEFAULT 0,"
        " PRIMARY KEY(day,subject_type,subject_id,metric));"
        "CREATE INDEX IF NOT EXISTS idx_usage_daily_metric ON usage_daily(metric,day);"
        "CREATE INDEX IF NOT EXISTS idx_usage_daily_tenant ON usage_daily(tenant_id,day);"
        "CREATE INDEX IF NOT EXISTS idx_usage_events_tenant ON usage_events(tenant_id,ts_ms);"
        // 价目：每个指标一个计价单位与单价（金额 1e-6 元）。默认 0 元，先试算不收费
        "CREATE TABLE IF NOT EXISTS price_rules("
        " metric TEXT PRIMARY KEY,"
        " unit_size INTEGER NOT NULL DEFAULT 1,"
        " price_micro INTEGER NOT NULL DEFAULT 0,"
        " currency TEXT NOT NULL DEFAULT 'CNY',"
        " updated_ms INTEGER NOT NULL DEFAULT 0);"
        "INSERT OR IGNORE INTO price_rules(metric,unit_size,price_micro,currency,updated_ms) VALUES"
        " ('bytes',1073741824,0,'CNY',0),"
        " ('sessions',1000,0,'CNY',0),"
        " ('session_seconds',3600,0,'CNY',0),"
        " ('online_seconds',86400,0,'CNY',0);"
        // 结算单：按账期冻结金额与明细，避免价目/用量变动影响已出账数据
        "CREATE TABLE IF NOT EXISTS billing_statements("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " tenant_id INTEGER NOT NULL,"
        " period TEXT NOT NULL,"
        " from_day TEXT NOT NULL,"
        " to_day TEXT NOT NULL,"
        " currency TEXT NOT NULL DEFAULT 'CNY',"
        " total_micro INTEGER NOT NULL DEFAULT 0,"
        " status TEXT NOT NULL DEFAULT 'draft',"
        " lines_json TEXT NOT NULL DEFAULT '[]',"
        " note TEXT NOT NULL DEFAULT '',"
        " created_by TEXT NOT NULL DEFAULT '',"
        " created_ms INTEGER NOT NULL,"
        " updated_ms INTEGER NOT NULL,"
        " issued_ms INTEGER NOT NULL DEFAULT 0,"
        " paid_ms INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(tenant_id,period));"
        "CREATE INDEX IF NOT EXISTS idx_billing_stmt_period ON billing_statements(period,status);"
        // relay 地址字典 + 有序下发列表 + 同步任务（逐设备行复用 distribution_tasks）
        "CREATE TABLE IF NOT EXISTS relay_dict("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " host TEXT NOT NULL,"
        " port INTEGER NOT NULL,"
        " name TEXT NOT NULL DEFAULT '',"
        " description TEXT NOT NULL DEFAULT '',"
        " enabled INTEGER NOT NULL DEFAULT 1,"
        " created_ms INTEGER NOT NULL,"
        " updated_ms INTEGER NOT NULL,"
        " UNIQUE(host,port));"
        "CREATE TABLE IF NOT EXISTS relay_list("
        " position INTEGER PRIMARY KEY,"
        " endpoint_id INTEGER NOT NULL UNIQUE,"
        " added_by TEXT NOT NULL DEFAULT '',"
        " added_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS relay_sync_task("
        " batch_id TEXT PRIMARY KEY,"
        " name TEXT NOT NULL DEFAULT '',"
        " list_json TEXT NOT NULL DEFAULT '',"
        " list_rev INTEGER NOT NULL DEFAULT 0,"
        " allow_leave INTEGER NOT NULL DEFAULT 0,"
        " mode TEXT NOT NULL DEFAULT 'once',"
        " wave_size INTEGER NOT NULL DEFAULT 0,"
        " wave_interval_s INTEGER NOT NULL DEFAULT 0,"
        " fail_threshold_pct INTEGER NOT NULL DEFAULT 0,"
        " total_waves INTEGER NOT NULL DEFAULT 1,"
        " current_wave INTEGER NOT NULL DEFAULT 0,"
        " wave_done_ms INTEGER NOT NULL DEFAULT 0,"
        " status TEXT NOT NULL DEFAULT 'scheduled',"
        " start_at_ms INTEGER NOT NULL DEFAULT 0,"
        " deadline_at_ms INTEGER NOT NULL DEFAULT 0,"
        " max_retries INTEGER NOT NULL DEFAULT 3,"
        " actor TEXT NOT NULL DEFAULT '',"
        " created_ms INTEGER NOT NULL DEFAULT 0,"
        " finished_ms INTEGER NOT NULL DEFAULT 0);"
        "CREATE INDEX IF NOT EXISTS idx_relay_task_due ON relay_sync_task(status, start_at_ms);"
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
    // relay 同步任务：列表版本与灰度批次（老库没有）
    if (!hasColumn("distribution_tasks", "payload_rev")) {
        if (!exec("ALTER TABLE distribution_tasks ADD COLUMN payload_rev INTEGER NOT NULL DEFAULT 0", err)) return false;
    }
    if (!hasColumn("distribution_tasks", "wave")) {
        if (!exec("ALTER TABLE distribution_tasks ADD COLUMN wave INTEGER NOT NULL DEFAULT 0", err)) return false;
    }
    exec("CREATE INDEX IF NOT EXISTS idx_dist_kind ON distribution_tasks(kind, status)", err);
    // 任务详情/巡检都按 batch_id + kind + status 过滤
    exec("CREATE INDEX IF NOT EXISTS idx_dist_batch ON distribution_tasks(batch_id, kind, status)", err);
    // 角色不再直接授予权限：老库把「未显式分组」的账号补进对应内置组，再删掉旧标记。
    // 显式分过组（perm_explicit=1）但不属于任何组的账号保持零权限；
    // 例外：role=admin 的账号没有组时补进超级组，避免唯一管理员被锁在门外。
    if (hasColumn("users", "perm_explicit")) {
        if (!exec("INSERT OR IGNORE INTO perm_group_members(group_id,subject_type,subject_id,added_ms)"
                  " SELECT g.id,'user',CAST(u.id AS TEXT),CAST(strftime('%s','now') AS INTEGER)*1000"
                  " FROM users u JOIN perm_groups g ON g.builtin_key=u.role"
                  " WHERE (u.perm_explicit=0 OR u.role='admin') AND NOT EXISTS("
                  "  SELECT 1 FROM perm_group_members m WHERE m.subject_type='user'"
                  "  AND m.subject_id=CAST(u.id AS TEXT))",
                  err))
            return false;
        if (!exec("ALTER TABLE users DROP COLUMN perm_explicit", err)) return false;
    }
    // 令牌绑定过权限组 = 显式；显式但组为空是「无权限」，不再回落到归属账号
    if (!hasColumn("admin_tokens", "groups_explicit")) {
        if (!exec("ALTER TABLE admin_tokens ADD COLUMN groups_explicit INTEGER NOT NULL DEFAULT 0", err))
            return false;
    }
    if (!hasColumn("client_tokens", "groups_explicit")) {
        if (!exec("ALTER TABLE client_tokens ADD COLUMN groups_explicit INTEGER NOT NULL DEFAULT 0", err))
            return false;
    }
    exec("UPDATE admin_tokens SET groups_explicit=1 WHERE id IN ("
         "SELECT CAST(subject_id AS INTEGER) FROM perm_group_members WHERE subject_type='atk')", err);
    exec("UPDATE client_tokens SET groups_explicit=1 WHERE id IN ("
         "SELECT CAST(subject_id AS INTEGER) FROM perm_group_members WHERE subject_type='rtk')", err);
    if (!hasColumn("perm_groups", "seeded")) {
        if (!exec("ALTER TABLE perm_groups ADD COLUMN seeded INTEGER NOT NULL DEFAULT 0", err)) return false;
    }
    if (!hasColumn("perm_groups", "all_devices")) {
        if (!exec("ALTER TABLE perm_groups ADD COLUMN all_devices INTEGER NOT NULL DEFAULT 0", err)) return false;
    }
    if (!hasColumn("perm_groups", "global_scope")) {
        if (!exec("ALTER TABLE perm_groups ADD COLUMN global_scope INTEGER NOT NULL DEFAULT 0", err)) return false;
        // 老库的内置角色组按传统语义补上全局权限（设备范围仍由显式授权决定）
        exec("UPDATE perm_groups SET global_scope=1 WHERE builtin_key<>''", err);
    }
    if (!hasColumn("perm_groups", "ports")) {
        if (!exec("ALTER TABLE perm_groups ADD COLUMN ports TEXT NOT NULL DEFAULT '*'", err)) return false;
    }
    // 令牌带宽上限（kilobit/s，0 = 不限）：老库补列，默认不限，行为不变
    if (!hasColumn("client_tokens", "rate_kbps")) {
        if (!exec("ALTER TABLE client_tokens ADD COLUMN rate_kbps INTEGER NOT NULL DEFAULT 0", err))
            return false;
    }
    // 多租户：老库补列。账号与设备归默认租户 1（升级前就是单租户）；
    // 用量行可能是本功能上线前产生的，没有归属，统一记 0（运营方自用/无法归属）。
    if (!hasColumn("client_accounts", "tenant_id")) {
        if (!exec("ALTER TABLE client_accounts ADD COLUMN tenant_id INTEGER NOT NULL DEFAULT 1", err))
            return false;
    }
    if (!hasColumn("res_devices", "tenant_id")) {
        if (!exec("ALTER TABLE res_devices ADD COLUMN tenant_id INTEGER NOT NULL DEFAULT 1", err))
            return false;
    }
    if (!hasColumn("tenants", "max_devices")) {
        if (!exec("ALTER TABLE tenants ADD COLUMN max_devices INTEGER NOT NULL DEFAULT 0", err))
            return false;
    }
    if (!hasColumn("tenants", "max_sessions")) {
        if (!exec("ALTER TABLE tenants ADD COLUMN max_sessions INTEGER NOT NULL DEFAULT 0", err))
            return false;
    }
    if (!hasColumn("tenants", "quota_bytes")) {
        if (!exec("ALTER TABLE tenants ADD COLUMN quota_bytes INTEGER NOT NULL DEFAULT 0", err))
            return false;
    }
    if (!hasColumn("usage_events", "tenant_id")) {
        if (!exec("ALTER TABLE usage_events ADD COLUMN tenant_id INTEGER NOT NULL DEFAULT 0", err)) return false;
        exec("CREATE INDEX IF NOT EXISTS idx_usage_events_tenant ON usage_events(tenant_id,ts_ms)", err);
    }
    if (!hasColumn("usage_daily", "tenant_id")) {
        if (!exec("ALTER TABLE usage_daily ADD COLUMN tenant_id INTEGER NOT NULL DEFAULT 0", err)) return false;
        exec("CREATE INDEX IF NOT EXISTS idx_usage_daily_tenant ON usage_daily(tenant_id,day)", err);
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

// 用户名用于日志与审计行：禁止控制字符，允许中间空格，禁止首尾空白，长度 1..64
static bool validUserName(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    if (s.front() == ' ' || s.back() == ' ') return false;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool AdminDb::createUser(const std::string& user, const std::string& pass,
                         const std::string& role, std::string& err, long long* out_id) {
    if (!validUserName(user)) { err = "用户名不合法（1-64 字符，不含控制字符，首尾不能是空格）"; return false; }
    if (pass.size() < 8) { err = "password < 8 chars"; return false; }
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
    // 角色只决定初始权限组：新账号直接进入对应的内置组，之后权限只由组求值
    sqlite3_stmt* g = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO perm_group_members(group_id,subject_type,subject_id,added_ms)"
            " SELECT id,'user',?,? FROM perm_groups WHERE builtin_key=?", -1, &g, nullptr) == SQLITE_OK) {
        std::string sid = std::to_string(uid);
        sqlite3_bind_text(g, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(g, 2, nowMs());
        sqlite3_bind_text(g, 3, role.c_str(), -1, SQLITE_TRANSIENT);
        int grc = sqlite3_step(g);
        bool joined = grc == SQLITE_DONE && sqlite3_changes(db_) > 0;
        if (!joined) {
            // 角色组缺失时不留半成品账号：没有组的账号登录后什么都做不了
            sqlite3_stmt* del = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM users WHERE id=?", -1, &del, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(del, 1, uid);
                sqlite3_step(del);
            }
            sqlite3_finalize(del);
            err = "角色权限组不存在";
            return false;
        }
    }
    sqlite3_finalize(g);
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
    if (pass.size() < 8) { err = "password < 8 chars"; return false; }
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
    std::string e;
    if (!exec("BEGIN", e)) return false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "UPDATE users SET role=? WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e);
        return false;
    }
    sqlite3_bind_text(st, 1, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        exec("ROLLBACK", e);
        return false;
    }
    // 换角色 = 换内置组：自定义组保留，角色组替换成新角色的内置组
    std::string sid = std::to_string(id);
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM perm_group_members WHERE subject_type='user' AND subject_id=?"
            " AND group_id IN (SELECT id FROM perm_groups WHERE builtin_key<>'')",
            -1, &st, nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e);
        return false;
    }
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        exec("ROLLBACK", e);
        return false;
    }
    if (sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO perm_group_members(group_id,subject_type,subject_id,added_ms)"
            " SELECT id,'user',?,? FROM perm_groups WHERE builtin_key=?", -1, &st, nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e);
        return false;
    }
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, nowMs());
    sqlite3_bind_text(st, 3, role.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    if (rc != SQLITE_DONE) {
        exec("ROLLBACK", e);
        return false;
    }
    exec("COMMIT", e);
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

