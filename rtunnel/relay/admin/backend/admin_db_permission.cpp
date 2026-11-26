#include "admin_db.hpp"
#include "cpputil/hstring.h"
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
    for (auto& one : hv::split(s, ',')) {
        one = hv::trim(one);
        if (!one.empty()) out.push_back(one);
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
    return hv::trim(s);
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

// 令牌是否曾显式绑定过权限组：显式但组为空 = 无权限，不再回落归属账号
bool tokenGroupsExplicitLocked(sqlite3* db, const char* table, long long id) {
    std::string sql = std::string("SELECT groups_explicit FROM ") + table + " WHERE id=?";
    sqlite3_stmt* st = nullptr;
    bool on = false;
    if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        if (sqlite3_step(st) == SQLITE_ROW) on = sqlite3_column_int(st, 0) != 0;
        sqlite3_finalize(st);
    }
    return on;
}

// 组内已有令牌记为「显式分配过」：被移出组或删组后保持零权限，不回落归属账号。
// 账号没有回落路径，移出组即零权限，不需要额外标记。
void markGroupMembersExplicit(sqlite3* db, long long group_id) {
    const char* sqls[] = {
        "UPDATE admin_tokens SET groups_explicit=1 WHERE id IN (SELECT CAST(subject_id AS INTEGER)"
        " FROM perm_group_members WHERE group_id=? AND subject_type='atk')",
        "UPDATE client_tokens SET groups_explicit=1 WHERE id IN (SELECT CAST(subject_id AS INTEGER)"
        " FROM perm_group_members WHERE group_id=? AND subject_type='rtk')",
    };
    for (const char* sql : sqls) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, group_id);
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
}

