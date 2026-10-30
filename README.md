# DMS — Device Monitoring System

C++/Qt 多源设备监控、数据采集与联调平台。基于 C++17 与 Qt 6 的跨平台桌面客户端，统一接入 TCP / UDP / 串口设备，完成协议解析、会话状态管理、实时曲线、告警、SQLite 历史存储与日志诊断，并配套可复现故障注入的设备模拟器。

## 功能特性

- **多源接入**：TCP（建链超时、64 KiB 发送背压）、UDP（同一绑定端点共享 socket，按来源四元组路由，未配置来源整报丢弃）、串口（QSerialPort；权限/参数错误进入 Fault，拔出后退避重连、插回即恢复）。
- **二进制协议 v1**：18 字节大端固定头 + TLV 载荷 + CRC-32/ISO-HDLC。有界流式解码器处理半包、粘包与坏帧重同步；UDP 一报一帧，按序号模差丢弃旧帧与重复帧。
- **会话状态机**：Connecting / Online / Degraded / Reconnecting / Disconnected / Fault 六态；心跳与遥测双路存活判定，四个指标独立判断新鲜度；指数退避重连（1/2/4/8/10 s，±10% 抖动）。
- **命令生命周期**：Start / Stop / Reset / SetParameter；Sent → Succeeded / Rejected / TimedOut / Cancelled / Failed，ACK 按会话与序号精确关联，不自动重发。
- **告警引擎**：阈值越界（带迟滞）、数据陈旧、连续 CRC 错误、通信失联四类；同键去重，确认与恢复相互独立，历史全部保留。
- **持久化**：独立线程异步批量写入 SQLite（WAL）；队列有界，暂时性失败回滚重试，约束冲突单条隔离，退出时排空并报告未提交数。历史数据支持按接收时间分页查询与 CSV 导出（目前只有 Repository 层接口，界面未接入）。
- **桌面界面**：设备列表（状态色点、最近遥测时间）、四指标实时曲线（QPainter 自绘，100 ms 合并刷新）、告警面板与确认、Reset 与采样周期命令。
- **配置与日志**：`devices.json` + `app.ini` 全量校验，所有问题一次报全；结构化日志支持按大小滚动、同类错误 1 s 聚合、可选原始帧记录。
- **设备模拟器**：单进程模拟多台设备，支持三种传输形态，固定种子可复现；内置 11 种故障注入，覆盖坏 CRC、异常值、停发、断链、ACK 延迟/不回、分片、合帧，以及数据报丢弃/乱序/重复。

## 架构

```text
GUI 线程   MainWindow · DeviceListModel · CurveWidget
              ▲ queued 值对象信号              │ 命令 / 告警确认(queued)
IO 线程    DeviceManager ── DeviceSession ×N ── ITransport(TCP / UDP / 串口)
           AlarmEngine(与 DeviceManager 同线程直连)
              │ queued
DB 线程    DatabaseWorker ── SQLite(WAL)
```

| 目录 | 职责 |
|---|---|
| `src/common` | 日志与错误码 |
| `src/protocol` | 帧编解码、CRC、TLV 消息，不依赖网络与界面 |
| `src/transport` | `ITransport` 抽象及 TCP / UDP / 串口实现，只搬运字节 |
| `src/domain` | 会话状态机、命令跟踪、设备管理、告警引擎 |
| `src/persistence` | SQLite 连接、异步写入 worker、历史查询与导出 |
| `src/ui` | Qt Widgets 界面 |
| `src/app` | 配置加载与三线程装配，生成 `dms_monitor` |
| `simulator/` | 设备模拟器 `dms_simulator` |
| `tests/` | Qt Test 单元测试，以及拉起真实模拟器的集成测试 |

设计细节见 [架构设计](docs/03_HLD_ARCHITECTURE.md) 与 [详细设计](docs/04_DETAILED_DESIGN.md)。

## 构建

### 依赖

- C++17 编译器、CMake ≥ 3.21、Ninja
- Qt ≥ 6.4，需要 Core、Network、Widgets、Sql（含 SQLite 驱动）、SerialPort、Test 模块
- 已验证环境：Windows 10 + MSVC 2022 + Qt 6.8.3；Ubuntu 24.04（含 WSL2）+ GCC 13 + Qt 6.4.2

Ubuntu 安装依赖：

