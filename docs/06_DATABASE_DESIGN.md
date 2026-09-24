# 06. 数据库设计

> D1 · 2026-09-23 · SQLite schema v1 设计，当前不存在业务数据库。下列 DDL 为首次建库规范；不能直接用于覆盖已有库。

## 1. 数据与所有权

对应 REQ-DB-001/002/003、REQ-CMD-002、REQ-ALARM-001。config/devices.json 是连接配置唯一来源；devices 表保存当前/已停用设备元数据用于历史关联，不反向覆盖 JSON。删除设备采用 enabled=0，不级联删除历史；ID 不得分配给另一设备。

所有 ts、received_at、created_at 等为 UTC Unix 毫秒。telemetry.ts 为设备时间（0=未知），received_at 为客户端接收时间；查询默认依据 received_at，不能假定设备时钟已同步。每指标一行；double/SQLite REAL 可精确容纳 float32 及 uint32 的数值，metric_id 决定单位/类型。

## 2. 首次建库 DDL

启用外键必须在事务外且每个连接执行，并检查实际值。业务连接由 DB worker 在所属线程创建/使用/关闭，连接名唯一，查询对象销毁后再 removeDatabase。

```sql
PRAGMA foreign_keys = ON;
PRAGMA journal_mode = WAL;
PRAGMA busy_timeout = 1000;
PRAGMA synchronous = FULL;

BEGIN IMMEDIATE;
CREATE TABLE devices (
    id TEXT PRIMARY KEY NOT NULL CHECK(length(id) BETWEEN 1 AND 64),
    name TEXT NOT NULL CHECK(length(name) BETWEEN 1 AND 128),
    transport_type TEXT NOT NULL CHECK(transport_type IN ('tcp','udp','serial')),
    config_json TEXT NOT NULL,
    enabled INTEGER NOT NULL DEFAULT 1 CHECK(enabled IN (0,1)),
    created_at INTEGER NOT NULL CHECK(created_at >= 0)
);
CREATE TABLE telemetry (
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    session_id TEXT NOT NULL,
    seq INTEGER NOT NULL CHECK(seq BETWEEN 0 AND 4294967295),
    metric_id INTEGER NOT NULL CHECK(metric_id BETWEEN 1 AND 4),
    value REAL NOT NULL,
    ts INTEGER NOT NULL CHECK(ts >= 0),
    received_at INTEGER NOT NULL CHECK(received_at >= 0),
    UNIQUE(device_id, session_id, seq, metric_id)
);
CREATE INDEX idx_telemetry_device_time ON telemetry(device_id, received_at, id);
CREATE INDEX idx_telemetry_metric_time ON telemetry(device_id, metric_id, received_at, id);
CREATE TABLE alarms (
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    metric_id INTEGER NOT NULL DEFAULT 0 CHECK(metric_id BETWEEN 0 AND 4),
    level INTEGER NOT NULL CHECK(level BETWEEN 1 AND 3),
    code TEXT NOT NULL,
    message TEXT NOT NULL,
    ts INTEGER NOT NULL CHECK(ts >= 0),
    acknowledged INTEGER NOT NULL DEFAULT 0 CHECK(acknowledged IN (0,1)),
    acknowledged_at INTEGER,
    cleared_at INTEGER,
    clear_reason TEXT,
    CHECK((acknowledged = 0 AND acknowledged_at IS NULL)
       OR (acknowledged = 1 AND acknowledged_at IS NOT NULL AND acknowledged_at >= ts)),
    CHECK(cleared_at IS NULL OR cleared_at >= ts)
);
CREATE UNIQUE INDEX idx_alarm_active
    ON alarms(device_id, code, metric_id) WHERE cleared_at IS NULL;
CREATE INDEX idx_alarm_time ON alarms(device_id, ts, id);
CREATE TABLE commands (
    id INTEGER PRIMARY KEY,
    device_id TEXT NOT NULL REFERENCES devices(id) ON DELETE RESTRICT,
    session_id TEXT NOT NULL,
    seq INTEGER NOT NULL CHECK(seq BETWEEN 0 AND 4294967295),
    command_type TEXT NOT NULL CHECK(command_type IN ('Start','Stop','Reset','SetParameter')),
    request_json TEXT NOT NULL,
    result TEXT NOT NULL CHECK(result IN
        ('Pending','Sent','Succeeded','Rejected','TimedOut','Cancelled','Failed','Interrupted')),
    ack_status INTEGER CHECK(ack_status BETWEEN 0 AND 4),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    finished_at INTEGER,
    UNIQUE(device_id, session_id, seq),
    CHECK((result IN ('Pending','Sent') AND finished_at IS NULL)
       OR (result NOT IN ('Pending','Sent') AND finished_at IS NOT NULL AND finished_at >= created_at))
);
CREATE INDEX idx_command_time ON commands(device_id, created_at, id);
PRAGMA user_version = 1;
COMMIT;
```

