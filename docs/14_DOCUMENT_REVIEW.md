# 14. 技术文档评审与修改记录

> 评审开始：2026-09-23；完成核验：2026-09-24。范围：README、设计文档与追踪矩阵；补充本记录及配置契约。应用尚未实现。

## 1. 结论与范围确认

初版文档覆盖了需求、架构、协议、数据库和测试；直接据此编码仍存在接口歧义、相互冲突和无法量化验收的问题。本轮保持 C++17/Qt6/CMake 与分层架构，确认**保留全部必需功能，按阶段完善**。UDP/串口没有被降为可选。

评审时仓库只有文档与追踪矩阵，没有 C++ 源码、CMakeLists.txt、测试可执行文件或数据库；本轮只修改文档，AGENTS.md 与 CLAUDE.md 保持原样。

## 2. 发现的问题与处理

严重程度指“照此开发可能产生的影响”，不表示已运行软件存在这些缺陷。

| 优先级 | 原问题 | 修订及位置 |
|---|---|---|
| 高 | 心跳 2s×3 周期与 5s 内发现断线矛盾 | 统一 1000ms 心跳、4000ms 失联、100ms 检查；区分陈旧与失联；SRS/LLD/协议/配置 |
| 高 | PRD 状态缺 Reconnecting，架构图缺 Fault/人工停止路径 | 六状态枚举、事件转换、人工断开取消重试；HLD/LLD/手册 |
| 高 | CRC 仅叫 CRC32，命令/TLV 无明确长度和语义 | 固定端序、偏移、CRC 参数/覆盖区、TLV、四命令/ACK、两个完整向量；协议 |
| 高 | 流解析可能无限等帧/累积，UDP 没有来源和报文边界 | 长度先验、有界缓存/超时、恢复算法；一报一帧、来源路由、重复/乱序；协议/LLD |
| 高 | worker/SQL 线程和退出只给原则；允许 MVP 同步写 | 明确对象归属、异步批量、排队信号的容量风险、退出顺序；HLD/LLD/数据库 |
| 高 | DB 仅列字段，无约束、版本、失败策略或命令唯一键 | 可执行 DDL、外键/索引/状态约束、session_id、迁移/回滚、损失可见；数据库 |
| 中 | ACK 超时可能被理解为设备没执行，重复/迟到关联不清 | 命令结果未知、不自动重发、精确关联和去重；协议/LLD |
| 中 | 3 设备/30min 与 10 设备/8h 目标混用 | 区分阶段冒烟和最终验收；PRD/测试/计划 |
| 中 | “无明显卡顿/内存增长”不可复现；20 个测试只有少量例子 | 定义负载、探针、内存采样、数据核对和用例目录；SRS/测试 |
| 中 | 矩阵漏需求、测试映射错误或缩写 | 保留原 ID/六列，补齐全部 29 个 SRS 需求和完整测试 ID，状态均 Planned |
| 中 | JSON/INI、数据库设备配置两套来源未明确 | 连接配置单一来源、原子保存、字段/路径校验、默认值；新增配置契约 |
| 中 | 告警没有确认/恢复/去重定义 | 独立时间、迟滞、活跃唯一键、重启处理；LLD/数据库 |
| 中 | 手册假定程序已存在，实施顺序与项目执行说明不一致 | 标设计态；按 S1–S8 重排，测试随模块；手册/PRD/README |

## 3. 本轮新增设计默认值

协议 v1 采用 CRC-32/ISO-HDLC、TLV 和明确的命令参数；DEVICE_INFO/ERROR_REPORT 原来只有名称，暂保留类型号、不列为必需消息。模拟器控制仅 Start/Stop/Reset/采样周期。设备 ID 由端点配置关联，不擅自增加帧头字段。

线程采用 GUI+共享 IO+DB worker；存储全量四指标，队列/刷新/日志有界；配置 JSON 是连接源、SQLite 为历史镜像。这些是供后续实现的设计选择，不是对现存代码的描述。

Qt 精确版本、编译器、绘图组件、安装方式、真实串口硬件仍待实施环境确认，未虚构“已安装/可构建”。性能和内存阈值需实际验证；修改时按质量流程记录理由。

