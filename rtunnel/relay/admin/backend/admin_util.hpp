// 管理台 HTTP 处理器共用的小工具。
// Cookie 解析、请求 scheme、URL 脱敏此前在多个 api_*.cpp 里各有一份，统一到这里。
#pragma once

#include "admin_server.hpp"

#include <string>

// 读取 Cookie；hv 的 GetCookie 已解析大部分情况，这里兜底手写解析
std::string adminCookieValue(const HttpContextPtr& ctx, const std::string& key);

// 本次请求是否 HTTPS：管理台本地 TLS，或反代声明 X-Forwarded-Proto
bool adminReqIsHttps(const HttpContextPtr& ctx);

// 只保留 scheme+主机+末 4 位，够判断配没配、指向谁，又不会泄露完整 token
std::string adminMaskUrl(const std::string& url);
