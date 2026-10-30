# 04. 详细设计 LLD

> D1 · 2026-09-23 · 待实现。字节定义见 [协议](05_PROTOCOL_SPEC.md)，存储见 [数据库](06_DATABASE_DESIGN.md)，默认值见 [配置](15_CONFIGURATION.md)。

## 1. ITransport

对应 REQ-CONN-001、REQ-UDP-001、REQ-SERIAL-001。

异步接口：open(config)、close()、write(bytes, requestId)；信号 opened/closed/writeAccepted/errorOccurred。writeAccepted 只表示发送入队，设备执行成功需 ACK。isOpen 仅所属线程读取，GUI 用快照。

TCP/串口发 bytesReceived(bytes)；UDP 发 datagramReceived(bytes, senderAddress, senderPort)，保留来源和报文边界。UDP opened 仅表示 bind 成功。处理部分写入与异步失败；每设备发送待处理总量上限 64KiB，包含应用队列与底层 bytesToWrite，超限拒绝并报 SEND_QUEUE_FULL，不能只限制应用队列却无限填充 Qt 内部缓冲。每次重建 transport 使用 generation，旧回调不得修改新会话。

## 2. FrameDecoder / MessageCodec

对应 REQ-PROTO-001/002/003。流缓存每 Session 独立，UDP 不复用。

1. 有限分块读入，缓存≤64KiB，单轮最多处理 100 帧后让出事件循环，剩余缓存排队续解，不等下一块数据到达。
2. 找 AA 55；无 Magic 时仅保留末尾 AA，避免跨块 Magic 丢失。
3. 至少 18B 才读固定头；检查版本、payload_len≤4096，再计算总长 22+N。
4. 不足帧等待；从候选 Magic 出现起最多 2000ms，不随追加数据无限延长；前一候选完成或丢弃后，对新候选重新计时。
5. 头非法、CRC 错、组帧超时，从候选起点后一字节重新搜索，保留后续有效帧。
6. 合法帧完整消费；未知 Type 跳过并计数，不刷新存活时间；已知 Type 再校验载荷。
7. 缓存超限记录 RX_OVERFLOW 并按同步规则恢复。输出值对象，不暴露缓存内部地址。

所有外部长度先校验再读取，禁止未对齐字节强转 struct。载荷错误整帧丢弃，不先交付部分指标。

## 3. DeviceSession 状态与恢复

对应 REQ-CONN-002/003、REQ-UI-001。所有时间间隔用可注入的单调时钟。

| 事件 | 新状态 / 动作 |
|---|---|
| 人工连接，配置合法 | Connecting；新 generation，启用自动恢复 |
| 首条有效消息 | 四指标新鲜则 Online，否则 Degraded |
| 建链 3000ms 超时 / 打开后 4000ms 无首个有效帧 | Reconnecting，关闭旧 transport 并退避 |
| Online 下任一必需指标 3000ms 无更新 / 连续 5 个 CRC 错 | Degraded，触发对应告警，通信继续 |
| 四指标恢复新鲜且连续 3 个有效帧 | Degraded → Online |
| socket 错误 / 4000ms 无有效接收 | Reconnecting，取消未决命令并触发失联 |
| 重试到期 | Connecting；只允许一个在途尝试 |
| 人工断开 / 禁用 / 退出 | Disconnected，取消定时器和命令，不新建失联告警 |
| 配置错误 / 不可恢复本地错误 | Fault，停止重试等待修正 |

有效消息需来源正确、类型支持、CRC 与载荷通过。任意字节/坏 CRC/未知类型不能延长存活。lastValidReceive 与每指标 lastTelemetry 分离，心跳不刷新遥测时间。新会话将旧值标陈旧，未收到首个指标也从打开时刻计时。

默认心跳 1000ms，失联阈值 4000ms，检查 100ms；受控测试从模拟器停发到状态更新≤5s。重试基数 1、2、4、8、10、10…s，±10% 抖动后限制在 0.1–10s；稳定 Online 10s 才清零计数。稳定可达端点恢复后≤15s Online。

串口暂时拔出可恢复，权限/配置错误进入 Fault。UDP 保持绑定等待数据，Reconnecting 表示失联等待，不建立 TCP 式连接；收到新的有效遥测直接恢复。bind 错误独立处理。

## 4. 命令生命周期

对应 REQ-CMD-001/002。仅 TCP/串口 Online/Degraded 且可写时允许；每设备最多一个未决命令。键为 device_id + session_id + Seq + command_id；session_id 是本地每连接 UUID（TCP/串口每次打开、UDP 每次重建 seq 基准时轮换；设备在新连接上序号可能归零），不在帧头中。

