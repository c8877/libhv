#pragma once
#include <string>

// 文件名校验：禁止 / \ .. 前导点 与控制字符，允许 UTF-8，最长 128 字节
bool adminValidFileName(const std::string& s);

// 文件 SHA-256（十六进制小写）；无 OpenSSL 构建返回空
std::string adminSha256File(const std::string& path);
