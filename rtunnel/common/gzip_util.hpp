#pragma once
#include <string>

// gzip（RFC1952），失败返回 false
bool gzipCompress(const std::string& in, std::string& out, int level = 6);
// max_out > 0 时限制解压后大小，超出返回 false（防解压炸弹）
bool gzipDecompress(const char* data, size_t len, std::string& out, size_t max_out = 0);