// 与设备范围无关的资源：只有拿到全部设备（或 super）才生效，避免受限组
// 通过 relay/dict/权限组等全局键影响范围外的设备或整机配置。
bool permIsGlobalKey(const std::string& key) {
    static const char* kGlobal[] = {
        "relay.read", "relay.write",
        "dict.read", "dict.write",
        "group.read", "group.write", "group.member",
        "account.read", "account.write", "account.delete",
        "token.read", "token.write",
        "cert.read", "cert.write",
        "audit.read", "usage.read",
        "tenant.read", "tenant.write",
        "billing.read", "billing.write",
        "settings.read", "settings.write", "system.shutdown",
        "repo.read", "repo.write", "repo.delete",
        "op.approve",
        "playbook.read", "playbook.write",
        "ai.chat", "ai.config",
    };
    for (const char* k : kGlobal)
        if (key == k) return true;
    return false;
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

// 资源键 -> 数据面能力的唯一映射。数据面规则与「有效权限」预览都走这里，
// 避免两处各写一套导致 device.view 隐含 svc、串口只读可写之类的不一致。
struct ClientCaps {
    bool view = false;
    std::string ports;
    std::string tty;
    bool tty_write = false;
    bool shell = false;
    int  file = 0;
    bool svc = false;
};

ClientCaps capsForScope(const DbPermScope& sc, const std::string& portsIn,
                        const std::vector<PortRange>& disabledPorts,
                        const std::set<std::string>& disabledTtys) {
    auto has = [&](const char* k) { return sc.super || sc.resources.count(k) > 0; };
    ClientCaps c;
    c.view = has("device.view");
    if (has("device.port")) c.ports = intersectPorts(portsIn, "*", disabledPorts);
    if (has("device.tty.read") || has("device.tty.write")) c.tty = intersectTty("*", "*", disabledTtys);
    c.tty_write = has("device.tty.write");
    c.shell = has("device.terminal");
    c.file = has("file.write") ? 2 : has("file.read") ? 1 : 0;
    c.svc = has("device.svc");
    return c;
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

    struct AccRow { long long id; std::string user; long long tenant; };
    std::vector<AccRow> accounts;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,username,tenant_id FROM client_accounts WHERE status='active'"
                               " ORDER BY id", -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW)
            accounts.push_back({sqlite3_column_int64(st, 0), dbColText(st, 1),
                                sqlite3_column_int64(st, 2)});
    }
    sqlite3_finalize(st);

    for (const auto& acc : accounts) {
        // 所有接入账号都纳入分组鉴权：未分组 = 无权限，不再回落到 --acl
        users.insert(acc.user);

        auto emitFor = [&](const DbPermScope& sc, const std::string& portsIn, long long token_id) {
            if (!sc.has_groups) return;
            ClientCaps caps = capsForScope(sc, portsIn, disabledPorts, disabledTtys);
            DbGroupAclRule base;
            base.user = acc.user;
            base.token_id = token_id;
            base.tenant_id = acc.tenant;
            base.ports = caps.ports;
            base.tty = caps.tty;
            base.tty_write = caps.tty_write;
            base.shell = caps.shell;
            base.file = caps.file;
            base.svc = caps.svc;
            auto emit = [&](const std::string& devid) {
                DbGroupAclRule r = base;
                r.devid = devid;
                out.push_back(r);
            };
            if (sc.super || sc.all_devices) emit("*");
            else for (const auto& d : sc.devices) emit(d);
        };

        DbPermScope accScope;
        scopeFromGroupsLocked(db_, "client", std::to_string(acc.id), accScope);
        std::string accPorts = subjectPortsLocked(db_, "client", std::to_string(acc.id));
        emitFor(accScope, accPorts, 0);

        // 该账号下未过期的 rtk 令牌：有令牌组的生成收窄规则
        std::vector<long long> token_ids;
        st = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT id FROM client_tokens WHERE account_id=? AND status='active' AND revoked_ms=0"
                " AND (expires_at_ms=0 OR expires_at_ms>?) ORDER BY id",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, acc.id);
            sqlite3_bind_int64(st, 2, nowMs());
            while (sqlite3_step(st) == SQLITE_ROW) token_ids.push_back(sqlite3_column_int64(st, 0));
        }
        sqlite3_finalize(st);

        for (long long tid : token_ids) {
            DbPermScope tokScope;
            scopeFromGroupsLocked(db_, "rtk", std::to_string(tid), tokScope);
            bool explicit_groups = tokenGroupsExplicitLocked(db_, "client_tokens", tid);
            if (!tokScope.has_groups && !explicit_groups) continue;
            scoped_tokens.insert(tid);
            if (!tokScope.has_groups) {
                // 显式绑定过组但当前为空：下发一条全空规则，明确拒绝该令牌
                DbGroupAclRule deny;
                deny.user = acc.user;
                deny.tenant_id = acc.tenant;
                deny.token_id = tid;
                deny.devid = "*";
                out.push_back(deny);
                continue;
            }
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
    ClientCaps cp = capsForScope(sc, ports, disabledPorts, disabledTtys);
    caps["tcp_ports"] = cp.ports;
    caps["shell"] = cp.shell ? "1" : "0";
    caps["tty"] = cp.tty;
    caps["tty_write"] = cp.tty_write ? "1" : "0";
    caps["file"] = cp.file == 2 ? "rw" : cp.file == 1 ? "ro" : "none";
    caps["svc"] = cp.svc ? "1" : "0";
    caps["sysinfo"] = cp.view ? "1" : "0";
    caps["metrics"] = cp.view ? "1" : "0";
    caps["config"] = has("device.caps.write") ? "1" : "0";
    caps["web"] = cp.view ? "1" : "0";

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
    {"device.terminal", "设备", "打开终端", "建立 Shell 会话"},
    {"device.svc", "设备", "服务管理", "查看与启停服务、修改 unit 文件"},
    {"device.port", "设备", "端口转发", "按权限组的端口范围建立 TCP 转发"},
    {"device.caps.write", "设备", "修改能力配置", "转发端口、Shell、文件与服务能力"},
    {"device.ota.write", "设备", "OTA 升级开关", "允许/禁止设备接收升级包"},
    {"device.tty.read", "设备", "查看串口", "串口配置、发现列表与日志"},
    {"device.tty.write", "设备", "修改串口", "串口配置与 allow_tty 白名单"},
    {"device.metrics.write", "设备", "修改采样", "采样指标、间隔与保留点数"},
    {"device.kick", "设备", "断开连接", "强制下线设备或结束会话"},
    {"device.power", "设备", "电源操作", "重启或关闭设备"},
    {"device.diag", "设备", "下载诊断包", "生成并下载设备诊断信息"},
    {"device.proc.read", "设备", "查看进程", "进程列表、资源占用与进程详情"},
    {"device.proc.signal", "设备", "管理进程", "结束进程或发送信号"},
    {"device.exec", "设备", "执行命令", "在设备上执行白名单内的命令（argv，不经过 shell）"},

    {"file.read", "文件", "查看文件", "仓库列表、下载、Hex 与尾部"},
    {"file.write", "文件", "写入文件", "上传、新建、编辑、移动"},
    {"file.delete", "文件", "删除文件", "删除文件或目录、打包下载"},

    {"repo.read", "文件", "查看仓库", "relay 本机文件仓库列表与下载（全局）"},
    {"repo.write", "文件", "写入仓库", "向仓库上传、编辑、移动，直推设备（全局）"},
    {"repo.delete", "文件", "删除仓库", "删除仓库文件或目录（全局）"},

    {"dist.read", "分发", "查看任务", "分发与 OTA 任务列表、详情"},
    {"dist.write", "分发", "创建与调度", "创建、重试、取消、延长分发任务"},

    {"relay.read", "中继", "查看中继列表", "中继地址字典、下发列表与同步任务"},
    {"relay.write", "中继", "维护中继列表", "地址增删改、调整下发顺序、创建与调度同步任务"},

    {"dict.read", "权限", "查看字典", "资源/设备/端口/串口字典"},
    {"dict.write", "权限", "维护字典", "字典增删改、设备资源启停与改名"},

    {"group.read", "权限", "查看权限组", "权限组列表、详情、成员与授权对象"},
    {"group.write", "权限", "维护权限组", "创建、修改、删除权限组及其资源与设备范围"},
    {"group.member", "权限", "调整组成员", "添加或移除权限组成员、修改授权对象所属组"},

    {"account.read", "账号", "查看账号", "管理台账号与接入账号"},
    {"account.write", "账号", "修改账号", "新建、改角色/状态/密码、调整权限组"},
    {"account.delete", "账号", "删除账号", "删除管理台或接入账号"},

    {"token.read", "令牌", "查看 Token", "管理台 API Token 列表"},
    {"token.write", "令牌", "管理 Token", "创建、轮换、吊销 Token"},

    {"cert.read", "证书", "查看证书", "证书台账与到期状态"},
    {"cert.write", "证书", "签发证书", "签发、吊销、签署证书"},

    {"audit.read", "审计", "查看审计", "管理台操作审计日志"},
    {"op.approve", "运营", "审批操作作业", "批准或拒绝 risk ≥ 2 的设备写操作（不能批自己发起的）"},
    {"playbook.read", "运营", "查看自愈规则", "告警触发的自动应答规则与执行流水"},
    {"playbook.write", "运营", "管理自愈规则", "新建/修改/启停/删除自愈规则并手动触发"},
    {"ai.chat", "运营", "使用 AI 助手", "在「AI 助手」里与模型对话（只读工具，只能查不能改）"},
    {"ai.config", "运营", "配置 AI 模型", "设置模型 endpoint/型号/参数与 API 密钥"},
    {"usage.read", "运营", "查看用量", "隧道流量、会话与设备在线用量报表（全局）"},
    {"tenant.read", "运营", "查看租户", "租户列表与设备/账号归属"},
    {"tenant.write", "运营", "管理租户", "新建、改名、启停租户"},
    {"billing.read", "运营", "查看费用", "按租户查看用量金额与价目表"},
    {"billing.write", "运营", "修改价目", "调整各指标计价单位与单价"},

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
                    k != "account.delete" && k != "billing.write";
        } else if (builtin == "operator") {
            allow = k == "device.view" || k == "device.terminal" || k == "device.port" ||
                    k == "device.svc" || k == "device.tty.read" || k == "device.tty.write" ||
                    k == "device.proc.read" || k == "device.proc.signal" ||
                    k == "device.exec" ||
                    k == "file.read" || k == "file.write" || k == "dist.read" ||
                    k == "repo.read" || k == "repo.write" ||
                    k == "dict.read" || k == "account.read" || k == "settings.read" ||
                    k == "usage.read";
        } else if (builtin == "viewer") {
            allow = k == "device.view" || k == "device.tty.read" || k == "device.proc.read" ||
                    k == "settings.read";
        }
        if (allow) out.push_back(k);
    }
    return out;
}

