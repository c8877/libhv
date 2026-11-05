#!/usr/bin/env bash
# ============================================================================
# scripts/install_hooks.sh —— 一键启用本仓库的 .githooks
#
# 作用：git config core.hooksPath .githooks
#   钩子随仓库分发，不依赖每个人手工拷贝 .git/hooks/pre-commit；
#   启用后每次提交由 .githooks/pre-commit 把 base/hversion.c 的
#   HV_VERSION_PATCH +1、重算 HV_VERSION_STRING，并 git add 进本次提交。
#
# 用法：bash scripts/install_hooks.sh
# ============================================================================

set -euo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT"

[ -d .githooks ] || { echo "[hooks] FAIL: 缺少 .githooks/（请在仓库根运行）" >&2; exit 1; }
[ -f .githooks/pre-commit ] || { echo "[hooks] FAIL: 缺少 .githooks/pre-commit" >&2; exit 1; }

old="$(git config --get core.hooksPath || true)"
if [ -n "$old" ] && [ "$old" != ".githooks" ]; then
    echo "[hooks] 注意: 原 core.hooksPath = $old，将被覆盖为 .githooks"
fi

chmod +x .githooks/pre-commit
git config core.hooksPath .githooks

echo "[hooks] core.hooksPath = $(git config core.hooksPath)"
echo "[hooks] 已启用 $ROOT/.githooks/pre-commit（每次提交 patch+1、重算 string）"
