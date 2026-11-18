#include "hash_util.hpp"

#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#endif

#include <stdio.h>

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
#if defined(WITH_OPENSSL)
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return std::string();
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) EVP_DigestUpdate(ctx, buf, n);
    unsigned char md[32];
    unsigned int mdlen = 0;
    EVP_DigestFinal_ex(ctx, md, &mdlen);
    EVP_MD_CTX_free(ctx);
    fclose(fp);
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (unsigned int i = 0; i < mdlen; ++i) { out += hex[md[i] >> 4]; out += hex[md[i] & 0xF]; }
    return out;
#else
    (void)path;
    return std::string();
#endif
}
