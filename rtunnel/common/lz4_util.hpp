#pragma once
#include <string>

// LZ4 block，前缀 4 字节原始长度（LE）。库不可用时返回 false。
bool lz4Compress(const std::string& in, std::string& out);
bool lz4Decompress(const char* data, size_t len, std::string& out);
bool lz4Available();
