#!/usr/bin/env bash
# 在 Windows 上编译 shinxbot2 插件（经由 WSL）。
#
# 用法:
#   bash tools/build-plugin-wsl.sh nikohug
#   bash tools/build-plugin-wsl.sh all
#
# 前置条件（首次一次性准备，见 docs/BUILD-WINDOWS.md）:
#   - WSL2 + Debian/Ubuntu 发行版
#   - build-essential cmake pkg-config libfmt-dev libjsoncpp-dev libzip-dev
#   - ImageMagick 7（发行版 apt 只有 IM6，需源码编译或自行安装）
#
# 产物: ./lib/functions/lib<plugin>.so

set -euo pipefail

DISTRO="${WSL_DISTRO:-Ubuntu-24.04}"
PLUGIN="${1:-}"

if [ -z "$PLUGIN" ]; then
    echo "用法: bash tools/build-plugin-wsl.sh <plugin-name>|all" >&2
    exit 1
fi

REPO_WIN="$(cd "$(dirname "$0")/.." && pwd)"
REPO_WSL="$(wsl.exe -d "$DISTRO" -- wslpath -a "$REPO_WIN" 2>/dev/null | tr -d '\r')"

if [ -z "$REPO_WSL" ]; then
    echo "无法解析 WSL 路径，请确认 WSL 与 $DISTRO 可用" >&2
    exit 1
fi

echo "==> distribution: $DISTRO"
echo "==> repo (win):  $REPO_WIN"
echo "==> repo (wsl):  $REPO_WSL"

build_one() {
    local name="$1"
    echo ""
    echo "==> building $name"
    wsl.exe -d "$DISTRO" -u root -- bash -lc "
        set -e
        export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:\${PKG_CONFIG_PATH:-}
        export LD_LIBRARY_PATH=/usr/local/lib:\${LD_LIBRARY_PATH:-}
        cd '$REPO_WSL/functions/$name'
        rm -rf build && mkdir -p build && cd build
        cmake -DCMAKE_BUILD_TYPE=Release .. > /tmp/cm.log 2>&1 || { tail -20 /tmp/cm.log; exit 1; }
        grep -i 'Found ImageMagick' /tmp/cm.log || true
        make -j\$(nproc) 2>&1 | tail -5
    "
}

if [ "$PLUGIN" = "all" ]; then
    for d in "$REPO_WIN"/functions/*/; do
        n="$(basename "$d")"
        [ -f "$d/CMakeLists.txt" ] || continue
        [ "$n" = "functions" ] && continue
        build_one "$n"
    done
else
    build_one "$PLUGIN"
fi

echo ""
echo "==> 产物:"
ls -lh "$REPO_WIN"/lib/functions/*.so 2>/dev/null || echo "(无)"
