@echo off
rem ============================================================
rem DMS Windows 发布包(S8,TC-DEPLOY-002)
rem 用法:仓库根目录执行 scripts\package.bat
rem 流程:vcvars -> Release 构建 -> cmake --install 装发布目录
rem      -> windeployqt 带 Qt DLL/插件 -> Compress-Archive 出 zip
rem --compiler-runtime 连 VC 运行库一起带上,干净机器免装 redist
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

set PATH=%DMS_QT_DIR_WIN%\bin;%PATH%

set BUILD_DIR=build-release
set DIST_ROOT=dist
set DIST_NAME=dms-win64-x64

for /f %%i in ('git rev-parse --short HEAD 2^>nul') do set GIT_SHA=%%i
if "%GIT_SHA%"=="" set GIT_SHA=uncommitted

cmake -S . -B %BUILD_DIR% -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_PREFIX_PATH=%DMS_QT_DIR% ^
  -DCMAKE_MAKE_PROGRAM=%DMS_VS_DIR%/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe
if errorlevel 1 (
    echo [ERROR] CMake 配置失败
    exit /b 1
)

cmake --build %BUILD_DIR%
if errorlevel 1 (
    echo [ERROR] 编译失败
    exit /b 1
)

if exist "%DIST_ROOT%\%DIST_NAME%" rmdir /s /q "%DIST_ROOT%\%DIST_NAME%"
cmake --install %BUILD_DIR% --prefix "%DIST_ROOT%\%DIST_NAME%"
if errorlevel 1 (
    echo [ERROR] 发布目录安装失败
    exit /b 1
)

rem windeployqt 按可执行链接的 Qt 模块选插件(Widgets/Network/Sql/SerialPort)
windeployqt --release --compiler-runtime --no-translations ^
  "%DIST_ROOT%\%DIST_NAME%\bin\dms_monitor.exe"
if errorlevel 1 (
    echo [ERROR] windeployqt 失败
    exit /b 1
)

powershell -NoProfile -Command "Compress-Archive -Path '%DIST_ROOT%\%DIST_NAME%' -DestinationPath '%DIST_ROOT%\%DIST_NAME%-%GIT_SHA%.zip' -Force"
if errorlevel 1 (
    echo [ERROR] 打 zip 失败
    exit /b 1
)

echo [package.bat] 发布目录: %DIST_ROOT%\%DIST_NAME%
echo [package.bat] 发布包:   %DIST_ROOT%\%DIST_NAME%-%GIT_SHA%.zip
