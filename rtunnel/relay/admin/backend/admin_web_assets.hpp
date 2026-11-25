#pragma once
#include <string>

// 管理台前端的编译期内嵌资源（可选）。
// rtunnel-relay 编译 admin_web_stub.cpp，永远返回 false，前端只能来自 --admin-docroot；
// rtunnel-relay-embedded 编译 admin_web_embedded.cpp，资源在构建期由 web/dist 生成。
// 两者接口一致，admin_server.cpp 不需要条件编译。
struct AdminWebAsset {
    std::string body;          // 响应体
    bool        gzip = false;  // body 是否为 gzip 压缩体
    std::string ctype;         // Content-Type
};

// 查到返回 true；gzip_ok 表示客户端接受 gzip（否则解压后返回）。
bool adminWebAssetGet(const std::string& name, bool gzip_ok, AdminWebAsset& out);

// 这个二进制是否带内嵌前端（rtunnel-relay 为 false）。
bool adminWebAssetsAvailable();
