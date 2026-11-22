#include "sha256.hpp"

#include "openssl/evp.h"

#include <stdio.h>
#include <string.h>

namespace rtutil {
namespace {

std::string toHex(const uint8_t* p, size_t n) {
    static const char* hx = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = hx[p[i] >> 4];
        out[i * 2 + 1] = hx[p[i] & 0xF];
    }
    return out;
}

EVP_MD_CTX* ctxOf(void* p) { return static_cast<EVP_MD_CTX*>(p); }

} // namespace

Sha256::Sha256() {
    ctx_ = EVP_MD_CTX_new();
    reset();
}

Sha256::~Sha256() {
    if (ctx_) EVP_MD_CTX_free(ctxOf(ctx_));
}

void Sha256::reset() {
    if (ctx_) EVP_DigestInit_ex(ctxOf(ctx_), EVP_sha256(), nullptr);
}

void Sha256::update(const void* data, size_t len) {
    if (ctx_ && len) EVP_DigestUpdate(ctxOf(ctx_), data, len);
}

void Sha256::final(uint8_t out[32]) {
    unsigned int n = 0;
    if (ctx_) EVP_DigestFinal_ex(ctxOf(ctx_), out, &n);
    if (n != 32) memset(out, 0, 32);
}

std::string Sha256::hex() {
    uint8_t md[32];
    final(md);
    return toHex(md, sizeof(md));
}

void sha256(const void* data, size_t len, uint8_t out[32]) {
    unsigned int n = 0;
    EVP_Digest(data, len, out, &n, EVP_sha256(), nullptr);
    if (n != 32) memset(out, 0, 32);
}

std::string sha256Hex(const void* data, size_t len) {
    uint8_t md[32];
    sha256(data, len, md);
    return toHex(md, sizeof(md));
}

std::string sha256Hex(const std::string& data) {
    return sha256Hex(data.data(), data.size());
}

std::string sha256File(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) return std::string();
    Sha256 ctx;
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) ctx.update(buf, n);
    fclose(fp);
    return ctx.hex();
}

} // namespace rtutil
