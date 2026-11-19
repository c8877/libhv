// 设备网络与 IPC 快照采集（见 net_service.hpp）
#include "net_service.hpp"

#include <arpa/inet.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>
#include <vector>

using nlohmann::json;

namespace rtnet {
namespace {

const size_t kMaxFile = 4 << 20;

struct SockEntry {
    std::string local_addr;
    int local_port = 0;
    std::string remote_addr;
    int remote_port = 0;
    std::string state;
    int state_code = 0;
    unsigned long inode = 0;
    int uid = 0;
    int pid = 0;
    std::string comm;
};

struct UnixEntry {
    std::string path;
    bool abstract = false;
    std::string type;   // STREAM / DGRAM / SEQPACKET
    std::string state;  // LISTEN / CONNECTED / ...
    int state_code = 0;
    int type_code = 0;
    unsigned long inode = 0;
    int pid = 0;
    std::string comm;
};

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string readFileText(const char* path, size_t max = kMaxFile) {
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) return std::string();
    std::string s;
    char buf[8192];
    while (s.size() < max && f.read(buf, sizeof buf)) s.append(buf, (size_t)f.gcount());
    s.append(buf, (size_t)f.gcount());
    if (s.size() > max) s.resize(max);
    return s;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::string();
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string commOf(int pid) {
    if (pid <= 0) return std::string();
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/comm", pid);
    return trim(readFileText(p, 4096));
}

std::string ip4Text(const std::string& h) {
    unsigned long v = strtoul(h.c_str(), nullptr, 16);
    char b[32];
    snprintf(b, sizeof b, "%lu.%lu.%lu.%lu", v & 0xff, (v >> 8) & 0xff, (v >> 16) & 0xff,
             (v >> 24) & 0xff);
    return b;
}

// /proc/net/tcp6 每 32 位按小端存放
std::string ip6Text(const std::string& h) {
    if (h.size() < 32) return h;
    unsigned char raw[16];
    for (int w = 0; w < 4; ++w) {
        unsigned long v = strtoul(h.substr((size_t)w * 8, 8).c_str(), nullptr, 16);
        raw[w * 4 + 0] = (unsigned char)(v & 0xff);
        raw[w * 4 + 1] = (unsigned char)((v >> 8) & 0xff);
        raw[w * 4 + 2] = (unsigned char)((v >> 16) & 0xff);
        raw[w * 4 + 3] = (unsigned char)((v >> 24) & 0xff);
    }
    char out[64];
    if (!inet_ntop(AF_INET6, raw, out, sizeof out)) return h;
    return out;
}

const char* tcpStateName(int st) {
    switch (st) {
        case 0x01: return "ESTABLISHED";
        case 0x02: return "SYN_SENT";
        case 0x03: return "SYN_RECV";
        case 0x04: return "FIN_WAIT1";
        case 0x05: return "FIN_WAIT2";
        case 0x06: return "TIME_WAIT";
        case 0x07: return "CLOSE";
        case 0x08: return "CLOSE_WAIT";
        case 0x09: return "LAST_ACK";
        case 0x0A: return "LISTEN";
        case 0x0B: return "CLOSING";
        case 0x0C: return "NEW_SYN_RECV";
        default: return "UNKNOWN";
    }
}

const char* udpStateName(int st) {
    switch (st) {
        case 0x01: return "ESTAB";
        case 0x07: return "UNCONN";
        default: return "UNKNOWN";
    }
}

const char* unixTypeName(int t) {
    switch (t) {
        case 1: return "STREAM";
        case 2: return "DGRAM";
        case 5: return "SEQPACKET";
        default: return "OTHER";
    }
}

// scope 为空表示读取失败，调用方按空表处理
std::vector<SockEntry> loadSocks(const char* path, bool v6, bool tcp) {
    std::vector<SockEntry> out;
    std::string text = readFileText(path);
    if (text.empty()) return out;
    std::istringstream in(text);
    std::string line;
    std::getline(in, line); // 表头
    while (std::getline(in, line)) {
        std::istringstream is(line);
        // sl local rem st tx:rx tr:tm retrnsmt uid timeout inode
        std::string sl, local, remote, st, txrx, tr, retr, uid, timeout, inode;
        if (!(is >> sl >> local >> remote >> st >> txrx >> tr >> retr >> uid >> timeout >> inode)) continue;
        size_t c1 = local.rfind(':');
        size_t c2 = remote.rfind(':');
        if (c1 == std::string::npos || c2 == std::string::npos) continue;
        SockEntry e;
        e.local_addr = v6 ? ip6Text(local.substr(0, c1)) : ip4Text(local.substr(0, c1));
        e.local_port = (int)strtoul(local.substr(c1 + 1).c_str(), nullptr, 16);
        e.remote_addr = v6 ? ip6Text(remote.substr(0, c2)) : ip4Text(remote.substr(0, c2));
        e.remote_port = (int)strtoul(remote.substr(c2 + 1).c_str(), nullptr, 16);
        e.state_code = (int)strtoul(st.c_str(), nullptr, 16);
        e.state = tcp ? tcpStateName(e.state_code) : udpStateName(e.state_code);
        e.uid = (int)strtoul(uid.c_str(), nullptr, 10);
        e.inode = strtoul(inode.c_str(), nullptr, 10);
        out.push_back(e);
    }
    return out;
}

std::vector<UnixEntry> loadUnix() {
    std::vector<UnixEntry> out;
    std::string text = readFileText("/proc/net/unix");
    if (text.empty()) return out;
    std::istringstream in(text);
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        std::istringstream is(line);
        std::string num, ref, proto, flags, type, st, inode;
        if (!(is >> num >> ref >> proto >> flags >> type >> st >> inode)) continue;
        UnixEntry e;
        e.type_code = (int)strtoul(type.c_str(), nullptr, 16);
        e.type = unixTypeName(e.type_code);
        e.state_code = (int)strtoul(st.c_str(), nullptr, 16);
        e.inode = strtoul(inode.c_str(), nullptr, 10);
        std::string path;
        std::getline(is, path);
        e.path = trim(path);
        if (!e.path.empty() && e.path[0] == '@') e.abstract = true;
        switch (e.state_code) {
            case 0x01:
                e.state = (e.type_code == 1) ? "LISTEN" : "UNCONN";
                break;
            case 0x02: e.state = "CONNECTING"; break;
            case 0x03: e.state = "CONNECTED"; break;
            case 0x04: e.state = "DISCONNECTING"; break;
            default: e.state = "UNKNOWN"; break;
        }
        out.push_back(e);
    }
    return out;
}

// 只扫需要的 inode，找齐即停
void mapSocketOwners(const std::set<unsigned long>& want,
                     std::map<unsigned long, std::pair<int, std::string>>& owners) {
    if (want.empty()) return;
    std::set<unsigned long> remain = want;
    DIR* proc = opendir("/proc");
    if (!proc) return;
    struct dirent* pe;
    while ((pe = readdir(proc)) != nullptr) {
        if (pe->d_name[0] < '0' || pe->d_name[0] > '9') continue;
        int pid = atoi(pe->d_name);
        if (pid <= 0) continue;
        char fddir[64];
        snprintf(fddir, sizeof fddir, "/proc/%d/fd", pid);
        DIR* fd = opendir(fddir);
        if (!fd) continue;
        struct dirent* fe;
        while ((fe = readdir(fd)) != nullptr) {
            if (fe->d_name[0] == '.') continue;
            char lp[384];
            snprintf(lp, sizeof lp, "%s/%s", fddir, fe->d_name);
            char tgt[128];
            ssize_t n = readlink(lp, tgt, sizeof tgt - 1);
            if (n <= 0) continue;
            tgt[n] = '\0';
            if (strncmp(tgt, "socket:[", 8) != 0) continue;
            unsigned long ino = strtoul(tgt + 8, nullptr, 10);
            if (!remain.erase(ino)) continue;
            owners[ino] = std::make_pair(pid, commOf(pid));
            if (remain.empty()) break;
        }
        closedir(fd);
        if (remain.empty()) break;
    }
    closedir(proc);
}

size_t sectionBudget(const Options& opt, const char* kind) {
    if (opt.scope != "all") return 24000;
    if (strcmp(kind, "tcp") == 0) return 9000;
    if (strcmp(kind, "udp") == 0) return 5000;
    if (strcmp(kind, "unix") == 0) return 8000;
    return 4000; // ipc
}

std::string lowerText(const std::string& s) {
    std::string o = s;
    for (size_t i = 0; i < o.size(); ++i) o[i] = (char)tolower((unsigned char)o[i]);
    return o;
}

void pageBounds(int matched, const Options& opt, int& page, int& page_size, int& pages, size_t& begin,
                size_t& end) {
    page_size = opt.page_size;
    if (page_size < 1) page_size = 10;
    if (page_size > 200) page_size = 200;
    pages = matched > 0 ? (matched + page_size - 1) / page_size : 1;
    page = opt.page;
    if (page < 1) page = 1;
    if (page > pages) page = pages;
    begin = (size_t)(page - 1) * (size_t)page_size;
    end = begin + (size_t)page_size;
    if (begin > (size_t)matched) begin = (size_t)matched;
    if (end > (size_t)matched) end = (size_t)matched;
}

bool pushEntry(json& arr, const json& e, size_t& used, size_t budget, bool& truncated) {
    size_t n = e.dump().size() + 1;
    if (used + n > budget) {
        truncated = true;
        return false;
    }
    arr.push_back(e);
    used += n;
    return true;
}

// ---------------------------------------------------------------- IPC

std::string runCmd(const char* cmd) {
    FILE* fp = popen(cmd, "r");
    if (!fp) return std::string();
    std::string out;
    char buf[8192];
    while (out.size() < kMaxFile) {
        size_t n = fread(buf, 1, sizeof buf, fp);
        if (n == 0) break;
        out.append(buf, n);
    }
    pclose(fp);
    return out;
}

std::string jstr(const json& j, const char* key) {
    if (!j.is_object() || !j.contains(key)) return std::string();
    const json& v = j[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_integer()) return std::to_string(v.get<long long>());
    if (v.is_number_unsigned()) return std::to_string(v.get<unsigned long long>());
    return std::string();
}

int jint(const json& j, const char* key, int def = 0) {
    std::string s = jstr(j, key);
    if (s.empty()) return def;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (end == s.c_str()) return def;
    return (int)v;
}

// lsipc 在空列表时返回 {"": []}，资源一律取对象里的第一个数组
json firstArray(const std::string& text, const char* prefer) {
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded()) return json::array();
    if (j.is_array()) return j;
    if (!j.is_object()) return json::array();
    if (j.contains(prefer) && j[prefer].is_array()) return j[prefer];
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (it.value().is_array()) return it.value();
    }
    return json::array();
}

