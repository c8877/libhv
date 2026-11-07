#include "hash_util.hpp"

#if defined(WITH_OPENSSL)
#include "openssl/evp.h"
#endif

#include <stdio.h>
#include <ctype.h>

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

std::string adminUrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size() &&
            isxdigit((unsigned char)in[i + 1]) && isxdigit((unsigned char)in[i + 2])) {
            auto hex = [](char c) {
                return (c <= '9') ? (c - '0') : (tolower((unsigned char)c) - 'a' + 10);
            };
            out.push_back((char)((hex(in[i + 1]) << 4) | hex(in[i + 2])));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return out;
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
