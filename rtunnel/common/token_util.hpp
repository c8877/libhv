#pragma once
// 接入账号 Token 的公共工具：周期边界、Token 生成、base64url。
#include <stdint.h>
#include <string>

namespace rtutil {

enum TokenPeriod {
    PERIOD_TOTAL   = 0,
    PERIOD_DAILY   = 1,
    PERIOD_MONTHLY = 2,
};

TokenPeriod tokenPeriodFromName(const std::string& name);
const char* tokenPeriodName(TokenPeriod p);

// 当前周期的起点（UTC）；total 返回 0
int64_t tokenPeriodStart(TokenPeriod p, int64_t now_ms);
// 下一个周期起点；total 返回 0（表示永不到期）
int64_t tokenPeriodNext(TokenPeriod p, int64_t period_start_ms);
// 从「用量所在周期起点」推算下一个起点，period_start=0 时按 now 对齐
int64_t tokenPeriodNextFrom(TokenPeriod p, int64_t period_start_ms);

// 生成 rtk_<43 字符 base64url>；随机源不可用时返回空串
std::string newClientToken();
// 明文前缀，用于列表展示：rtk_ + 8 字符 + "…"
std::string clientTokenPrefix(const std::string& token);
bool isClientToken(const std::string& s);

} // namespace rtutil
