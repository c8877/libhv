// 运维 → 部署：把 agent / relay / client 装到指定的内网主机。
//
// 做法是最朴素也最可控的一条路：relay 上用 ssh/scp 登录目标主机，
// 把仓库里的安装包传上去，执行包内的 install.sh。
// 不依赖目标机预装任何 agent，也不需要中间的跳板服务。
//
// 凭据只落在 <admin.db 同目录>/deploy/ 下（0600），接口永远不回显。
// SSH/SFTP 走 libssh2（deploy_ssh.cpp）：不依赖 relay 主机上装 ssh/scp/sshpass，
// 密码和私钥都在进程内完成认证，sudo 的密码直接写进通道 stdin。
#include "admin_server.hpp"
#include "deploy_ssh.hpp"
#include "deploy_scan.hpp"
#include "hash_util.hpp"
#include "common/logging.hpp"
#include "cpputil/json.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <vector>

using nlohmann::json;

namespace {

// ---------------------------------------------------------------- 小工具

std::string dTrim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// 目标地址：字母数字和 . : _ -（IPv6 用冒号），不允许以 - 开头（避免被当成 ssh 选项）
bool dSafeHost(const std::string& s) {
    if (s.empty() || s.size() > 255 || s[0] == '-') return false;
    for (char c : s) {
        if (!isalnum((unsigned char)c) && c != '.' && c != ':' && c != '_' && c != '-') return false;
    }
    return true;
}

// 用户名/设备号/组件名这类单段标识
bool dSafeToken(const std::string& s, size_t maxlen = 64) {
    if (s.empty() || s.size() > maxlen || s[0] == '-') return false;
    for (char c : s) {
        if (!isalnum((unsigned char)c) && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

// 远端绝对路径：必须以 / 开头，不能有 ..，字符集收窄
bool dSafeAbsPath(const std::string& s) {
    if (s.empty() || s[0] != '/' || s.size() > 200) return false;
    if (s.find("..") != std::string::npos) return false;
    for (char c : s) {
        if (!isalnum((unsigned char)c) && c != '/' && c != '.' && c != '_' && c != '-') return false;
    }
    return true;
}

// 端口列表 "22,8080,9000-9010"
bool dSafePorts(const std::string& s) {
    if (s.empty()) return true;
    if (s.size() > 200) return false;
    for (char c : s) {
        if (!isdigit((unsigned char)c) && c != ',' && c != '-') return false;
    }
    return true;
}

std::string dQuote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

std::string dBaseDir(const std::string& db_path) {
    size_t slash = db_path.find_last_of('/');
    std::string dir = (slash == std::string::npos) ? std::string(".") : db_path.substr(0, slash);
    return dir + "/deploy";
}

bool dEnsureDir(const std::string& path) {
    if (mkdir(path.c_str(), 0700) == 0) return true;
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool dWriteFile(const std::string& path, const std::string& data, mode_t mode) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < data.size()) {
        ssize_t w = write(fd, data.data() + off, data.size() - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
    close(fd);
    return off == data.size();
}

bool dReadFile(const std::string& path, std::string& out) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

std::string dNowStamp() {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

// ---------------------------------------------------------------- 取消

std::mutex          g_run_mu;
std::set<long long> g_cancel;   // 请求取消的作业

bool dCanceled(long long job_id) {
    std::lock_guard<std::mutex> lk(g_run_mu);
    return g_cancel.count(job_id) > 0;
}

// ---------------------------------------------------------------- 主机

struct DeployHostCtx {
    long long   id = 0;
    std::string name, host, user, auth, note;
    int         port = 22;
    bool        sudo = false;
    std::string secret;       // 密码或私钥内容（只在内存里）
    std::string secret_path;  // 私钥文件路径（key 认证时用）
    std::string known_hosts;
};

void dFillHost(const std::string& db_path, const json& h, DeployHostCtx& out) {
    out.id = h.value("id", (long long)0);
    out.name = h.value("name", std::string());
    out.host = h.value("host", std::string());
    out.user = h.value("username", std::string());
    out.auth = h.value("auth", std::string("password"));
    out.note = h.value("note", std::string());
    out.port = h.value("port", 22);
    out.sudo = h.value("sudo", false);
    std::string base = dBaseDir(db_path);
    out.known_hosts = base + "/known_hosts";
    if (out.auth == "key") {
        std::string path = base + "/" + std::to_string(out.id) + ".key";
        dReadFile(path, out.secret);
        chmod(path.c_str(), 0600);
    } else {
        dReadFile(base + "/" + std::to_string(out.id) + ".secret", out.secret);
        while (!out.secret.empty() &&
               (out.secret.back() == '\n' || out.secret.back() == '\r'))
            out.secret.pop_back();
    }
}

// 一次作业共用一条 SSH 连接；失败时重新连一次（掉线/空闲被回收）
struct DeployConn {
    DeploySsh   ssh;
    bool        ready = false;
    DeployHostCtx h;

    bool ensure(const std::string& err_prefix, std::string& err) {
        if (ready) return true;
        DeploySshTarget t;
        t.host = h.host;
        t.port = h.port;
        t.user = h.user;
        t.auth = h.auth;
        t.secret = h.secret;
        t.known_hosts = h.known_hosts;
        if (!ssh.connect(t, err)) {
            err = err_prefix + err;
            return false;
        }
        ready = true;
        return true;
    }
};

std::string dSudoPrefix(const DeployHostCtx& h) {
    if (!h.sudo) return std::string();
    // 有密码时用 sudo -S 从 stdin 读；密钥登录一般已免密 sudo
    if (h.auth == "password") return "sudo -S -p '' ";
    return "sudo -n ";
}

struct StepOut {
    bool        ok = false;
    std::string out;
    std::string err;
};

StepOut dRemote(DeployConn& c, const std::string& cmd, int timeout_s, long long job_id) {
    StepOut r;
    std::string err;
    if (!c.ensure("连不上目标主机：", err)) {
        r.err = err;
        return r;
    }
    std::string stdin_data;
    if (c.h.sudo && c.h.auth == "password") stdin_data = c.h.secret + "\n";
    auto res = c.ssh.exec(cmd, stdin_data, timeout_s, [job_id] { return dCanceled(job_id); });
    r.out = res.out;
    if (res.ok) {
        r.ok = true;
        return r;
    }
    if (!res.err.empty()) {
        // 会话级错误（掉线/超时/取消）：下次调用重新连
        c.ready = false;
        c.ssh.close();
        r.err = res.err;
    } else {
        r.err = "退出码 " + std::to_string(res.status);
    }
    return r;
}

StepOut dUpload(DeployConn& c, const std::string& local, const std::string& remote,
                long long job_id) {
    StepOut r;
    std::string err;
    if (!c.ensure("连不上目标主机：", err)) {
        r.err = err;
        return r;
    }
    if (!c.ssh.upload(local, remote, 900, [job_id] { return dCanceled(job_id); }, err)) {
        if (!err.empty() && err != "已取消" && err != "上传超时") {
            c.ready = false;
            c.ssh.close();
        }
        r.err = err;
        return r;
    }
    r.ok = true;
    return r;
}

// ---------------------------------------------------------------- 安装包

struct PkgInfo {
    std::string name;       // 文件名
    std::string component;  // agent / relay / client
    std::string version;
    std::string arch;
    std::string variant;    // agent: lite/web
    long long   size = 0;
    long long   mtime_ms = 0;
};

// 包名约定：rtunnel-<component>[-<variant>]-<version>-<arch>.tar.gz
bool dParsePkg(const std::string& name, PkgInfo& p) {
    const std::string suffix = ".tar.gz";
    if (name.size() <= suffix.size() ||
        name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    std::string stem = name.substr(0, name.size() - suffix.size());
    if (stem.compare(0, 8, "rtunnel-") != 0) return false;
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= stem.size()) {
        size_t dash = stem.find('-', pos);
        parts.push_back(stem.substr(pos, (dash == std::string::npos ? stem.size() : dash) - pos));
        if (dash == std::string::npos) break;
        pos = dash + 1;
    }
    if (parts.size() < 4) return false;
    p.name = name;
    p.component = parts[1];
    if (p.component != "agent" && p.component != "relay" && p.component != "client") return false;
    p.arch = parts.back();
    p.version = parts[parts.size() - 2];
    if (p.component == "agent" && parts.size() >= 5) p.variant = parts[2];
    return true;
}

bool dListPackages(const std::string& repo_root, std::vector<PkgInfo>& out) {
    std::string dir = repo_root + "/deploy";
    DIR* d = opendir(dir.c_str());
    if (!d) return false;
    struct dirent* de;
    while ((de = readdir(d)) != nullptr) {
        if (de->d_name[0] == '.') continue;
        PkgInfo p;
        if (!dParsePkg(de->d_name, p)) continue;
        std::string path = dir + "/" + de->d_name;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
        p.size = (long long)st.st_size;
        p.mtime_ms = (long long)st.st_mtime * 1000;
        out.push_back(p);
    }
    closedir(d);
    std::sort(out.begin(), out.end(),
              [](const PkgInfo& a, const PkgInfo& b) { return a.mtime_ms > b.mtime_ms; });
    return true;
}

// ---------------------------------------------------------------- 作业日志

void dLog(AdminDb& db, long long job_id, const std::string& step, const std::string& line,
          const std::string& status = std::string(), const std::string& error = std::string()) {
    std::string add = "[" + dNowStamp() + "]" +
                      (step.empty() ? std::string(" ") : (" [" + step + "] ")) + line + "\n";
    db.deployJobUpdate(job_id, status.empty() ? "running" : status, step, error, add);
}

}  // namespace

// ---------------------------------------------------------------- HTTP

void AdminServer::handleDeployHosts(const HttpContextPtr& ctx) {
    std::string out;
    if (!db_.deployHostList(out)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleDeployHostCreate(const HttpContextPtr& ctx) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::string name = dTrim(req.value("name", std::string()));
    std::string host = dTrim(req.value("host", std::string()));
    std::string user = dTrim(req.value("username", std::string()));
    std::string auth = req.value("auth", std::string("password"));
    std::string note = req.value("note", std::string());
    int port = req.value("port", 22);
    bool sudo = req.value("sudo", false);
    if (name.empty()) name = host;
    if (!dSafeHost(host)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"主机地址不合法\"}");
        return;
    }
    if (!dSafeToken(user)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"用户名不合法\"}");
        return;
    }
    std::string secret = req.value("secret", std::string());
    if (secret.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"需要提供密码或私钥\"}");
        return;
    }
    std::string err;
    long long id = 0;
    if (!db_.deployHostSave(0, name, host, port, user, auth, sudo, note, id, err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":" + json(err).dump() + "}");
        return;
    }
    std::string base = dBaseDir(cfg_.db_path);
    dEnsureDir(base);
    std::string path = base + "/" + std::to_string(id) + (auth == "key" ? ".key" : ".secret");
    if (!dWriteFile(path, auth == "key" ? secret : secret + "\n", 0600)) {
        db_.deployHostDelete(id);
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"凭据保存失败\"}");
        return;
    }
    chmod(path.c_str(), 0600);
    db_.deployHostSetSecret(id, true);
    auditEx("deploy.host.add", "deploy_host", std::to_string(id), "ok",
            host + ":" + std::to_string(port) + " " + user);
    json out;
    out["id"] = id;
    ctx->send(out.dump());
}

void AdminServer::handleDeployHostUpdate(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string cur;
    if (id <= 0 || !db_.deployHostGet(id, cur)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"host not found\"}");
        return;
    }
    auto old = json::parse(cur, nullptr, false);
    auto req = json::parse(ctx->body(), nullptr, false);
    if (old.is_discarded() || req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::string name = dTrim(req.value("name", old.value("name", std::string())));
    std::string host = dTrim(req.value("host", old.value("host", std::string())));
    std::string user = dTrim(req.value("username", old.value("username", std::string())));
    std::string auth = req.value("auth", old.value("auth", std::string("password")));
    std::string note = req.value("note", old.value("note", std::string()));
    int port = req.value("port", old.value("port", 22));
    bool sudo = req.value("sudo", old.value("sudo", false));
    if (name.empty()) name = host;
    if (!dSafeHost(host) || !dSafeToken(user)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"主机地址或用户名不合法\"}");
        return;
    }
    std::string err;
    if (!db_.deployHostSave(id, name, host, port, user, auth, sudo, note, id, err)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":" + json(err).dump() + "}");
        return;
    }
    // 换认证方式要把旧凭据删掉，避免钥匙串里留一份
    std::string base = dBaseDir(cfg_.db_path);
    if (auth != old.value("auth", std::string("password"))) {
        unlink((base + "/" + std::to_string(id) + ".secret").c_str());
        unlink((base + "/" + std::to_string(id) + ".key").c_str());
        db_.deployHostSetSecret(id, false);
    }
    std::string secret = req.value("secret", std::string());
    if (!secret.empty()) {
        dEnsureDir(base);
        std::string path = base + "/" + std::to_string(id) + (auth == "key" ? ".key" : ".secret");
        if (!dWriteFile(path, auth == "key" ? secret : secret + "\n", 0600)) {
            ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
            ctx->send("{\"error\":\"凭据保存失败\"}");
            return;
        }
        chmod(path.c_str(), 0600);
        db_.deployHostSetSecret(id, true);
    }
    auditEx("deploy.host.update", "deploy_host", std::to_string(id), "ok", host + ":" + std::to_string(port));
    ctx->send("{\"ok\":true}");
}

