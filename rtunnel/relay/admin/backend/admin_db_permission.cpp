#include "admin_db.hpp"
#include "cpputil/json.hpp"
#include "sqlite3.h"

#include <algorithm>
#include <cstdio>
#include <set>

using nlohmann::json;

namespace {
// ---- 端口/TTY 上限求交 ----
struct PortRange { int lo = 0; int hi = 0; };

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t comma = s.find(',', pos);
        std::string one = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
        size_t b = one.find_first_not_of(" \t");
        size_t e = one.find_last_not_of(" \t");
        if (b != std::string::npos) out.push_back(one.substr(b, e - b + 1));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

bool parsePort(const std::string& t, PortRange& r) {
    if (t == "*") { r.lo = 1; r.hi = 65535; return true; }
    size_t d = t.find('-');
    int lo, hi;
    if (d == std::string::npos) {
        lo = hi = atoi(t.c_str());
    } else {
        lo = atoi(t.substr(0, d).c_str());
        hi = atoi(t.substr(d + 1).c_str());
    }
    if (lo < 1 || hi > 65535 || lo > hi) return false;
    r.lo = lo; r.hi = hi;
    return true;
}

std::vector<PortRange> parsePorts(const std::string& s) {
    std::vector<PortRange> out;
    for (auto& t : splitCsv(s)) {
        PortRange r;
        if (parsePort(t, r)) out.push_back(r);
    }
    return out;
}

std::vector<PortRange> intersectRanges(const std::vector<PortRange>& a, const std::vector<PortRange>& b) {
    std::vector<PortRange> out;
    for (auto& x : a) for (auto& y : b) {
        PortRange r;
        r.lo = std::max(x.lo, y.lo);
        r.hi = std::min(x.hi, y.hi);
        if (r.lo <= r.hi) out.push_back(r);
    }
    return out;
}

void subtractRange(std::vector<PortRange>& v, PortRange d) {
    std::vector<PortRange> out;
    for (auto& r : v) {
        if (d.hi < r.lo || d.lo > r.hi) { out.push_back(r); continue; }
        if (r.lo < d.lo) { PortRange x; x.lo = r.lo; x.hi = d.lo - 1; out.push_back(x); }
        if (r.hi > d.hi) { PortRange x; x.lo = d.hi + 1; x.hi = r.hi; out.push_back(x); }
    }
    v.swap(out);
}

std::string joinRanges(std::vector<PortRange> v) {
    if (v.empty()) return "";
    std::sort(v.begin(), v.end(), [](const PortRange& a, const PortRange& b) { return a.lo < b.lo; });
    std::vector<PortRange> merged;
    for (auto& r : v) {
        if (!merged.empty() && r.lo <= merged.back().hi + 1) merged.back().hi = std::max(merged.back().hi, r.hi);
        else merged.push_back(r);
    }
    if (merged.size() == 1 && merged[0].lo == 1 && merged[0].hi == 65535) return "*";
    std::string out;
    char buf[32];
    for (auto& r : merged) {
        if (!out.empty()) out += ",";
        if (r.lo == r.hi) snprintf(buf, sizeof(buf), "%d", r.lo);
        else snprintf(buf, sizeof(buf), "%d-%d", r.lo, r.hi);
        out += buf;
    }
    return out;
}

std::string trimWs(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

bool wildcard(const std::string& p) { return !p.empty() && p[p.size() - 1] == '*'; }
std::string wildcardPrefix(const std::string& p) { return p.substr(0, p.size() - 1); }

// cap 是否覆盖 acl 模式
bool ttyCovers(const std::string& cap, const std::string& acl) {
    if (cap == "*" || cap == acl) return true;
    if (wildcard(cap)) {
        std::string cp = wildcardPrefix(cap);
        if (wildcard(acl)) {
            std::string ap = wildcardPrefix(acl);
            return ap.compare(0, cp.size(), cp) == 0 || cp.compare(0, ap.size(), ap) == 0;
        }
        return acl.compare(0, cp.size(), cp) == 0;
    }
    if (wildcard(acl)) {
        std::string ap = wildcardPrefix(acl);
        return cap.compare(0, ap.size(), ap) == 0;
    }
    return false;
}

// 取交集中更窄的一项（前缀更长者更窄；字面量比通配窄）
std::string ttyNarrow(const std::string& cap, const std::string& acl) {
    if (cap == "*" || cap == acl) return acl;
    if (wildcard(cap)) {
        if (!wildcard(acl)) return acl;
        return wildcardPrefix(acl).size() >= wildcardPrefix(cap).size() ? acl : cap;
    }
    return cap;
}

std::string intersectTty(const std::string& acl, const std::string& cap, const std::set<std::string>& disabled) {
    std::vector<std::string> out;
    for (auto& a : splitCsv(acl)) {
        if (disabled.count(a)) continue;
        std::string keep;
        for (auto& c : splitCsv(cap)) {
            if (!ttyCovers(c, a)) continue;
            std::string n = ttyNarrow(c, a);
            if (keep.empty() || n.size() > keep.size()) keep = n;
        }
        if (keep.empty() || disabled.count(keep)) continue;
        if (std::find(out.begin(), out.end(), keep) == out.end()) out.push_back(keep);
    }
    std::string joined;
    for (auto& x : out) { if (!joined.empty()) joined += ","; joined += x; }
    return joined;
}

std::string intersectPorts(const std::string& acl, const std::string& cap, const std::vector<PortRange>& disabled) {
    std::vector<PortRange> r = intersectRanges(parsePorts(acl), parsePorts(cap));
    for (auto& d : disabled) subtractRange(r, d);
    return joinRanges(r);
}

std::vector<PortRange> disabledPortRanges(sqlite3* db) {
    std::vector<PortRange> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT port_start,port_end FROM port_dict WHERE enabled=0", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            PortRange r;
            r.lo = sqlite3_column_int(st, 0);
            r.hi = sqlite3_column_int(st, 1);
            out.push_back(r);
        }
    }
    sqlite3_finalize(st);
    return out;
}

std::set<std::string> disabledTtyPatterns(sqlite3* db) {
    std::set<std::string> out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT pattern FROM tty_dict WHERE enabled=0", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) out.insert((const char*)sqlite3_column_text(st, 0));
    }
    sqlite3_finalize(st);
    return out;
}

} // namespace

namespace {
void scopeFromGroupsLocked(sqlite3* db, const char* type, const std::string& id, DbPermScope& out);
}

// 数据面规则：接入账号 -> 权限组资源 + 设备集合
namespace {
void scopeFromGroupsLocked(sqlite3* db, const char* type, const std::string& id, DbPermScope& out);
DbPermScope scopeIntersect(const DbPermScope& a, const DbPermScope& b);
std::string subjectPortsLocked(sqlite3* db, const char* type, const std::string& id);
}