```bash
sudo apt install cmake ninja-build g++ qt6-base-dev qt6-serialport-dev libqt6sql6-sqlite
sudo apt install socat            # 可选：串口集成测试使用的虚拟串口对
sudo apt install fonts-noto-cjk   # 可选：系统缺少中文字体时，界面中文显示为方框
```

### 一键构建与测试

```bash
scripts/build.sh [Debug|Release]      # Linux：配置 → 编译 → 自动搭建 socat 串口对 → CTest
scripts\build.bat [Debug|Release]     # Windows：vcvars → 配置 → 编译 → CTest
```

- 构建目录：Linux 为 `build-debug` / `build-release`，Windows 为 `build` / `build-release`。
- Windows 脚本默认使用 `D:\VS2022BT`（VS Build Tools）与 `D:\Qt\6.8.3\msvc2022_64`，可通过环境变量 `DMS_VS_DIR`、`DMS_QT_DIR`、`DMS_QT_DIR_WIN` 覆盖。

也可以手动构建：

```bash
cmake -S . -B build-debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
```

### 打包

```bash
scripts/package.sh                               # Linux：Release → 安装目录 → dist/*.tar.gz(不含 Qt 库)
scripts\package.bat                              # Windows：Release → windeployqt 独立包 → dist\*.zip
scripts/deploy-smoke.sh <发布目录> [--read-only]  # 发布包冒烟(Windows 为 deploy-smoke.bat)
```

Linux 发布包的运行期依赖：

```bash
sudo apt install libqt6core6t64 libqt6gui6 libqt6widgets6 libqt6network6 \
                 libqt6serialport6 libqt6sql6 libqt6sql6-sqlite
```

## 运行

```bash
# 终端 1：启动 1 台 TCP 模拟设备(127.0.0.1:9001，遥测 100 ms，心跳 1 s)
build-debug/simulator/dms_simulator -c 1 -p 9001

# 终端 2：启动上位机
build-debug/src/app/dms_monitor                      # 运行根为系统 AppLocalDataLocation
build-debug/src/app/dms_monitor --config-dir ./run   # 或指定独立的运行根
```

首次启动时，程序把可执行文件旁 `config/` 下的模板复制到运行根的 `config/`，已存在的文件不会覆盖。默认模板只启用一台 TCP 设备 `sim-tcp-01`（127.0.0.1:9001）。要接入更多设备，修改运行根下的 `config/devices.json`。日志写入运行根的 `logs/`，数据库默认为 `data/dms.db`。

模拟器常用参数（完整列表见 `dms_simulator --help`）：

| 参数 | 说明 |
|---|---|
| `-c, --count <n>` | 设备数量，默认 10 |
| `-t, --transport tcp\|udp\|serial` | 传输形态，默认 tcp |
| `-p, --port <port>` | 起始端口：TCP 为监听端口，UDP 为固定源端口；默认 9001 |
| `--peer <host:port>` | UDP 发送目标，即上位机的绑定端点；udp 形态必填 |
| `--serial-ports <list>` | 串口名列表，以逗号分隔；serial 形态必填 |
| `-s, --period <ms>` | 遥测周期，100–1000，默认 100 |
| `-d, --seed <n>` | 随机种子，决定数据与故障注入序列，默认 42 |
| `-f, --fault <mode@startMs+durationMs>` | 故障注入，可重复，例如 `-f badCrc@2000+3000` |
| `-r, --run-seconds <s>` | 运行时长，0 表示直到手动停止 |

故障模式：`badCrc`、`abnormalValue`、`stopSending`、`disconnect`、`ackDelay`、`ackNoReply`、`fragment`、`merge`、`datagramDrop`、`datagramReorder`、`datagramDuplicate`。其中 `fragment`、`merge` 只用于 TCP/串口；三种 `datagram*` 只用于 UDP；`disconnect`、`ackDelay`、`ackNoReply` 不适用于 UDP。

## 测试

- CTest 共 15 个套件：14 个单元测试，加 1 个集成测试。集成测试用 QProcess 拉起真实的 `dms_simulator`，覆盖 TCP / UDP / 串口全链路、故障注入和 SQLite 落库读回。
- 串口集成测试需要虚拟串口对，通过 `DMS_TEST_SERIAL_PAIR="<模拟器端>,<上位机端>"` 指定。Linux 下 `scripts/build.sh` 会自动用 socat 搭建，Windows 下可用 com0com。未设置时该用例跳过。
- 用例编号与通过条件见 [测试计划](docs/07_TEST_PLAN.md)，需求与测试的对应关系见 [追踪矩阵](templates/TRACEABILITY_MATRIX.csv)。

