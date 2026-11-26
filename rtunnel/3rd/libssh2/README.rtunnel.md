# libssh2（裁剪源码）

- 版本：libssh2 1.11.1（上游 tag `libssh2-1.11.1`，commit `a312b43325e3383c865a87bb1d26cb52e3292641`）
- 来源：https://github.com/libssh2/libssh2
- 许可：BSD-3-Clause，见 COPYING

只保留 `include/` 与 `src/`，去掉 mbedTLS / libgcrypt / WinCNG / OS400 后端和
Windows agent。加密后端固定用 OpenSSL（rtunnel 本来就必须链接 OpenSSL）。

上游的 CMakeLists 依赖 docs/example/tests，这里用自己的 `CMakeLists.txt` 直接编译
`src/*.c`，并把 `libssh2_config_cmake.h.in` 生成为 `libssh2_config.h`（只针对
Linux/POSIX 打开需要的 `HAVE_*`）。升级时替换 `src/` 与 `include/` 后重新构建即可。
