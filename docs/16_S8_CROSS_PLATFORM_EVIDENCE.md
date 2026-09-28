# 16. S8 跨平台构建与部署证据

记录日期：2026-09-28。源码提交：`4c69f14`（跨平台缺陷修复）+ `14e9ad3`（发布工程），基于 S1–S7 全量代码。证据规则：只记当次实测的命令、退出码与结果，不预设、不跨次搬运；通过即通过，跳过写明原因。

测试计划（docs/07 §1）要求记录环境版本；本文件同时作为 TC-DEPLOY-001/002 与 TC-SERIAL-001（Ubuntu 侧）的执行记录。

## 1. 环境矩阵

| 记录项 | Windows（开发机） | Ubuntu（WSL2 验收机） |
|---|---|---|
| OS | Windows 10 Home China 10.0.19045 | Ubuntu 24.04.4 LTS（WSL2，systemd 启用） |
| 编译器 | MSVC v143 14.44.35207（VS Build Tools 2022 17.14.41） | g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 |
| CMake | VS 自带 3.30.x（package.bat 调用，见 §2 命令行） | 3.28.3 |
| Ninja | VS 自带 1.11.x | 1.11.1 |
| Qt | 6.8.3 msvc2022_64（官方安装，含 SerialPort/Test/Sql 模块） | 6.4.2+dfsg-21.1build5（apt qt6-base-dev；qt6-serialport-dev 6.4.2-4build2） |
| SerialPort 模块 | Qt 6.8.3 官方包 | qt6-serialport-dev 6.4.2-4build2 |
| SQLite | Qt 内置 QSQLITE 驱动（引擎版本随启动日志留痕，见 §3） | 系统库 libsqlite3-0 3.45.1-1ubuntu2.8（libqt6sql6-sqlite 驱动），sqlite3 CLI 3.45.1 |
| 虚拟串口 | com0com（本机未装，Windows 侧 TC-SERIAL-001 QSKIP（Blocked）） | socat 1.8.0.0 |
| C 运行时 | ucrt/vcruntime（windeployqt --compiler-runtime 随包分发） | glibc 2.39 |

Ubuntu 部署机运行期依赖（apt）：`libqt6core6t64 libqt6gui6 libqt6widgets6 libqt6network6 libqt6serialport6 libqt6sql6 libqt6sql6-sqlite`（Linux 包不带 Qt 库，干净环境按此清单安装；构建机另需 `qt6-base-dev qt6-serialport-dev ninja-build cmake g++`）。

## 2. TC-DEPLOY-001：双平台 Debug+Release 构建与 CTest

命令均为仓库根目录执行；退出码取当次实测。

| 平台/配置 | 命令 | 结果 | 退出码 |
|---|---|---|---|
| Windows / Debug | `scripts\build.bat` | 15 套件全部通过（Total Test time = 59.92 s） | 0 |
| Windows / Release | `scripts\build.bat Release` | 15 套件全部通过（Total Test time = 69.19 s） | 0 |
| Ubuntu / Debug | `scripts/build.sh Debug` | 15 套件全部通过（Total Test time = 59.95 s；含 socat 串口对自动搭建） | 0 |
| Ubuntu / Release | `scripts/build.sh Release` | 15 套件全部通过（Total Test time = 63.90 s；串口 E2E 经 socat 真跑，见 §4） | 0 |

15 套件 = 14 单元（FrameDecoder/MessageCodec/Crc32→合并套件口径以 ctest 输出为准）+ 1 集成（QProcess 拉起真实模拟器）。Windows 侧串口 E2E 无 com0com 时按既有语义 QSKIP（Blocked），Ubuntu 侧经 socat 真跑（§4）。

## 3. TC-DEPLOY-002：发布包与发布目录

### Windows 包（windeployqt 独立包）

构建：`scripts\package.bat` = Release 构建 → `cmake --install --prefix dist\dms-win64-x64` → `windeployqt --release --compiler-runtime --no-translations` → `Compress-Archive` 出 zip。

当次产物（2026-09-28，源码提交 `14e9ad3`）：

