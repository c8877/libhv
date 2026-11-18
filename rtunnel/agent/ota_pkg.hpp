#pragma once
// OTA 部署包：gzip 外的 ustar 解包与 manifest 读取。
// 只接受普通文件和目录，拒绝绝对路径、..、符号链接/硬链接/设备文件。

#include <string>

namespace rtopkg {

struct Manifest {
    std::string name;
    std::string variant;
    std::string version;
    std::string binary;
};

// 是否是 ustar 数据（用于与旧的裸二进制升级包区分）
bool isTar(const std::string& data);

// 解包到 dir；dir 不存在则创建。失败时返回 false 并写 err。
bool extract(const std::string& data, const std::string& dir, std::string& err);

// 包通常有一层与包同名的顶层目录；返回真正包含 manifest.json 的目录
bool resolveRoot(const std::string& dir, std::string& root, std::string& err);

// 读取 root/manifest.json；校验 name/variant/binary 形态，version 由调用方比对
bool readManifest(const std::string& root, Manifest& m, std::string& err);

// 递归删除目录（不跟随符号链接）
void removeTree(const std::string& path);

} // namespace rtopkg
