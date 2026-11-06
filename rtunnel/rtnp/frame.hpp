#pragma once
// RTNP v1 —— 反向通道协议（见 docs/design/rtunnel.md）
//
// 帧头固定 16 字节，大端：
//   magic[4] "RTNP" | version(1) | type(1) | flags(2) | stream_id(4) | length(2) | reserved(2)
// 字符串编码：u16 len + bytes（无 NUL）。

#include <stdint.h>
#include <stddef.h>

namespace rtnp {

constexpr int      VERSION       = 1;
constexpr size_t   HEADER_SIZE   = 16;
constexpr uint16_t MAX_PAYLOAD   = 32768;   // 单帧 payload 上限（HELLO_ACK.max_payload）
constexpr uint32_t DEFAULT_CREDIT = 128 * 1024;
constexpr uint32_t LOW_WATER      = 64 * 1024;
constexpr uint32_t CONN_INFLIGHT  = 512 * 1024;

// type
enum Type : uint8_t {
    HELLO          = 0x01,
    HELLO_ACK      = 0x02,
    AUTH           = 0x03,
    AUTH_OK        = 0x04,
    AUTH_FAIL      = 0x05,
    PING           = 0x06,
    PONG           = 0x07,
    DEVICE_QUERY   = 0x08,
    DEVICE_STATUS  = 0x09,
    ERROR          = 0x0F,

    OPEN           = 0x10,
    OPEN_OK        = 0x11,
    OPEN_FAIL      = 0x12,
    DATA           = 0x13,
    WINDOW         = 0x14,
    CLOSE          = 0x15,
    RESET          = 0x16,
    RESIZE         = 0x17,   // 会话级：u16 cols + u16 rows（shell 终端尺寸）
    SPAWN_AGENT    = 0x18,   // 连接级：str devid。B 通知 A 在本机按需拉起 agent
};

// 错误码
enum Err : uint16_t {
    OK                 = 0,
    UNAUTHORIZED       = 1,
    DEVICE_OFFLINE     = 2,
    TARGET_REFUSED     = 3,
    TIMEOUT            = 4,
    PROTOCOL_ERROR     = 5,
    LIMIT_EXCEEDED     = 6,
    ACL_DENIED         = 7,
    DEVICE_REPLACED    = 8,
    TARGET_NOT_ALLOWED = 9,
    CLOSE_TIMEOUT      = 10,
    CANCELLED          = 11,
};

enum Role : uint8_t {
    ROLE_AGENT  = 1,
    ROLE_CLIENT = 2,
};

constexpr uint8_t AUTH_MTLS  = 0x01;
constexpr uint8_t AUTH_TOKEN = 0x02;

// HELLO.capabilities 位：client 声明自己也实现 agent 能力（可接收 B 路由的 OPEN/OPEN_SHELL）
constexpr uint32_t CAP_AGENT = 0x0001;

struct Frame {
    uint8_t        type;
    uint16_t       flags;
    uint32_t       sid;
    uint16_t       len;
    const uint8_t* payload;
};

} // namespace rtnp
