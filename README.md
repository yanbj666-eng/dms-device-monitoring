# DMS — Device Monitoring System

## 当前状态与阅读顺序

截至本轮文档核查，当前目录已进入实施阶段。**S1 协议层完成（2026-09-24）**：CMake target-based 工程、CRC-32/ISO-HDLC、18B 大端帧编解码、有界流式 FrameDecoder（半包/粘包/坏 CRC 重同步）、TLV MessageCodec（遥测/命令/ACK）。**S2 TCP 通信层完成（2026-09-24）**：结构化日志（滚动+同键聚合+RAW 帧）、`TcpTransport`（64KiB 背压、generation 防陈旧回调、结构化错误、无自动重连）、`DeviceSession` 雏形（解码分发+计数+值对象信号）、多设备 TCP 模拟器 `dms_simulator`（固定种子、8 种故障注入、SIM READY 同步行）。**S3 会话状态机完成（2026-09-26）**：`DeviceSession` 六状态状态机（Connecting/Online/Degraded/Reconnecting/Disconnected/Fault，可注入时钟+抖动源）、自动重连指数退避（1/2/4/8/10s ±10% 抖动，稳定 Online 10s 清零）、存活判定（心跳+遥测任一刷新、四指标独立新鲜度、连续 5 CRC 错降级）、组帧 2000ms 超时、命令生命周期（`CommandTracker`：Sent→Succeeded/Rejected/TimedOut/Cancelled/Failed，每设备一未决槽、迟到/重复 ACK 只记日志、Seq 跨重连不复位）。**S4 上位机与设备管理完成（2026-09-26）**：配置装配层（devices.json + app.ini 全量校验 → `TcpTransportConfig`/`SessionConfig` 值对象，未知字段/类型/范围/跨字段约束一次报全，首启模板初始化）、`DeviceManager`（IO 线程 worker-object：聚合会话、全量替换装配、统一启停、命令路由、信号带 deviceId 转发）、最小 UI（`dms_monitor`：设备列表 + 四指标实时曲线 + Reset/SetParameter 命令，100ms 合并刷新，QPainter 自绘曲线——本机 Qt 无 QtCharts，选定记录见 HLD §5）、IO 线程装配与退出顺序（阻塞停会话 → quit/wait）。9 个测试套件全部通过（8 单元 + 1 集成共 150 用例；集成用 QProcess 拉起真实模拟器验证自动重连、命令超时/取消、失联降级-恢复全链路；`scripts\build.bat` 一键构建）。S4 集成冒烟实测 10 台设备「全 Online → 强杀模拟器退避重连（真实归因）→ 重启自动恢复 → 优雅退出」闭环，并发现修复 S3 一处缺陷：重连点火未清上一轮打开时刻，异步建链期间误报「无首帧超时」（已补回归用例）。**S5 异步存储完成（2026-09-26）**：SQLite 持久化层（docs/06 schema v1）——`Database`（RAII 连接、WAL/PRAGMA、user_version 版本检查：v1 正常/高版本拒绝/空 v0 建库/未知旧库拒绝、启动恢复：遗留命令标 Interrupted、旧活跃告警按 restart 关闭、devices 表镜像全量配置含停用设备）、`DatabaseWorker`（DB 线程 worker-object：遥测有界批队列 batch_samples/flush_interval_ms 双触发、失败整批回滚保留按节奏重试、重复键 ON CONFLICT DO NOTHING 保原值、命令事件 FIFO 受理/终态保序直写、未入库设备与未知命令入队前拒绝防毒丸、退出 drainAndClose 预算内重试并如实上报未提交数）、`Repository`（received_at 半开区间 + 设备/指标过滤、(received_at,id) 键集游标分页、首查 MAX(id) 边界快照隔离翻页期间新写入、每页上限 1000、`openExisting` 只读连接 PRAGMA query_only、CSV 导出：QSaveFile 临时文件原子替换、RFC4180 转义与 =+-@ 公式防护、可取消）、`dms_monitor` 三线程装配（GUI/IO/DB，DB 先于采集就绪防首批丢样本）与退出顺序（停采集 → IO 收线程 → DB 排空或上报 → DB 收线程）。12 个测试套件全部通过（11 单元 + 1 集成；集成新增模拟器 → TCP → DeviceManager → DatabaseWorker → SQLite → Repository 只读读回的端到端用例，含命令 Sent→Succeeded 全生命周期落库与 session_id 一致性校验）。S6 起按 PRD 阶段推进；下文“支持”仍指目标功能，未实现部分不能作为已完成成果。

