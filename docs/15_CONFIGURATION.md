# 15. 配置与运行目录契约

> D1 · 2026-09-23 · 设计态；以下示例是文档内的配置样例，当前没有可读取它们的程序。对应 REQ-CONFIG-001、REQ-DEVICE-001。

## 1. 路径与来源

规划通过 QStandardPaths::AppLocalDataLocation 取得可写运行根目录，启动日志显示解析后的绝对路径。根目录下为 config/devices.json、config/app.ini、data/dms.db、logs/、exports/。发布包内 config 只作首次初始化模板；已有用户配置不覆盖。数据库路径相对该根目录解析，不能相对当前工作目录或强制写入安装目录。

连接配置只以 devices.json 为源，数据库 devices 是历史关联镜像。保存流程：全量校验→同目录临时文件→原子替换→停用受影响会话→同步元数据→应用新快照。写文件失败保留旧文件；元数据失败时相关设备保持禁用并显示 CONFIG_APPLY_FAILED，重试成功后才允许连接，不假称配置已完全生效。设备删除只禁用 DB 元数据，历史数据保留。

## 2. devices.json 示例

```json
{
  "schema_version": 1,
  "devices": [
    {
      "id": "sim-tcp-01",
      "name": "TCP 模拟设备 01",
      "enabled": true,
      "transport": "tcp",
      "host": "127.0.0.1",
      "port": 9001,
      "alarm_rules": [
        {"metric_id": 1, "low": 0.0, "high": 80.0, "hysteresis": 2.0}
      ]
    },
    {
      "id": "sim-udp-01",
      "name": "UDP 模拟设备 01",
      "enabled": false,
      "transport": "udp",
      "bind_address": "127.0.0.1",
      "bind_port": 9100,
      "peer_address": "127.0.0.1",
      "peer_port": 9101,
      "alarm_rules": []
    },
    {
      "id": "sim-serial-01",
      "name": "串口模拟设备 01",
      "enabled": false,
      "transport": "serial",
      "port_name": "COM3",
      "baud_rate": 115200,
      "data_bits": 8,
      "parity": "none",
      "stop_bits": 1,
      "flow_control": "none",
      "alarm_rules": []
    }
  ]
}
```

COM3 仅示例；Ubuntu 填实际枚举的 /dev/tty… 路径，不假定设备存在。UDP/串口默认禁用仅避免误连接，不表示功能不属最终范围。

## 3. 校验规则

- schema_version 必须为整数 1；未知版本拒绝加载，不能猜字段含义。
- id 为 1–64 个 ASCII 字母/数字/下划线/短横线，全局唯一且永久标识设备；name 为 1–128 字符；enabled 必须是布尔类型。
- transport 为 tcp/udp/serial；其专用字段必须齐全，混用字段或未知字段拒绝并指明路径，避免拼写错误被静默忽略。
- TCP host 非空、最长 253 字符，合法 IP 或 DNS 名；port 为整数 1–65535。相同 host/port 不允许映射多个设备。
- UDP bind_address/peer_address 必须是合法 IP，peer 不允许通配地址；端口 1–65535；四元组 (bind_address,bind_port,peer_address,peer_port) 唯一；同一 bind 复用 socket。
- 串口 port_name 非空；baud_rate∈{9600,19200,38400,57600,115200}，data_bits∈{7,8}，parity∈{none,even,odd}，stop_bits∈{1,2}，flow_control∈{none,hardware,software}；不支持参数须明确报错，不能悄悄改为默认值。启用设备不能重复占用同一串口。
- alarm_rules 可为空；每个 metric_id∈1..4 且不能重复；low/high 都是有限数且 low<high；0≤hysteresis≤(high-low)/2。阈值使用协议单位。
- 首次无配置提示初始化，非法配置不自动覆盖；reload 失败保留当前有效快照并给出字段错误。在线编辑需先停止对应会话，不能边收流边更改协议配置。

## 4. app.ini 默认值

```ini
[app]
schema_version=1

[connection]
connect_timeout_ms=3000
heartbeat_interval_ms=1000
offline_timeout_ms=4000
health_check_ms=100
stale_timeout_ms=3000
command_timeout_ms=2000
reconnect_max_ms=10000

[protocol]
assembly_timeout_ms=2000
max_buffer_bytes=65536
max_tx_bytes=65536

[ui]
refresh_interval_ms=100
curve_points=600

[database]
path=data/dms.db
batch_samples=500
flush_interval_ms=1000
queue_samples=50000
event_queue_size=1000
busy_timeout_ms=1000
shutdown_flush_ms=5000

[logging]
level=INFO
raw_frames=false
max_file_bytes=10485760
max_files=5
```