// 超级组必须至少保留一个账号成员：移除唯一成员会让所有人都进不了权限管理
bool superMembersExistLocked(sqlite3* db) {
    sqlite3_stmt* st = nullptr;
    bool ok = false;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM perm_group_members m JOIN perm_groups g ON g.id=m.group_id"
            " WHERE g.super=1 AND m.subject_type='user'", -1, &st, nullptr) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) ok = sqlite3_column_int64(st, 0) > 0;
    }
    sqlite3_finalize(st);
    return ok;
}

// 角色跟着内置组走：组成员变化后把 users.role 同步为最高的内置组
void syncUserRoleLocked(sqlite3* db, const std::string& sid) {
    std::string cur, best;
    int best_rank = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT role FROM users WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) cur = dbColText(st, 0);
    }
    sqlite3_finalize(st);
    st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT g.builtin_key FROM perm_group_members m JOIN perm_groups g ON g.id=m.group_id"
            " WHERE m.subject_type='user' AND m.subject_id=? AND g.builtin_key<>''",
            -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
            std::string k = dbColText(st, 0);
            int r = k == "admin" ? 4 : k == "manager" ? 3 : k == "operator" ? 2 : k == "viewer" ? 1 : 0;
            if (r > best_rank) {
                best_rank = r;
                best = k;
            }
        }
    }
    sqlite3_finalize(st);
    // 没有任何内置组时退回最低角色：角色只是内置组的别名，不再保留历史角色
    if (best_rank == 0) best = "viewer";
    if (best == cur) return;
    if (sqlite3_prepare_v2(db, "UPDATE users SET role=? WHERE id=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, best.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, sid.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
    }
    sqlite3_finalize(st);
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
                "INSERT OR IGNORE INTO perm_groups(name,description,builtin_key,super,global_scope,"
                "created_ms,updated_ms) VALUES(?,?,?,?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            return false;
        }
        sqlite3_bind_text(st, 1, g.name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, g.desc, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, g.key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 4, g.super);
        // 内置角色组保留传统语义：全局资源（仓库/设置/审计等）不因设备范围为空而失效
        sqlite3_bind_int(st, 5, 1);
        sqlite3_bind_int64(st, 6, nowMs());
        sqlite3_bind_int64(st, 7, nowMs());
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
            if (sqlite3_prepare_v2(db_,
                    "UPDATE perm_groups SET builtin_key=?,super=?,global_scope=1 WHERE name=?", -1, &st,
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

    // 4) 账号权限只来自组成员关系；老库的角色组补齐在 open() 的迁移里完成

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
            "SELECT id,name,description,builtin_key,super,all_devices,global_scope,ports,created_ms,updated_ms"
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
        j["global_scope"] = sqlite3_column_int(st, 6) != 0 || j["super"].get<bool>();
        j["ports"] = dbColText(st, 7);
        j["created_ms"] = (long long)sqlite3_column_int64(st, 8);
        j["updated_ms"] = (long long)sqlite3_column_int64(st, 9);
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
            "SELECT name,description,builtin_key,super,all_devices,global_scope,ports,created_ms,updated_ms"
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
    g["global_scope"] = sqlite3_column_int(st, 5) != 0;
    g["ports"] = dbColText(st, 6);
    g["created_ms"] = (long long)sqlite3_column_int64(st, 7);
    g["updated_ms"] = (long long)sqlite3_column_int64(st, 8);
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

bool AdminDb::permGroupSaveAll(long long id, const std::string& name, const std::string& desc,
                               const std::vector<std::string>& resources,
                               const std::vector<std::string>& devices,
                               int all_devices, int global_scope, const std::string& ports, bool set_members,
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
                "UPDATE perm_groups SET name=?,description=?,all_devices=?,global_scope=?,ports=?,"
                "updated_ms=? WHERE id=?",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, all_devices ? 1 : 0);
        sqlite3_bind_int(st, 4, global_scope ? 1 : 0);
        sqlite3_bind_text(st, 5, ports.empty() ? "*" : ports.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 6, nowMs());
        sqlite3_bind_int64(st, 7, id);
        int rc = sqlite3_step(st);
        sqlite3_finalize(st);
        if (rc != SQLITE_DONE) {
            exec("ROLLBACK", e);
            err = (rc == SQLITE_CONSTRAINT) ? "权限组名称已存在" : sqlite3_errmsg(db_);
            return false;
        }
    } else {
        if (sqlite3_prepare_v2(db_,
                "INSERT INTO perm_groups(name,description,builtin_key,super,seeded,all_devices,"
                "global_scope,ports,created_ms,updated_ms) VALUES(?,?,'',0,1,?,?,?,?,?)",
                -1, &st, nullptr) != SQLITE_OK) {
            exec("ROLLBACK", e);
            err = sqlite3_errmsg(db_);
            return false;
        }
        sqlite3_bind_text(st, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, desc.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, all_devices ? 1 : 0);
        sqlite3_bind_int(st, 4, global_scope ? 1 : 0);
        sqlite3_bind_text(st, 5, ports.empty() ? "*" : ports.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 6, nowMs());
        sqlite3_bind_int64(st, 7, nowMs());
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
    std::vector<std::string> role_users;
    if (set_members) {
        {
            sqlite3_stmt* q = nullptr;
            if (sqlite3_prepare_v2(db_,
                    "SELECT subject_id FROM perm_group_members WHERE group_id=? AND subject_type='user'",
                    -1, &q, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(q, 1, id);
                while (sqlite3_step(q) == SQLITE_ROW) role_users.push_back(dbColText(q, 0));
            }
            sqlite3_finalize(q);
        }
        // 先标记现有成员：被移出组后不能回落到角色/归属账号的权限
        markGroupMembersExplicit(db_, id);
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
            if (m.first == "user" &&
                std::find(role_users.begin(), role_users.end(), m.second) == role_users.end())
                role_users.push_back(m.second);
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
            // 令牌一进组就记为显式：被移出后保持零权限，不继承归属账号
            if (m.first == "atk" || m.first == "rtk") {
                const char* table = m.first == "atk" ? "admin_tokens" : "client_tokens";
                std::string sql = std::string("UPDATE ") + table + " SET groups_explicit=1 WHERE id=?";
                sqlite3_stmt* u = nullptr;
                if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &u, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(u, 1, atoll(m.second.c_str()));
                    sqlite3_step(u);
                }
                sqlite3_finalize(u);
            }
        }
        for (const auto& sid : role_users) syncUserRoleLocked(db_, sid);
    }

    if (!superMembersExistLocked(db_)) {
        exec("ROLLBACK", e);
        err = "超级管理员组至少保留一个成员";
        return false;
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
    // 删组前标记现有成员：之后保持零权限，不回落角色/归属账号
    markGroupMembersExplicit(db_, id);
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

bool AdminDb::permGroupMemberList(long long id, std::vector<std::pair<std::string, std::string>>& out) {
    out.clear();
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT subject_type,subject_id FROM perm_group_members WHERE group_id=?"
            " ORDER BY subject_type,subject_id",
            -1, &st, nullptr) != SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    while (sqlite3_step(st) == SQLITE_ROW) out.emplace_back(dbColText(st, 0), dbColText(st, 1));
    sqlite3_finalize(st);
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
    // 先把现有令牌标记为显式：移出组后保持零权限，不回落归属账号
    {
        const char* marks[] = {
            "UPDATE admin_tokens SET groups_explicit=1 WHERE id IN (SELECT CAST(subject_id AS INTEGER)"
            " FROM perm_group_members WHERE group_id=? AND subject_type='atk')",
            "UPDATE client_tokens SET groups_explicit=1 WHERE id IN (SELECT CAST(subject_id AS INTEGER)"
            " FROM perm_group_members WHERE group_id=? AND subject_type='rtk')",
        };
        for (const char* sql : marks) {
            sqlite3_stmt* mk = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &mk, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(mk, 1, id);
                sqlite3_step(mk);
            }
            sqlite3_finalize(mk);
        }
    }
    std::vector<std::string> role_users;
    {
        sqlite3_stmt* q = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT subject_id FROM perm_group_members WHERE group_id=? AND subject_type='user'",
                -1, &q, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(q, 1, id);
            while (sqlite3_step(q) == SQLITE_ROW) role_users.push_back(dbColText(q, 0));
        }
        sqlite3_finalize(q);
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
        if (m.first == "user" &&
            std::find(role_users.begin(), role_users.end(), m.second) == role_users.end())
            role_users.push_back(m.second);
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
        // 令牌一进组就记为显式，之后被移出时保持「无权限」而不是继承账号
        if (m.first == "atk" || m.first == "rtk") {
            const char* table = m.first == "atk" ? "admin_tokens" : "client_tokens";
            std::string sql = std::string("UPDATE ") + table + " SET groups_explicit=1 WHERE id=?";
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(st, 1, atoll(m.second.c_str()));
                sqlite3_step(st);
            }
            sqlite3_finalize(st);
        }
    }
    if (!superMembersExistLocked(db_)) {
        exec("ROLLBACK", e);
        err = "超级管理员组至少保留一个成员";
        return false;
    }
    for (const auto& sid : role_users) syncUserRoleLocked(db_, sid);
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

