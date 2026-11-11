#pragma once
// SHA-256（FIPS 180-4），无外部依赖，relay 在无 OpenSSL 构建下也能校验 Token。
#include <stddef.h>
#include <stdint.h>
#include <string>

namespace rtutil {

// 输出 64 位小写十六进制
std::string sha256Hex(const void* data, size_t len);
std::string sha256Hex(const std::string& data);

// 32 字节原始摘要
void sha256(const void* data, size_t len, uint8_t out[32]);

} // namespace rtutil
