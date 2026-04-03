@echo off
TITLE TFTP Server Runner

ECHO ====================================
ECHO PowerShell TFTP Server Runner
ECHO ====================================
ECHO.
ECHO Checking if running as Administrator...
net session >nul 2>&1
if %errorLevel% == 0 (
    ECHO [OK] Running as Administrator
    ECHO Starting PowerShell TFTP Server...
    powershell -ExecutionPolicy Bypass -File "%~dp0ps_tftp_server.ps1"
) else (
    ECHO [INFO] Not running as Administrator
    ECHO Trying to elevate privileges...
    powershell -Command "Start-Process PowerShell -ArgumentList '-ExecutionPolicy Bypass -File \"%~dp0ps_tftp_server.ps1\"' -Verb RunAs"
)

ECHO.
ECHO Press any key to exit...
pause >nul