- Sent：入发送队列开始 2000ms ACK 超时。受理事件在写 transport 之前发出，同步写失败产生的 Failed 也排在其后，落库不会先终态后受理。
- Succeeded/Rejected：当前会话 Seq、command_id 匹配有效 ACK。
- TimedOut：未按时收到 ACK，执行结果未知。
- Cancelled：人工断开/链路失联；记录原因。
- Failed：编码或本地发送失败。
- 迟到/重复 ACK 只记日志，不修改已结束结果；有副作用的命令不自动重发。

序号按方向递增，完整 32 位空间用尽前更换会话，避免旧 ACK 关联。设备针对同会话相同 Seq+内容的重包复用 ACK、不重复执行；同 Seq 不同内容拒绝。Reset 只复位模拟业务参数/计数，先 ACK 后复位，不重启协议连接。

S5 起先持久化 Pending 成功再发送；DB 不可受理则拒绝新命令。结果更新失败标“未持久化”；重启把遗留 Pending/Sent 标 Interrupted（未知结果）。

## 5. DeviceManager 与 UI

IO 线程的 DeviceManager 拥有 Session，负责配置快照、统一停止与路由。ID 稳定且不在线上帧内传递。GUI 每 100ms 合并显示，每设备每指标曲线≤600 点；长历史异步分页查询，过时 requestId 结果丢弃。原始采集不被 UI 降采样影响。

## 6. AlarmEngine

对应 REQ-ALARM-001/002；规则可脱离 GUI/DB 测试。

- value>high 或 value<low 触发，等于边界不触发；高值恢复至≤high-h，低值恢复至≥low+h，h≥0。
- 每 device_id/code/metric_id 最多一个活跃事件，持续异常不逐帧插入。
- acknowledged_at 与 cleared_at 独立；先恢复仍可确认；再次越界新建事件。
- 链路已建立（Online/Degraded）时，每指标 3000ms 无有效值产生 DATA_STALE，从链路建立时起算；Connecting/Reconnecting/Fault/人工断开期间不评估，链路问题由 COMM_LOST 表达。Stop 已 ACK 后暂停遥测陈旧检测，Start 或 Reset 已 ACK 后重新计时（Reset 按协议会启动遥测）。此时在线健康判定也免除被明确停止的遥测新鲜条件；人工断开后设备采集状态未知，会话与告警引擎都按“采集活跃”重新评估。
- 连续 5 个坏 CRC 触发 PROTOCOL_CRC；有效帧清零连续错误计数，连续 3 个有效帧解除该退化原因。
- COMM_LOST 在有效通信恢复时解除；人工断开关闭会话相关活跃告警，原因记 manual_disconnect。
- 非有限值在解码层拒绝；未配置阈值的指标不触发阈值告警。

## 7. DB 队列与退出

从 S5 起异步写，不在 GUI 同步插入。遥测队列≤50000 样本，500 样本或 1000ms 提交一次，先到者触发。告警/命令预留独立 1000 事件容量。

暂时性失败（锁等待超时、IO、磁盘满）回滚并保留批次，每 1s 重试，数量触发路径同样遵守该节奏；约束冲突（CHECK/UNIQUE/FK）是确定性失败，回滚后单条隔离丢弃并计数，其余重试，不让一条毒丸永久堵死队列。遥测队列满丢新到样本并累计 droppedSamples。事件队列满拒绝新控制并明确报告未持久化事件，不无限增长、不伪报成功。

退出先停生产，最多等待 5000ms 排空（仅失败后按 1s 节奏等待，积压多批时连续写出）；超时显示未提交数，让用户选择继续等待或取消退出。异步等待完成后在所属线程关闭资源，再 quit/finished；不强杀线程。崩溃可能损失内存待写批次，不能承诺断电零丢失。

## 8. 日志 / 界面 / 可选 AI

日志字段：UTC 时间、level、category、device_id、session_id、code、message。等级 DEBUG/INFO/WARN/ERROR；分类 APP/NET/SERIAL/PROTOCOL/DB/UI/TEST。

至少定义 CONFIG_INVALID、CONNECT_TIMEOUT、CONNECTION_LOST、FRAME_LENGTH、FRAME_CRC、FRAME_PAYLOAD、FRAME_TIMEOUT、RX_OVERFLOW、SEND_QUEUE_FULL、COMMAND_TIMEOUT、DB_WRITE_FAILED、DB_QUEUE_FULL、DB_SCHEMA_UNSUPPORTED。RX/TX 默认关闭，开启时每条≤256B 并标截断；10MiB/文件，最多 5 文件含当前文件；同类高频错误按 1s 聚合计数。

页面：Dashboard、Devices、Telemetry、Alarms、History、Diagnostics。History 支持查询/CSV；Diagnostics 展示错误、丢样本和重连计数。可选 ONNX score(features) 与规则告警解耦，模型缺失不影响必需功能。
