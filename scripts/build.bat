@echo off
rem ============================================================
rem DMS 一键构建脚本(Windows / MSVC / Ninja)
rem 用法:在仓库根目录执行 scripts\build.bat
rem 流程:vcvars 设置 MSVC 环境 -> CMake 配置 -> 编译 -> CTest
rem 路径可用环境变量覆盖,便于换机器(WSL 侧另用 CMakePresets)
rem ============================================================
setlocal

if "%DMS_VS_DIR%"=="" set DMS_VS_DIR=D:\VS2022BT
if "%DMS_QT_DIR%"=="" set DMS_QT_DIR=D:/Qt/6.8.3/msvc2022_64
if "%DMS_QT_DIR_WIN%"=="" set DMS_QT_DIR_WIN=D:\Qt\6.8.3\msvc2022_64

call "%DMS_VS_DIR%\VC\Auxiliary\Build\vcvarsall.bat" x64
if errorlevel 1 (
    echo [ERROR] vcvarsall 调用失败,检查 DMS_VS_DIR=%DMS_VS_DIR%
    exit /b 1
)

rem 测试/运行期需要 Qt DLL 可被加载(0xc0000135 = DLL 未找到)
set PATH=%DMS_QT_DIR_WIN%\bin;%PATH%

cmake -S . -B build -G Ninja ^
  -DCMAKE_PREFIX_PATH=%DMS_QT_DIR% ^
  -DCMAKE_MAKE_PROGRAM=%DMS_VS_DIR%/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe
if errorlevel 1 (
    echo [ERROR] CMake 配置失败
    exit /b 1
)

cmake --build build
if errorlevel 1 (
    echo [ERROR] 编译失败
    exit /b 1
)

ctest --test-dir build --output-on-failure
exit /b %errorlevel%