默认值是设计输入，不是实测最优值。未知 INI 节/键、类型错误和超范围拒绝。缺失可选键采用这里的默认值并记录最终生效快照。

| 参数 | 接受范围/关联约束 |
|---|---|
| connect_timeout_ms | 100–3000 |
| heartbeat_interval_ms | 100–1000 |
| offline_timeout_ms | 3×heartbeat_interval_ms 至 4500 |
| health_check_ms | 10–100；offline_timeout_ms+health_check_ms≤4600，给调度保留余量 |
| stale_timeout_ms | 1000 至 offline_timeout_ms |
| command_timeout_ms | 100–2000 |
| reconnect_max_ms | 1000–10000；实际延迟截断至上限 |
| assembly_timeout_ms | 100–2000；115200/8N1 的最大帧约需 358ms；低波特率须限制对端连续发送帧长，不能承诺 9600 下最大帧仍满足超时 |
| max_buffer_bytes / max_tx_bytes | 4118–1048576 |
| refresh_interval_ms / curve_points | 100–200ms / 10–6000 点 |
| batch_samples / queue_samples | 1–5000 / batch_samples 至 500000 |
| flush_interval_ms / event_queue_size | 100–5000ms / 10–10000 |
| busy_timeout_ms / shutdown_flush_ms | 100–1000ms / 1000–10000ms |
| max_file_bytes / max_files | 1–100MiB / 1–20（含当前日志） |
| level / raw_frames | DEBUG、INFO、WARN、ERROR / true、false |

数据库 path 只允许运行根下的相对路径，拒绝越界 ..、绝对路径和解析后逃逸的路径。路径合法仍需检查可写性。payload 最大 4096、CRC 参数、协议类型不是可随意修改的配置项。默认条件为性能/稳定性验收基线；自定义参数另记测试条件。

串口 9600/8N1 传 4118B 约需 4.29s，会超过组帧与失联门槛；低波特率下只发送标准 54B 遥测、小命令和心跳。对端若需要大载荷，应提高波特率或单独评审协议/超时配置，不静默放宽 5s 失联目标。

## 5. 模拟器配置与故障注入

`dms_simulator`（S2 已实现）CLI 参数：

| 参数 | 含义 | 接受范围/默认 |
|---|---|---|
| `-c, --count <n>` | 模拟设备数，每设备独立监听端口 | 1–100，默认 10 |
| `-p, --port <port>` | 起始 TCP 监听端口，端口段不得越界 | 1–65535，默认 9001 |
| `-s, --period <ms>` | 遥测采样周期 | 100–1000，默认 100 |
| `-d, --seed <n>` | 随机种子；每设备实际种子 = seed + index×1000003，数据与注入序列可复现 | uint32，默认 42 |
| `-f, --fault <spec>` | 故障注入，可重复；`mode@startMs+durationMs`，时间相对进程启动 | mode 见下 |
| `-r, --run-seconds <s>` | 运行秒数，0=直到手动停止 | 0–86400，默认 0 |
| `-l, --log-dir <dir>` | 日志目录；缺省只输出 stderr | 合法目录 |
| `--log-level <level>` | 日志级别 | DEBUG/INFO/WARN/ERROR，默认 INFO |

非法参数明确报错退出（stderr + CONFIG_INVALID 日志），不悄悄改用默认值。全部端口就绪后向 stdout 输出一行 `SIM READY p0 p1 …` 作为集成测试同步点；每 10s 记录累计发送帧/样本数，退出时汇总。`-v/--version` 被 Qt 保留，`--log-level` 为长选项。

标准 TCP 负载：10 个独立端点 127.0.0.1:9001–9010，每 100ms 一帧、四指标，心跳 1000ms。S7 增加 UDP 来源固定端口和虚拟串口对。

故障模式（`--fault` 的 mode）：`badCrc`（坏 CRC，帧尾字节翻转）、`abnormalValue`（遥测超常值，喂告警引擎）、`stopSending`（遥测+心跳全静默）、`disconnect`（开窗即刻拆链，窗口内新连接接受即拆）、`ackDelay`（ACK 延迟 500ms）、`ackNoReply`（不回 ACK）、`fragment`（每帧拆两段、间隔 15ms，多帧并发时整帧排队保序）、`merge`（相邻两帧并一次写，孤帧 300ms 兜底）。UDP 才注入数据报丢弃/乱序/重复；TCP 跳过一条应用帧不能称为 TCP 网络丢包。
