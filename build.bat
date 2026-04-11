@echo off
REM build.bat - Fast H-Exo kernel build for Windows

echo ========================================
echo   H-Exo Omni-Core: Fast Build
echo ========================================
echo.

set GCC=C:\gcc-arm\bin\aarch64-none-elf-gcc.exe
set OBJCOPY=C:\gcc-arm\bin\aarch64-none-elf-objcopy.exe

if not exist %GCC% (
    echo ERROR: GCC not found at %GCC%
    exit /b 1
)

echo [*] Cleaning old files...
del /Q *.o hal\*.o neuro\*.o core\*.o kernel_*.elf kernel_*.bin 2>nul

echo.
echo [*] Compiling assembly...
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c boot.s -o boot.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c mmu.s -o mmu.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c vectors.s -o vectors.o
if errorlevel 1 goto error

echo.
echo [*] Compiling HAL...
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c hal\uart.c -o hal\uart.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c hal\cci.c -o hal\cci.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c hal\gicv3.c -o hal\gicv3.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c hal\gmac.c -o hal\gmac.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c hal\net.c -o hal\net.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c hal\exceptions.c -o hal\exceptions.o
if errorlevel 1 goto error

echo.
echo [*] Compiling Core...
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c core\heartbeat.c -o core\heartbeat.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c core\slab.c -o core\slab.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c core\chaos.c -o core\chaos.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c core\logger.c -o core\logger.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c core\smp.c -o core\smp.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c core\workqueue.c -o core\workqueue.o
if errorlevel 1 goto error

echo.
echo [*] Compiling Neuro...
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c neuro\neuro_sync.c -o neuro\neuro_sync.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c neuro\telemetry.c -o neuro\telemetry.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c neuro\weight_validation.c -o neuro\weight_validation.o
if errorlevel 1 goto error
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c neuro\adaptive_scheduler.c -o neuro\adaptive_scheduler.o
if errorlevel 1 goto error

echo.
echo [*] Compiling main...
%GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c main_neuro.c -o main_neuro.o
if errorlevel 1 goto error

echo.
echo [*] Linking...
%GCC% -T linker.ld -o kernel_neuro.elf boot.o mmu.o vectors.o main_neuro.o hal\uart.o hal\cci.o hal\gicv3.o hal\gmac.o hal\net.o hal\exceptions.o core\heartbeat.o core\slab.o core\chaos.o core\logger.o core\smp.o core\workqueue.o neuro\neuro_sync.o neuro\telemetry.o neuro\weight_validation.o neuro\adaptive_scheduler.o -ffreestanding -nostdlib -Wl,--gc-sections -Wl,--no-warn-rwx-segments
if errorlevel 1 goto error

echo.
echo [*] Creating binary...
%OBJCOPY% -O binary kernel_neuro.elf kernel_neuro.bin
if errorlevel 1 goto error

echo.
echo [*] Checking neural weights CRC...
powershell -ExecutionPolicy Bypass -File tools\extract_weights_crc.ps1 -KernelPath kernel_neuro.bin -ElfPath kernel_neuro.elf
if errorlevel 3 goto error
if errorlevel 2 (
    echo [!] CRC updated - partial rebuild to embed new value...
    %GCC% -Wall -Os -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -ffunction-sections -fdata-sections -fomit-frame-pointer -I. -c neuro\weight_validation.c -o neuro\weight_validation.o
    if errorlevel 1 goto error
    %GCC% -T linker.ld -o kernel_neuro.elf boot.o mmu.o vectors.o main_neuro.o hal\uart.o hal\cci.o hal\gicv3.o hal\gmac.o hal\net.o hal\exceptions.o core\heartbeat.o core\slab.o core\chaos.o core\logger.o core\smp.o core\workqueue.o neuro\neuro_sync.o neuro\telemetry.o neuro\weight_validation.o neuro\adaptive_scheduler.o -ffreestanding -nostdlib -Wl,--gc-sections -Wl,--no-warn-rwx-segments
    if errorlevel 1 goto error
    %OBJCOPY% -O binary kernel_neuro.elf kernel_neuro.bin
    if errorlevel 1 goto error
    echo [OK] Kernel rebuilt with updated CRC
)

echo.
echo ========================================
echo   BUILD SUCCESS!
echo ========================================
for %%I in (kernel_neuro.bin) do (
    echo Size: %%~zI bytes
)
echo.
echo [*] Copying to tftpboot...
copy /Y kernel_neuro.bin C:\tftpboot\
echo.
echo Done! Run: .\deploy_tftp_fixed.ps1
goto end

:error
echo.
echo ========================================
echo   BUILD FAILED!
echo ========================================
exit /b 1

:end