// 数据面规则：接入账号 -> 账号级规则（token_id=0），配了权限组的 rtk 令牌再生成令牌级规则。
// 令牌级规则 = 账号授权 ∩ 令牌组；有组但结果为空时靠 scoped_tokens 拒绝，而不是回落到账号规则。
bool AdminDb::permClientRules(std::vector<DbGroupAclRule>& out, std::set<std::string>& users,
                              std::set<long long>& scoped_tokens) {
    std::lock_guard<std::mutex> lk(mu_);
    out.clear();
    users.clear();
    scoped_tokens.clear();
    std::vector<PortRange> disabledPorts = disabledPortRanges(db_);
    std::set<std::string> disabledTtys = disabledTtyPatterns(db_);

    std::vector<std::pair<long long, std::string>> accounts;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,username FROM client_accounts WHERE status='active' ORDER BY id",
                           -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            accounts.emplace_back(sqlite3_column_int64(st, 0), dbColText(st, 1));
    }
    sqlite3_finalize(st);

    for (const auto& acc : accounts) {
        // 所有接入账号都纳入分组鉴权：未分组 = 无权限，不再回落到 --acl
        users.insert(acc.second);

        auto emitFor = [&](const DbPermScope& sc, const std::string& portsIn, long long token_id) {
            if (!sc.has_groups) return;
            std::string ports;
            if (sc.super || sc.resources.count("device.port")) ports = portsIn;
            ports = intersectPorts(ports, "*", disabledPorts);

            std::string tty;
            if (sc.super || sc.resources.count("device.tty.read") || sc.resources.count("device.tty.write"))
                tty = "*";
            tty = intersectTty(tty, "*", disabledTtys);

            DbGroupAclRule base;
            base.user = acc.second;
            base.token_id = token_id;
            base.ports = ports;
            base.tty = tty;
            base.shell = sc.super || sc.resources.count("device.terminal") > 0;
            base.file =
                sc.super ? 2 : sc.resources.count("file.write") ? 2 : sc.resources.count("file.read") ? 1 : 0;
            base.svc = sc.super || sc.resources.count("device.view") || sc.resources.count("device.terminal");
            auto emit = [&](const std::string& devid) {
                DbGroupAclRule r = base;
                r.devid = devid;
                out.push_back(r);
            };
            if (sc.super || sc.all_devices) emit("*");
            else for (const auto& d : sc.devices) emit(d);
        };

        DbPermScope accScope;
        scopeFromGroupsLocked(db_, "client", std::to_string(acc.first), accScope);
        std::string accPorts = subjectPortsLocked(db_, "client", std::to_string(acc.first));
        emitFor(accScope, accPorts, 0);

        // 该账号下未过期的 rtk 令牌：有令牌组的生成收窄规则
        std::vector<long long> token_ids;
        st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT id FROM client_tokens WHERE account_id=? AND status='active' AND revoked_ms=0"
                " AND (expires_at_ms=0 OR expires_at_ms>?) ORDER BY id",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, acc.first);
            sqlite3_bind_int64(st, 2, nowMs());
            while (sqlite3_step(st) == SQLITE_ROW) token_ids.push_back(sqlite3_column_int64(st, 0));
        }
        sqlite3_finalize(st);

        for (long long tid : token_ids) {
            DbPermScope tokScope;
            scopeFromGroupsLocked(db_, "rtk", std::to_string(tid), tokScope);
            if (!tokScope.has_groups) continue;
            scoped_tokens.insert(tid);
            DbPermScope eff = scopeIntersect(accScope, tokScope);
            std::string tokPorts =
                intersectPorts(accPorts, subjectPortsLocked(db_, "rtk", std::to_string(tid)), disabledPorts);
            emitFor(eff, tokPorts, tid);
        }
    }
    return true;
}

// 接入账号的当前生效权限（账号级，不含 rtk 令牌收窄）。
bool AdminDb::permEffectiveClient(const std::string& user, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    long long account_id = 0;
    std::string status;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,status FROM client_accounts WHERE username=?", -1, &st, nullptr) !=
        SQLITE_OK) {
        err = "db error";
        return false;
    }
    sqlite3_bind_text(st, 1, user.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        account_id = sqlite3_column_int64(st, 0);
        status = dbColText(st, 1);
    }
    sqlite3_finalize(st);
    if (!account_id) {
        err = "account not found";
        return false;
    }

    std::string sid = std::to_string(account_id);
    DbPermScope sc;
    scopeFromGroupsLocked(db_, "client", sid, sc);
    std::string ports = subjectPortsLocked(db_, "client", sid);
    std::vector<PortRange> disabledPorts = disabledPortRanges(db_);
    std::set<std::string> disabledTtys = disabledTtyPatterns(db_);
    auto has = [&](const char* k) { return sc.super || sc.resources.count(k) > 0; };

    json groups = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT g.name FROM perm_group_members m JOIN perm_groups g ON g.id=m.group_id"
            " WHERE m.subject_type='client' AND m.subject_id=? ORDER BY g.name",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) groups.push_back(dbColText(st, 0));
    }
    sqlite3_finalize(st);

    json devices = json::array();
    if (sc.super || sc.all_devices) {
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT devid FROM res_devices WHERE enabled=1 ORDER BY devid", -1, &st,
                               nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) devices.push_back(dbColText(st, 0));
        }
        sqlite3_finalize(st);
    } else {
        for (const auto& d : sc.devices) devices.push_back(d);
    }

    json resources = json::array();
    if (sc.super) {
        resources.push_back("*");
    } else {
        for (const auto& k : sc.resources) resources.push_back(k);
    }

    json caps = json::object();
    caps["tcp_ports"] = has("device.port") ? intersectPorts(ports, "*", disabledPorts) : "";
    caps["shell"] = has("device.terminal") ? "1" : "0";
    caps["tty"] = (has("device.tty.read") || has("device.tty.write")) ? intersectTty("*", "*", disabledTtys) : "";
    caps["file"] = has("file.write") ? "rw" : (has("file.read") ? "ro" : "none");
    caps["sysinfo"] = has("device.view") ? "1" : "0";
    caps["metrics"] = has("device.view") ? "1" : "0";
    caps["config"] = has("device.caps.write") ? "1" : "0";
    caps["web"] = has("device.view") ? "1" : "0";

    json out;
    out["client"] = user;
    out["status"] = status;
    out["has_groups"] = sc.has_groups;
    out["super"] = sc.super;
    out["groups"] = groups;
    out["devices"] = devices;
    out["resources"] = resources;
    out["caps"] = caps;
    json_out = out.dump();
    return true;
}

// ---------------------------------------------------------------- 资源权限

namespace {

struct PermResDef {
    const char* key;
    const char* cat;
    const char* name;
    const char* desc;
};

// 所有可授权的动作都在这里登记；新增接口时先加资源再加路由。
const PermResDef kPermRes[] = {
    {"device.view", "设备", "查看设备", "设备列表、详情、概览、指标曲线与会话"},
    {"device.terminal", "设备", "打开终端", "建立 Shell / 串口调试会话"},
    {"device.port", "设备", "端口转发", "按权限组的端口范围建立 TCP 转发"},
    {"device.caps.write", "设备", "修改能力配置", "转发端口、Shell、文件与服务能力"},
    {"device.ota.write", "设备", "OTA 升级开关", "允许/禁止设备接收升级包"},
    {"device.tty.read", "设备", "查看串口", "串口配置、发现列表与日志"},
    {"device.tty.write", "设备", "修改串口", "串口配置与 allow_tty 白名单"},
    {"device.metrics.write", "设备", "修改采样", "采样指标、间隔与保留点数"},
    {"device.kick", "设备", "断开连接", "强制下线设备或结束会话"},

    {"file.read", "文件", "查看文件", "仓库列表、下载、Hex 与尾部"},
    {"file.write", "文件", "写入文件", "上传、新建、编辑、移动"},
    {"file.delete", "文件", "删除文件", "删除文件或目录、打包下载"},

    {"dist.read", "分发", "查看任务", "分发与 OTA 任务列表、详情"},
    {"dist.write", "分发", "创建与调度", "创建、重试、取消、延长分发任务"},

    {"dict.read", "权限", "查看字典", "资源/设备/端口/串口字典与授权对象"},
    {"dict.write", "权限", "维护字典", "字典增删改、设备资源启停与改名"},

    {"account.read", "账号", "查看账号", "管理台账号与接入账号"},
    {"account.write", "账号", "修改账号", "新建、改角色/状态/密码、调整权限组"},
    {"account.delete", "账号", "删除账号", "删除管理台或接入账号"},

    {"token.read", "令牌", "查看 Token", "管理台 API Token 列表"},
    {"token.write", "令牌", "管理 Token", "创建、轮换、吊销 Token"},

    {"cert.read", "证书", "查看证书", "证书台账与到期状态"},
    {"cert.write", "证书", "签发证书", "签发、吊销、签署证书"},

    {"audit.read", "审计", "查看审计", "管理台操作审计日志"},

    {"settings.read", "系统", "查看设置", "系统设置与告警状态"},
    {"settings.write", "系统", "修改设置", "系统设置与告警测试"},
    {"system.shutdown", "系统", "关闭 relay", "停止 relay 进程"},
};

struct PermGroupDef {
    const char* key;
    const char* name;
    const char* desc;
    int super;
};

// 预置权限组；builtin_key 与 users.role 对应，用于账号未入组时的兜底
const PermGroupDef kPermGroups[] = {
    {"admin", "超级管理员", "全部资源，含系统设置、证书与关机", 1},
    {"manager", "管理员", "日常运维：设备、文件、分发、权限字典、账号与令牌", 0},
    {"operator", "操作员", "查看设备、使用终端、上传文件、查看字典", 0},
    {"viewer", "观察者", "只读查看设备与设置", 0},
};

std::vector<std::string> presetResources(const std::string& builtin) {
    std::vector<std::string> out;
    for (const auto& r : kPermRes) {
        std::string k = r.key;
        bool allow = false;
        if (builtin == "admin") {
            allow = true;
        } else if (builtin == "manager") {
            allow =                     k != "settings.write" && k != "system.shutdown" && k != "cert.write" &&
                    k != "account.delete";
        } else if (builtin == "operator") {
            allow = k == "device.view" || k == "device.terminal" || k == "device.port" ||
                    k == "device.tty.read" || k == "file.read" || k == "file.write" ||
                    k == "dict.read" || k == "account.read" || k == "settings.read";
        } else if (builtin == "viewer") {
            allow = k == "device.view" || k == "device.tty.read" || k == "settings.read";
        }
        if (allow) out.push_back(k);
    }
    return out;
}

} // namespace