void AdminServer::handleDeployHostDelete(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    if (id <= 0) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad id\"}");
        return;
    }
    if (db_.deployJobRunningForHost(id)) {
        ctx->response->status_code = HTTP_STATUS_CONFLICT;
        ctx->send("{\"error\":\"这台主机还有正在执行的部署作业\"}");
        return;
    }
    std::string base = dBaseDir(cfg_.db_path);
    unlink((base + "/" + std::to_string(id) + ".secret").c_str());
    unlink((base + "/" + std::to_string(id) + ".key").c_str());
    db_.deployHostDelete(id);
    auditEx("deploy.host.delete", "deploy_host", std::to_string(id), "ok", "");
    ctx->send("{\"ok\":true}");
}

// 连接测试 + 环境探测：返回架构、是否 root、是否 systemd、/tmp 剩余、已装版本
void AdminServer::handleDeployProbe(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string cur;
    if (id <= 0 || !db_.deployHostGet(id, cur)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"host not found\"}");
        return;
    }
    auto hj = json::parse(cur, nullptr, false);
    if (hj.is_discarded()) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"bad host row\"}");
        return;
    }
    DeployHostCtx h;
    dFillHost(cfg_.db_path, hj, h);
    if (h.secret.empty()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"这台主机还没有保存凭据\"}");
        return;
    }
    std::string cmd =
        "echo \"ARCH=$(uname -m)\"; echo \"UID=$(id -u)\"; "
        "echo \"HOSTNAME=$(hostname 2>/dev/null)\"; "
        "if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then "
        "echo SYSTEMD=1; else echo SYSTEMD=0; fi; "
        "echo \"FREE_KB=$(df -Pk /tmp 2>/dev/null | awk 'NR==2{print $4}')\"; "
        "if [ -x /opt/rtunnel/agent/install.sh ]; then "
        "/opt/rtunnel/agent/install.sh --status 2>/dev/null | sed 's/^/AGENT_/'; "
        "elif [ -x /opt/rtunnel/relay/install.sh ]; then "
        "/opt/rtunnel/relay/install.sh --status 2>/dev/null | sed 's/^/RELAY_/'; "
        "else echo INSTALLED=none; fi";
    DeployConn pc;
    pc.h = h;
    auto r = dRemote(pc, cmd, 25, 0);
    if (!r.ok) {
        db_.deployHostMark(id, "unreachable");
        json out;
        out["ok"] = false;
        out["error"] = r.err + (r.out.empty() ? std::string() : ("：" + dTrim(r.out)));
        ctx->response->status_code = HTTP_STATUS_BAD_GATEWAY;
        ctx->send(out.dump());
        return;
    }
    json out;
    out["ok"] = true;
    out["raw"] = r.out;
    auto grab = [&](const char* key) -> std::string {
        std::string k = std::string(key) + "=";
        size_t p = r.out.find(k);
        if (p == std::string::npos) return std::string();
        p += k.size();
        size_t e = r.out.find('\n', p);
        std::string v = r.out.substr(p, (e == std::string::npos ? r.out.size() : e) - p);
        if (!v.empty() && v.back() == '\r') v.pop_back();
        return dTrim(v);
    };
    out["arch"] = grab("ARCH");
    out["uid"] = atoi(grab("UID").c_str());
    out["hostname"] = grab("HOSTNAME");
    out["systemd"] = grab("SYSTEMD") == "1";
    out["free_kb"] = atoll(grab("FREE_KB").c_str());
    out["installed"] = grab("INSTALLED");
    out["agent_status"] = grab("AGENT_current");
    out["relay_status"] = grab("RELAY_current");
    db_.deployHostMark(id, "ok");
    auditEx("deploy.probe", "deploy_host", std::to_string(id), "ok",
            "arch=" + out.value("arch", std::string()));
    ctx->send(out.dump());
}