## 配置

运行根下有两份配置：
- `config/devices.json`：设备清单，包括 tcp / udp / serial 三种连接参数和阈值告警规则；
- `config/app.ini`：超时、缓存、数据库、日志与界面参数。

所有字段、默认值与校验规则见 [配置契约](docs/15_CONFIGURATION.md)。

## 文档

| 文档 | 内容 |
|---|---|
| [01 PRD](docs/01_PRD.md) | 产品需求、范围与实施阶段 |
| [02 SRS](docs/02_SRS.md) | 需求规格与验收条件 |
| [03 架构设计](docs/03_HLD_ARCHITECTURE.md) | 分层、线程模型与退出顺序 |
| [04 详细设计](docs/04_DETAILED_DESIGN.md) | 传输、解码、状态机、命令、告警、存储 |
| [05 通信协议](docs/05_PROTOCOL_SPEC.md) | 协议 v1 帧格式、消息与测试向量 |
| [06 数据库设计](docs/06_DATABASE_DESIGN.md) | SQLite schema、写入与查询策略 |
| [07 测试计划](docs/07_TEST_PLAN.md) | 单元 / 集成 / 配置项测试用例 |
| [08 质量流程](docs/08_QUALITY_PROCESS.md) | 版本控制、需求追踪与发布检查 |
| [09 用户手册](docs/09_USER_MANUAL.md) | 操作步骤与故障排查 |
| [14 评审记录](docs/14_DOCUMENT_REVIEW.md) | 文档评审与代码审查记录 |
| [15 配置契约](docs/15_CONFIGURATION.md) | 运行目录、JSON / INI 字段与校验 |
| [16 跨平台验证](docs/16_S8_CROSS_PLATFORM_EVIDENCE.md) | 环境版本矩阵与构建、部署实测记录 |

`AGENTS.md` 与 `CLAUDE.md` 是给 AI 编程助手的开发约束。

## 开发状态

| 阶段 | 内容 | 状态 |
|---|---|---|
| S1 | CMake 工程、帧编解码、CRC、TLV 消息 | 完成 |
| S2 | TCP 传输、结构化日志、TCP 模拟器 | 完成 |
| S3 | 会话状态机、心跳与重连、命令生命周期 | 完成 |
| S4 | 配置装配、设备管理、界面与实时曲线 | 完成 |
| S5 | SQLite 异步存储、历史查询与 CSV 导出 | 完成 |
| S6 | 告警引擎与告警面板 | 完成 |
| S7 | UDP / 串口传输及对应模拟与故障注入 | 完成 |
| S8 | Windows / Ubuntu 双平台构建、打包与部署验证 | 完成 |

2026-10-30 完成一轮代码审查：修复 10 类实现与设计不一致的问题，补充 25 个回归测试，详见 [评审记录 §6](docs/14_DOCUMENT_REVIEW.md)。

待完成：
- 10 设备标准负载下的 30 分钟性能测试（TC-PERF-001）与 8 小时稳定性测试（TC-STABILITY-001）；
- 历史查询与导出的界面入口；
- Diagnostics 页面；
- 在界面内编辑设备配置（目前修改配置后需重启）。

可选扩展：历史回放、UDP 广播发现、STM32/ESP32 实物联调、ONNX 异常检测、Modbus。

## 开发约定

- **分层依赖**：ui → app → domain → transport / protocol，persistence 只依赖领域值类型。业务逻辑不写进 MainWindow。
- **线程**：QWidget 只在 GUI 线程访问；transport、会话、定时器与 SQL 连接在所属线程中创建、使用和销毁；跨线程只传值对象。
- **健壮性**：外部字节流先校验长度再解析；网络与数据库错误转为结构化错误码，并写入日志。
- **同步要求**：修改协议须同步 `docs/05_PROTOCOL_SPEC.md`，修改数据库须同步 `docs/06_DATABASE_DESIGN.md`；核心逻辑须附带测试。
- **提交信息**：使用 `feat` / `fix` / `test` / `docs` / `refactor` / `build` 前缀。