std::string bytesText(unsigned long long bytes) {
    char b[32];
    if (bytes >= 1024ULL * 1024 * 1024) snprintf(b, sizeof b, "%.1fG", bytes / 1073741824.0);
    else if (bytes >= 1024ULL * 1024) snprintf(b, sizeof b, "%.1fM", bytes / 1048576.0);
    else if (bytes >= 1024) snprintf(b, sizeof b, "%.1fK", bytes / 1024.0);
    else snprintf(b, sizeof b, "%lluB", bytes);
    return b;
}

json ipcFromLsipc(int limit, bool with_proc, size_t& used, size_t budget, bool& truncated) {
    json shm = firstArray(runCmd("lsipc -m -J 2>/dev/null"), "sharedmemory");
    json msg = firstArray(runCmd("lsipc -q -J 2>/dev/null"), "messages");
    json sem = firstArray(runCmd("lsipc -s -J 2>/dev/null"), "semaphores");
    json limits = firstArray(runCmd("lsipc -J 2>/dev/null"), "ipclimits");

    json o;
    o["source"] = "lsipc";
    json eshm = json::array(), emsg = json::array(), esem = json::array();
    for (size_t i = 0; i < shm.size() && (int)i < limit; ++i) {
        const json& r = shm[i];
        int pid = jint(r, "cpid");
        json e;
        e["key"] = jstr(r, "key");
        e["id"] = jstr(r, "id");
        e["perms"] = jstr(r, "perms");
        e["size"] = jstr(r, "size");
        e["nattch"] = jstr(r, "nattch");
        e["owner"] = jstr(r, "owner");
        e["ctime"] = jstr(r, "ctime");
        e["pid"] = pid > 0 ? json(pid) : json(nullptr);
        e["comm"] = jstr(r, "command");
        if (e["comm"].is_null() && pid > 0 && with_proc) e["comm"] = commOf(pid);
        if (!pushEntry(eshm, e, used, budget, truncated)) break;
    }
    for (size_t i = 0; i < msg.size() && (int)i < limit; ++i) {
        const json& r = msg[i];
        json e;
        e["key"] = jstr(r, "key");
        e["id"] = jstr(r, "id");
        e["perms"] = jstr(r, "perms");
        e["used"] = jstr(r, "usedbytes");
        e["msgs"] = jstr(r, "msgs");
        e["lspid"] = jint(r, "lspid");
        e["lrpid"] = jint(r, "lrpid");
        e["owner"] = jstr(r, "owner");
        if (!pushEntry(emsg, e, used, budget, truncated)) break;
    }
    for (size_t i = 0; i < sem.size() && (int)i < limit; ++i) {
        const json& r = sem[i];
        json e;
        e["key"] = jstr(r, "key");
        e["id"] = jstr(r, "id");
        e["perms"] = jstr(r, "perms");
        e["nsems"] = jstr(r, "nsems");
        e["owner"] = jstr(r, "owner");
        if (!pushEntry(esem, e, used, budget, truncated)) break;
    }
    json elim = json::array();
    for (auto& r : limits) {
        json e;
        e["resource"] = jstr(r, "resource");
        e["limit"] = jstr(r, "limit");
        e["used"] = jstr(r, "used");
        e["use"] = jstr(r, "use%");
        elim.push_back(e);
    }
    o["shm"] = eshm;
    o["msg"] = emsg;
    o["sem"] = esem;
    o["limits"] = elim;
    o["total"] = (int)(shm.size() + msg.size() + sem.size());
    o["truncated"] = truncated;
    return o;
}

