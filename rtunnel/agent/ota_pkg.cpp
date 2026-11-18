#include "ota_pkg.hpp"

#include "cpputil/json.hpp"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <vector>

namespace rtopkg {

namespace {

const size_t BLOCK = 512;
const uint64_t MAX_ENTRIES = 4096;
const uint64_t MAX_TOTAL = 256ULL * 1024 * 1024;
const uint64_t MAX_FILE = 128ULL * 1024 * 1024;
const size_t MAX_NAME = 512;

std::string field(const char* h, size_t off, size_t len) {
    size_t n = 0;
    while (n < len && h[off + n] != '\0') ++n;
    return std::string(h + off, n);
}

bool parseOctal(const char* p, size_t len, uint64_t& v) {
    size_t i = 0;
    while (i < len && (p[i] == ' ' || p[i] == '\0')) ++i;
    // GNU base-256（高位为 1）不接受：部署包都是小文件
    if (i < len && (((unsigned char)p[i]) & 0x80)) return false;
    uint64_t r = 0;
    bool any = false;
    for (; i < len; ++i) {
        char c = p[i];
        if (c == '\0' || c == ' ') break;
        if (c < '0' || c > '7') return false;
        r = (r << 3) | (uint64_t)(c - '0');
        any = true;
    }
    v = any ? r : 0;
    return true;
}

bool checksumOk(const char* h) {
    uint64_t want = 0;
    if (!parseOctal(h + 148, 8, want)) return false;
    uint64_t sum = 0;
    for (size_t i = 0; i < BLOCK; ++i) {
        unsigned char c = (i >= 148 && i < 156) ? (unsigned char)' ' : (unsigned char)h[i];
        sum += c;
    }
    return sum == want;
}

bool safeName(const std::string& name) {
    if (name.empty() || name.size() > MAX_NAME) return false;
    if (name[0] == '/') return false;
    size_t pos = 0;
    while (pos <= name.size()) {
        size_t slash = name.find('/', pos);
        std::string one = name.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (one.empty() || one == "." || one == "..") return false;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

bool mkdirs(const std::string& path) {
    if (path.empty()) return true;
    std::string cur = (path[0] == '/') ? "/" : "";
    size_t pos = 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string one = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        if (!one.empty()) {
            if (!cur.empty() && cur != "/") cur += '/';
            cur += one;
            if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return true;
}

bool writeFile(const std::string& path, const char* data, size_t len, mode_t mode) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, mode);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            close(fd);
            unlink(path.c_str());
            return false;
        }
        off += (size_t)n;
    }
    close(fd);
    return true;
}

bool versionOk(const std::string& v) {
    if (v.empty() || v.size() > 40) return false;
    for (char c : v) {
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '+')) return false;
    }
    return true;
}

} // namespace

bool isTar(const std::string& data) {
    return data.size() >= BLOCK && memcmp(data.data() + 257, "ustar", 5) == 0;
}

