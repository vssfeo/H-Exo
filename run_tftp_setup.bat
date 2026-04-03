@echo off
TITLE TFTP Setup for NanoPi M4

ECHO ================================
ECHO TFTP Setup for NanoPi M4 (RK3399)
ECHO ================================
ECHO.

ECHO Creating TFTP directory...
if not exist "C:\tftpboot" mkdir "C:\tftpboot"

ECHO Copying kernel file to TFTP directory...
if exist "kernel_neuro.bin" (
    copy "kernel_neuro.bin" "C:\tftpboot\kernel_neuro.bin" >nul
    if %ERRORLEVEL% EQU 0 (
        ECHO [OK] Kernel file copied successfully
    ) else (
        ECHO [ERROR] Failed to copy kernel file
    )
) else (
    ECHO [WARNING] kernel_neuro.bin not found
)

ECHO.
ECHO ================================
ECHO TFTP Setup Information:
ECHO ================================
ECHO TFTP Directory: C:\tftpboot
ECHO TFTP Server IP: 192.168.1.166
ECHO Kernel File: kernel_neuro.bin
ECHO.
ECHO Next steps:
ECHO 1. Install Tftpd64 and configure it to use C:\tftpboot
ECHO 2. Configure NanoPi M4 in U-Boot:
ECHO    setenv ipaddr 192.168.1.10
ECHO    setenv serverip 192.168.1.166
ECHO    setenv bootfile kernel_neuro.bin
ECHO    setenv netboot 'tftp ${loadaddr} ${bootfile}; go ${loadaddr}'
ECHO    saveenv
ECHO 3. Load kernel with: run netboot
ECHO.
ECHO Setup complete! You can now speed up your development cycle.
ECHO ================================

pause