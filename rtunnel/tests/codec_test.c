// RTNP 编解码单测：对照设计文档 §8.6 的十六进制样例。
#include <stdio.h>
#include <string.h>

#include "rtnp.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL %s\n", msg); ++failures; } \
    else { printf("PASS %s\n", msg); } \
} while (0)

int main(void) {
    // 1) payload 编码：OPEN(devid="PILE-000123", port=22)
    rtnp_wr_t w; rtnp_wr_init(&w);
    rtnp_wr_str(&w, "PILE-000123");
    rtnp_wr_u16(&w, 22);
    const uint8_t expect_payload[] = {
        0x00, 0x0B, 'P', 'I', 'L', 'E', '-', '0', '0', '0', '1', '2', '3', 0x00, 0x16
    };
    CHECK(w.len == sizeof(expect_payload), "payload 长度 = 15");
    CHECK(memcmp(w.data, expect_payload, sizeof(expect_payload)) == 0, "payload 字节与设计样例一致");
    rtnp_wr_free(&w);

    // 2) 整帧解析：header + payload
    const uint8_t frame[] = {
        0x52, 0x54, 0x4E, 0x50, 0x01, 0x10, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x07, 0x00, 0x0F, 0x00, 0x00,
        0x00, 0x0B, 'P', 'I', 'L', 'E', '-', '0', '0', '0', '1', '2', '3', 0x00, 0x16
    };

    rtnp_buf_t b; rtnp_buf_init(&b);
    // 分片喂入：前 5 字节应“数据不足”
    rtnp_buf_append(&b, frame, 5);
    rtnp_frame_t f;
    CHECK(rtnp_buf_parse(&b, &f) == 0, "半帧时返回 0（数据不足）");
    rtnp_buf_append(&b, frame + 5, sizeof(frame) - 5);
    int rc = rtnp_buf_parse(&b, &f);
    CHECK(rc == 1, "补全后解析出一帧");
    CHECK(f.type == RTNP_OPEN, "type = OPEN(0x10)");
    CHECK(f.sid == 7, "stream_id = 7");
    CHECK(f.len == 15, "length = 15");
    CHECK(f.payload && memcmp(f.payload, expect_payload, sizeof(expect_payload)) == 0,
          "payload 内容正确");
    rtnp_buf_free(&b);

    // 3) 非法 magic
    const uint8_t bad[] = { 'X', 'T', 'N', 'P', 1, 0x10, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0 };
    rtnp_buf_init(&b);
    rtnp_buf_append(&b, bad, sizeof(bad));
    CHECK(rtnp_buf_parse(&b, &f) == -1, "magic 错误返回 -1");
    rtnp_buf_free(&b);

    // 4) reserved 非 0
    uint8_t bad2[sizeof(frame)];
    memcpy(bad2, frame, sizeof(frame));
    bad2[15] = 1;
    rtnp_buf_init(&b);
    rtnp_buf_append(&b, bad2, sizeof(bad2));
    CHECK(rtnp_buf_parse(&b, &f) == -1, "reserved != 0 返回 -1");
    rtnp_buf_free(&b);

    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures ? 1 : 0;
}