json ipcFromProc(int limit, size_t& used, size_t budget, bool& truncated) {
    json o;
    o["source"] = "proc";
    json eshm = json::array(), emsg = json::array(), esem = json::array();
    int total = 0;

    {
        std::string text = readFileText("/proc/sysvipc/shm");
        std::istringstream in(text);
        std::string line;
        std::getline(in, line);
        int n = 0;
        while (std::getline(in, line)) {
            std::istringstream is(line);
            long long key = 0, shmid = 0, cpid = 0;
            unsigned long perms = 0, size = 0, nattch = 0, uid = 0;
            if (!(is >> key >> shmid >> perms >> size >> cpid)) continue;
            is >> nattch >> uid;
            ++total;
            if (n++ >= limit) continue;
            json e;
            char k[24];
            snprintf(k, sizeof k, "0x%08llx", (unsigned long long)(key & 0xffffffffULL));
            e["key"] = k;
            e["id"] = std::to_string(shmid);
            char pm[16];
            snprintf(pm, sizeof pm, "%03lo", perms & 0777);
            e["perms"] = pm;
            e["size"] = bytesText(size);
            e["nattch"] = std::to_string(nattch);
            e["owner"] = std::to_string(uid);
            e["ctime"] = std::string();
            e["pid"] = cpid > 0 ? json((int)cpid) : json(nullptr);
            e["comm"] = cpid > 0 ? commOf((int)cpid) : std::string();
            if (!pushEntry(eshm, e, used, budget, truncated)) break;
        }
    }
    {
        std::string text = readFileText("/proc/sysvipc/msg");
        std::istringstream in(text);
        std::string line;
        std::getline(in, line);
        int n = 0;
        while (std::getline(in, line)) {
            std::istringstream is(line);
            long long key = 0, msqid = 0, cbytes = 0, qnum = 0, lspid = 0, lrpid = 0, uid = 0;
            unsigned long perms = 0;
            if (!(is >> key >> msqid >> perms >> cbytes >> qnum >> lspid >> lrpid >> uid)) continue;
            ++total;
            if (n++ >= limit) continue;
            json e;
            char k[24];
            snprintf(k, sizeof k, "0x%08llx", (unsigned long long)(key & 0xffffffffULL));
            e["key"] = k;
            e["id"] = std::to_string(msqid);
            char pm[16];
            snprintf(pm, sizeof pm, "%03lo", perms & 0777);
            e["perms"] = pm;
            e["used"] = bytesText((unsigned long long)(cbytes < 0 ? 0 : cbytes));
            e["msgs"] = std::to_string(qnum);
            e["lspid"] = (int)lspid;
            e["lrpid"] = (int)lrpid;
            e["owner"] = std::to_string(uid);
            if (!pushEntry(emsg, e, used, budget, truncated)) break;
        }
    }
    {
        std::string text = readFileText("/proc/sysvipc/sem");
        std::istringstream in(text);
        std::string line;
        std::getline(in, line);
        int n = 0;
        while (std::getline(in, line)) {
            std::istringstream is(line);
            long long key = 0, semid = 0, nsems = 0, uid = 0;
            unsigned long perms = 0;
            if (!(is >> key >> semid >> perms >> nsems >> uid)) continue;
            ++total;
            if (n++ >= limit) continue;
            json e;
            char k[24];
            snprintf(k, sizeof k, "0x%08llx", (unsigned long long)(key & 0xffffffffULL));
            e["key"] = k;
            e["id"] = std::to_string(semid);
            char pm[16];
            snprintf(pm, sizeof pm, "%03lo", perms & 0777);
            e["perms"] = pm;
            e["nsems"] = std::to_string(nsems);
            e["owner"] = std::to_string(uid);
            if (!pushEntry(esem, e, used, budget, truncated)) break;
        }
    }
    o["shm"] = eshm;
    o["msg"] = emsg;
    o["sem"] = esem;
    o["limits"] = json::array();
    o["total"] = total;
    o["truncated"] = truncated;
    return o;
}