bool extract(const std::string& data, const std::string& dir, std::string& err) {
    if (!mkdirs(dir)) {
        err = "create staging dir failed";
        return false;
    }
    size_t off = 0;
    uint64_t entries = 0;
    uint64_t total = 0;
    while (off + BLOCK <= data.size()) {
        const char* h = data.data() + off;
        bool zero = true;
        for (size_t i = 0; i < BLOCK; ++i) {
            if (h[i] != '\0') { zero = false; break; }
        }
        if (zero) return true; // 结束块
        if (memcmp(h + 257, "ustar", 5) != 0) {
            err = "bad tar magic";
            return false;
        }
        if (!checksumOk(h)) {
            err = "bad tar checksum";
            return false;
        }
        if (++entries > MAX_ENTRIES) {
            err = "too many entries";
            return false;
        }
        std::string name = field(h, 0, 100);
        std::string prefix = field(h, 345, 155);
        if (!prefix.empty()) name = prefix + "/" + name;
        // 目录条目通常以 / 结尾
        while (name.size() > 1 && name.back() == '/') name.pop_back();
        if (!safeName(name)) {
            err = "unsafe entry path";
            return false;
        }
        uint64_t size = 0;
        uint64_t mode = 0;
        if (!parseOctal(h + 124, 12, size) || !parseOctal(h + 100, 8, mode)) {
            err = "bad tar header";
            return false;
        }
        char type = h[156];
        if (type == '5') { // 目录
            if (size != 0 || !mkdirs(dir + "/" + name)) {
                err = "create dir failed";
                return false;
            }
        }
        else if (type == '0' || type == '\0') { // 普通文件
            if (size > MAX_FILE || (total += size) > MAX_TOTAL) {
                err = "package too large";
                return false;
            }
            if (size > (uint64_t)(data.size() - off - BLOCK)) {
                err = "truncated tar";
                return false;
            }
            std::string path = dir + "/" + name;
            size_t slash = path.find_last_of('/');
            if (slash != std::string::npos && !mkdirs(path.substr(0, slash))) {
                err = "create parent dir failed";
                return false;
            }
            mode_t m = (mode & 0111) ? 0755 : 0644;
            if (!writeFile(path, data.data() + off + BLOCK, (size_t)size, m)) {
                err = "write " + name + " failed";
                return false;
            }
        }
        else {
            // 符号链接、硬链接、设备、fifo、pax 头一律拒绝
            err = "unsupported tar entry type";
            return false;
        }
        off += BLOCK + (size_t)(((size + BLOCK - 1) / BLOCK) * BLOCK);
    }
    err = "truncated tar";
    return false;
}

bool resolveRoot(const std::string& dir, std::string& root, std::string& err) {
    if (access((dir + "/manifest.json").c_str(), F_OK) == 0) {
        root = dir;
        return true;
    }
    DIR* d = opendir(dir.c_str());
    if (!d) {
        err = "staging dir missing";
        return false;
    }
    struct dirent* e;
    bool found = false;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        std::string cand = dir + "/" + e->d_name;
        struct stat st {};
        if (lstat(cand.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (access((cand + "/manifest.json").c_str(), F_OK) != 0) continue;
        if (found) {
            closedir(d);
            err = "multiple package roots";
            return false;
        }
        root = cand;
        found = true;
    }
    closedir(d);
    if (!found) {
        err = "manifest.json not found";
        return false;
    }
    return true;
}

bool readManifest(const std::string& dir, Manifest& m, std::string& err) {
    std::string path = dir + "/manifest.json";
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        err = "manifest.json missing";
        return false;
    }
    std::string js;
    char buf[4096];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            err = "read manifest failed";
            return false;
        }
        js.append(buf, (size_t)n);
        if (js.size() > 64 * 1024) {
            close(fd);
            err = "manifest too large";
            return false;
        }
    }
    close(fd);
    auto j = nlohmann::json::parse(js, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        err = "bad manifest json";
        return false;
    }
    m.name = j.value("name", std::string());
    m.variant = j.value("variant", std::string());
    m.version = j.value("version", std::string());
    m.arch = j.value("arch", std::string());
    m.binary = j.value("binary", std::string());
    if (m.name != "rtunnel-agent") {
        err = "manifest name mismatch";
        return false;
    }
    if (m.variant != "lite" && m.variant != "web") {
        err = "manifest variant invalid";
        return false;
    }
    if (!versionOk(m.version)) {
        err = "manifest version invalid";
        return false;
    }
    if (m.arch.empty() || m.arch.size() > 32) {
        err = "manifest arch invalid";
        return false;
    }
    for (char c : m.arch) {
        if (!(isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-')) {
            err = "manifest arch invalid";
            return false;
        }
    }
    std::string want = "bin/rtunnel-agent-" + m.variant + "-" + m.version;
    if (m.binary != want) {
        err = "manifest binary mismatch";
        return false;
    }
    return true;
}

void removeTree(const std::string& path) {
    DIR* d = opendir(path.c_str());
    if (!d) {
        unlink(path.c_str());
        return;
    }
    struct dirent* e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        std::string p = path + "/" + e->d_name;
        struct stat st {};
        if (lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) removeTree(p);
        else unlink(p.c_str());
    }
    closedir(d);
    rmdir(path.c_str());
}

} // namespace rtopkg
