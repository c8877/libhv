#include "hash_util.hpp"
#include "common/sha256.hpp"

bool adminValidFileName(const std::string& s) {
    if (s.empty() || s.size() > 128) return false;
    if (s[0] == '.') return false;
    if (s.find("..") != std::string::npos) return false;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) return false;
        if (c == '/' || c == '\\') return false;
    }
    return true;
}

bool adminValidRepoPath(const std::string& s) {
    if (s.size() > 1024) return false;
    std::string p = s;
    if (!p.empty() && p[0] == '/') p.erase(0, 1);
    if (p.empty()) return true;
    size_t start = 0;
    for (;;) {
        size_t pos = p.find('/', start);
        std::string seg = (pos == std::string::npos) ? p.substr(start) : p.substr(start, pos - start);
        if (!adminValidFileName(seg)) return false;
        if (pos == std::string::npos) break;
        start = pos + 1;
        if (start >= p.size()) return false;   // 尾随 /
    }
    return true;
}

bool adminRepoPath(const std::string& root, const std::string& rel, std::string& out) {
    if (!adminValidRepoPath(rel)) return false;
    std::string p = rel;
    if (!p.empty() && p[0] == '/') p.erase(0, 1);
    out = p.empty() ? root : (root + "/" + p);
    return true;
}

std::string adminSha256File(const std::string& path) {
    return rtutil::sha256File(path);
}
