#include "admin_web_assets.hpp"

// rtunnel-relay：不带内嵌前端，静态资源只从 --admin-docroot 读。
bool adminWebAssetGet(const std::string&, bool, AdminWebAsset&) {
    return false;
}

bool adminWebAssetsAvailable() {
    return false;
}
