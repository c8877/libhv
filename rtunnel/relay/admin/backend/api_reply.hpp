// 管理台 API 的统一错误响应
#pragma once

#include "admin_server.hpp"
#include "cpputil/json.hpp"

#include <string>

// 直接回写 {"error": msg} 与指定状态码，供各 API 文件复用
inline void apiError(const HttpContextPtr& ctx, int status, const std::string& msg) {
    ctx->response->status_code = (http_status)status;
    nlohmann::json j;
    j["error"] = msg;
    ctx->send(j.dump());
}

inline void apiBad(const HttpContextPtr& ctx, const std::string& msg) {
    apiError(ctx, HTTP_STATUS_BAD_REQUEST, msg);
}

inline void apiNotFound(const HttpContextPtr& ctx, const std::string& msg) {
    apiError(ctx, HTTP_STATUS_NOT_FOUND, msg);
}
