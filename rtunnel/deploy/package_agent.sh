#!/bin/bash
# 把 build/deploy 下的 agent 部署目录打成两份可拷贝的包：
#   rtunnel-agent-lite-<ver>.tar.gz  不带本地 Web
#   rtunnel-agent-web-<ver>.tar.gz   带本地 Web（页面已内嵌进二进制）
# 两个包解压后都是 agent/，部署路径一致，便于切换。
set -e
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPLOY="${RTUNNEL_DEPLOY_DIR:-$ROOT/build/deploy}"
VER="${1:-$(git -C "$ROOT" describe --tags --always 2>/dev/null || echo dev)}"

[ -x "$DEPLOY/agent/bin/rtunnel-agent" ] || { echo "缺少 $DEPLOY/agent/bin/rtunnel-agent，先构建" >&2; exit 1; }

mkdir -p "$DEPLOY/packages"
tar -C "$DEPLOY" -czf "$DEPLOY/packages/rtunnel-agent-lite-$VER.tar.gz" \
    agent/bin/rtunnel-agent agent/service/rtunnel-agent.service
if [ -x "$DEPLOY/agent-web/bin/rtunnel-agent" ]; then
    tar -C "$DEPLOY" --transform 's,^agent-web,agent,' \
        -czf "$DEPLOY/packages/rtunnel-agent-web-$VER.tar.gz" \
        agent-web/bin/rtunnel-agent agent-web/service/rtunnel-agent.service
else
    echo "警告: 未找到 $DEPLOY/agent-web/bin/rtunnel-agent，只生成 lite 包" >&2
fi
ls -lh "$DEPLOY"/packages/rtunnel-agent-*.tar.gz
