# DMS — Device Monitoring System

## 当前状态与阅读顺序

截至本轮文档核查，当前目录已进入实施阶段。**S1 协议层完成（2026-09-24）**：CMake target-based 工程、CRC-32/ISO-HDLC、18B 大端帧编解码、有界流式 FrameDecoder（半包/粘包/坏 CRC 重同步）、TLV MessageCodec（遥测/命令/ACK）。**S2 TCP 通信层完成（2026-09-24）**：结构化日志（滚动+同键聚合+RAW 帧）、`TcpTransport`（64KiB 背压、generation 防陈旧回调、结构化错误、无自动重连）、`DeviceSession` 雏形（解码分发+计数+值对象信号）、多设备 TCP 模拟器 `dms_simulator`（固定种子、8 种故障注入、SIM READY 同步行）。**S3 会话状态机完成（2026-09-26）**：`DeviceSession` 六状态状态机（Connecting/Online/Degraded/Reconnecting/Disconnected/Fault，可注入时钟+抖动源）、自动重连指数退避（1/2/4/8/10s ±10% 抖动，稳定 Online 10s 清零）、存活判定（心跳+遥测任一刷新、四指标独立新鲜度、连续 5 CRC 错降级）、组帧 2000ms 超时、命令生命周期（`CommandTracker`：Sent→Succeeded/Rejected/TimedOut/Cancelled/Failed，每设备一未决槽、迟到/重复 ACK 只记日志、Seq 跨重连不复位）。**S4 上位机与设备管理完成（2026-09-26）**：配置装配层（devices.json + app.ini 全量校验 → `TcpTransportConfig`/`SessionConfig` 值对象，未知字段/类型/范围/跨字段约束一次报全，首启模板初始化）、`DeviceManager`（IO 线程 worker-object：聚合会话、全量替换装配、统一启停、命令路由、信号带 deviceId 转发）、最小 UI（`dms_monitor`：设备列表 + 四指标实时曲线 + Reset/SetParameter 命令，100ms 合并刷新，QPainter 自绘曲线——本机 Qt 无 QtCharts，选定记录见 HLD §5）、IO 线程装配与退出顺序（阻塞停会话 → quit/wait）。9 个测试套件全部通过（8 单元 + 1 集成共 150 用例；集成用 QProcess 拉起真实模拟器验证自动重连、命令超时/取消、失联降级-恢复全链路；`scripts\build.bat` 一键构建）。S4 集成冒烟实测 10 台设备「全 Online → 强杀模拟器退避重连（真实归因）→ 重启自动恢复 → 优雅退出」闭环，并发现修复 S3 一处缺陷：重连点火未清上一轮打开时刻，异步建链期间误报「无首帧超时」（已补回归用例）。**S5 异步存储完成（2026-09-26）**：SQLite 持久化层（docs/06 schema v1）——`Database`（RAII 连接、WAL/PRAGMA、user_version 版本检查：v1 正常/高版本拒绝/空 v0 建库/未知旧库拒绝、启动恢复：遗留命令标 Interrupted、旧活跃告警按 restart 关闭、devices 表镜像全量配置含停用设备）、`DatabaseWorker`（DB 线程 worker-object：遥测有界批队列 batch_samples/flush_interval_ms 双触发、失败整批回滚保留按节奏重试、重复键 ON CONFLICT DO NOTHING 保原值、命令事件 FIFO 受理/终态保序直写、未入库设备与未知命令入队前拒绝防毒丸、退出 drainAndClose 预算内重试并如实上报未提交数）、`Repository`（received_at 半开区间 + 设备/指标过滤、(received_at,id) 键集游标分页、首查 MAX(id) 边界快照隔离翻页期间新写入、每页上限 1000、`openExisting` 只读连接 PRAGMA query_only、CSV 导出：QSaveFile 临时文件原子替换、RFC4180 转义与 =+-@ 公式防护、可取消）、`dms_monitor` 三线程装配（GUI/IO/DB，DB 先于采集就绪防首批丢样本）与退出顺序（停采集 → IO 收线程 → DB 排空或上报 → DB 收线程）。12 个测试套件全部通过（11 单元 + 1 集成；集成新增模拟器 → TCP → DeviceManager → DatabaseWorker → SQLite → Repository 只读读回的端到端用例，含命令 Sent→Succeeded 全生命周期落库与 session_id 一致性校验）。**S6 告警引擎完成（2026-09-26）**：`AlarmEngine`（IO 线程 worker-object，与 `DeviceManager` 同线程直连消费其转发信号）裁决四类告警生命周期（LLD §6）——THRESHOLD_HIGH/LOW 阈值+迟滞带回（触发 value>high、恢复 value≤high−h，低界方向独立，未配置阈值的指标不评估）、DATA_STALE 指标 3000ms 无有效值、PROTOCOL_CRC 连续 5 坏帧触发/连续 3 有效帧解除、COMM_LOST 失联触发/恢复解除；内存活跃表是唯一裁决点：同键 (code,metricId) 持续异常只触发一次，再触发产生全新记录保留历史，确认独立于恢复（先恢复仍可确认，确认目标=该键最近一条），手动断开以 manual_disconnect 关闭全部活跃告警而不触发失联；评估参数从 app.ini [connection] 同源映射，不引第二套配置。会话侧新增 `protocolHealth` 信号（坏/好帧计数）与采集启停健康豁免（Stop ACK 后心跳维持 Online、陈旧检测整设备暂停，Start ACK 恢复并重置计时）。`DatabaseWorker` 事件队列改 `std::variant<CommandEvent,AlarmEvent>` 命令/告警共用 FIFO 保序，alarms 三条 prepared SQL（触发 INSERT、恢复补 cleared_at+clear_reason、确认落该键最近一条未确认，与引擎内存裁决同语义），atMs 由引擎裁决点打点、worker 照抄；入队四道防御（stopped/fatal、未知设备防 FK 毒丸、Triggered 过表 CHECK 域、队列满丢新计数）。UI 新增告警面板（底部 dock QTableWidget：时间/设备/指标/代码/等级/状态/确认，新事件插顶、500 行封顶，确认按钮经 queued 信号请求引擎裁决、结果经 alarmEvent 回流不本地置位）。`dms_monitor` 装配接入：引擎四路输入同线程 direct、出口分流 DB 落库与 GUI 显示。13 个测试套件全部通过（12 单元 + 1 集成）：新增 tst_alarmengine 19 用例（注入假时钟确定性断言迟滞带/去重/独立确认/暂停豁免/CRC 交错复位）、tst_databaseworker 3 个告警落库用例（生命周期/未知设备/域外毒丸），集成新增 4 个故障注入 E2E（abnormalValue 越界→阈值触发/恢复/reason=recovered 落库读回、badCrc 窗口→PROTOCOL_CRC 触发与解除、stopSending 长窗→DATA_STALE×4+COMM_LOST 重连后全部解除、Stop/Start 采集豁免全程零 DATA_STALE 误报）。**S7 UDP/串口传输完成（2026-09-28）**：传输层第三、四实现补齐三形态——`UdpTransport`（同一 (bindAddress,bindPort) 经进程级端点池共享一个 `QUdpSocket`（协议 §7），来源四元组路由、未配置来源整报丢弃并计数；open=bind 无握手、重复 open 幂等；失联不关绑定，close 只注销来源路由由共享引用计数决定真关；报文边界天然保留，一报一帧不跨报拼接）、`SerialTransport`（`QSerialPort` 115200/8N1；open 同步、opened/errorOccurred 在 open 调用栈内同步发出，会话槽容忍重入；错误二分（LLD §3）：权限/不支持 → CONFIG_INVALID 进 Fault，设备不在/拔出/打开失败 → CONNECTION_LOST 退避重试插回即恢复；参数打开后逐一应用并检查，失败明确报错不静默改默认）。会话层三形态语义：UDP 失联清 seq 基准、Reconnecting 期间收有效帧直接恢复、seq 模差判定（d=(new−last) mod 2³²：0< d <2³¹ 新帧、d=0 重复、d≥2³¹ 旧）防乱序/重复且不刷存活、UDP 禁命令；配置层放开 udp/serial（UDP 四元组唯一、串口占用唯一），`DeviceSetup.transport` variant 三形态 + `main.cpp` 装配。模拟器三形态：UDP 固定源端口一报一帧单发 DMS 共享端点、串口全命令集（disconnect 故障=关端口窗口结束重开）、数据报三故障（datagramDrop 整报丢弃/datagramReorder 相邻对调/datagramDuplicate 同报连发），故障×形态组合启动期校验（TCP 跳帧不是网络丢包、UDP 无连接不收命令）。15 个测试套件全部通过（14 单元 + 1 集成）：新增 tst_udptransport/tst_serialtransport、DeviceSession 8 个 UDP 链路语义用例，集成新增 UDP E2E（双设备共享绑定端点来源隔离、未知来源丢弃、非「恰好一帧」整报拒绝、数据报三故障真注入窗口断言）与串口虚拟串口对条件 E2E（无 com0com/socat 时 QSKIP）。**S8 跨平台验证与打包完成（2026-09-28）**：WSL2 Ubuntu 24.04（apt Qt 6.4.2/g++ 13.3/CMake 3.28.3/Ninja 1.11.1/系统 SQLite 3.45.1）Debug+Release 双配置构建、15 套件 CTest 全绿；TC-SERIAL-001 经 socat 虚拟串口对在 Linux 真跑通过（Windows 无 com0com 时仍按既有语义 QSKIP（Blocked））。跨平台验收暴露并修复五处环境依赖缺陷：MainWindow.cpp 缺 `<QtEndian>` 显式包含（MSVC/Qt6.8 传递包含掩盖，g++/Qt6.4.2 直接编译失败）；app.ini 键大小写契约改为装载器自建小写折叠表（QSettings INI 后端语义随 Qt 版本漂移：6.8.3 代为折叠、6.4.2 按原样匹配，白名单校验与取值同源）；db-path 拒绝绝对路径的测试改用平台相关绝对路径（`C:/…` 在 Linux 是相对路径）；TCP 背压上限测试对端 `setReadBufferSize` 封顶（QAbstractSocket 默认不限量、会把内核收包持续搬进用户态堆，Linux 回环内核缓冲自动调谐到 MB 级时 64KiB 上限永不触发，Windows 环回缓冲小属侥幸通过）；串口 E2E 端口检查加 devpts 回退（socat pty 无 sysfs 表示、不进 `QSerialPortInfo` 的 udev 枚举，按设备节点存在性判断）。发布工程：CMake `install()` 发布目录（bin/ + config 模板；运行期数据落 `AppLocalDataLocation`，安装目录可只读）、`scripts/build.sh`（Linux 一键构建+CTest，自动搭/收 socat 串口对）、`build.bat` 增 `[Debug|Release]` 双配置、`package.bat`（Release→windeployqt 独立包→zip，含 VC 运行库）与 `package.sh`（Release→前缀安装→tar.gz，系统 Qt 依赖声明）、部署冒烟脚本（Windows `deploy-smoke.bat`、Linux `deploy-smoke.sh` 支持 `--read-only` 安装目录验证）；DB 就绪日志携带 SQLite 引擎版本随每次启动留痕。Windows 发布包冒烟实测：发布目录启动打包模拟器+上位机，模板初始化、Qt 平台/SQL/串口插件加载、建库（Qt 6.8.3 内置 SQLite 3.49.1）全链通过。Qt 最低要求放宽为 6.4——同代码跨 Qt 6.4.2/6.8.3 双小版本构建并全量测试通过，本身是跨平台验收要覆盖的维度。环境版本矩阵、双平台 Debug/Release 全量证据与部署冒烟记录见 docs/16_S8_CROSS_PLATFORM_EVIDENCE.md；下文“支持”仍指目标功能，未实现部分不能作为已完成成果。

**代码审查修复（2026-10-30）**：对照 LLD/协议/数据库设计逐条核查实现，修复 10 类偏差——重连后 session_id 不轮换致遥测撞唯一键被静默丢弃（E2E 实测丢 40%）、人工断开/退避期间 DATA_STALE 误报、同步写失败时命令终态先于受理落库、DB 事件约束冲突毒丸永久堵死队列、退出排空空等、组帧单轮上限无续解与超时锚点错误等；新增 25 个先失败后通过的回归用例，Debug/Release 零警告、15 个套件全部通过。详见 [评审记录 §6](docs/14_DOCUMENT_REVIEW.md)。

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
15. [S8 跨平台构建与部署证据](docs/16_S8_CROSS_PLATFORM_EVIDENCE.md)：环境版本矩阵、TC-DEPLOY-001/002 与 TC-SERIAL-001 实测记录

## 开发原则
- 不复制大型开源项目后改名。
- 可阅读 Serial Studio、QGroundControl 等开源项目的架构与实现思路，但自己重新设计、编码和测试。
- 每个核心模块至少配一个测试或可验证的验收步骤。
- 每个阶段保留 Git commit 与 README/设计文档演进记录。