bool AdminDb::permSeed() {
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) return false;

    // 1) 资源字典：只补新增资源，不覆盖管理员改过的名称与启用状态
    for (int i = 0; i < (int)(sizeof(kPermRes) / sizeof(kPermRes[0])); ++i) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT OR IGNORE INTO perm_resources(key,category,name,description,sort,enabled,updated_ms)"
                " VALUES(?,?,?,?,?,1,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            return false;
        }
        sqlite3_bind_text(st, 1, kPermRes[i].key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, kPermRes[i].cat, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, kPermRes[i].name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, kPermRes[i].desc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 5, i * 10);
        sqlite3_bind_int64(st, 6, nowMs());
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        bool inserted = ok && sqlite3_changes(db_) > 0;
        sqlite3_finalize(st);
        if (!ok) {
            exec("ROLLBACK", e);
            return false;
        }
        // 升级时新增的资源：按预置规则补进内置组，不影响管理员对旧资源的调整
        if (inserted) {
            for (const auto& g : kPermGroups) {
                if (g.super) continue;
                bool want = false;
                for (const auto& k : presetResources(g.key))
                    if (k == kPermRes[i].key) { want = true; break; }
                if (!want) continue;
                sqlite3_stmt* a = nullptr;
                if (sqlite3_prepare_v2(db_,
                        "INSERT OR IGNORE INTO perm_group_resources(group_id,resource_key,allowed)"
                        " SELECT id,?,1 FROM perm_groups WHERE builtin_key=?",
                        -1, &a, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(a, 1, kPermRes[i].key, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(a, 2, g.key, -1, SQLITE_TRANSIENT);
                    sqlite3_step(a);
                }
                sqlite3_finalize(a);
            }
        }
    }

    // 2) 清理代码里已删除的资源键，避免旧库在下拉和矩阵里残留
    {
        std::vector<std::string> stale;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT key FROM perm_resources", -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string k = dbColText(st, 0);
                bool known = false;
                for (int i = 0; i < (int)(sizeof(kPermRes) / sizeof(kPermRes[0])); ++i) {
                    if (k == kPermRes[i].key) {
                        known = true;
                        break;
                    }
                }
                if (!known) stale.push_back(k);
            }
        }
        sqlite3_finalize(st);
        for (const auto& k : stale) {
            st = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_resources WHERE resource_key=?", -1, &st,
                                   nullptr) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
            st = nullptr;
            if (sqlite3_prepare_v2(db_, "DELETE FROM perm_resources WHERE key=?", -1, &st, nullptr) ==
                SQLITE_OK) {
                sqlite3_bind_text(st, 1, k.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
        }
    }

    // 3) 预置权限组 + 组资源
    for (const auto& g : kPermGroups) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "INSERT OR IGNORE INTO perm_groups(name,description,builtin_key,super,created_ms,updated_ms)"
                " VALUES(?,?,?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            return false;
        }
        sqlite3_bind_text(st, 1, g.name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, g.desc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, g.key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, g.super);
        sqlite3_bind_int64(st, 5, nowMs());
        sqlite3_bind_int64(st, 6, nowMs());
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) {
            exec("ROLLBACK", e);
            return false;
        }
        long long gid = 0;
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM perm_groups WHERE builtin_key=?", -1, &st, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_text(st, 1, g.key, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) gid = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
        if (!gid) {
            // 同名组被手工建过：认领为内置组
            if (sqlite3_prepare_v2(db_, "UPDATE perm_groups SET builtin_key=?,super=? WHERE name=?", -1, &st,
                                   nullptr) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, g.key, -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(st, 2, g.super);
                sqlite3_bind_text(st, 3, g.name, -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
            st = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT id FROM perm_groups WHERE name=?", -1, &st, nullptr) ==
                SQLITE_OK) {
                sqlite3_bind_text(st, 1, g.name, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) gid = sqlite3_column_int64(st, 0);
            }
            sqlite3_finalize(st);
        }
        if (!gid) {
            exec("ROLLBACK", e);
            return false;
        }
        int seeded = 0;
        st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT seeded FROM perm_groups WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, gid);
            if (sqlite3_step(st) == SQLITE_ROW) seeded = sqlite3_column_int(st, 0);
        }
        sqlite3_finalize(st);
        if (seeded) continue;   // 已初始化过，保留管理员的勾选结果
        if (g.super) {
            sqlite3_prepare_v2(db_, "UPDATE perm_groups SET seeded=1 WHERE id=?", -1, &st, nullptr);
            sqlite3_bind_int64(st, 1, gid);
            sqlite3_step(st);
            sqlite3_finalize(st);
            continue;
        }
        for (const auto& key : presetResources(g.key)) {
            if (sqlite3_prepare_v2(db_,
                    "INSERT OR IGNORE INTO perm_group_resources(group_id,resource_key,allowed) VALUES(?,?,1)",
                    -1, &st, nullptr) != SQLITE_OK) {
                exec("ROLLBACK", e);
                return false;
            }
            sqlite3_bind_int64(st, 1, gid);
            sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
            ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok) {
                exec("ROLLBACK", e);
                return false;
            }
        }
        sqlite3_prepare_v2(db_, "UPDATE perm_groups SET seeded=1 WHERE id=?", -1, &st, nullptr);
        sqlite3_bind_int64(st, 1, gid);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }

    // 4) 已有组成员关系的老账号视为显式分配，避免删组后回落到角色基线；
    //    未入组账号不落库，由 permsOf 按 role 动态回落（只回落资源，不含设备）

    exec("UPDATE users SET perm_explicit=1 WHERE perm_explicit=0 AND EXISTS("
         "SELECT 1 FROM perm_group_members m WHERE m.subject_type='user'"
         " AND m.subject_id=CAST(users.id AS TEXT))",
         e);

    exec("COMMIT", e);
    return true;
}

bool AdminDb::permResources(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT key,category,name,description,sort,enabled FROM perm_resources ORDER BY sort,key", -1, &st,
            nullptr) != SQLITE_OK)
        return false;
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["key"] = dbColText(st, 0);
        j["category"] = dbColText(st, 1);
        j["name"] = dbColText(st, 2);
        j["description"] = dbColText(st, 3);
        j["sort"] = (int)sqlite3_column_int(st, 4);
        j["enabled"] = sqlite3_column_int(st, 5) != 0;
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::permResourceUpdate(const std::string& key, const std::string& name, const std::string& desc,
                                 int enabled, std::string& err) {
    if (key.empty()) {
        err = "bad resource";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "UPDATE perm_resources SET name=?,description=?,enabled=?,updated_ms=? WHERE key=?", -1, &st,
            nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, enabled ? 1 : 0);
    sqlite3_bind_int64(st, 4, nowMs());
    sqlite3_bind_text(st, 5, key.c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) err = sqlite3_errmsg(db_);
    else if (sqlite3_changes(db_) == 0) { err = "资源不存在"; ok = false; }
    return ok;
}

