#!/usr/bin/env bash
# ============================================================
# DMS 一键构建脚本(Linux / g++ / Ninja)
# 用法:仓库根目录执行 scripts/build.sh [Debug|Release](默认 Debug)
# 流程:CMake 配置 -> 编译 -> socat 虚拟串口对 -> CTest
#
# Qt 走系统包(Ubuntu 24.04: qt6-base-dev / qt6-serialport-dev /
# libqt6sql6-sqlite),CMake 默认前缀即可发现,无需 CMAKE_PREFIX_PATH。
# Debug/Release 分目录(build-Debug / build-Release),双配置证据可并存。
#
# 串口对仅服务于 TC-SERIAL-001:无 socat 或外部已设 DMS_TEST_SERIAL_PAIR
# 时不搭建,该用例按原有 QSKIP(Blocked) 语义跳过。
# ============================================================
set -euo pipefail

build_type="${1:-Debug}"
case "$build_type" in
    Debug | Release) ;;
    *) echo "用法: scripts/build.sh [Debug|Release]" >&2; exit 2 ;;
esac
build_dir="build-${build_type,,}"

cmake -S . -B "$build_dir" -G Ninja -DCMAKE_BUILD_TYPE="$build_type"
cmake --build "$build_dir"

# socat 虚拟串口对:模拟器占第一端,DMS 占第二端,测试按 DMS_TEST_SERIAL_PAIR 找。
# link= 固定符号链接,绕开 /dev/pts 编号随启动顺序漂移的问题
socat_pid=""
pair_dir=""
cleanup() {
    [[ -n "$socat_pid" ]] && kill "$socat_pid" 2>/dev/null || true
    [[ -n "$pair_dir" ]] && rm -rf "$pair_dir"
}
trap cleanup EXIT
if [[ -z "${DMS_TEST_SERIAL_PAIR:-}" ]] && command -v socat >/dev/null 2>&1; then
    pair_dir="$(mktemp -d /tmp/dms-serial.XXXXXX)"
    socat -d -d pty,raw,echo=0,link="$pair_dir/pty-sim" \
                 pty,raw,echo=0,link="$pair_dir/pty-dms" \
        >"$pair_dir/socat.log" 2>&1 &
    socat_pid=$!
    # socat 建链异步,等两个 link 就绪再进测试(上限 5s)
    for _ in $(seq 1 50); do
        [[ -e "$pair_dir/pty-sim" && -e "$pair_dir/pty-dms" ]] && break
        if ! kill -0 "$socat_pid" 2>/dev/null; then
            echo "[ERROR] socat 启动失败,日志: $pair_dir/socat.log" >&2
            exit 1
        fi
        sleep 0.1
    done
    export DMS_TEST_SERIAL_PAIR="$pair_dir/pty-sim,$pair_dir/pty-dms"
    echo "[build.sh] socat 串口对就绪: $DMS_TEST_SERIAL_PAIR"
else
    echo "[build.sh] 跳过串口对搭建(无 socat 或 DMS_TEST_SERIAL_PAIR 已设),TC-SERIAL-001 将 QSKIP"
fi

ctest --test-dir "$build_dir" --output-on-failure