// 扫描网段里开着 SSH 的主机：nmap 优先，没有 nmap 的镜像退回内置 TCP 扫描
void AdminServer::handleDeployScan(const HttpContextPtr& ctx) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    std::string cidr = dTrim(req.value("cidr", std::string()));
    int port = req.value("port", 22);
    int timeout_ms = req.value("timeout_ms", 800);
    std::string engine = req.value("engine", std::string("auto"));
    if (engine != "nmap" && engine != "tcp") engine = "auto";
    if (port < 1 || port > 65535) port = 22;
    auto r = deployScanSubnet(cidr, port, timeout_ms, engine);
    if (!r.ok) {
        json out;
        out["error"] = r.error;
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send(out.dump());
        return;
    }
    // 已经登记过的主机标出来，页面上就不用重复添加
    std::map<std::string, long long> known;
    std::string hosts_raw;
    if (db_.deployHostList(hosts_raw)) {
        auto hj = json::parse(hosts_raw, nullptr, false);
        if (hj.is_object() && hj.contains("items")) {
            for (auto& h : hj["items"]) {
                known[h.value("host", std::string()) + ":" +
                      std::to_string(h.value("port", 22))] = h.value("id", (long long)0);
            }
        }
    }
    json items = json::array();
    for (auto& h : r.items) {
        json o;
        o["host"] = h.host;
        o["port"] = h.port;
        o["service"] = h.service;
        auto it = known.find(h.host + ":" + std::to_string(h.port));
        o["known"] = (it != known.end());
        if (it != known.end()) o["host_id"] = it->second;
        items.push_back(o);
    }
    json out;
    out["engine"] = r.engine;
    out["cidr"] = cidr;
    out["port"] = port;
    out["scanned"] = r.scanned;
    out["duration_ms"] = r.duration_ms;
    out["items"] = items;
    auditEx("deploy.scan", "deploy_host", cidr, "ok",
            "engine=" + r.engine + " found=" + std::to_string(items.size()));
    ctx->send(out.dump());
}

