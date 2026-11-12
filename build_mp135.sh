#!/bin/bash
# 交叉编译 STM32MP135 的 rtunnel-agent，输出到 build/deploy/agent/bin
set -e
cd "$(dirname "$0")"
. /opt/mp135/local/stm32mp135.env

cmake -S . -B build_mp135 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mp135.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DBUILD_SHARED=OFF -DBUILD_STATIC=ON \
    -DBUILD_EXAMPLES=OFF -DBUILD_UNITTEST=OFF \
    -DWITH_OPENSSL=ON -DWITH_HTTP=ON -DWITH_EVPP=ON \
    -DWITH_RPC=OFF -DWITH_MQTT=OFF -DWITH_KCP=OFF -DWITH_NGHTTP2=OFF

cmake --build build_mp135 -j"$(nproc)" --target rtunnel-agent

mkdir -p build/deploy/agent/bin
install -m 0755 build_mp135/bin/rtunnel-agent build/deploy/agent/bin/rtunnel-agent
"${STRIP}" --strip-all build/deploy/agent/bin/rtunnel-agent
file build/deploy/agent/bin/rtunnel-agent
