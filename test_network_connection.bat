@echo off
TITLE Network Connection Test for NanoPi M4

ECHO ======================================
ECHO Network Connection Test for NanoPi M4
ECHO ======================================
ECHO.

ECHO Your PC IP Address: 192.168.1.166
ECHO Expected NanoPi IP Address: 192.168.1.10
ECHO.

ECHO Testing network connection...
ECHO.

ECHO 1. Checking if TFTP port (69) is accessible:
netstat -an | findstr :69
ECHO.

ECHO 2. Checking network interfaces:
ipconfig | findstr "IPv4 Address"
ECHO.

ECHO 3. To test connection with NanoPi M4:
ECHO    - Connect NanoPi M4 to the same network
ECHO    - Configure NanoPi M4 in U-Boot with:
ECHO      setenv ipaddr 192.168.1.10
ECHO      setenv serverip 192.168.1.166
ECHO      setenv bootfile kernel_neuro.bin
ECHO      setenv netboot 'tftp ${loadaddr} ${bootfile}; go ${loadaddr}'
ECHO      saveenv
ECHO    - Then run: ping 192.168.1.166 in U-Boot
ECHO.

ECHO Network setup is ready. Please proceed with NanoPi M4 configuration.
ECHO ======================================

pause