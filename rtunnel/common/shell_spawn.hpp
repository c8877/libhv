#pragma once
// 通用 forkpty 起交互 shell（A 端 client 与 C 端 agent 共用）
// 在 loop 上 forkpty 并把 pty 挂成 hio（设好 context/read/close 回调）。
// 返回 NULL 表示失败；pid 通过 out_pid 带出，由调用方在关闭时回收。

#include "rtnp/codec.hpp"

hio_t* shellSpawnPty(hloop_t* loop, void* ctx, hread_cb read_cb, hclose_cb close_cb,
                     pid_t* out_pid);