bool AdminDb::permGroups(std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id,name,description,builtin_key,super,all_devices,ports,created_ms,updated_ms"
            " FROM perm_groups ORDER BY super DESC, id",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    json arr = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        long long gid = sqlite3_column_int64(st, 0);
        j["id"] = gid;
        j["name"] = dbColText(st, 1);
        j["description"] = dbColText(st, 2);
        j["builtin_key"] = dbColText(st, 3);
        j["super"] = sqlite3_column_int(st, 4) != 0;
        j["all_devices"] = sqlite3_column_int(st, 5) != 0 || j["super"].get<bool>();
        j["ports"] = dbColText(st, 6);
        j["created_ms"] = (long long)sqlite3_column_int64(st, 7);
        j["updated_ms"] = (long long)sqlite3_column_int64(st, 8);
        {
            sqlite3_stmt* m = nullptr;
            int n = 0;
            if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM perm_group_members WHERE group_id=?", -1, &m,
                                   nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(m, 1, gid);
                if (sqlite3_step(m) == SQLITE_ROW) n = (int)sqlite3_column_int(m, 0);
            }
            sqlite3_finalize(m);
            j["member_count"] = n;
        }
        {
            sqlite3_stmt* d = nullptr;
            int n = 0;
            if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM perm_group_devices WHERE group_id=?", -1, &d,
                                   nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(d, 1, gid);
                if (sqlite3_step(d) == SQLITE_ROW) n = (int)sqlite3_column_int(d, 0);
            }
            sqlite3_finalize(d);
            j["device_count"] = n;
        }
        json list = json::array();
        {
            sqlite3_stmt* r = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "SELECT resource_key FROM perm_group_resources WHERE group_id=? AND allowed=1"
                    " ORDER BY resource_key",
                    -1, &r, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(r, 1, gid);
                while (sqlite3_step(r) == SQLITE_ROW) list.push_back(dbColText(r, 0));
            }
            sqlite3_finalize(r);
        }
        j["resources"] = list;
        arr.push_back(j);
    }
    sqlite3_finalize(st);
    json_out = arr.dump();
    return true;
}

bool AdminDb::permGroupDetail(long long id, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT name,description,builtin_key,super,all_devices,ports,created_ms,updated_ms"
            " FROM perm_groups WHERE id=?",
            -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        err = "权限组不存在";
        return false;
    }
    json g;
    g["id"] = id;
    g["name"] = dbColText(st, 0);
    g["description"] = dbColText(st, 1);
    g["builtin_key"] = dbColText(st, 2);
    g["super"] = sqlite3_column_int(st, 3) != 0;
    g["all_devices"] = sqlite3_column_int(st, 4) != 0;
    g["ports"] = dbColText(st, 5);
    g["created_ms"] = (long long)sqlite3_column_int64(st, 6);
    g["updated_ms"] = (long long)sqlite3_column_int64(st, 7);
    sqlite3_finalize(st);

    json resources = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT resource_key FROM perm_group_resources WHERE group_id=? AND allowed=1"
            " ORDER BY resource_key",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) resources.push_back(dbColText(st, 0));
    }
    sqlite3_finalize(st);
    g["resources"] = resources;

    json devices = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT g.devid FROM perm_group_devices g LEFT JOIN res_devices d ON d.devid=g.devid"
            " WHERE g.group_id=? ORDER BY g.devid",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) devices.push_back(dbColText(st, 0));
    }
    sqlite3_finalize(st);
    g["devices"] = devices;

    json members = json::array();
    st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT subject_type,subject_id FROM perm_group_members WHERE group_id=?"
            " ORDER BY subject_type,subject_id",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) {
            json m;
            m["type"] = dbColText(st, 0);
            m["id"] = dbColText(st, 1);
            members.push_back(m);
        }
    }
    sqlite3_finalize(st);
    g["members"] = members;
    json_out = g.dump();
    return true;
}

bool AdminDb::permGroupSave(long long id, const std::string& name, const std::string& desc,
                            const std::vector<std::string>& resources,
                            const std::vector<std::string>& devices,
                            int all_devices, const std::string& ports,
                            long long& out_id, std::string& err) {
    return permGroupSaveAll(id, name, desc, resources, devices, all_devices, ports, false, {}, out_id, err);
}

