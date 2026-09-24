# DMS — Device Monitoring System

## 当前状态与阅读顺序

截至本轮文档核查，当前目录是**需求/设计/测试规划包**，尚无 C++ 源码、CMake 工程、测试程序或可执行发布包，也未初始化 Git。下文“支持”均指目标功能，不能作为已经完成的成果。

文档基线 D1（2026-09-23 起）；本轮仅完善文档与追踪模板。先读 [评审记录](docs/14_DOCUMENT_REVIEW.md)，再按 [PRD](docs/01_PRD.md) → [SRS](docs/02_SRS.md) → [架构](docs/03_HLD_ARCHITECTURE.md) → [详细设计](docs/04_DETAILED_DESIGN.md) 阅读。实施前对照 [协议](docs/05_PROTOCOL_SPEC.md)、[数据库](docs/06_DATABASE_DESIGN.md)、[配置](docs/15_CONFIGURATION.md) 和 [测试计划](docs/07_TEST_PLAN.md)。

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
- 实时曲线后端：在 S4 按可用 Qt 组件选定并记录；保留 Qt Charts/简单绘图候选，新增第三方库前评估维护与许可
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