level 为 1=INFO、2=WARN、3=ERROR；metric_id=0 表示设备级告警。JSON 的结构合法性和遥测非有限值检查由应用层负责，DDL 不依赖额外 SQLite 扩展。所有写入用绑定参数，不能字符串拼接 SQL。

## 3. 写入、查询和导出

每批遥测一个事务，全部成功才提交；任一失败回滚并保留待写批次，策略见 LLD。重复的 device/session/seq/metric 使用针对该唯一键的 ON CONFLICT DO NOTHING；不能用全局 INSERT OR IGNORE 吞掉外键/非空错误。同键不同内容要日志报告，不覆盖原值。

一帧多个指标共享 session_id、seq、ts、received_at。session_id 每连接/UDP 重建基准生成新 UUID；同会话用尽序列空间前更新会话，避免回绕唯一键冲突。

查询使用 received_at≥start AND received_at<end，ORDER BY received_at,id；每页最多 1000 行。翻页使用末行 (received_at,id) 游标，并在首次查询记录最大 id 作为本次查询边界，避免写入期间重复/漏翻新行。导出复用同一过滤、排序和边界，列为 device_id,metric_id,value,unit,ts,received_at。

CSV 使用 UTF-8，含逗号/换行/引号的字段按 CSV 引号规则转义；供电子表格打开的文本若以 =、+、-、@ 开头，前置单引号防止作为公式执行，数值列仍按数值输出。导出中断不得宣称成功，先写临时文件，成功后再替换目标。

## 4. 初始化、升级与恢复

- 不存在数据库时首次建库，失败回滚并保留错误，不反复删除重建。
- user_version=1 正常打开；大于应用支持版本拒绝写入，显示 DB_SCHEMA_UNSUPPORTED。
- user_version=0 且无业务表才可初始化；存在未知表的 0 版本库视为未识别旧库，要求明确迁移方案。
- 后续每个版本提供顺序迁移和测试；迁移前停止写入，使用 SQLite 备份 API，或在所有连接关闭后做一致文件副本；不可只复制活跃 WAL 库的主文件。
- 迁移 DDL 和 user_version 同事务，失败回滚并保留原版本。当前不虚构 1→2 的迁移脚本。
- 启动时把遗留 Pending/Sent 标为 Interrupted；旧活跃告警以 restart 原因关闭，新会话重新评价，不能把重启当正常恢复。

## 5. 容量与故障

10×10Hz×4 指标=400 行/s；8h 为 11,520,000 行。容量需实测行和索引开销；不能仅按浮点 8B 估算磁盘。稳定性测试前确认剩余空间并记录 DB/WAL 大小。

默认不自动清除历史。轮转日志不等于自动清理 DB；任何未来保留期/清理功能单独评审。写失败、锁等待超过 1000ms、磁盘满均回滚并告警；有限缓存满显式计数丢新样本，通信/UI 继续。WAL 不消除 SQLITE_BUSY，也不提供无限吞吐；长查询分页，避免长期持有读事务阻碍 checkpoint。

## 6. 官方依据

[Qt QSqlDatabase](https://doc.qt.io/qt-6/qsqldatabase.html) 定义连接及查询对象生命周期；[SQLite 外键](https://www.sqlite.org/foreignkeys.html) 说明按连接启用；[SQLite PRAGMA](https://www.sqlite.org/pragma.html) 定义 user_version、busy_timeout 和 journal_mode。DDL、批量与错误策略为本项目设计。
