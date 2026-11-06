#include "codec.hpp"

#include <stdlib.h>
#include <string.h>

namespace rtnp {

static int buf_reserve(void** pdata, size_t* cap, size_t need) {
    if (*cap >= need) return 0;
    size_t ncap = *cap ? *cap : 256;
    while (ncap < need) ncap <<= 1;
    void* p = realloc(*pdata, ncap);
    if (p == NULL) return -1;
    *pdata = p;
    *cap = ncap;
    return 0;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// ---------------------------------------------------------------- Buffer

Buffer::~Buffer() { free(data); }

int Buffer::append(const void* d, size_t n) {
    if (n == 0) return 0;
    if (buf_reserve((void**)&data, &cap, len + n) != 0) return -1;
    memcpy(data + len, d, n);
    len += n;
    return 0;
}

void Buffer::consume(size_t n) {
    if (n >= len) { len = 0; return; }
    memmove(data, data + n, len - n);
    len -= n;
}

// ---------------------------------------------------------------- Decoder

int Decoder::next(Frame& out) {
    if (in_.len < HEADER_SIZE) return 0;

    const uint8_t* p = in_.data;
    if (memcmp(p, "RTNP", 4) != 0) return -1;
    if (p[4] != VERSION) return -1;

    uint16_t flags = rd16(p + 6);
    uint32_t sid   = rd32(p + 8);
    uint16_t len   = rd16(p + 12);
    uint16_t rsv   = rd16(p + 14);
    if (rsv != 0) return -1;
    if (len > MAX_PAYLOAD) return -1;
    if (in_.len < HEADER_SIZE + len) return 0;

    out.type    = p[5];
    out.flags   = flags;
    out.sid     = sid;
    out.len     = len;

    // payload 必须拷出：下面的 memmove 会把后续帧移到缓冲头部，覆盖当前 payload。
    if (buf_reserve((void**)&frame_.data, &frame_.cap, len ? len : 1) != 0) return -1;
    if (len > 0) memcpy(frame_.data, p + HEADER_SIZE, len);
    out.payload = frame_.data;

    in_.consume(HEADER_SIZE + len);
    return 1;
}

// ---------------------------------------------------------------- Writer

Writer::~Writer() { free(data); }

void Writer::bytes(const void* p, size_t n) {
    if (err) return;
    if (buf_reserve((void**)&data, &cap, len + n) != 0) { err = 1; return; }
    memcpy(data + len, p, n);
    len += n;
}

void Writer::u8(uint8_t v) { bytes(&v, 1); }

void Writer::u16(uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    bytes(b, 2);
}

void Writer::u32(uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    bytes(b, 4);
}

void Writer::u64(uint64_t v) {
    u32((uint32_t)(v >> 32));
    u32((uint32_t)v);
}

void Writer::str(const char* s) {
    if (s == NULL) s = "";
    size_t n = strlen(s);
    if (n > 0xFFFF) n = 0xFFFF;
    u16((uint16_t)n);
    bytes(s, n);
}

// ---------------------------------------------------------------- Reader

const uint8_t* Reader::take(size_t n) {
    if (err || len_ < n) { err = 1; return NULL; }
    const uint8_t* p = p_;
    p_ += n;
    len_ -= n;
    return p;
}

uint8_t Reader::u8() {
    const uint8_t* p = take(1);
    return p ? p[0] : 0;
}

uint16_t Reader::u16() {
    const uint8_t* p = take(2);
    return p ? rd16(p) : 0;
}

uint32_t Reader::u32() {
    const uint8_t* p = take(4);
    return p ? rd32(p) : 0;
}

uint64_t Reader::u64() {
    uint64_t hi = u32();
    uint64_t lo = u32();
    return (hi << 32) | lo;
}

void Reader::str(char* out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    uint16_t n = u16();
    const uint8_t* p = take(n);
    if (p == NULL) return;
    size_t copy = n < cap - 1 ? n : cap - 1;
    memcpy(out, p, copy);
    out[copy] = '\0';
}

// ---------------------------------------------------------------- send

int send(hio_t* io, uint8_t type, uint32_t sid, const void* payload, uint16_t len) {
    if (len > MAX_PAYLOAD) return -1;
    uint8_t head[HEADER_SIZE];
    memcpy(head, "RTNP", 4);
    head[4] = VERSION;
    head[5] = type;
    head[6] = 0;
    head[7] = 0;
    head[8]  = (uint8_t)(sid >> 24);
    head[9]  = (uint8_t)(sid >> 16);
    head[10] = (uint8_t)(sid >> 8);
    head[11] = (uint8_t)sid;
    head[12] = (uint8_t)(len >> 8);
    head[13] = (uint8_t)len;
    head[14] = 0;
    head[15] = 0;

    // NOTE: hio_write 返回本次立刻写出的字节数，未写完的部分由 libhv 入队；
    // 只有返回负值才是真错误，不能把 nwrite < len 当失败（否则会重发导致重复数据）。
    if (hio_write(io, head, sizeof(head)) < 0) return -1;
    if (len > 0 && hio_write(io, payload, len) < 0) return -1;
    return 0;
}

} // namespace rtnp