json ipcPage(json o, const Options& opt) {
    const char* kinds[3] = {"shm", "msg", "sem"};
    json counts = json::object();
    int total = 0;
    for (int i = 0; i < 3; ++i) {
        int n = o.contains(kinds[i]) && o[kinds[i]].is_array() ? (int)o[kinds[i]].size() : 0;
        counts[kinds[i]] = n;
        total += n;
    }
    std::string kind = opt.ipc_kind;
    if (kind != "msg" && kind != "sem") kind = "shm";

    std::string q = lowerText(opt.q);
    std::vector<json> rows;
    json src = o.contains(kind) && o[kind].is_array() ? o[kind] : json::array();
    for (auto& e : src) {
        if (!q.empty()) {
            std::string hay = jstr(e, "key") + " " + jstr(e, "id") + " " + jstr(e, "owner") + " " +
                              jstr(e, "comm");
            if (lowerText(hay).find(q) == std::string::npos) continue;
        }
        rows.push_back(e);
    }
    std::sort(rows.begin(), rows.end(), [](const json& a, const json& b) {
        int ia = atoi(jstr(a, "id").c_str());
        int ib = atoi(jstr(b, "id").c_str());
        if (ia != ib) return ia < ib;
        return jstr(a, "id") < jstr(b, "id");
    });

    int matched = (int)rows.size();
    int page = 1, page_size = 10, pages = 1;
    size_t begin = 0, end = 0;
    pageBounds(matched, opt, page, page_size, pages, begin, end);

    json pagearr = json::array();
    for (size_t i = begin; i < end; ++i) pagearr.push_back(rows[i]);
    for (int i = 0; i < 3; ++i) o[kinds[i]] = json::array();
    o[kind] = pagearr;
    o["kind"] = kind;
    o["counts"] = counts;
    o["total"] = total;
    o["matched"] = matched;
    o["page"] = page;
    o["page_size"] = page_size;
    o["pages"] = pages;
    return o;
}

