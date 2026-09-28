#!/usr/bin/env bash
# ============================================================
# TC-DEPLOY-002 发布包冒烟(Linux):
#   发布目录启动 dms_simulator + dms_monitor(offscreen 平台),
#   验证模板初始化/插件加载/建库/遥测落库全链,再优雅回收。
#   Windows 10 无 WSLg,GUI 冒烟只证启动与数据面,交互以 Windows 包为准。
# 用法: scripts/deploy-smoke.sh <发布目录> [--read-only]
#   --read-only: chmod 去写权后启动,证明运行期数据不依赖安装目录可写
# 输出为可粘贴证据:运行根结构、DB 就绪日志行、telemetry 行数
# ============================================================
set -euo pipefail

dist_dir="${1:?用法: deploy-smoke.sh <发布目录> [--read-only]}"
read_only="${2:-}"
bin_dir="$dist_dir/bin"
if [[ ! -x "$bin_dir/dms_monitor" || ! -x "$bin_dir/dms_simulator" ]]; then
    echo "[ERROR] $bin_dir 下缺少可执行文件" >&2
    exit 2
fi

run_root="$(mktemp -d /tmp/dms-deploy.XXXXXX)"
sim_pid=""
mon_pid=""
restore_and_kill() {
    if [[ "$read_only" == "--read-only" ]]; then
        chmod -R u+w "$dist_dir" 2>/dev/null || true
    fi
    [[ -n "$sim_pid" ]] && kill "$sim_pid" 2>/dev/null || true
    [[ -n "$mon_pid" ]] && kill "$mon_pid" 2>/dev/null || true
}
trap restore_and_kill EXIT

if [[ "$read_only" == "--read-only" ]]; then
    chmod -R a-w "$dist_dir"
    echo "[smoke] 安装目录已整体去写权: $dist_dir"
fi

# 模拟器默认 10 台 TCP 9001-9010,与发布包 config 模板 devices.json 对齐
"$bin_dir/dms_simulator" >"$run_root/sim.log" 2>&1 &
sim_pid=$!
sleep 2 # 等 SIM READY(监听就绪)

QT_QPA_PLATFORM=offscreen "$bin_dir/dms_monitor" --config-dir "$run_root" \
    >"$run_root/monitor.out" 2>&1 &
mon_pid=$!
sleep 10 # 等建库 + 首批遥测入库(批 500/flush 1000ms)

kill -TERM "$mon_pid" 2>/dev/null || true
wait "$mon_pid" 2>/dev/null || true
kill "$sim_pid" 2>/dev/null || true

echo "===== 运行根结构(模板初始化 + 运行期数据) ====="
find "$run_root" -maxdepth 2 | sort
echo "===== DB 就绪日志(含 SQLite 引擎版本) ====="
grep -h "数据库就绪" "$run_root"/logs/*.log || { echo "[FAIL] 无 DB 就绪日志"; exit 1; }
echo "===== 遥测落库行数 ====="
sqlite3 "$run_root/data/dms.db" "SELECT count(*) FROM telemetry" \
    || { echo "[FAIL] telemetry 查询失败"; exit 1; }
echo "===== 冒烟结果 ====="
echo "[smoke] 发布包全链路 OK:模拟器→上位机→SQLite"
