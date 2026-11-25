#include "admin_web_assets.hpp"

#include "common/gzip_util.hpp"

#include <map>
#include <memory>
#include <mutex>
#include <string>

// 构建期由 rtunnel/cmake/gen_admin_web.cmake 从 relay/admin/web/dist 生成：
// 每个文件一条 { 相对路径, 数据, 长度, 是否已 gzip, Content-Type }。
struct AdminWebEntry {
    const char*          name;
    const unsigned char* data;
    unsigned int         len;
    bool                 gzip;
    const char*          ctype;
};
#include "admin_web_assets_data.hpp"

namespace {

// 不支持 gzip 的客户端：首次访问时解压一份并缓存（浏览器几乎都支持 gzip，
// 这里只是兜底，避免为了兼容把未压缩的整份前端再嵌一遍）。
struct PlainCache {
    std::mutex mu;
    std::map<std::string, std::string> body;
    std::map<std::string, bool> ok;
};

PlainCache& cache() {
    static PlainCache* c = new PlainCache();
    return *c;
}

}  // namespace

bool adminWebAssetGet(const std::string& name, bool gzip_ok, AdminWebAsset& out) {
    if (name.empty() || name.find("..") != std::string::npos) return false;
    const AdminWebEntry* hit = nullptr;
    for (unsigned int i = 0; i < kAdminWebEntryCount; ++i) {
        if (name == kAdminWebEntries[i].name) {
            hit = &kAdminWebEntries[i];
            break;
        }
    }
    if (hit == nullptr) return false;

    out.ctype = hit->ctype;
    if (!hit->gzip) {
        out.gzip = false;
        out.body.assign((const char*)hit->data, hit->len);
        return true;
    }
    if (gzip_ok) {
        out.gzip = true;
        out.body.assign((const char*)hit->data, hit->len);
        return true;
    }

    PlainCache& c = cache();
    std::lock_guard<std::mutex> lk(c.mu);
    auto it = c.body.find(name);
    if (it == c.body.end()) {
        std::string plain;
        bool ok = gzipDecompress((const char*)hit->data, hit->len, plain, 64 * 1024 * 1024);
        it = c.body.emplace(name, std::move(plain)).first;
        c.ok[name] = ok;
    }
    if (!c.ok[name]) return false;
    out.gzip = false;
    out.body = it->second;
    return true;
}

bool adminWebAssetsAvailable() {
    return kAdminWebEntryCount > 0;
}
