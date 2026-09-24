# CLAUDE.md — Claude Code 开发约束

## 工作方式

每个任务执行前：

1. 说明将修改哪些模块。
2. 指出涉及的 C++/Qt 知识点。
3. 优先给出最小可验证实现。
4. 完成后给出编译/测试命令。

## 技术标准

- C++17
- Qt6 Widgets
- CMake target-based 写法
- RAII
- smart pointers only where ownership requires them
- QObject ownership遵循 Qt parent-child 机制，不机械套 shared_ptr
- 不跨线程直接操作 QWidget
- 协议解析必须防御非法长度
- 网络错误必须转成结构化错误信息和日志

## 代码风格

- 类型 PascalCase
- 函数/变量 camelCase
- 成员变量 `_` 后缀或项目统一风格
- 一个类一个主要职责
- 函数尽量短
- 注释解释 why，不重复代码 what

## 测试标准

优先测试：

- FrameDecoder 半包/粘包/错 CRC
- 状态机
- AlarmEngine
- Repository
- 重连策略可测试部分
