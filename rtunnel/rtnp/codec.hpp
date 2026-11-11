#include <string>
#pragma once

#include "frame.hpp"
#include "hloop.h"

namespace rtnp {

// 通用字节缓冲（追加/前段消费），析构自动释放
class Buffer {
public:
    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    int  append(const void* data, size_t len);
    void consume(size_t n);
    void clear() { len = 0; }

    uint8_t* data = nullptr;
    size_t   len  = 0;
    size_t   cap  = 0;
};

// 增量解帧器：喂入任意分片，next() 逐帧吐出
class Decoder {
public:
    int append(const void* data, size_t len) { return in_.append(data, len); }
    // 1 = 解析出一帧（payload 指向内部缓冲，下次调用后失效）
    // 0 = 数据不足；-1 = 协议错误（magic/version/length 非法）
    int  next(Frame& out);
    void reset() { in_.clear(); }   // 重连时丢弃残帧

private:
    Buffer in_;
    Buffer frame_;
};

// payload 写（RAII，析构释放）
class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    void u8(uint8_t v);
    void u16(uint16_t v);
    void u32(uint32_t v);
    void u64(uint64_t v);
    void str(const char* s);   // u16 len + bytes
    void strn(const void* p, size_t n);

    uint8_t* data = nullptr;
    size_t   len  = 0;
    size_t   cap  = 0;
    int      err  = 0;

private:
    void bytes(const void* p, size_t n);
};

// payload 读（越界读置 err，不抛异常）
class Reader {
public:
    Reader(const uint8_t* p, size_t len) : p_(p), len_(len) {}

    uint8_t  u8();
    uint16_t u16();
    uint32_t u32();
    uint64_t u64();
    void     str(char* out, size_t cap);   // 读到 out（NUL 结尾），超长截断
    std::string strBytes();
    size_t   remaining() const { return len_; }   // 尚未消费的字节数

    int err = 0;

private:
    const uint8_t* take(size_t n);
    const uint8_t* p_;
    size_t         len_;
};

// 发送一帧（payload 可为 NULL；flags 默认 0，扩展位见 frame.hpp）
int send(hio_t* io, uint8_t type, uint32_t sid, const void* payload, uint16_t len, uint16_t flags = 0);

// OPEN 的 flags 扩展位：请求设备侧直接起 shell（forkpty），而非转发到本地 TCP 端口。
// relay 只透传不解释；agent 不支持时按协议错误拒绝。
constexpr uint16_t OPEN_SHELL = 0x0001;
constexpr uint16_t OPEN_TTY   = 0x0002;   // 打开设备串口，payload: devid + path + baud + db/parity/stop
constexpr uint16_t OPEN_TTY_MONITOR  = 0x0004;   // 只读监听已打开的 tty
constexpr uint16_t OPEN_TTY_TAKEOVER = 0x0008;   // 强制接管，断开现有会话

} // namespace rtnp
