#pragma once
#include <string>

// gzip（RFC1952），失败返回 false
bool gzipCompress(const std::string& in, std::string& out, int level = 6);
bool gzipDecompress(const char* data, size_t len, std::string& out);
