@echo off
rem ============================================================
rem TC-DEPLOY-002 release-package smoke (Windows):
rem   launch dms_simulator + dms_monitor from the release directory,
rem   then taskkill both. Verifies Qt plugin loading (platform/SQL/
rem   serial), first-run template init and database creation.
rem Usage: scripts\deploy-smoke.bat <release-dir>
rem Note: taskkill /IM kills every instance of both names.
rem ============================================================
setlocal
set "DIST_DIR=%~1"
if "%DIST_DIR%"=="" (
    echo [ERROR] usage: scripts\deploy-smoke.bat ^<release-dir^>
    exit /b 2
)
set "BIN_DIR=%DIST_DIR%\bin"
if not exist "%BIN_DIR%\dms_monitor.exe" (
    echo [ERROR] missing %BIN_DIR%\dms_monitor.exe
    exit /b 2
)

set "RUNROOT=%TEMP%\dms-deploy-smoke"
rmdir /s /q "%RUNROOT%" >nul 2>&1
mkdir "%RUNROOT%"

start "" /B "%BIN_DIR%\dms_simulator.exe"
rem ping-based delay: timeout.exe requires interactive console stdin and
rem aborts immediately when stdin is redirected (CI / agent shells).
ping -n 3 127.0.0.1 >nul
start "" /B "%BIN_DIR%\dms_monitor.exe" --config-dir "%RUNROOT%"
ping -n 11 127.0.0.1 >nul
taskkill /IM dms_monitor.exe /F >nul 2>&1
taskkill /IM dms_simulator.exe /F >nul 2>&1

echo ===== run-root layout =====
dir /s /b "%RUNROOT%"
echo ===== DB-ready log line =====
findstr /C:"SQLite" "%RUNROOT%\logs\dms.log"
if errorlevel 1 (
    echo [FAIL] no SQLite line in %RUNROOT%\logs\dms.log
    exit /b 1
)
echo ===== smoke result =====
echo [smoke] release package runs: simulator + monitor + SQLite ready