bool AdminDb::permSetUserGroups(long long user_id, const std::vector<long long>& ids, std::string& err) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string e;
    if (!exec("BEGIN", e)) {
        err = e;
        return false;
    }
    std::string sid = std::to_string(user_id);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM perm_group_members WHERE subject_type='user' AND subject_id=?", -1, &st,
                           nullptr) != SQLITE_OK) {
        exec("ROLLBACK", e);
        err = sqlite3_errmsg(db_);
        return false;
    }
    sqlite3_bind_text(st, 1, sid.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);

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
    }
    syncUserRoleLocked(db_, sid);
    if (!superMembersExistLocked(db_)) {
        exec("ROLLBACK", e);
        err = "超级管理员组至少保留一个成员";
        return false;
    }
    exec("COMMIT", e);
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

DbPermScope scopeIntersect(const DbPermScope& a, const DbPermScope& b) {    if (a.super) return b;
    if (b.super) return a;
    DbPermScope r;
    r.has_groups = a.has_groups || b.has_groups;
    r.all_devices = a.all_devices && b.all_devices;
    r.global_scope = a.global_scope && b.global_scope;
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
    // 端口范围取交集：令牌分组只能收窄，不能超出归属账号
    std::vector<PortRange> pr;
    for (auto& x : parsePorts(a.ports))
        for (auto& y : parsePorts(b.ports)) {
            int lo = std::max(x.lo, y.lo), hi = std::min(x.hi, y.hi);
            if (lo <= hi) {
                PortRange r2;
                r2.lo = lo;
                r2.hi = hi;
                pr.push_back(r2);
            }
        }
    r.ports = joinRanges(pr);
    return r;
}

