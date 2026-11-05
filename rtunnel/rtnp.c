#include "rtnp.h"

#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- 通用缓冲

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

void rtnp_buf_init(rtnp_buf_t* b) {
    memset(b, 0, sizeof(*b));
}

void rtnp_buf_free(rtnp_buf_t* b) {
    free(b->data);
    free(b->frame);
    memset(b, 0, sizeof(*b));
}

int rtnp_buf_append(rtnp_buf_t* b, const void* data, size_t len) {
    if (len == 0) return 0;
    if (buf_reserve((void**)&b->data, &b->cap, b->len + len) != 0) return -1;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 0;
}

static uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

int rtnp_buf_parse(rtnp_buf_t* b, rtnp_frame_t* out) {
    if (b->len < RTNP_HEADER_SIZE) return 0;

    const uint8_t* p = b->data;
    if (memcmp(p, RTNP_MAGIC, 4) != 0) return -1;
    if (p[4] != RTNP_VERSION) return -1;

    uint16_t flags = rd16(p + 6);
    uint32_t sid   = rd32(p + 8);
    uint16_t len   = rd16(p + 12);
    uint16_t rsv   = rd16(p + 14);
    if (rsv != 0) return -1;
    if (len > RTNP_MAX_PAYLOAD) return -1;
    if (b->len < (size_t)RTNP_HEADER_SIZE + len) return 0;

    out->type    = p[5];
    out->flags   = flags;
    out->sid     = sid;
    out->len     = len;

    // payload 必须拷出：下面的 memmove 会把后续帧移到缓冲头部，覆盖当前 payload。
    if (buf_reserve((void**)&b->frame, &b->frame_cap, len ? len : 1) != 0) return -1;
    if (len > 0) memcpy(b->frame, p + RTNP_HEADER_SIZE, len);
    out->payload = b->frame;

    size_t consumed = (size_t)RTNP_HEADER_SIZE + len;
    memmove(b->data, b->data + consumed, b->len - consumed);
    b->len -= consumed;
    return 1;
}

int rtnp_send(hio_t* io, uint8_t type, uint32_t sid, const void* payload, uint16_t len) {
    if (len > RTNP_MAX_PAYLOAD) return -1;
    uint8_t head[RTNP_HEADER_SIZE];
    memcpy(head, RTNP_MAGIC, 4);
    head[4] = RTNP_VERSION;
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

// ---------------------------------------------------------------- payload 写

void rtnp_wr_init(rtnp_wr_t* w) {
    memset(w, 0, sizeof(*w));
}

void rtnp_wr_free(rtnp_wr_t* w) {
    free(w->data);
    memset(w, 0, sizeof(*w));
}

static void wr_bytes(rtnp_wr_t* w, const void* p, size_t n) {
    if (w->err) return;
    if (buf_reserve((void**)&w->data, &w->cap, w->len + n) != 0) { w->err = 1; return; }
    memcpy(w->data + w->len, p, n);
    w->len += n;
}

void rtnp_wr_u8(rtnp_wr_t* w, uint8_t v) {
    wr_bytes(w, &v, 1);
}

void rtnp_wr_u16(rtnp_wr_t* w, uint16_t v) {
    uint8_t b[2] = { (uint8_t)(v >> 8), (uint8_t)v };
    wr_bytes(w, b, 2);
}

void rtnp_wr_u32(rtnp_wr_t* w, uint32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    wr_bytes(w, b, 4);
}

void rtnp_wr_u64(rtnp_wr_t* w, uint64_t v) {
    rtnp_wr_u32(w, (uint32_t)(v >> 32));
    rtnp_wr_u32(w, (uint32_t)v);
}

void rtnp_wr_str(rtnp_wr_t* w, const char* s) {
    if (s == NULL) s = "";
    size_t n = strlen(s);
    if (n > 0xFFFF) n = 0xFFFF;
    rtnp_wr_u16(w, (uint16_t)n);
    wr_bytes(w, s, n);
}

// ---------------------------------------------------------------- payload 读

void rtnp_rd_init(rtnp_rd_t* r, const uint8_t* p, size_t len) {
    r->p = p;
    r->len = len;
    r->err = 0;
}

static const uint8_t* take(rtnp_rd_t* r, size_t n) {
    if (r->err || r->len < n) { r->err = 1; return NULL; }
    const uint8_t* p = r->p;
    r->p += n;
    r->len -= n;
    return p;
}

uint8_t rtnp_rd_u8(rtnp_rd_t* r) {
    const uint8_t* p = take(r, 1);
    return p ? p[0] : 0;
}

uint16_t rtnp_rd_u16(rtnp_rd_t* r) {
    const uint8_t* p = take(r, 2);
    return p ? rd16(p) : 0;
}

uint32_t rtnp_rd_u32(rtnp_rd_t* r) {
    const uint8_t* p = take(r, 4);
    return p ? rd32(p) : 0;
}

uint64_t rtnp_rd_u64(rtnp_rd_t* r) {
    uint64_t hi = rtnp_rd_u32(r);
    uint64_t lo = rtnp_rd_u32(r);
    return (hi << 32) | lo;
}

void rtnp_rd_str(rtnp_rd_t* r, char* out, size_t cap) {
    if (out == NULL || cap == 0) return;
    out[0] = '\0';
    uint16_t n = rtnp_rd_u16(r);
    const uint8_t* p = take(r, n);
    if (p == NULL) return;
    size_t copy = n < cap - 1 ? n : cap - 1;
    memcpy(out, p, copy);
    out[copy] = '\0';
}