## 4. 文档验证记录

2026-09-24 校验通过：

- 当时全部 Markdown 文档的 UTF-8、代码围栏和本地链接检查通过。
- 29 条 SRS 需求唯一且全部进入 CSV；49 个测试用例编号唯一、引用有效并全部映射，其中 44 个计划自动化、5 个人工/环境验收。这里是用例数量，**不是通过数量**。
- 两个完整帧的长度/端序/载荷通过检查；按位 CRC 实现与 Python zlib 对标准检查串、空串及帧尾 CRC 一致。
- 在 Python SQLite 3.40.1 的临时内存数据库执行 DDL 成功；四表、外键、唯一键、告警/命令约束、半开区间查询、批事务回滚、DDL+版本回滚及 integrity_check 通过。内存库没有验证 WAL 文件行为、Qt SQL 驱动、磁盘满或真实并发。
- JSON/INI 示例可解析，三种传输、默认心跳/失联和队列关系检查通过；这不代表应用 ConfigValidator 已实现。
- 本轮修改既有文档并新增配置契约与本记录；未创建业务代码、数据库、Git 提交或标签。

临时校验脚本共执行 193 项结构/示例断言，通过后仍人工复核了范围与文字一致性。产品 Qt Test、CTest、编译、GUI 和跨平台运行当前均 **Not Run**。本轮验证不构成 30min/8h 产品验收结果。

## 5. 参考依据

