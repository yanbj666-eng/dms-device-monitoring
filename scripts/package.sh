#!/usr/bin/env bash
# ============================================================
# DMS Linux 发布包(S8,TC-DEPLOY-002)
# 用法:仓库根目录执行 scripts/package.sh
# 流程:Release 构建 -> cmake --install 装发布目录 -> tar.gz
#
# Linux 侧 Qt 走系统包,包内不带 Qt 库;干净环境部署时安装 README
# 声明的系统依赖(qt6-base / sqldrivers.sqlite / serialport)即可。
# 运行期数据(logs/、SQLite)落 AppLocalDataLocation,安装目录可只读。
# ============================================================
set -euo pipefail

build_dir="build-Release"
dist_root="dist"
dist_name="dms-linux-x86_64"
version="$(git rev-parse --short HEAD 2>/dev/null || echo uncommitted)"

cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir"

rm -rf "$dist_root/$dist_name"
cmake --install "$build_dir" --prefix "$dist_root/$dist_name"

# 版本号随包名固化,证据可追溯(时间戳防同名覆盖)
archive="$dist_root/${dist_name}-${version}-$(date +%Y%m%d).tar.gz"
tar -C "$dist_root" -czf "$archive" "$dist_name"
echo "[package.sh] 发布目录: $dist_root/$dist_name"
echo "[package.sh] 发布包:   $archive"