文档基线 D1（2026-09-23 起）。先读 [评审记录](docs/14_DOCUMENT_REVIEW.md)，再按 [PRD](docs/01_PRD.md) → [SRS](docs/02_SRS.md) → [架构](docs/03_HLD_ARCHITECTURE.md) → [详细设计](docs/04_DETAILED_DESIGN.md) 阅读。实施前对照 [协议](docs/05_PROTOCOL_SPEC.md)、[数据库](docs/06_DATABASE_DESIGN.md)、[配置](docs/15_CONFIGURATION.md) 和 [测试计划](docs/07_TEST_PLAN.md)。

保留全部必需功能，按 S1–S8 分阶段推进；TCP 阶段演示不能代替 UDP/串口及最终跨平台验收。历史回放、广播发现、实物板卡、AI 为可选。

## 项目题目
**DMS — Device Monitoring System**

中文：**C++/Qt 多源设备监控、数据采集与联调平台**

目标描述（待实现）：
> 基于 C++17 + Qt6 开发跨平台设备监控客户端，支持 TCP/UDP/串口多源设备接入、协议解析、多线程数据处理、实时曲线、告警、历史数据与日志，并通过模拟设备/可选 STM32 完成软硬件联调与完整测试文档闭环。

## 推荐技术栈
- C++17
- Qt 6.x Widgets
- Qt Network
- Qt SerialPort
- 实时曲线后端：S4 已选定 QPainter 自绘（`CurveWidget`，本机 Qt 6.8.3 官方安装未含 QtCharts；600 点重绘开销可忽略，无第三方依赖）
- SQLite / Qt SQL
- CMake
- Qt Test + CTest
- Git
- Windows 11 + Ubuntu 24.04
- 可选：ONNX Runtime、STM32/ESP32、Modbus

## 文档索引
1. `docs/01_PRD.md`：产品需求文档 PRD
2. `docs/02_SRS.md`：软件需求规格说明书
3. `docs/03_HLD_ARCHITECTURE.md`：概要设计/架构设计
4. `docs/04_DETAILED_DESIGN.md`：详细设计
5. `docs/05_PROTOCOL_SPEC.md`：通信协议设计
6. `docs/06_DATABASE_DESIGN.md`：数据库设计
7. `docs/07_TEST_PLAN.md`：单元/集成/配置项测试计划
8. `docs/08_QUALITY_PROCESS.md`：质量与配置管理流程
9. `docs/09_USER_MANUAL.md`：用户手册草案
10. `templates/TRACEABILITY_MATRIX.csv`：需求-设计-测试追踪矩阵模板
11. `AGENTS.md`：给 Codex 的项目执行说明
12. `CLAUDE.md`：给 Claude Code 的项目执行说明

13. [文档评审与修改记录](docs/14_DOCUMENT_REVIEW.md)：问题、修订、验证范围
14. [配置与运行目录契约](docs/15_CONFIGURATION.md)：默认参数、JSON/INI 与校验

## 开发原则
- 不复制大型开源项目后改名。
- 可阅读 Serial Studio、QGroundControl 等开源项目的架构与实现思路，但自己重新设计、编码和测试。
- 每个核心模块至少配一个测试或可验证的验收步骤。
- 每个阶段保留 Git commit 与 README/设计文档演进记录。
