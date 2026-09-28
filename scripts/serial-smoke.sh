#!/usr/bin/env bash
# ============================================================
# TC-SERIAL-001 单项冒烟:搭 socat 虚拟串口对,只跑串口 E2E
# 用法:仓库根目录执行 scripts/serial-smoke.sh [build-dir]
#      (默认 build-debug,与 build.sh Debug 的目录命名一致)
# 输出 Qt Test 的 PASS/SKIP/Totals 行,作为"真跑过/被跳过"的证据
# ============================================================
set -euo pipefail

build_dir="${1:-build-debug}"
test_bin="$build_dir/tests/integration/tst_integration"
if [[ ! -x "$test_bin" ]]; then
    echo "[ERROR] 缺少 $test_bin,先执行 scripts/build.sh Debug" >&2
    exit 2
fi

pair_dir="$(mktemp -d /tmp/dms-serial.XXXXXX)"
socat -d -d pty,raw,echo=0,link="$pair_dir/pty-sim" \
             pty,raw,echo=0,link="$pair_dir/pty-dms" >"$pair_dir/socat.log" 2>&1 &
socat_pid=$!
trap 'kill "$socat_pid" 2>/dev/null || true; rm -rf "$pair_dir"' EXIT
for _ in $(seq 1 50); do
    [[ -e "$pair_dir/pty-sim" && -e "$pair_dir/pty-dms" ]] && break
    sleep 0.1
done

DMS_TEST_SERIAL_PAIR="$pair_dir/pty-sim,$pair_dir/pty-dms" "$test_bin" serialLinkEndToEnd