bool AdminDb::permGroupSaveAll(long long id, const std::string& name, const std::string& desc,
                               const std::vector<std::string>& resources,
                               const std::vector<std::string>& devices,
                               int all_devices, const std::string& ports, bool set_members,
                               const std::vector<std::pair<std::string, std::string>>& members,
                               long long& out_id, std::string& err) {
    if (name.empty() || name.size() > 64) {
        err = "权限组名称不能为空且不超过 64 字";
        return false;
    }
    if (ports.size() > 256) {
        err = "端口范围过长";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) {
        err = e;
        return false;
    }
    sqlite3_stmt* st = nullptr;
    if (id > 0) {
        if (sqlite3_prepare_v2(db_,
                "UPDATE perm_groups SET name=?,description=?,all_devices=?,ports=?,updated_ms=? WHERE id=?",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, all_devices ? 1 : 0);
        sqlite3_bind_text(st, 4, ports.empty() ? "*" : ports.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, nowMs());
        sqlite3_bind_int64(st, 6, id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            exec("ROLLBACK", e);
            err = (rc == SQLITE_CONSTRAINT) ? "权限组名称已存在" : sqlite3_errmsg(db_);
            return false;
        }
    } else {
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO perm_groups(name,description,builtin_key,super,seeded,all_devices,ports,"
                "created_ms,updated_ms) VALUES(?,?,'',0,1,?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, all_devices ? 1 : 0);
        sqlite3_bind_text(st, 4, ports.empty() ? "*" : ports.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, nowMs());
        sqlite3_bind_int64(st, 6, nowMs());
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            exec("ROLLBACK", e);
            err = (rc == SQLITE_CONSTRAINT) ? "权限组名称已存在" : sqlite3_errmsg(db_);
            return false;
        }
        id = sqlite3_last_insert_rowid(db_);
    }
    out_id = id;

    bool is_super = false;
    if (sqlite3_prepare_v2(db_, "SELECT super FROM perm_groups WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) is_super = sqlite3_column_int(st, 0) != 0;
    }
    sqlite3_finalize(st);

    if (!is_super) {
        // 功能资源：整组覆盖
        if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_resources WHERE group_id=?", -1, &st, nullptr) !=
            SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        for (const auto& key : resources) {
            if (sqlite3_prepare_v2(db_,
                    "INSERT OR IGNORE INTO perm_group_resources(group_id,resource_key,allowed) VALUES(?,?,1)",
                    -1, &st, nullptr) != SQLITE_OK) {
                exec("ROLLBACK", e);
                err = sqlite3_errmsg(db_);
                return false;
            }
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_text(st, 2, key.c_str(), -1, SQLITE_TRANSIENT);
            bool ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok) {
                exec("ROLLBACK", e);
                err = "写入资源失败";
                return false;
            }
        }
    }

    // 设备资源：整组覆盖（超级组忽略，因为默认全部设备）
    if (!is_super) {
        if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_devices WHERE group_id=?", -1, &st, nullptr) !=
            SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        if (!all_devices) {
            for (const auto& devid : devices) {
                if (devid.empty() || devid.size() > 128) continue;
                if (sqlite3_prepare_v2(db_,
                        "INSERT OR IGNORE INTO perm_group_devices(group_id,devid,added_ms) VALUES(?,?,?)",
                        -1, &st, nullptr) != SQLITE_OK) {
                    exec("ROLLBACK", e);
                    err = sqlite3_errmsg(db_);
                    return false;
                }
                sqlite3_bind_int64(st, 1, id);
                sqlite3_bind_text(st, 2, devid.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(st, 3, nowMs());
                bool ok = sqlite3_step(st) == SQLITE_DONE;
                sqlite3_finalize(st);
                if (!ok) {
                    exec("ROLLBACK", e);
                    err = "写入设备失败";
                    return false;
                }
            }
        }
    }

    // 成员：调用方显式要求时才覆盖（空数组 = 清空）
    if (set_members) {
        if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_members WHERE group_id=?", -1, &st, nullptr) !=
            SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st, 1, id);
        sqlite3_step(st);
        sqlite3_finalize(st);
        for (const auto& m : members) {
            if (m.first != "user" && m.first != "atk" && m.first != "client" && m.first != "rtk") continue;
            if (m.second.empty() || m.second.size() > 64) continue;
            if (sqlite3_prepare_v2(db_,
                    "INSERT OR IGNORE INTO perm_group_members(group_id,subject_type,subject_id,added_ms)"
                    " VALUES(?,?,?,?)",
                    -1, &st, nullptr) != SQLITE_OK) {
                exec("ROLLBACK", e);
                err = sqlite3_errmsg(db_);
                return false;
            }
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_text(st, 2, m.first.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, m.second.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 4, nowMs());
            bool ok = sqlite3_step(st) == SQLITE_DONE;
            sqlite3_finalize(st);
            if (!ok) {
                exec("ROLLBACK", e);
                err = "写入成员失败";
                return false;
            }
            // 账号被显式分配过：之后不再按 role 回落
            if (m.first == "user") {
                sqlite3_stmt* u = nullptr;
                if (sqlite3_prepare_v2(db_, "UPDATE users SET perm_explicit=1 WHERE id=?", -1, &u, nullptr) ==
                    SQLITE_OK) {
                    sqlite3_bind_int64(u, 1, atoll(m.second.c_str()));
                    sqlite3_step(u);
                    sqlite3_finalize(u);
                }
            }
        }
        if (sqlite3_prepare_v2(db_, "UPDATE users SET perm_explicit=1 WHERE perm_explicit=0 AND EXISTS("
                                   "SELECT 1 FROM perm_group_members mm WHERE mm.subject_type='user'"
                                   " AND mm.subject_id=CAST(users.id AS TEXT))", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
    }

    exec("COMMIT", e);
    return true;
}

bool AdminDb::permGroupDelete(long long id, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT builtin_key FROM perm_groups WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    std::string builtin = found ? dbColText(st, 0) : "";
    sqlite3_finalize(st);
    if (!found) {
        err = "权限组不存在";
        return false;
    }
    if (!builtin.empty()) {
        err = "预置权限组不能删除";
        return false;
    }
    std::string e;
    if (!exec("BEGIN", e)) {
        err = e;
        return false;
    }
    exec(("DELETE FROM perm_group_resources WHERE group_id=" + std::to_string(id)).c_str(), e);
    exec(("DELETE FROM perm_group_devices WHERE group_id=" + std::to_string(id)).c_str(), e);
    exec(("DELETE FROM perm_group_members WHERE group_id=" + std::to_string(id)).c_str(), e);
    exec(("DELETE FROM perm_groups WHERE id=" + std::to_string(id)).c_str(), e);
    exec("COMMIT", e);
    return true;
}

bool AdminDb::permGroupMembers(long long id, std::string& json_out, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT name FROM perm_groups WHERE id=?", -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    std::string name = found ? dbColText(st, 0) : "";
    sqlite3_finalize(st);
    if (!found) {
        err = "权限组不存在";
        return false;
    }
    json out;
    out["id"] = id;
    out["name"] = name;
    out["members"] = json::parse(permMembersOfGroupLocked(id), nullptr, false);
    json_out = out.dump();
    return true;
}

// 组装权限组成员候选（4 类），返回 JSON 数组字符串；调用方需持锁
std::string AdminDb::permMembersOfGroupLocked(long long gid) {
    json arr = json::array();
    auto inGroup = [&](const char* type, const std::string& id) {
        sqlite3_stmt* st = nullptr;
        bool yes = false;
        if (sqlite3_prepare_v2(db_,
                "SELECT 1 FROM perm_group_members WHERE group_id=? AND subject_type=? AND subject_id=?",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, gid);
            sqlite3_bind_text(st, 2, type, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 3, id.c_str(), -1, SQLITE_TRANSIENT);
            yes = sqlite3_step(st) == SQLITE_ROW;
        }
        sqlite3_finalize(st);
        return yes;
    };
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id,username,role,status FROM users ORDER BY id", -1, &st, nullptr) ==
            SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string id = std::to_string((long long)sqlite3_column_int64(st, 0));
                json j;
                j["type"] = "user";
                j["id"] = id;
                j["name"] = dbColText(st, 1);
                j["detail"] = dbColText(st, 2);
                j["status"] = dbColText(st, 3);
                j["in_group"] = inGroup("user", id);
                arr.push_back(j);
            }
        }
        sqlite3_finalize(st);
    }
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT t.id,t.name,t.token_prefix,t.username,t.status FROM admin_tokens t"
                " ORDER BY t.id",
                -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string id = std::to_string((long long)sqlite3_column_int64(st, 0));
                json j;
                j["type"] = "atk";
                j["id"] = id;
                j["name"] = dbColText(st, 1).empty() ? dbColText(st, 2) : dbColText(st, 1);
                j["detail"] = "归属 " + dbColText(st, 3);
                j["status"] = dbColText(st, 4);
                j["in_group"] = inGroup("atk", id);
                arr.push_back(j);
            }
        }
        sqlite3_finalize(st);
    }
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id,username,display_name,status FROM client_accounts ORDER BY id",
                               -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string id = std::to_string((long long)sqlite3_column_int64(st, 0));
                json j;
                j["type"] = "client";
                j["id"] = id;
                j["name"] = dbColText(st, 1);
                j["detail"] = dbColText(st, 2);
                j["status"] = dbColText(st, 3);
                j["in_group"] = inGroup("client", id);
                arr.push_back(j);
            }
        }
        sqlite3_finalize(st);
    }
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT t.id,t.name,t.token_prefix,a.username,t.status FROM client_tokens t"
                " JOIN client_accounts a ON a.id=t.account_id ORDER BY t.id",
                -1, &st, nullptr) == SQLITE_OK) {
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string id = std::to_string((long long)sqlite3_column_int64(st, 0));
                json j;
                j["type"] = "rtk";
                j["id"] = id;
                j["name"] = dbColText(st, 1).empty() ? dbColText(st, 2) : dbColText(st, 1);
                j["detail"] = "归属 " + dbColText(st, 3);
                j["status"] = dbColText(st, 4);
                j["in_group"] = inGroup("rtk", id);
                arr.push_back(j);
            }
        }
        sqlite3_finalize(st);
    }
    return arr.dump();
}

bool AdminDb::permMembersOfGroup(long long gid, std::string& json_out) {
    std::lock_guard<std::mutex> lk(mu_);
    json_out = permMembersOfGroupLocked(gid);
    return true;
}