void AdminServer::handleDeployPackages(const HttpContextPtr& ctx) {
    json out;
    json items = json::array();
    std::vector<PkgInfo> pkgs;
    if (cfg_.file_dir.empty() || !dListPackages(cfg_.file_dir, pkgs)) {
        out["items"] = items;
        out["hint"] = "仓库目录未配置或 deploy/ 下还没有安装包";
        ctx->send(out.dump());
        return;
    }
    for (auto& p : pkgs) {
        json j;
        j["name"] = p.name;
        j["component"] = p.component;
        j["version"] = p.version;
        j["arch"] = p.arch;
        j["variant"] = p.variant;
        j["size"] = p.size;
        j["mtime_ms"] = p.mtime_ms;
        items.push_back(j);
    }
    out["items"] = items;
    ctx->send(out.dump());
}

void AdminServer::handleDeployJobCreate(const HttpContextPtr& ctx) {
    auto req = json::parse(ctx->body(), nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"bad json\"}");
        return;
    }
    long long host_id = req.value("host_id", (long long)0);
    std::string cur;
    if (host_id <= 0 || !db_.deployHostGet(host_id, cur)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"host not found\"}");
        return;
    }
    auto hj = json::parse(cur, nullptr, false);
    std::string component = req.value("component", std::string());
    if (component != "agent" && component != "relay" && component != "client") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"component 只能是 agent/relay/client\"}");
        return;
    }
    std::string pkg = req.value("package", std::string());
    PkgInfo pi;
    if (!dParsePkg(pkg, pi) || pi.component != component) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"安装包名不合法或与组件不匹配\"}");
        return;
    }
    std::string path;
    struct stat st{};
    if (cfg_.file_dir.empty() || !adminRepoPath(cfg_.file_dir, "deploy/" + pkg, path) ||
        stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"安装包不存在，先上传到 deploy/ 目录\"}");
        return;
    }
    std::string variant = req.value("variant", pi.variant);
    if (component == "agent" && variant != "lite" && variant != "web") {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"agent 变体只能是 lite 或 web\"}");
        return;
    }
    std::string prefix = dTrim(req.value("prefix", std::string()));
    if (prefix.empty()) {
        prefix = component == "agent" ? "/opt/rtunnel/agent"
                 : component == "relay" ? "/opt/rtunnel/relay"
                                        : "/opt/rtunnel/client";
    }
    if (!dSafeAbsPath(prefix)) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"安装目录必须是绝对路径且不含 ..\"}");
        return;
    }
    json options = req.value("options", json::object());
    if (!options.is_object()) options = json::object();
    if (req.contains("no_units") && !req["no_units"].is_boolean()) {
        ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
        ctx->send("{\"error\":\"no_units 必须是布尔值\"}");
        return;
    }
    bool no_units = req.value("no_units", false);
    options["no_units"] = no_units;
    // 组件参数校验（这些值会被拼进远端命令，先卡死字符集）
    if (component == "agent") {
        std::string devid = options.value("devid", std::string());
        std::string server = options.value("server", std::string());
        if (!dSafeToken(devid)) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"设备 ID 只能是字母数字和 . _ -\"}");
            return;
        }
        if (!dSafeHost(server) || server.find(':') == std::string::npos) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"relay 地址要写成 host:port\"}");
            return;
        }
        if (!dSafePorts(options.value("allow", std::string()))) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"端口白名单格式不对（如 22,8080）\"}");
            return;
        }
    }
    if (component == "client" || component == "relay") {
        std::string server = options.value("server", std::string());
        if (!server.empty() && (!dSafeHost(server) || server.find(':') == std::string::npos)) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"relay 地址要写成 host:port\"}");
            return;
        }
        int admin_port = options.value("admin_port", 9443);
        if (admin_port < 1 || admin_port > 65535) {
            ctx->response->status_code = HTTP_STATUS_BAD_REQUEST;
            ctx->send("{\"error\":\"管理端口不合法\"}");
            return;
        }
    }
    if (db_.deployJobRunningForHost(host_id)) {
        ctx->response->status_code = HTTP_STATUS_CONFLICT;
        ctx->send("{\"error\":\"这台主机已有排队中或执行中的部署，等它结束再试\"}");
        return;
    }
    std::string actor = actorLabel();
    long long job_id = 0;
    std::string err;
    if (!db_.deployJobCreate(host_id, hj.value("host", std::string()), hj.value("username", std::string()),
                             component, pi.version, pi.arch, variant, pkg, prefix,
                             options.dump(), actor, job_id, err)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":" + json(err).dump() + "}");
        return;
    }
    auditEx("deploy.job.create", "deploy_job", std::to_string(job_id), "ok",
            component + " -> " + hj.value("host", std::string()) + " " + pi.version);
    {
        std::lock_guard<std::mutex> lk(g_run_mu);
        g_cancel.erase(job_id);
    }
    spawnWorker([this, job_id]() { deployRunJob(job_id); });
    json out;
    out["id"] = job_id;
    ctx->send(out.dump());
}

