# AGENTS.md — Codex 项目执行说明

## 项目目标

实现 `C++/Qt 多源设备监控、数据采集与联调平台`。

## 技术约束

- C++17
- Qt6 Widgets
- CMake
- Qt Network
- Qt SerialPort
- Qt SQL / SQLite
- Qt Test + CTest
- Windows + Ubuntu

## 架构约束

- 禁止把业务逻辑堆在 MainWindow。
- transport/protocol/domain/persistence/ui 分层。
- QWidget 只能在 GUI 线程访问。
- 网络/串口工作对象采用 QObject + moveToThread 的 worker-object 模式优先。
- 资源优先 RAII，避免裸 new/delete。
- 任何外部字节流必须先长度校验再解析。

## 实施顺序

1. 创建 CMake 工程和目录。
2. 实现 protocol Frame/Codec/CRC + 单测。
3. 实现 TCP simulator。
4. 实现 TcpTransport + DeviceSession。
5. 实现设备状态机、心跳、重连。
6. 实现最小 Qt UI。
7. SQLite。
8. 告警。
9. UDP/Serial。
10. 集成测试和 Linux 构建。

## 每次修改要求

- 先读相关 `docs/`。
- 不擅自扩展非 PRD 功能。
- 核心逻辑新增测试。
- 修改协议必须同步 `docs/05_PROTOCOL_SPEC.md`。
- 修改数据库必须同步 `docs/06_DATABASE_DESIGN.md`。
- 每个阶段保持可编译。

## 禁止事项

- 不引入与 MVP 无关的复杂框架。
- 不为了炫技写模板元编程。
- 不把 AI 生成的未理解代码直接合并。
- 不声称通过任何质量体系认证。

## Definition of Done

一个功能完成必须满足：

- 编译通过
- 基本异常处理
- 日志
- 测试或明确的人工验收步骤
- 文档同步
