#ifndef RTNP_H_
#define RTNP_H_

// RTNP v1 —— 反向隧道协议（见 docs/design/remote-ssh-tunnel.md）
//
// 帧头固定 16 字节，大端：
//   magic[4] "RTNP" | version(1) | type(1) | flags(2) | stream_id(4) | length(2) | reserved(2)
// 字符串编码：u16 len + bytes（无 NUL）。

#include <stdint.h>
#include <stddef.h>

#include "hloop.h"

#define RTNP_MAGIC          "RTNP"
#define RTNP_VERSION        1
#define RTNP_HEADER_SIZE    16
#define RTNP_MAX_PAYLOAD    32768   // 单帧 payload 上限（HELLO_ACK.max_payload）
#define RTNP_DEFAULT_CREDIT (128 * 1024)
#define RTNP_LOW_WATER      (64 * 1024)
#define RTNP_CONN_INFLIGHT  (512 * 1024)

// type
enum {
    RTNP_HELLO          = 0x01,
    RTNP_HELLO_ACK      = 0x02,
    RTNP_AUTH           = 0x03,
    RTNP_AUTH_OK        = 0x04,
    RTNP_AUTH_FAIL      = 0x05,
    RTNP_PING           = 0x06,
    RTNP_PONG           = 0x07,
    RTNP_DEVICE_QUERY   = 0x08,
    RTNP_DEVICE_STATUS  = 0x09,
    RTNP_ERROR          = 0x0F,

    RTNP_OPEN           = 0x10,
    RTNP_OPEN_OK        = 0x11,
    RTNP_OPEN_FAIL      = 0x12,
    RTNP_DATA           = 0x13,
    RTNP_WINDOW         = 0x14,
    RTNP_CLOSE          = 0x15,
    RTNP_RESET          = 0x16,
};

// 错误码
enum {
    RTNP_OK                = 0,
    RTNP_UNAUTHORIZED      = 1,
    RTNP_DEVICE_OFFLINE    = 2,
    RTNP_TARGET_REFUSED    = 3,
    RTNP_TIMEOUT           = 4,
    RTNP_PROTOCOL_ERROR    = 5,
    RTNP_LIMIT_EXCEEDED    = 6,
    RTNP_ACL_DENIED        = 7,
    RTNP_DEVICE_REPLACED   = 8,
    RTNP_TARGET_NOT_ALLOWED= 9,
    RTNP_CLOSE_TIMEOUT     = 10,
    RTNP_CANCELLED         = 11,
};

enum {
    RTNP_ROLE_AGENT  = 1,
    RTNP_ROLE_CLIENT = 2,
};

#define RTNP_AUTH_MTLS   0x01
#define RTNP_AUTH_TOKEN  0x02

typedef struct {
    uint8_t         type;
    uint16_t        flags;
    uint32_t        sid;
    uint16_t        len;
    const uint8_t*  payload;
} rtnp_frame_t;

// 接收缓冲
typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
    // 解析出一帧时 payload 拷到这里：解析要 memmove 掉已消费的字节，
    // 若直接指向 data 会被后续数据覆盖（曾导致帧头混入业务流）。
    uint8_t* frame;
    size_t   frame_cap;
} rtnp_buf_t;

void rtnp_buf_init(rtnp_buf_t* b);
void rtnp_buf_free(rtnp_buf_t* b);
int  rtnp_buf_append(rtnp_buf_t* b, const void* data, size_t len);
// 1 = 解析出一帧（payload 指向内部缓冲，下次调用后失效）
// 0 = 数据不足
// -1 = 协议错误（magic/version/length 非法）
int  rtnp_buf_parse(rtnp_buf_t* b, rtnp_frame_t* out);

// 发送（payload 可为 NULL）
int rtnp_send(hio_t* io, uint8_t type, uint32_t sid, const void* payload, uint16_t len);

// payload 写
typedef struct {
    uint8_t* data;
    size_t   len;
    size_t   cap;
    int      err;
} rtnp_wr_t;

void rtnp_wr_init(rtnp_wr_t* w);
void rtnp_wr_free(rtnp_wr_t* w);
void rtnp_wr_u8(rtnp_wr_t* w, uint8_t v);
void rtnp_wr_u16(rtnp_wr_t* w, uint16_t v);
void rtnp_wr_u32(rtnp_wr_t* w, uint32_t v);
void rtnp_wr_u64(rtnp_wr_t* w, uint64_t v);
void rtnp_wr_str(rtnp_wr_t* w, const char* s);   // u16 len + bytes

// payload 读
typedef struct {
    const uint8_t* p;
    size_t         len;
    int            err;
} rtnp_rd_t;

void     rtnp_rd_init(rtnp_rd_t* r, const uint8_t* p, size_t len);
uint8_t  rtnp_rd_u8(rtnp_rd_t* r);
uint16_t rtnp_rd_u16(rtnp_rd_t* r);
uint32_t rtnp_rd_u32(rtnp_rd_t* r);
uint64_t rtnp_rd_u64(rtnp_rd_t* r);
// 读到 out（NUL 结尾），超长截断
void     rtnp_rd_str(rtnp_rd_t* r, char* out, size_t cap);

#endif // RTNP_H_