void AdminServer::handleDeployJobs(const HttpContextPtr& ctx) {
    int limit = atoi(ctx->param("limit").c_str());
    std::string out;
    if (!db_.deployJobList(limit, out)) {
        ctx->response->status_code = HTTP_STATUS_INTERNAL_SERVER_ERROR;
        ctx->send("{\"error\":\"db error\"}");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleDeployJobGet(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out;
    if (id <= 0 || !db_.deployJobGet(id, out)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"job not found\"}");
        return;
    }
    ctx->send(out);
}

void AdminServer::handleDeployJobCancel(const HttpContextPtr& ctx) {
    long long id = atoll(ctx->param("id").c_str());
    std::string out;
    if (id <= 0 || !db_.deployJobGet(id, out)) {
        ctx->response->status_code = HTTP_STATUS_NOT_FOUND;
        ctx->send("{\"error\":\"job not found\"}");
        return;
    }
    auto j = json::parse(out, nullptr, false);
    std::string status = j.value("status", std::string());
    if (status != "queued" && status != "running") {
        ctx->response->status_code = HTTP_STATUS_CONFLICT;
        ctx->send("{\"error\":\"作业已经结束\"}");
        return;
    }
    // 只置标志：执行线程在每个读循环里检查，随后断开通道
    {
        std::lock_guard<std::mutex> lk(g_run_mu);
        g_cancel.insert(id);
    }
    auditEx("deploy.job.cancel", "deploy_job", std::to_string(id), "ok", "");
    ctx->send("{\"ok\":true}");
}

// ---------------------------------------------------------------- 执行

void AdminServer::deployRunJob(long long job_id) {
    std::string job_json;
    if (!db_.deployJobGet(job_id, job_json)) return;
    auto job = json::parse(job_json, nullptr, false);
    if (job.is_discarded()) return;
    long long host_id = job.value("host_id", (long long)0);
    std::string component = job.value("component", std::string());
    std::string pkg = job.value("pkg", std::string());
    std::string prefix = job.value("prefix", std::string());
    std::string variant = job.value("variant", std::string());
    json options = json::parse(job.value("options", std::string("{}")), nullptr, false);
    if (!options.is_object()) options = json::object();
    bool no_units = options.value("no_units", false);

    std::string host_json;
    if (!db_.deployHostGet(host_id, host_json)) {
        dLog(db_, job_id, "准备", "主机记录不存在", "failed", "host not found");
        return;
    }
    auto hj = json::parse(host_json, nullptr, false);
    DeployHostCtx h;
    dFillHost(cfg_.db_path, hj, h);
    DeployConn conn;
    conn.h = h;

    std::string actor = job.value("actor", std::string("system"));
    auto fail = [&](const std::string& step, const std::string& msg) {
        {
            std::lock_guard<std::mutex> lk(g_run_mu);
            g_cancel.erase(job_id);
        }
        dLog(db_, job_id, step, "失败：" + msg, "failed", msg);
        json ev;
        ev["id"] = job_id;
        ev["component"] = component;
        ev["host"] = h.host;
        ev["status"] = "failed";
        ev["error"] = msg;
        emitEvent("deploy", ev.dump());
        auditAs(actor, "deploy.job.failed", std::to_string(job_id) + " " + component + " " + msg);
    };
    auto cancel = [&](const std::string& step) {
        {
            std::lock_guard<std::mutex> lk(g_run_mu);
            g_cancel.erase(job_id);
        }
        dLog(db_, job_id, step, "已取消", "canceled", "用户取消");
        json ev;
        ev["id"] = job_id;
        ev["component"] = component;
        ev["host"] = h.host;
        ev["status"] = "canceled";
        emitEvent("deploy", ev.dump());
    };

    if (h.secret.empty()) {
        fail("准备", "这台主机没有可用的凭据，请重新保存密码或私钥");
        return;
    }
    dLog(db_, job_id, "准备",
         "开始部署 " + component + " " + job.value("version", std::string()) +
             " 到 " + h.user + "@" + h.host + ":" + std::to_string(h.port));

    // 1. 预检：连得上、架构对得上、不是 Windows
    dLog(db_, job_id, "预检", "检查连通性与目标架构");
    auto pre = dRemote(conn,
                       "echo \"ARCH=$(uname -m)\"; echo \"UID=$(id -u)\"; "
                       "if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then "
                       "echo SYSTEMD=1; else echo SYSTEMD=0; fi",
                       30, job_id);
    if (!pre.ok) {
        if (dCanceled(job_id)) { cancel("预检"); return; }
        fail("预检", pre.err + (pre.out.empty() ? "" : " " + dTrim(pre.out)));
        return;
    }
    auto grab = [&](const char* key) -> std::string {
        std::string k = std::string(key) + "=";
        size_t p = pre.out.find(k);
        if (p == std::string::npos) return std::string();
        p += k.size();
        size_t e = pre.out.find('\n', p);
        return dTrim(pre.out.substr(p, (e == std::string::npos ? pre.out.size() : e) - p));
    };
    std::string arch = grab("ARCH");
    bool systemd = grab("SYSTEMD") == "1";
    int uid = atoi(grab("UID").c_str());
    std::string pkg_arch = job.value("arch", std::string());
    if (!arch.empty() && !pkg_arch.empty() && arch != pkg_arch) {
        fail("预检", "架构不匹配：安装包是 " + pkg_arch + "，目标是 " + arch);
        return;
    }
    dLog(db_, job_id, "预检", "架构 " + arch + "，systemd=" + (systemd ? "1" : "0") +
                                      "，uid=" + std::to_string(uid));

    // 只有装到系统默认目录才强制 root/sudo；自定义前缀按目标目录权限自然失败
    bool system_prefix = prefix == "/opt/rtunnel/agent" || prefix == "/opt/rtunnel/relay" ||
                         prefix == "/opt/rtunnel/client";
    if (uid != 0 && !h.sudo && system_prefix && !no_units) {
        fail("预检", "登录用户不是 root 且没有启用 sudo，无法写入 " + prefix + " 与 systemd");
        return;
    }

    // 2. 传包
    std::string remote_dir = "/tmp/rtunnel-deploy-" + std::to_string(job_id);
    std::string local_pkg = cfg_.file_dir + "/deploy/" + pkg;
    dLog(db_, job_id, "上传", "传输 " + pkg);
    auto mk = dRemote(conn, "mkdir -p " + dQuote(remote_dir), 20, job_id);
    if (!mk.ok) {
        if (dCanceled(job_id)) { cancel("上传"); return; }
        fail("上传", "创建临时目录失败：" + mk.err);
        return;
    }
    auto scp = dUpload(conn, local_pkg, remote_dir + "/" + pkg, job_id);
    if (!scp.ok) {
        if (dCanceled(job_id)) { cancel("上传"); return; }
        fail("上传", "传包失败：" + scp.err + (scp.out.empty() ? "" : " " + dTrim(scp.out)));
        return;
    }

    // 3. 解包（包内一层目录，--strip-components=1 摊平）
    dLog(db_, job_id, "解包", "解压到 " + remote_dir);
    std::string tar_cmd = "cd " + dQuote(remote_dir) + " && tar -xzf " + dQuote(pkg) +
                          " --strip-components=1 -C " + dQuote(remote_dir);
    auto tx = dRemote(conn, tar_cmd, 300, job_id);
    if (!tx.ok) {
        if (dCanceled(job_id)) { cancel("解包"); return; }
        fail("解包", "解压失败：" + tx.err + (tx.out.empty() ? "" : " " + dTrim(tx.out)));
        return;
    }

    const std::string sudop = dSudoPrefix(h);
    auto feed = [&]() { return (h.sudo && h.auth == "password") ? h.secret + "\n" : std::string(); };

    // 4. 写配置（必须在 install.sh --start 之前，agent 会检查 args 文件）
    if (component == "agent") {
        std::string devid = options.value("devid", std::string());
        std::string server = options.value("server", std::string());
        std::string allow = options.value("allow", std::string());
        std::string args = "--devid " + devid + " --server " + server;
        if (!allow.empty()) args += " --allow " + allow;
        std::string conf_dir = (prefix == "/opt/rtunnel/agent") ? std::string("/opt/data/etc")
                                                                : (prefix + "/etc");
        std::string args_path = conf_dir + "/rtunnel-agent.args";
        std::string body = "AGENT_ARGS=" + dQuote(args) + "\n";
        std::string cmd = "mkdir -p " + dQuote(conf_dir) + " && printf %s " + dQuote(body) +
                          " > " + dQuote(args_path + ".new") + " && chmod 0644 " +
                          dQuote(args_path + ".new") + " && mv -f " + dQuote(args_path + ".new") +
                          " " + dQuote(args_path);
        dLog(db_, job_id, "配置", "写入 " + args_path + "（devid=" + devid +
                                      " server=" + server + "）");
        auto cr = dRemote(conn, sudop + "sh -c " + dQuote(cmd), 60, job_id);
        if (!cr.ok) {
            if (dCanceled(job_id)) { cancel("配置"); return; }
            fail("配置", "写 agent 参数失败：" + cr.err + (cr.out.empty() ? "" : " " + dTrim(cr.out)));
            return;
        }
    } else if (component == "relay") {
        int admin_port = options.value("admin_port", 9443);
        std::string token = options.value("admin_token", std::string());
        bool token_generated = token.empty();
        if (token_generated) token = "rt-" + AdminDb::randomHex(12);
        std::string relay_args = "--admin 0.0.0.0:" + std::to_string(admin_port) +
                                 " --admin-tls-selfsigned --admin-token " + token;
        std::string conf_dir = (prefix == "/opt/rtunnel/relay") ? std::string("/etc/rtunnel")
                                                                : (prefix + "/etc");
        std::string conf = conf_dir + "/relay.env";
        std::string body = "RELAY_ARGS=" + dQuote(relay_args) + "\n";
        std::string cmd = "mkdir -p " + dQuote(conf_dir) + " && printf %s " + dQuote(body) +
                          " > " + dQuote(conf + ".new") + " && chmod 0600 " + dQuote(conf + ".new") +
                          " && mv -f " + dQuote(conf + ".new") + " " + dQuote(conf);
        dLog(db_, job_id, "配置", "写入 " + conf + "（管理端口 " +
                                             std::to_string(admin_port) + "）");
        auto cr = dRemote(conn, sudop + "sh -c " + dQuote(cmd), 60, job_id);
        if (!cr.ok) {
            if (dCanceled(job_id)) { cancel("配置"); return; }
            fail("配置", "写 relay 配置失败：" + cr.err + (cr.out.empty() ? "" : " " + dTrim(cr.out)));
            return;
        }
        if (token_generated)
            dLog(db_, job_id, "配置", "管理台初始令牌：" + token + "（登录后请修改）");
    } else if (component == "client") {
        std::string server = options.value("server", std::string());
        if (!server.empty()) {
            // 与 install.sh 一致：默认前缀用 /etc/rtunnel，自定义前缀用 <prefix>/etc
            std::string conf_dir = (prefix == "/opt/rtunnel/client") ? std::string("/etc/rtunnel")
                                                                     : (prefix + "/etc");
            std::string conf = conf_dir + "/client.env";
            std::string body = "RT=\"https://127.0.0.1:9443\"\nATK_FILE=\"" + conf_dir +
                               "/atk\"\nSERVER=\"" + server + "\"\nTOKEN=\"\"\n";
            std::string cmd = "mkdir -p " + dQuote(conf_dir) + " && printf %s " + dQuote(body) +
                              " > " + dQuote(conf + ".new") + " && chmod 0600 " +
                              dQuote(conf + ".new") + " && mv -f " + dQuote(conf + ".new") + " " +
                              dQuote(conf);
            dLog(db_, job_id, "配置", "写入 " + conf + "（SERVER=" + server + "）");
            auto cr = dRemote(conn, sudop + "sh -c " + dQuote(cmd), 60, job_id);
            if (!cr.ok) {
                if (dCanceled(job_id)) { cancel("配置"); return; }
                fail("配置", "写 client 配置失败：" + cr.err);
                return;
            }
        }
    }

    // 5. 安装
    std::string inst = "sh " + dQuote(remote_dir + "/install.sh") + " --pkg-dir " + dQuote(remote_dir) +
                       " --prefix " + dQuote(prefix);
    if (component == "agent") {
        inst += " --variant " + dQuote(variant) + " --version " + dQuote(job.value("version", std::string()));
    }
    // 容器/临时前缀里不接管 systemd：只装文件，--no-units 且不自动启动
    if (no_units) {
        inst += " --no-units";
    } else {
        inst += " --start";
    }
    dLog(db_, job_id, "安装", "执行包内 install.sh");
    auto in = dRemote(conn, sudop + "sh -c " + dQuote(inst), 600, job_id);
    if (!in.ok) {
        if (dCanceled(job_id)) { cancel("安装"); return; }
        std::string tail = dTrim(in.out);
        if (tail.size() > 4000) tail = tail.substr(tail.size() - 4000);
        fail("安装", "install.sh 失败（" + in.err + "）：" + tail);
        return;
    }
    {
        std::string tail = dTrim(in.out);
        if (tail.size() > 4000) tail = tail.substr(tail.size() - 4000);
        dLog(db_, job_id, "安装", tail);
    }

    // 6. 验证
    dLog(db_, job_id, "验证", "确认版本与运行状态");
    std::string verify;
    if (component == "relay") {
        int admin_port = options.value("admin_port", 9443);
        verify = "echo \"VER=$(" + prefix + "/bin/rtunnel-relay --version 2>/dev/null | tr -d '\\r')\"; "
                 "if [ -d /run/systemd/system ]; then systemctl is-active rtunnel-relay 2>/dev/null | "
                 "sed 's/^/SVC=/'; else echo SVC=unknown; fi; "
                 "curl -sk -o /dev/null -w 'HTTP=%{http_code}\\n' https://127.0.0.1:" +
                 std::to_string(admin_port) + "/ 2>/dev/null || echo HTTP=000";
    } else if (component == "agent") {
        verify = "echo \"VER=$(" + prefix + "/bin/rtunnel-agent --version 2>/dev/null | tr -d '\\r')\"; "
                 "if [ -d /run/systemd/system ]; then systemctl is-active rtunnel-agent 2>/dev/null | "
                 "sed 's/^/SVC=/'; else echo SVC=unknown; fi; "
                 "pgrep -f '" + prefix + "/bin/rtunnel-agent' >/dev/null 2>&1 && echo PROC=1 || echo PROC=0";
    } else {
        verify = "echo \"VER=$(" + prefix + "/bin/rtunnel-client --version 2>/dev/null | tr -d '\\r')\"; "
                 "if [ -d /run/systemd/system ]; then systemctl is-active rtunnel-client-socks 2>/dev/null | "
                 "sed 's/^/SVC=/'; else echo SVC=unknown; fi";
    }
    auto vf = dRemote(conn, verify, 60, job_id);
    std::string vtxt = dTrim(vf.out);
    dLog(db_, job_id, "验证", vtxt.empty() ? "没有返回状态" : vtxt);

    bool svc_ok = true;
    if (no_units) {
        // 明确没接管服务，只看安装出来的版本
        if (vtxt.find("VER=") == std::string::npos || vtxt.find("VER=\n") == 0) svc_ok = false;
    } else {
        if (vtxt.find("SVC=inactive") != std::string::npos ||
            vtxt.find("SVC=failed") != std::string::npos)
            svc_ok = false;
        if (component == "agent" && vtxt.find("PROC=0") != std::string::npos && systemd)
            svc_ok = false;
        if (component == "relay" && vtxt.find("HTTP=000") != std::string::npos) svc_ok = false;
    }
    if (!vf.ok || !svc_ok) {
        fail("验证", "文件装好了但服务没有正常起来：" + vtxt);
        return;
    }

    // 7. 清理临时目录（失败不算部署失败）
    dRemote(conn, "rm -rf " + dQuote(remote_dir), 30, job_id);

    {
        std::lock_guard<std::mutex> lk(g_run_mu);
        g_cancel.erase(job_id);
    }

    std::string okmsg = "部署完成：" + component + " " + job.value("version", std::string()) +
                        " @ " + h.host;
    db_.deployHostMark(host_id, "ok");
    db_.deployJobUpdate(job_id, "success", "完成", "", "[" + dNowStamp() + "] " + okmsg + "\n");
    json ev;
    ev["id"] = job_id;
    ev["component"] = component;
    ev["host"] = h.host;
    ev["status"] = "success";
    ev["version"] = job.value("version", std::string());
    emitEvent("deploy", ev.dump());
    auditAs(actor, "deploy.job.success",
            std::to_string(job_id) + " " + component + " " + h.host + " " +
                job.value("version", std::string()));
}