bool AdminDb::permGroupSetMembers(long long id,
                                  const std::vector<std::pair<std::string, std::string>>& members,
                                  std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) {
        err = e;
        return false;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_members WHERE group_id=?", -1, &st, nullptr) !=
        SQLITE_OK) {
        exec("ROLLBACK", e);
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    sqlite3_step(st);
    sqlite3_finalize(st);
    for (const auto& m : members) {
        if (m.first != "user" && m.first != "atk" && m.first != "client" && m.first != "rtk") continue;
        if (m.second.empty() || m.second.size() > 64) continue;
        if (sqlite3_prepare_v2(db_,
                "INSERT OR IGNORE INTO perm_group_members(group_id,subject_type,subject_id,added_ms)"
                " VALUES(?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_text(st, 2, m.first.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, m.second.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, nowMs());
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) {
            exec("ROLLBACK", e);
            err = "写入成员失败";
            return false;
        }
    }
    exec("UPDATE users SET perm_explicit=1 WHERE perm_explicit=0 AND EXISTS("
         "SELECT 1 FROM perm_group_members mm WHERE mm.subject_type='user'"
         " AND mm.subject_id=CAST(users.id AS TEXT))",
         e);
    exec("COMMIT", e);
    return true;
}

bool AdminDb::permGroupsOfUser(long long user_id, std::vector<long long>& ids) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT group_id FROM perm_group_members WHERE subject_type='user' AND subject_id=? ORDER BY group_id",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    std::string sid = std::to_string(user_id);
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::permSetUserGroups(long long user_id, const std::string& role, const std::vector<long long>& ids,
                                std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) {
        err = e;
        return false;
    }
    std::string sid = std::to_string(user_id);
    sqlite3_stmt* st = nullptr;
    // 显式分配过权限组后，不再按 role 回落预置组（即使清空全部组）
    if (sqlite3_prepare_v2(db_, "UPDATE users SET perm_explicit=1 WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, user_id);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
    st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_members WHERE subject_type='user' AND subject_id=?", -1, &st,
                           nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e);
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

    int rank = 0;
    std::string new_role = role;
    for (long long gid : ids) {
        if (sqlite3_prepare_v2(db_, "INSERT OR IGNORE INTO perm_group_members(group_id,subject_type,subject_id,added_ms)"
                                   " VALUES(?, 'user', ?, ?)",
                               -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st, 1, gid);
        sqlite3_bind_text(st, 2, sid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, nowMs());
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) {
            exec("ROLLBACK", e);
            err = "写入权限组失败";
            return false;
        }
        std::string bk;
        if (sqlite3_prepare_v2(db_, "SELECT builtin_key FROM perm_groups WHERE id=?", -1, &st, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_int64(st, 1, gid);
            if (sqlite3_step(st) == SQLITE_ROW) bk = dbColText(st, 0);
        }
        sqlite3_finalize(st);
        int r = bk == "admin" ? 4 : bk == "manager" ? 3 : bk == "operator" ? 2 : bk == "viewer" ? 1 : 0;
        if (r > rank) {
            rank = r;
            new_role = bk;
        }
    }
    if (rank > 0 && new_role != role) {
        if (sqlite3_prepare_v2(db_, "UPDATE users SET role=? WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, new_role.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(st, 2, user_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    exec("COMMIT", e);
    return true;
}

bool AdminDb::permsOf(long long user_id, const std::string& role, std::set<std::string>& out, bool& super) {
    DbPermScope sc;
    permScopeOfUser(user_id, role, sc);
    out = sc.resources;
    super = sc.super;
    return true;
}

bool AdminDb::permResourcesPage(const std::string& q, int page, int page_size, std::string& json_out) {
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 10;
    if (page_size > 200) page_size = 200;
    std::lock_guard<std::mutex> lk(mu_);
    std::string where;
    if (!q.empty()) where = " WHERE key LIKE ? OR name LIKE ? OR description LIKE ?";

    long long total = 0;
    sqlite3_stmt* st = nullptr;
    std::string csql = "SELECT COUNT(*) FROM perm_resources" + where;
    if (sqlite3_prepare_v2(db_, csql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    if (!q.empty()) {
        std::string like = "%" + q + "%";
        sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, like.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    std::string sql = "SELECT key,category,name,description,sort,enabled FROM perm_resources" + where +
                      " ORDER BY sort,key LIMIT ? OFFSET ?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    if (!q.empty()) {
        std::string like = "%" + q + "%";
        sqlite3_bind_text(st, idx++, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, idx++, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, idx++, like.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st, idx++, page_size);
    sqlite3_bind_int(st, idx++, (page - 1) * page_size);
    json items = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["key"] = dbColText(st, 0);
        j["category"] = dbColText(st, 1);
        j["name"] = dbColText(st, 2);
        j["description"] = dbColText(st, 3);
        j["sort"] = (int)sqlite3_column_int(st, 4);
        j["enabled"] = sqlite3_column_int(st, 5) != 0;
        items.push_back(j);
    }
    sqlite3_finalize(st);

    json out;
    out["kind"] = "resources";
    out["page"] = page;
    out["page_size"] = page_size;
    out["total"] = total;
    out["items"] = items;
    json_out = out.dump();
    return true;
}

bool AdminDb::permGroupsOfClient(long long account_id, std::vector<long long>& ids) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT group_id FROM perm_group_members WHERE subject_type='client' AND subject_id=?"
            " ORDER BY group_id",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    std::string sid = std::to_string(account_id);
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::permSetClientGroups(long long account_id, const std::vector<long long>& ids, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    std::string sid = std::to_string(account_id);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "DELETE FROM perm_group_members WHERE subject_type='client' AND subject_id=?", -1, &st,
            nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e);
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    for (long long gid : ids) {
        if (sqlite3_prepare_v2(db_,
                "INSERT OR IGNORE INTO perm_group_members(group_id,subject_type,subject_id,added_ms)"
                " VALUES(?,'client',?,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st, 1, gid);
        sqlite3_bind_text(st, 2, sid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, nowMs());
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) {
            exec("ROLLBACK", e);
            err = "写入权限组失败";
            return false;
        }
    }
    exec("COMMIT", e);
    return true;
}

bool AdminDb::clientPermsOf(long long account_id, std::set<std::string>& out, bool& super) {
    out.clear();
    super = false;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    const char* sql =
        "SELECT g.super,r.resource_key FROM perm_group_members m"
        " JOIN perm_groups g ON g.id=m.group_id"
        " JOIN perm_group_resources r ON r.group_id=g.id AND r.allowed=1"
        " JOIN perm_resources p ON p.key=r.resource_key AND p.enabled=1"
        " WHERE m.subject_type='client' AND m.subject_id=?";
    if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) return false;
    std::string sid = std::to_string(account_id);
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (sqlite3_column_int(st, 0) != 0) super = true;
        std::string k = dbColText(st, 1);
        if (!k.empty()) out.insert(k);
    }
    sqlite3_finalize(st);
    // 超级组没有逐条资源，单独判断一次
    st = nullptr;
    if (!super &&
        sqlite3_prepare_v2(db_, "SELECT MAX(g.super) FROM perm_group_members m"
                               " JOIN perm_groups g ON g.id=m.group_id"
                               " WHERE m.subject_type='client' AND m.subject_id=?",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) super = sqlite3_column_int(st, 0) != 0;
    }
    sqlite3_finalize(st);
    return true;
}


// ---------------------------------------------------------------- 有效授权与设备资源

namespace {

DbPermScope scopeIntersect(const DbPermScope& a, const DbPermScope& b) {
    if (a.super) return b;
    if (b.super) return a;
    DbPermScope r;
    r.has_groups = a.has_groups || b.has_groups;
    r.all_devices = a.all_devices && b.all_devices;
    for (const auto& k : a.resources)
        if (b.resources.count(k)) r.resources.insert(k);
    if (r.all_devices) {
        r.devices.clear();
    } else if (a.all_devices) {
        r.devices = b.devices;
    } else if (b.all_devices) {
        r.devices = a.devices;
    } else {
        for (const auto& d : a.devices)
            if (b.devices.count(d)) r.devices.insert(d);
    }
    return r;
}

// 某个主体（user/atk/client/rtk）所有权限组的并集
void scopeFromGroupsLocked(sqlite3* db, const char* type, const std::string& id, DbPermScope& out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT g.id,g.super,g.all_devices FROM perm_group_members m"
            " JOIN perm_groups g ON g.id=m.group_id"
            " WHERE m.subject_type=? AND m.subject_id=?",
            -1, &st, nullptr) != SQLITE_OK)
        return;
    sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) {
        out.has_groups = true;
        if (sqlite3_column_int(st, 1) != 0) out.super = true;
        if (sqlite3_column_int(st, 2) != 0) out.all_devices = true;
    }
    sqlite3_finalize(st);
    if (!out.has_groups) return;

    // 资源（停用的资源不生效）
    st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT DISTINCT r.resource_key FROM perm_group_members m"
            " JOIN perm_group_resources r ON r.group_id=m.group_id AND r.allowed=1"
            " JOIN perm_resources p ON p.key=r.resource_key AND p.enabled=1"
            " WHERE m.subject_type=? AND m.subject_id=?",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            std::string k = dbColText(st, 0);
            if (!k.empty()) out.resources.insert(k);
        }
    }
    sqlite3_finalize(st);

    // 设备（禁用设备不生效）
    st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT DISTINCT d.devid FROM perm_group_members m"
            " JOIN perm_group_devices d ON d.group_id=m.group_id"
            " LEFT JOIN res_devices rd ON rd.devid=d.devid"
            " WHERE m.subject_type=? AND m.subject_id=? AND (rd.devid IS NULL OR rd.enabled=1)",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) out.devices.insert(dbColText(st, 0));
    }
    sqlite3_finalize(st);
}

