#include "shell_spawn.hpp"

#include <pty.h>
#include <fcntl.h>
#include <stdlib.h>

hio_t* shellSpawnPty(hloop_t* loop, void* ctx, hread_cb read_cb, hclose_cb close_cb,
                     pid_t* out_pid) {
    struct winsize wsz = {24, 80, 0, 0};
    int master = -1;
    pid_t pid = forkpty(&master, NULL, NULL, &wsz);
    if (pid < 0) return NULL;
    if (pid == 0) {
        // 全屏程序（top/htop/vi）需要 TERM；与前端 xterm.js 对齐
        setenv("TERM", "xterm", 1);
        execl("/bin/sh", "sh", "-i", (char*)NULL);
        _exit(127);
    }
    if (out_pid) *out_pid = pid;
    fcntl(master, F_SETFL, O_NONBLOCK);
    hio_t* io = hio_get(loop, master);
    if (io == NULL) {
        close(master);
        return NULL;
    }
    // 标成流类型：libhv 在 read 返回 0/EIO 时才会走 close 路径；
    // 否则 pty 子进程退出后 agent 收不到 EOF，CLOSE 永远不发。
    hio_set_type(io, HIO_TYPE_SOCK_STREAM);
    hio_set_context(io, ctx);
    hio_setcb_read(io, read_cb);
    hio_setcb_close(io, close_cb);
    hio_set_max_write_bufsize(io, rtnp::CONN_INFLIGHT + rtnp::MAX_PAYLOAD);
    hio_read(io);
    return io;
}
