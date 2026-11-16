#!/bin/bash
# 交叉编译 STM32MP135 的 rtunnel-agent（带/不带本地 Web 两个变体）
#   不带 Web -> build/deploy/agent/bin/rtunnel-agent
#   带 Web   -> build/deploy/agent-web/bin/rtunnel-agent
set -e
cd "$(dirname "$0")"
. /opt/mp135/local/stm32mp135.env

# CodeMirror 编辑器组件（本地页从 /assets/cm-editor.js 加载，需内嵌进二进制）
if command -v npm >/dev/null 2>&1; then
    (cd rtunnel/relay/admin/web && npm run build:editor)
else
    echo "warning: npm 不可用，跳过 cm-editor.js，本地页编辑器退化为 textarea"
fi

cmake -S . -B build_mp135 \
    -DCMAKE_TOOLCHAIN_FILE=cmake/mp135.toolchain.cmake \
    -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DBUILD_SHARED=OFF -DBUILD_STATIC=ON \
    -DBUILD_EXAMPLES=OFF -DBUILD_UNITTEST=OFF \
    -DWITH_OPENSSL=ON -DWITH_HTTP=ON -DWITH_EVPP=ON \
    -DWITH_RPC=OFF -DWITH_MQTT=OFF -DWITH_KCP=OFF -DWITH_NGHTTP2=OFF \
    -DRTUNNEL_AGENT_WEB=ON

cmake --build build_mp135 -j"$(nproc)" --target rtunnel-agent rtunnel-agent-noweb

mkdir -p build/deploy/agent/bin build/deploy/agent/service \
         build/deploy/agent-web/bin build/deploy/agent-web/service
install -m 0755 build_mp135/bin/rtunnel-agent-noweb build/deploy/agent/bin/rtunnel-agent
install -m 0755 build_mp135/bin/rtunnel-agent       build/deploy/agent-web/bin/rtunnel-agent
"${STRIP}" --strip-all build/deploy/agent/bin/rtunnel-agent
"${STRIP}" --strip-all build/deploy/agent-web/bin/rtunnel-agent
install -m 0644 rtunnel/deploy/agent/rtunnel-agent.service     build/deploy/agent/service/rtunnel-agent.service
install -m 0644 rtunnel/deploy/agent/rtunnel-agent-web.service build/deploy/agent-web/service/rtunnel-agent.service
file build/deploy/agent/bin/rtunnel-agent build/deploy/agent-web/bin/rtunnel-agent

# 两份可拷贝部署包：rtunnel-agent-lite-<ver>.tar.gz / rtunnel-agent-web-<ver>.tar.gz
bash rtunnel/deploy/package_agent.sh