// 主体所有权限组 ports 的并集（组级端口范围）
std::string subjectPortsLocked(sqlite3* db, const char* type, const std::string& id) {
    sqlite3_stmt* st = nullptr;
    std::vector<PortRange> rs;
    if (sqlite3_prepare_v2(db,
            "SELECT g.ports FROM perm_group_members m JOIN perm_groups g ON g.id=m.group_id"
            " WHERE m.subject_type=? AND m.subject_id=?",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            for (auto& r : parsePorts(dbColText(st, 0))) rs.push_back(r);
        }
    }
    sqlite3_finalize(st);
    return joinRanges(rs);
}

// 未显式分配权限组时按 users.role 回落预置组；设备只由权限组授权
void fallbackRoleLocked(sqlite3* db, const std::string& role, DbPermScope& out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT g.id,g.super,g.all_devices FROM perm_groups g WHERE g.builtin_key=?", -1, &st,
            nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, role.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            if (sqlite3_column_int(st, 1) != 0) out.super = true;
            if (sqlite3_column_int(st, 2) != 0) out.all_devices = true;
        }
        sqlite3_finalize(st);
        st = nullptr;
        if (sqlite3_prepare_v2(db,
                "SELECT DISTINCT r.resource_key FROM perm_groups g"
                " JOIN perm_group_resources r ON r.group_id=g.id AND r.allowed=1"
                " JOIN perm_resources p ON p.key=r.resource_key AND p.enabled=1"
                " WHERE g.builtin_key=?",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, role.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                std::string k = dbColText(st, 0);
                if (!k.empty()) out.resources.insert(k);
            }
        }
        sqlite3_finalize(st);
    }
}

} // namespace

bool AdminDb::permScopeOfUser(long long user_id, const std::string& role, DbPermScope& out) {
    out = DbPermScope();
    // role=admin 永远保留超级权限，避免误把管理员加入受限组后失去管理能力
    if (role == "admin") {
        out.super = true;
        out.all_devices = true;
        out.explicit_perm = true;
        return true;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT perm_explicit FROM users WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, user_id);
        if (sqlite3_step(st) == SQLITE_ROW) out.explicit_perm = sqlite3_column_int(st, 0) != 0;
    }
    sqlite3_finalize(st);
    scopeFromGroupsLocked(db_, "user", std::to_string(user_id), out);
    if (out.has_groups || out.explicit_perm) return true;
    fallbackRoleLocked(db_, role, out);
    return true;
}

bool AdminDb::permScopeOfAdminToken(long long token_id, long long user_id, const std::string& role,
                                    DbPermScope& out) {
    DbPermScope owner;
    permScopeOfUser(user_id, role, owner);
    DbPermScope tok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        scopeFromGroupsLocked(db_, "atk", std::to_string(token_id), tok);
    }
    // 令牌未分组 = 完全继承；分组后只能收窄，不能超过归属账号
    out = tok.has_groups ? scopeIntersect(owner, tok) : owner;
    out.explicit_perm = owner.explicit_perm;
    out.token_groups = tok.has_groups;
    return true;
}

bool AdminDb::permScopeOfClient(long long account_id, DbPermScope& out) {
    out = DbPermScope();
    std::lock_guard<std::mutex> lk(mu_);
    scopeFromGroupsLocked(db_, "client", std::to_string(account_id), out);
    out.explicit_perm = true;
    return true;
}

bool AdminDb::permScopeOfClientToken(long long token_id, long long account_id, DbPermScope& out) {
    DbPermScope owner;
    permScopeOfClient(account_id, owner);
    DbPermScope tok;
    {
        std::lock_guard<std::mutex> lk(mu_);
        scopeFromGroupsLocked(db_, "rtk", std::to_string(token_id), tok);
    }
    out = tok.has_groups ? scopeIntersect(owner, tok) : owner;
    out.token_groups = tok.has_groups;
    return true;
}

bool AdminDb::resDeviceSync(const std::string& devid, const std::string& name, const std::string& version,
                            int online, long long seen_ms) {
    if (devid.empty() || devid.size() > 128) return false;
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO res_devices(devid,name,version,online,last_seen_ms,enabled,source,updated_ms)"
            " VALUES(?,?,?,?,?,1,'agent',?)"
            " ON CONFLICT(devid) DO UPDATE SET version=excluded.version,online=excluded.online,"
            " last_seen_ms=excluded.last_seen_ms,updated_ms=excluded.updated_ms,"
            " name=CASE WHEN excluded.name!='' THEN excluded.name ELSE res_devices.name END",
            -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, version.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 4, online ? 1 : 0);
    sqlite3_bind_int64(st, 5, seen_ms);
    sqlite3_bind_int64(st, 6, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}

