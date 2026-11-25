#pragma once
// agent 与 client 共用的「本地 socket ↔ 隧道流」收尾逻辑。
// 两边各有自己的 stream 结构，但半关闭与补窗口的判定完全一致，放在这里避免两份实现漂移。

#include "hloop.h"
#include "rtnp/frame.hpp"

// 半关闭：本地写队列排空后再 SHUT_WR，避免丢掉最后一段数据。
inline void rtShutdownLocalWrite(hio_t* local, int& want_shutdown) {
    if (!want_shutdown || local == NULL || hio_is_closed(local)) return;
    if (hio_write_is_complete(local)) {
        shutdown(hio_fd(local), SHUT_WR);
        want_shutdown = 0;
        hio_setcb_write(local, NULL);
    }
}

// 补窗口：只有本地写队列降到低水位才回 WINDOW —— hio_write 入队不代表对端已消费。
// write_cb 是调用方的 localWriteCb，队列未排空时挂上，排空后由它再次触发。
inline void rtMaybeGrantWindow(hio_t* local, hio_t* tunnel, uint32_t sid, uint32_t& unacked,
                               int& want_shutdown, hwrite_cb write_cb) {
    if (local == NULL || hio_is_closed(local) || unacked == 0) return;
    if (hio_write_bufsize(local) > rtnp::LOW_WATER) {
        hio_setcb_write(local, write_cb);
        return;
    }
    if (tunnel) {
        rtnp::Writer w;
        w.u32(unacked);
        rtnp::send(tunnel, rtnp::WINDOW, sid, w.data, (uint16_t)w.len);
    }
    unacked = 0;
    if (!want_shutdown) hio_setcb_write(local, NULL);
}