已对照 Qt 官方的 [对象与线程](https://doc.qt.io/qt-6/threads-qobject.html)、[QThread](https://doc.qt.io/qt-6/qthread.html)、[QSqlDatabase](https://doc.qt.io/qt-6/qsqldatabase.html) 以及 SQLite 官方的 [外键](https://www.sqlite.org/foreignkeys.html)、[PRAGMA](https://www.sqlite.org/pragma.html)。具体链接也写入相关设计章节。引用用于核对 API 行为；项目参数和验收门槛为本轮设计。

## 6. 代码审查记录（2026-10-30）

范围：src/ 全部模块、simulator/、tests/，对照 LLD/协议/数据库设计逐条核对实现。每项先写出在原代码上失败的测试，再修复；原 15 个套件保持全部通过。

| 优先级 | 问题（实现与设计的偏差） | 影响 | 修复及回归测试 |
|---|---|---|---|
| 高 | session_id 只在 DeviceSession 构造时生成，违背 docs/06 §3“每连接新 UUID” | 设备在新连接上序号归零，重连后遥测撞 (device,session,seq,metric) 唯一键被 DO NOTHING 静默丢弃；E2E 实测 100 个样本只入库 60 个 | TCP/串口每次打开、UDP 每次重建 seq 基准轮换；命令终态用受理时 ID。`sessionIdRotatesPerConnection`、`udpSessionIdRotatesWhenSeqBaseRebuilt`、集成 `persistenceSurvivesReconnect` |
| 高 | AlarmEngine 对所有设备评估 DATA_STALE，不看链路状态 | 人工断开 3s 后对已断开设备重新触发 4 条 DATA_STALE；退避 ≥4s 时重连期间误报；建链耗时被计入陈旧窗口 | 仅 Online/Degraded 评估，链路建立时起算。`stale_notReevaluatedAfterManualDisconnect` 等 3 例 |
| 中 | 本地写失败在 sendCommand 栈内同步终结为 Failed，而受理记录在调用返回后才发出 | DB 先执行终态 UPDATE（未命中）再插入 Sent 行，该行永久停在 Sent | 受理记录随 commandSent 在写 transport 前发出。`submissionPrecedesSynchronousFailure` |
| 中 | DB 事件“毒丸”：ACK status 越界（协议只定义 0–4）、墙钟回拨使 cleared_at/acknowledged_at < ts、丢失恢复事件后同键再触发撞部分唯一索引 | 约束失败整批回滚、永久重试，事件队列堵死，之后全部告警/命令记录丢失 | 协议层拒收越界 status；告警时间 MAX(..,ts)；约束冲突（QSqlError 原生码低 8 位=19）单条隔离计数。`alarmTimestampsClampedOnClockRollback`、`constraintViolationIsolatedNotRetriedForever`、`ackUndefinedStatusRejected` 等 |
| 中 | drainAndClose 每个成功批次后也按 flush 间隔休眠 | 积压多于一批时，退出预算内本可提交的样本被报为未提交 | 仅失败后等待。`drainCommitsBacklogWithoutSleeping` |
| 中 | 单轮 100 帧上限后无人续解；组帧计时锚点从不随新候选重置；组帧超时清空整个缓存（LLD §2.1/§2.4/§2.5） | 突发帧滞留直至下一块数据；背靠背连续流被误判超时丢帧；排在假候选后的有效心跳被丢弃 | 排队续解、头部前进即重新计时、`skipStalledCandidate()` 从候选后一字节重同步。`roundLimitBacklogDrainedWithoutNewData` 等 3 例 + 解码器 2 例 |
| 低 | Reset ACK 未恢复陈旧检测（协议 §4.2 Reset 启动遥测，TC-ALARM-004）；人工断开后会话保留 Stop 状态而告警引擎复位 | 会话显示 Online 而引擎报 DATA_STALE；Reset 后遥测再停也不告警 | 会话与引擎同口径。`resetAckResumesStaleWindow`、`stale_resumedByResetAck`、`manualReconnectResetsAcquisitionState` |
| 低 | 坏 CRC 帧未打断 Degraded→Online 的“连续 3 个有效帧”计数；closeByUs_ 标记可能残留到下一轮连接 | 交错坏帧下提前恢复 Online；残留标记会吞掉下一次真实对端关闭（依赖 transport 先报错的时序才未暴露） | 坏帧清零好帧计数；标记只覆盖 close() 调用期间。`degradedRecoveryNeedsConsecutiveGoodFrames`、`peerCloseAfterReconnectStillDetected` |
| 低 | 数量触发的遥测 flush 失败后每来一帧都立即重试 | 锁被长期占用时每帧阻塞 busy_timeout，DB 线程跨线程事件队列无界堆积 | 失败后按 flush_interval 节奏重试。`countTriggerRetriesAtCadenceAfterFailure` |
| 低 | 命令参数值 >255B 时 TLV len 被静默截断；TelemetryBuffer 裸 new 不释放；设备列表状态色/最近遥测不显示（手册 §0）；曲线顶部刻度压住标题 | 畸形帧；RAII 违例；UI 与文档不符 | 编码失败走 Failed（`commandOversizedParamRejected`、`commandEncodeFailureFinishesFailed`）；值语义存储；DecorationRole 色点 + 文字；离屏截图核对 |

验证：Ubuntu 24.04 / Qt 6.4.2 / GCC 13.3，Debug 与 Release 均以 `-Wall -Wextra -Wpedantic` 零警告编译；CTest 15 个套件全部通过；串口 E2E（TC-SERIAL-001）另以 socat 虚拟串口对在两种构建下通过。UI 改动经离屏渲染截图核对。Windows/Qt 6.8.3 侧本轮未复测。

本轮识别但未修改（改动面或风险大于当前收益，或属功能增强），建议后续处理：

- TCP/串口 `readAll()` 读取不设上限，单次突发超过 64KiB 时解码器整体丢弃（LLD §2.1“有限分块读入”只落实了缓存上限）；需 `setReadBufferSize` + 分块读取形成背压。
- 退出排空超时只记日志，LLD §7 要求的“显示未提交数、由用户选择继续等待或取消退出”未实现；`ioThread.wait(5000)` 超时后析构仍在运行的 QThread 会中止进程。
- UDP 未知来源丢报的 WARN 日志不带错误码，不进入 1s 聚合，配置错误的发送端可造成日志刷屏。
- app.ini 未检查 `QSettings::status()`，格式错误时报“缺失必填键”；UDP 四元组唯一性按原始字符串比较（`::1` 与 `0:0:0:0:0:0:0:1` 视为不同，运行期路由冲突检查兜底）。