bool AdminDb::resDevicesPage(const std::string& q, int page, int page_size, std::string& json_out) {
    if (page < 1) page = 1;
    if (page_size < 1) page_size = 10;
    if (page_size > 200) page_size = 200;
    std::lock_guard<std::mutex> lk(mu_);
    std::string where;
    if (!q.empty()) where = " WHERE devid LIKE ? OR name LIKE ?";
    long long total = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, ("SELECT COUNT(*) FROM res_devices" + where).c_str(), -1, &st, nullptr) !=
        SQLITE_OK)
        return false;
    if (!q.empty()) {
        std::string like = "%" + q + "%";
        sqlite3_bind_text(st, 1, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, like.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (sqlite3_step(st) == SQLITE_ROW) total = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    std::string sql =
        "SELECT d.devid,d.name,d.version,d.online,d.last_seen_ms,d.enabled,d.source,"
        "(SELECT COUNT(*) FROM perm_group_devices g WHERE g.devid=d.devid)"
        " FROM res_devices d" + where + " ORDER BY d.devid LIMIT ? OFFSET ?";
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    int idx = 1;
    if (!q.empty()) {
        std::string like = "%" + q + "%";
        sqlite3_bind_text(st, idx++, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, idx++, like.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_int(st, idx++, page_size);
    sqlite3_bind_int(st, idx++, (page - 1) * page_size);
    json items = json::array();
    while (sqlite3_step(st) == SQLITE_ROW) {
        json j;
        j["devid"] = dbColText(st, 0);
        j["name"] = dbColText(st, 1);
        j["version"] = dbColText(st, 2);
        j["online"] = sqlite3_column_int(st, 3) != 0;
        j["last_seen_ms"] = (long long)sqlite3_column_int64(st, 4);
        j["enabled"] = sqlite3_column_int(st, 5) != 0;
        j["source"] = dbColText(st, 6);
        j["group_count"] = (int)sqlite3_column_int(st, 7);
        items.push_back(j);
    }
    sqlite3_finalize(st);
    json out;
    out["kind"] = "devices";
    out["page"] = page;
    out["page_size"] = page_size;
    out["total"] = total;
    out["items"] = items;
    json_out = out.dump();
    return true;
}

bool AdminDb::resDeviceUpdate(const std::string& devid, const std::string& name, int enabled,
                              std::string& err) {
    if (devid.empty()) {
        err = "bad devid";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    // 允许手工登记尚未上线的设备；已存在的设备只改显示名与启用状态
    if (sqlite3_prepare_v2(db_,
            "INSERT INTO res_devices(devid,name,version,online,last_seen_ms,enabled,source,updated_ms)"
            " VALUES(?,?,'',0,0,?,'manual',?)"
            " ON CONFLICT(devid) DO UPDATE SET name=excluded.name,enabled=excluded.enabled,"
            " source='manual',updated_ms=excluded.updated_ms",
            -1, &st, nullptr) != SQLITE_OK) {
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, devid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 3, enabled ? 1 : 0);
    sqlite3_bind_int64(st, 4, nowMs());
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok) err = sqlite3_errmsg(db_);
    return ok;
}

bool AdminDb::permSubjects(std::string& json_out) {
    json arr = json::array();
    auto groupNames = [&](const char* type, const std::string& id) {
        json gs = json::array();
        std::lock_guard<std::mutex> lk(mu_);
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT g.id,g.name FROM perm_group_members m JOIN perm_groups g ON g.id=m.group_id"
                " WHERE m.subject_type=? AND m.subject_id=? ORDER BY g.id",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, type, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
            while (sqlite3_step(st) == SQLITE_ROW) {
                json j;
                j["id"] = (long long)sqlite3_column_int64(st, 0);
                j["name"] = dbColText(st, 1);
                gs.push_back(j);
            }
        }
        sqlite3_finalize(st);
        return gs;
    };
    auto base = [&](const char* type, const std::string& id, const std::string& name,
                    const std::string& detail, const std::string& status, const DbPermScope& sc) {
        json j;
        j["type"] = type;
        j["id"] = id;
        j["name"] = name;
        j["detail"] = detail;
        j["status"] = status;
        j["groups"] = groupNames(type, id);
        j["super"] = sc.super;
        j["all_devices"] = sc.all_devices;
        j["resource_count"] = (int)sc.resources.size();
        j["device_count"] = sc.all_devices || sc.super ? -1 : (int)sc.devices.size();
        return j;
    };
    // 账号
    {
        std::vector<std::tuple<long long, std::string, std::string, std::string>> list;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT id,username,role,status FROM users ORDER BY id", -1, &st,
                                   nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW)
                    list.emplace_back(sqlite3_column_int64(st, 0), dbColText(st, 1), dbColText(st, 2),
                                      dbColText(st, 3));
            }
            sqlite3_finalize(st);
        }
        for (auto& u : list) {
            DbPermScope sc;
            permScopeOfUser(std::get<0>(u), std::get<2>(u), sc);
            arr.push_back(base("user", std::to_string(std::get<0>(u)), std::get<1>(u), "角色 " + std::get<2>(u),
                               std::get<3>(u), sc));
        }
    }
    // atk
    {
        std::vector<std::tuple<long long, long long, std::string, std::string, std::string, std::string>>
            list;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "SELECT t.id,t.user_id,t.name,t.token_prefix,t.username,t.status FROM admin_tokens t"
                    " ORDER BY t.id",
                    -1, &st, nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW)
                    list.emplace_back(sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1),
                                      dbColText(st, 2), dbColText(st, 3), dbColText(st, 4), dbColText(st, 5));
            }
            sqlite3_finalize(st);
        }
        for (auto& x : list) {
            DbUser u;
            std::string uname = std::get<4>(x), role = "viewer";
            {
                std::lock_guard<std::mutex> lk(mu_);
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db_, "SELECT role FROM users WHERE id=?", -1, &st, nullptr) ==
                    SQLITE_OK) {
                    sqlite3_bind_int64(st, 1, std::get<1>(x));
                    if (sqlite3_step(st) == SQLITE_ROW) role = dbColText(st, 0);
                }
                sqlite3_finalize(st);
            }
            DbPermScope sc;
            permScopeOfAdminToken(std::get<0>(x), std::get<1>(x), role, sc);
            std::string nm = std::get<2>(x).empty() ? std::get<3>(x) : std::get<2>(x);
            json j = base("atk", std::to_string(std::get<0>(x)), nm, "归属 " + uname, std::get<5>(x), sc);
            // 令牌可选组限定在归属账号已有的组，避免界面出现「选了也没用」的选项
            std::vector<long long> og;
            permGroupsOfUser(std::get<1>(x), og);
            j["owner_groups"] = og;
            arr.push_back(j);
        }
    }
    // 接入账号
    {
        std::vector<std::tuple<long long, std::string, std::string, std::string>> list;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_, "SELECT id,username,display_name,status FROM client_accounts ORDER BY id",
                                   -1, &st, nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW)
                    list.emplace_back(sqlite3_column_int64(st, 0), dbColText(st, 1), dbColText(st, 2),
                                      dbColText(st, 3));
            }
            sqlite3_finalize(st);
        }
        for (auto& c : list) {
            DbPermScope sc;
            permScopeOfClient(std::get<0>(c), sc);
            arr.push_back(base("client", std::to_string(std::get<0>(c)), std::get<1>(c), std::get<2>(c),
                               std::get<3>(c), sc));
        }
    }
    // rtk
    {
        std::vector<std::tuple<long long, long long, std::string, std::string, std::string, std::string>>
            list;
        {
            std::lock_guard<std::mutex> lk(mu_);
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "SELECT t.id,t.account_id,t.name,t.token_prefix,a.username,t.status FROM client_tokens t"
                    " JOIN client_accounts a ON a.id=t.account_id ORDER BY t.id",
                    -1, &st, nullptr) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW)
                    list.emplace_back(sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1),
                                      dbColText(st, 2), dbColText(st, 3), dbColText(st, 4), dbColText(st, 5));
            }
            sqlite3_finalize(st);
        }
        for (auto& x : list) {
            DbPermScope sc;
            permScopeOfClientToken(std::get<0>(x), std::get<1>(x), sc);
            std::string nm = std::get<2>(x).empty() ? std::get<3>(x) : std::get<2>(x);
            json j = base("rtk", std::to_string(std::get<0>(x)), nm, "归属 " + std::get<4>(x),
                          std::get<5>(x), sc);
            std::vector<long long> og;
            permGroupsOfClient(std::get<1>(x), og);
            j["owner_groups"] = og;
            arr.push_back(j);
        }
    }
    json_out = arr.dump();
    return true;
}

bool AdminDb::permGroupsOfSubject(const std::string& type, const std::string& id,
                                  std::vector<long long>& ids) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT group_id FROM perm_group_members WHERE subject_type=? AND subject_id=?"
            " ORDER BY group_id",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_text(st, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(st) == SQLITE_ROW) ids.push_back(sqlite3_column_int64(st, 0));
    sqlite3_finalize(st);
    return true;
}

bool AdminDb::permSetSubjectGroups(const std::string& type, const std::string& id,
                                   const std::vector<long long>& ids, std::string& err) {
    if (type != "user" && type != "atk" && type != "client" && type != "rtk") {
        err = "bad subject type";
        return false;
    }
    if (id.empty() || id.size() > 64) {
        err = "bad subject id";
        return false;
    }
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) { err = e; return false; }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_members WHERE subject_type=? AND subject_id=?", -1, &st,
                           nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e);
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
    for (long long gid : ids) {
        if (sqlite3_prepare_v2(db_,
                "INSERT OR IGNORE INTO perm_group_members(group_id,subject_type,subject_id,added_ms)"
                " VALUES(?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_int64(st, 1, gid);
        sqlite3_bind_text(st, 2, type.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 4, nowMs());
        bool ok = sqlite3_step(st) == SQLITE_DONE;
        sqlite3_finalize(st);
        if (!ok) {
            exec("ROLLBACK", e);
            err = "写入权限组失败";
            return false;
        }
    }
    if (type == "user") {
        if (sqlite3_prepare_v2(db_, "UPDATE users SET perm_explicit=1 WHERE id=?", -1, &st, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_int64(st, 1, atoll(id.c_str()));
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    exec("COMMIT", e);
    return true;
}

bool AdminDb::resDevicesMarkOffline(const std::vector<std::string>& online) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = "UPDATE res_devices SET online=0 WHERE devid NOT IN (";
    if (online.empty()) sql += "''";
    else {
        for (size_t i = 0; i < online.size(); i++) sql += (i ? ",?" : "?");
    }
    sql += ")";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    for (size_t i = 0; i < online.size(); i++)
        sqlite3_bind_text(st, (int)i + 1, online[i].c_str(), -1, SQLITE_TRANSIENT);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    return ok;
}