// 某个主体（user/atk/client/rtk）所有权限组的并集
void scopeFromGroupsLocked(sqlite3* db, const char* type, const std::string& id, DbPermScope& out) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT g.id,g.super,g.all_devices,g.global_scope FROM perm_group_members m"
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
        if (sqlite3_column_int(st, 3) != 0) out.global_scope = true;
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

    out.ports = subjectPortsLocked(db, type, id);
}

// 主体所有权限组 ports 的并集（只计入真正授予 device.port 的组）。
// 没有端口转发能力的组即使带了 ports（新建组默认 "*"）也不能放大别人的端口范围。
std::string subjectPortsLocked(sqlite3* db, const char* type, const std::string& id) {
    sqlite3_stmt* st = nullptr;
    std::vector<PortRange> rs;
    if (sqlite3_prepare_v2(db,
            "SELECT g.ports FROM perm_group_members m JOIN perm_groups g ON g.id=m.group_id"
            " WHERE m.subject_type=? AND m.subject_id=?"
            " AND EXISTS(SELECT 1 FROM perm_group_resources r WHERE r.group_id=g.id"
            "             AND r.resource_key='device.port' AND r.allowed=1)",
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

} // namespace

// 端口范围包含：target 的每个端口都要被 allowed 覆盖（先归一化 allowed，避免重复计数）
// 全局资源键：不受设备范围约束，只有 all_devices / super 才能生效
bool permIsGlobal(const std::string& key) { return permIsGlobalKey(key); }

bool adminPortsCovered(const std::string& allowed, const std::string& target) {    std::vector<PortRange> want = parsePorts(target);
    if (want.empty()) return true;   // 不授予任何端口
    std::vector<PortRange> have = parsePorts(joinRanges(parsePorts(allowed)));
    if (have.empty()) return false;
    for (auto& w : want) {
        int need = w.hi - w.lo + 1, got = 0;
        for (auto& h : have) {
            int lo = std::max(w.lo, h.lo), hi = std::min(w.hi, h.hi);
            if (lo <= hi) got += hi - lo + 1;
        }
        if (got < need) return false;
    }
    return true;
}

bool dbPermCovers(const DbPermScope& a, const DbPermScope& b) {
    if (a.super) return true;
    if (b.super) return false;
    if (b.global_scope && !a.global_scope) return false;
    for (const auto& k : b.resources)
        if (!a.resources.count(k)) return false;
    if (b.all_devices) {
        if (!a.all_devices) return false;
    } else if (!a.all_devices) {
        for (const auto& d : b.devices)
            if (!a.devices.count(d)) return false;
    }
    return adminPortsCovered(a.ports, b.ports);
}

bool AdminDb::permGroupBuiltin(long long id, std::string& key) {
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    key.clear();
    if (sqlite3_prepare_v2(db_, "SELECT builtin_key FROM perm_groups WHERE id=?", -1, &st, nullptr) !=
        SQLITE_OK)
        return false;
    sqlite3_bind_int64(st, 1, id);
    bool found = sqlite3_step(st) == SQLITE_ROW;
    if (found) key = dbColText(st, 0);
    sqlite3_finalize(st);
    return found;
}

bool AdminDb::permRoleScope(const std::string& role, DbPermScope& out) {
    long long gid = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT id FROM perm_groups WHERE builtin_key=?", -1, &st, nullptr) ==
            SQLITE_OK) {
            sqlite3_bind_text(st, 1, role.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) gid = sqlite3_column_int64(st, 0);
        }
        sqlite3_finalize(st);
    }
    if (gid <= 0) {
        out = DbPermScope();
        return false;
    }
    return permGroupScope(gid, out);
}