json collectIpc(int limit, bool with_proc, size_t& used, size_t budget, bool& truncated) {
    json o = ipcFromLsipc(limit, with_proc, used, budget, truncated);
    if (o["shm"].empty() && o["msg"].empty() && o["sem"].empty() && o["total"] == 0) {
        // 没有 lsipc 或解析失败：回退内核接口
        std::string probe = readFileText("/proc/sysvipc/shm", 64);
        if (probe.empty()) {
            std::string msg = readFileText("/proc/sysvipc/msg", 64);
            std::string sem = readFileText("/proc/sysvipc/sem", 64);
            if (msg.empty() && sem.empty()) return o;
        }
        size_t used2 = 0;
        bool tr2 = false;
        json p = ipcFromProc(limit, used2, budget, tr2);
        if (!p["shm"].empty() || !p["msg"].empty() || !p["sem"].empty() || p["total"] > 0) {
            used += used2;
            truncated = truncated || tr2;
            return p;
        }
    }
    return o;
}

// ---------------------------------------------------------------- 组装

json sockSection(const std::vector<SockEntry>& all, bool want_entries, const Options& opt,
                 bool tcp, size_t& used, size_t budget, bool& truncated) {
    json sec;
    int listening = 0;
    for (auto& e : all) {
        if (tcp && e.state_code == 0x0A) ++listening;
        if (!tcp && e.state_code == 0x07) ++listening; // UDP UNCONN
    }
    sec["total"] = (int)all.size();
    sec["listening"] = listening;
    sec["connected"] = (int)all.size() - listening;
    json arr = json::array();
    if (!want_entries) {
        sec["entries"] = arr;
        sec["truncated"] = false;
        sec["matched"] = 0;
        sec["page"] = 1;
        sec["page_size"] = opt.page_size;
        sec["pages"] = 1;
        return sec;
    }

    std::string q = lowerText(opt.q);
    std::vector<SockEntry> rows;
    rows.reserve(all.size());
    for (auto& e : all) {
        if (opt.listen_only) {
            bool listen = tcp ? e.state_code == 0x0A : e.state_code == 0x07;
            if (!listen) continue;
        }
        rows.push_back(e);
    }
    // 搜索进程名需要先反查 owner；只在有关键字时做一次全量映射
    std::map<unsigned long, std::pair<int, std::string>> searched_owners;
    if (!q.empty() && opt.with_proc) {
        std::set<unsigned long> want;
        for (auto& e : rows)
            if (e.inode) want.insert(e.inode);
        mapSocketOwners(want, searched_owners);
        for (auto& e : rows) {
            auto it = searched_owners.find(e.inode);
            if (it != searched_owners.end()) {
                e.pid = it->second.first;
                e.comm = it->second.second;
            }
        }
    }
    if (!q.empty()) {
        std::vector<SockEntry> kept;
        kept.reserve(rows.size());
        for (auto& e : rows) {
            std::string hay = e.local_addr + ":" + std::to_string(e.local_port) + " " + e.remote_addr +
                              ":" + std::to_string(e.remote_port) + " " + e.state + " " + e.comm + " " +
                              std::to_string(e.pid);
            if (lowerText(hay).find(q) != std::string::npos) kept.push_back(e);
        }
        rows.swap(kept);
    }

    std::sort(rows.begin(), rows.end(), [tcp](const SockEntry& a, const SockEntry& b) {
        bool la = tcp ? a.state_code == 0x0A : a.state_code == 0x07;
        bool lb = tcp ? b.state_code == 0x0A : b.state_code == 0x07;
        if (la != lb) return la;
        if (a.local_port != b.local_port) return a.local_port < b.local_port;
        if (a.local_addr != b.local_addr) return a.local_addr < b.local_addr;
        return a.remote_port < b.remote_port;
    });

    int matched = (int)rows.size();
    int page = 1, page_size = 10, pages = 1;
    size_t begin = 0, end = 0;
    pageBounds(matched, opt, page, page_size, pages, begin, end);
    std::vector<SockEntry> sliced(rows.begin() + begin, rows.begin() + end);

    if (opt.with_proc && q.empty()) {
        std::set<unsigned long> want;
        for (auto& e : sliced)
            if (e.inode) want.insert(e.inode);
        std::map<unsigned long, std::pair<int, std::string>> owners;
        mapSocketOwners(want, owners);
        for (auto& e : sliced) {
            auto it = owners.find(e.inode);
            if (it != owners.end()) {
                e.pid = it->second.first;
                e.comm = it->second.second;
            }
        }
    }
    for (auto& e : sliced) {
        json j;
        j["local_addr"] = e.local_addr;
        j["local_port"] = e.local_port;
        j["remote_addr"] = e.remote_addr;
        j["remote_port"] = e.remote_port;
        j["state"] = e.state;
        j["uid"] = e.uid;
        j["inode"] = (unsigned long long)e.inode;
        j["pid"] = e.pid > 0 ? json(e.pid) : json(nullptr);
        j["comm"] = e.comm;
        if (!pushEntry(arr, j, used, budget, truncated)) break;
    }
    sec["entries"] = arr;
    sec["truncated"] = truncated;
    sec["matched"] = matched;
    sec["page"] = page;
    sec["page_size"] = page_size;
    sec["pages"] = pages;
    return sec;
}