- 发布目录 `dist\dms-win64-x64\bin\`：`dms_monitor.exe`、`dms_simulator.exe`、`config\`（devices.json + app.ini 模板）
- Qt DLL（windeployqt 按可执行链接的模块选取）：`Qt6Core.dll Qt6Gui.dll Qt6Network.dll Qt6SerialPort.dll Qt6Sql.dll Qt6Svg.dll Qt6Widgets.dll`
- 插件目录：`platforms\`、`sqldrivers\`（qsqlite.dll）、`styles\`、`tls\`、`imageformats\`、`iconengines\`、`generic\`、`networkinformation\`
- 运行时辅助：`opengl32sw.dll`、`d3dcompiler_47.dll`、`dxcompiler.dll`、`dxil.dll`、`vc_redist.x64.exe`（`--compiler-runtime`，干净机器免装 redist）
- 归档：`dist\dms-win64-x64-14e9ad3.zip`（约 53 MiB）

冒烟（发布目录直接启动，`--config-dir` 指定独立运行根，不污染用户配置）：

命令：`scripts\deploy-smoke.bat dist\dms-win64-x64`（当次实测，退出码 0）。发布目录模拟器拉起 10 台 TCP 9001–9010，SIM READY 后启动上位机（`--config-dir` 指向独立运行根），驻留 10 s 后回收。当次关键输出：

```text
SIM READY 9001 9002 9003 9004 9005 9006 9007 9008 9009 9010
INFO  NET sim-tcp-01 - - client connected from 127.0.0.1:63983
INFO  APP - - - stats: devices=10 framesSent=1007 samplesSent=3640
===== DB-ready log line =====
2026-09-28T14:28:11.539Z INFO  DB - - - 数据库就绪:D:/AIData/ClaudeCodeTemp/dms-deploy-smoke/data/dms.db(SQLite 3.49.1,批 500/队列 50000/事件 1000)
[smoke] release package runs: simulator + monitor + SQLite ready
```

运行根结构（dir /s /b）：`config\app.ini` + `config\devices.json`（模板初始化）、`data\dms.db` + `dms.db-shm` + `dms.db-wal`（建库 + WAL 生效）、`logs\dms.log`。模板 devices.json 仅 1 台 tcp 设备，sim-tcp-01 一台连接即预期行为。SQLite 3.49.1 为 Qt 6.8.3 内置引擎版本，随每次启动日志留痕。

### Ubuntu 包（前缀安装 + tar.gz）

构建：`scripts/package.sh` = Release 构建 → `cmake --install --prefix dist/dms-linux-x86_64` → tar.gz。包内 `bin/dms_monitor`、`bin/dms_simulator`、`bin/config/`（模板）。

当次产物（2026-09-28）：发布目录 `dist/dms-linux-x86_64/`，归档 `dist/dms-linux-x86_64-14e9ad3-20260928.tar.gz`（约 370 KiB——Linux 包不带 Qt 库，运行期依赖按 §1 清单安装）。包内容（`tar -tzf` 当次实测）：

```text
dms-linux-x86_64/bin/dms_monitor
dms-linux-x86_64/bin/dms_simulator
dms-linux-x86_64/bin/config/app.ini
dms-linux-x86_64/bin/config/devices.json
```

归档名中的 `14e9ad3` 为实际源码提交：打包在 WSL 验收 rig（源码 tar 覆盖、不含 .git）内进行，脚本取不到提交号时回落 `uncommitted`，已按实际源码提交重命名归档保持可追溯。

冒烟（Windows 10 无 WSLg，GUI 经 `QT_QPA_PLATFORM=offscreen` 验证启动/配置初始化/插件加载/建库；交互演示以 Windows 包为准）：

命令：`scripts/deploy-smoke.sh dist/dms-linux-x86_64`（当次实测，退出码 0）。offscreen 平台上位机 + 发布目录模拟器，驻留 10 s。当次关键输出：

```text
===== 运行根结构(模板初始化 + 运行期数据) =====
/tmp/dms-deploy.8iV6Az/config/app.ini
/tmp/dms-deploy.8iV6Az/config/devices.json
/tmp/dms-deploy.8iV6Az/data/dms.db
/tmp/dms-deploy.8iV6Az/data/dms.db-shm
/tmp/dms-deploy.8iV6Az/data/dms.db-wal
/tmp/dms-deploy.8iV6Az/logs/dms.log
===== DB 就绪日志(含 SQLite 引擎版本) =====
2026-09-28T14:23:03.910Z INFO  DB - - - 数据库就绪:/tmp/dms-deploy.8iV6Az/data/dms.db(SQLite 3.45.1,批 500/队列 50000/事件 1000)
===== 遥测落库行数 =====
356
===== 冒烟结果 =====
[smoke] 发布包全链路 OK:模拟器→上位机→SQLite
```

遥测行数经 sqlite3 CLI 实查 `SELECT count(*) FROM telemetry`。Linux 侧引擎为系统 SQLite 3.45.1（libqt6sql6-sqlite 驱动），与 Windows 侧 Qt 内置 3.49.1 的版本差异如实留痕、互为证据。

只读安装目录检查（`chmod -R a-w` 后启动，运行期数据全部落运行根）：

命令：`scripts/deploy-smoke.sh dist/dms-linux-x86_64 --read-only`（当次实测，退出码 0）：

```text
[smoke] 安装目录已整体去写权: dist/dms-linux-x86_64
===== 遥测落库行数 =====
312
===== 冒烟结果 =====
[smoke] 发布包全链路 OK:模拟器→上位机→SQLite
```

`chmod -R a-w` 整体去写权后，模板初始化、建库（dms.db + WAL/shm）、日志、遥测落库（312 行）全部落在运行根——运行期数据零依赖安装目录可写，安装目录可按只读部署，退出时脚本恢复写权。

## 4. TC-SERIAL-001（Ubuntu，socat 虚拟串口对）

命令：`scripts/serial-smoke.sh build-release`（单独跑 serialLinkEndToEnd，输出 PASS/SKIP/Totals 作证据）。

当次输出（Release 构建，退出码 0）：

```text
PASS   : TestIntegration::initTestCase()
PASS   : TestIntegration::serialLinkEndToEnd()
PASS   : TestIntegration::cleanupTestCase()
Totals: 3 passed, 0 failed, 0 skipped, 0 blacklisted, 1158ms
```

socat pty 对由脚本自动搭建并经 `DMS_TEST_SERIAL_PAIR` 注入；Debug 构建下同用例亦真跑通过（3 passed / 0 skipped / 1040ms），两种构建状态一致、零跳过。

Windows 侧无 com0com 时同用例 QSKIP（Blocked）——两侧状态如实分开记录。

## 5. S8 发现并修复的跨平台缺陷

| # | 位置 | 根因 | 修复 |
|---|---|---|---|
| 1 | src/ui/MainWindow.cpp `qToBigEndian` | MSVC/Qt6.8 的传递包含掩盖了缺 `<QtEndian>` | 显式包含 |
| 2 | src/app/Config.cpp app.ini 键大小写 | QSettings INI 后端语义随 Qt 版本漂移（6.8.3 代为折叠、6.4.2 按原样匹配），契约被后端实现劫持 | 装载器自建小写折叠表，白名单校验与取值同源 |
| 3 | tst_config db-path 用例 | 硬编码 `C:/…`——在 Linux 是相对路径，"拒绝绝对路径"断言失效 | `QDir::temp().absoluteFilePath` 取平台相关绝对路径（正斜杠，无 INI 转义） |
| 4 | tst_tcptransport 背压用例 | QAbstractSocket 默认 readBufferSize=0（不限量），持续把内核收包搬进用户态堆，发送侧内核缓冲永远清空；Linux 回环缓冲自动调谐到 MB 级时 64KiB 上限永不触发（Windows 环回缓冲小属侥幸通过） | 对端 `setReadBufferSize(4096)` 封顶，上限触发变为跨平台确定性 |
| 5 | tst_integration 串口存在性检查 | socat pty 属 devpts、无 sysfs 表示，不进 QSerialPortInfo 的 udev 枚举 | Linux 回退按设备节点存在性判断；Windows COM 名不走 /dev，回退天然不生效 |

## 6. 已知限制与声明

- Qt 双版本声明：Windows 6.8.3 / Ubuntu 6.4.2。同代码跨两个小版本构建并全量测试通过；已知的后端行为差异（INI 键大小写）已在装载器内消除（§5 #2）。
- Windows 10 无 WSLg：Ubuntu 侧 GUI 冒烟为 offscreen 平台，验证启动、配置初始化、SQL/平台/串口插件加载与建库，不含交互演示；GUI 完整验收以 Windows 包为准。
- socat pty 不是物理串口：寄存器级时序（波特率/流控）在 pty 上为无操作，TC-SERIAL-001 验证的是帧收发/命令 ACK 全链路与错误归因，不验证真实 UART 电气时序。
- TC-PERF-001（30min）/TC-STABILITY-001（8h）属最终集成验收，不在 S8 范围。
- S6/S7/S8 提交推送由仓库所有者执行；本文件证据基于本地提交 `14e9ad3` 构建。