bool AdminDb::permGroupScope(long long id, DbPermScope& out) {
    out = DbPermScope();
    std::lock_guard<std::mutex> lk(mu_);
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT super,all_devices,global_scope,ports FROM perm_groups WHERE id=?",
                           -1, &st, nullptr) != SQLITE_OK) {
        return false;
    }
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st);
        return false;
    }
    out.super = sqlite3_column_int(st, 0) != 0;
    out.all_devices = sqlite3_column_int(st, 1) != 0;
    out.global_scope = sqlite3_column_int(st, 2) != 0;
    out.ports = dbColText(st, 3);
    out.has_groups = true;
    sqlite3_finalize(st);

    st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT resource_key FROM perm_group_resources WHERE group_id=? AND allowed=1",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) out.resources.insert(dbColText(st, 0));
    }
    sqlite3_finalize(st);

    st = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT devid FROM perm_group_devices WHERE group_id=?", -1, &st, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_int64(st, 1, id);
        while (sqlite3_step(st) == SQLITE_ROW) out.devices.insert(dbColText(st, 0));
    }
    sqlite3_finalize(st);
    // 没有端口转发能力时，ports 不参与授予范围比较，避免误报“端口越界”
    if (!out.super && !out.resources.count("device.port")) out.ports.clear();
    return true;
}