json unixSection(const std::vector<UnixEntry>& all, bool want_entries, const Options& opt,
                 size_t& used, size_t budget, bool& truncated) {
    json sec;
    int listening = 0, stream = 0, dgram = 0;
    for (auto& e : all) {
        if (e.type_code == 1) ++stream;
        else if (e.type_code == 2) ++dgram;
        if (e.type_code == 1 && e.state_code == 0x01) ++listening;
    }
    sec["total"] = (int)all.size();
    sec["listening"] = listening;
    sec["connected"] = (int)all.size() - listening;
    sec["stream"] = stream;
    sec["dgram"] = dgram;
    json arr = json::array();
    if (!want_entries) {
        sec["entries"] = arr;
        sec["truncated"] = false;
        sec["matched"] = 0;
        sec["page"] = 1;
        sec["page_size"] = opt.page_size;
        sec["pages"] = 1;
        return sec;
    }

    std::string q = lowerText(opt.q);
    std::vector<UnixEntry> rows;
    rows.reserve(all.size());
    for (auto& e : all) {
        if ((opt.utype == "STREAM" || opt.utype == "DGRAM") && e.type != opt.utype) continue;
        rows.push_back(e);
    }
    if (!q.empty() && opt.with_proc) {
        std::set<unsigned long> want;
        for (auto& e : rows)
            if (e.inode) want.insert(e.inode);
        std::map<unsigned long, std::pair<int, std::string>> owners;
        mapSocketOwners(want, owners);
        for (auto& e : rows) {
            auto it = owners.find(e.inode);
            if (it != owners.end()) {
                e.pid = it->second.first;
                e.comm = it->second.second;
            }
        }
    }
    if (!q.empty()) {
        std::vector<UnixEntry> kept;
        kept.reserve(rows.size());
        for (auto& e : rows) {
            std::string hay = e.path + " " + e.type + " " + e.state + " " + e.comm + " " +
                              std::to_string(e.pid) + " " + std::to_string(e.inode);
            if (lowerText(hay).find(q) != std::string::npos) kept.push_back(e);
        }
        rows.swap(kept);
    }

    std::sort(rows.begin(), rows.end(), [](const UnixEntry& a, const UnixEntry& b) {
        bool la = a.state_code == 0x01;
        bool lb = b.state_code == 0x01;
        if (la != lb) return la;
        if (a.path != b.path) return a.path < b.path;
        return a.inode < b.inode;
    });

    int matched = (int)rows.size();
    int page = 1, page_size = 10, pages = 1;
    size_t begin = 0, end = 0;
    pageBounds(matched, opt, page, page_size, pages, begin, end);
    std::vector<UnixEntry> sliced(rows.begin() + begin, rows.begin() + end);

    std::map<unsigned long, std::pair<int, std::string>> owners;
    if (opt.with_proc && q.empty()) {
        std::set<unsigned long> want;
        for (auto& e : sliced)
            if (e.inode) want.insert(e.inode);
        mapSocketOwners(want, owners);
    }
    for (auto& e : sliced) {
        json j;
        j["path"] = e.path;
        j["abstract"] = e.abstract;
        j["type"] = e.type;
        j["state"] = e.state;
        j["inode"] = (unsigned long long)e.inode;
        auto it = owners.find(e.inode);
        j["pid"] = (it != owners.end() && it->second.first > 0) ? json(it->second.first) : json(nullptr);
        j["comm"] = it != owners.end() ? it->second.second : std::string();
        if (!pushEntry(arr, j, used, budget, truncated)) break;
    }
    sec["entries"] = arr;
    sec["truncated"] = truncated;
    sec["matched"] = matched;
    sec["page"] = page;
    sec["page_size"] = page_size;
    sec["pages"] = pages;
    return sec;
}

} // namespace

