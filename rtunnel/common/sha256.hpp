#pragma once
// SHA-256：统一使用 OpenSSL EVP，不再自己实现算法。
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace rtutil {

// 输出 64 位小写十六进制
std::string sha256Hex(const void* data, size_t len);
std::string sha256Hex(const std::string& data);

// 32 字节原始摘要
void sha256(const void* data, size_t len, uint8_t out[32]);

// 文件哈希：按块读取，避免整文件读进内存
std::string sha256File(const std::string& path);

// 流式哈希：大文件按块 update，最后 final/hex
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void reset();
    void update(const void* data, size_t len);
    void final(uint8_t out[32]);
    std::string hex();

private:
    void* ctx_ = nullptr;
};

} // namespace rtutil