bool AdminDb::permScopeOfUser(long long user_id, DbPermScope& out) {
    out = DbPermScope();
    std::lock_guard<std::mutex> lk(mu_);
    scopeFromGroupsLocked(db_, "user", std::to_string(user_id), out);
    return true;
}

bool AdminDb::permScopeOfAdminToken(long long token_id, long long user_id, DbPermScope& out) {
    DbPermScope owner;
    permScopeOfUser(user_id, owner);
    DbPermScope tok;
    bool explicit_groups = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        scopeFromGroupsLocked(db_, "atk", std::to_string(token_id), tok);
        explicit_groups = tokenGroupsExplicitLocked(db_, "admin_tokens", token_id);
    }
    // 从未分组 = 继承归属账号；绑定过组（含清空）= 只能收窄，空组等于无权限
    bool scoped = tok.has_groups || explicit_groups;
    out = scoped ? scopeIntersect(owner, tok) : owner;
    out.token_groups = scoped;
    return true;
}

bool AdminDb::permScopeOfClient(long long account_id, DbPermScope& out) {
    out = DbPermScope();
    std::lock_guard<std::mutex> lk(mu_);
    scopeFromGroupsLocked(db_, "client", std::to_string(account_id), out);
    return true;
}

bool AdminDb::permScopeOfClientToken(long long token_id, long long account_id, DbPermScope& out) {
    DbPermScope owner;
    permScopeOfClient(account_id, owner);
    DbPermScope tok;
    bool explicit_groups = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        scopeFromGroupsLocked(db_, "rtk", std::to_string(token_id), tok);
        explicit_groups = tokenGroupsExplicitLocked(db_, "client_tokens", token_id);
    }
    bool scoped = tok.has_groups || explicit_groups;
    out = scoped ? scopeIntersect(owner, tok) : owner;
    out.token_groups = scoped;
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
    // tenants 也有 name，必须带别名限定，否则 join 后列名歧义
    if (!q.empty()) where = " WHERE d.devid LIKE ? OR d.name LIKE ?";
    long long total = 0;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, ("SELECT COUNT(*) FROM res_devices d" + where).c_str(), -1, &st, nullptr) !=
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
        "(SELECT COUNT(*) FROM perm_group_devices g WHERE g.devid=d.devid),"
        "d.tenant_id,COALESCE(t.name,'')"
        " FROM res_devices d LEFT JOIN tenants t ON t.id=d.tenant_id" + where +
        " ORDER BY d.devid LIMIT ? OFFSET ?";
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
        j["tenant_id"] = (long long)sqlite3_column_int64(st, 8);
        j["tenant_name"] = dbColText(st, 9);
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
            permScopeOfUser(std::get<0>(u), sc);
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
            permScopeOfAdminToken(std::get<0>(x), std::get<1>(x), sc);
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
    if (type == "user") syncUserRoleLocked(db_, id);
    if (type == "atk" || type == "rtk") {
        // 显式换组（包括清空）：之后组为空即无权限，不再继承归属账号
        const char* table = type == "atk" ? "admin_tokens" : "client_tokens";
        std::string sql = std::string("UPDATE ") + table + " SET groups_explicit=1 WHERE id=?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, atoll(id.c_str()));
            sqlite3_step(st);
        }
        sqlite3_finalize(st);
    }
    if (!superMembersExistLocked(db_)) {
        exec("ROLLBACK", e);
        err = "超级管理员组至少保留一个成员";
        return false;
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

bool AdminDb::permSubjectExists(const std::string& type, const std::string& id) {
    // 只接受数字串：组表里 user/atk/rtk/client 的 subject_id 都是主键，不接受用户名
    if (id.empty() || id.size() > 20) return false;
    for (char c : id)
        if (c < '0' || c > '9') return false;
    const char* table = nullptr;
    if (type == "user") table = "users";
    else if (type == "atk") table = "admin_tokens";
    else if (type == "rtk") table = "client_tokens";
    else if (type == "client") table = "client_accounts";
    else return false;
    std::lock_guard<std::mutex> lk(mu_);
    std::string sql = std::string("SELECT 1 FROM ") + table + " WHERE id=?";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(st, 1, atoll(id.c_str()));
    bool found = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return found;
}