json pageIpc(json ipc, const Options& opt) {
    return ipcPage(std::move(ipc), opt);
}

bool scopeValid(const std::string& s) {
    return s == "tcp" || s == "udp" || s == "unix" || s == "ipc" || s == "all";
}

json collect(const Options& opt, std::string& err) {
    err.clear();
    size_t used = 0;
    bool truncated = false;

    std::vector<SockEntry> tcp = loadSocks("/proc/net/tcp", false, true);
    std::vector<SockEntry> tcp6 = loadSocks("/proc/net/tcp6", true, true);
    tcp.insert(tcp.end(), tcp6.begin(), tcp6.end());
    std::vector<SockEntry> udp = loadSocks("/proc/net/udp", false, false);
    std::vector<SockEntry> udp6 = loadSocks("/proc/net/udp6", true, false);
    udp.insert(udp.end(), udp6.begin(), udp6.end());
    std::vector<UnixEntry> unixs = loadUnix();

    if (tcp.empty() && udp.empty() && unixs.empty()) err = "no /proc/net data";

    json out;
    out["scope"] = opt.scope;
    out["ts"] = nowMs();
    out["proc"] = opt.with_proc;

    json summary;
    summary["tcp"] = (int)tcp.size();
    summary["udp"] = (int)udp.size();
    summary["unix"] = (int)unixs.size();

    bool all = opt.scope == "all";
    bool any_trunc = false;
    used = 0;
    truncated = false;
    out["tcp"] = sockSection(tcp, all || opt.scope == "tcp", opt, true, used,
                             sectionBudget(opt, "tcp"), truncated);
    any_trunc = any_trunc || truncated;

    used = 0;
    truncated = false;
    out["udp"] = sockSection(udp, all || opt.scope == "udp", opt, false, used,
                             sectionBudget(opt, "udp"), truncated);
    any_trunc = any_trunc || truncated;

    used = 0;
    truncated = false;
    out["unix"] = unixSection(unixs, all || opt.scope == "unix", opt, used,
                              sectionBudget(opt, "unix"), truncated);
    any_trunc = any_trunc || truncated;

    if (opt.with_ipc) {
        used = 0;
        truncated = false;
        json ipc = collectIpc(2000, opt.with_proc, used, 1024 * 1024, truncated);
        summary["ipc"] = ipc.value("total", 0);
        out["ipc"] = ipc;
        any_trunc = any_trunc || truncated;
    } else {
        summary["ipc"] = -1;
    }
    truncated = any_trunc;
    out["summary"] = summary;
    out["truncated"] = truncated;
    out["page"] = opt.page;
    out["page_size"] = opt.page_size;
    return out;
}

} // namespace rtnet
