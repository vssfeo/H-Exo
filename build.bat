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
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c boot.s -o boot.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c mmu.s -o mmu.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c vectors.s -o vectors.o
if errorlevel 1 goto error

echo.
echo [*] Compiling HAL...
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c hal\uart.c -o hal\uart.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c hal\gicv3.c -o hal\gicv3.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c hal\gmac.c -o hal\gmac.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c hal\exceptions.c -o hal\exceptions.o
if errorlevel 1 goto error

echo.
echo [*] Compiling Core...
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c core\heartbeat.c -o core\heartbeat.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c core\slab.c -o core\slab.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c core\chaos.c -o core\chaos.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c core\logger.c -o core\logger.o
if errorlevel 1 goto error

echo.
echo [*] Compiling Neuro...
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c neuro\neuro_sync.c -o neuro\neuro_sync.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c neuro\telemetry.c -o neuro\telemetry.o
if errorlevel 1 goto error
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c neuro\weight_validation.c -o neuro\weight_validation.o
if errorlevel 1 goto error

echo.
echo [*] Compiling main...
%GCC% -Wall -O2 -ffreestanding -nostdlib -nostartfiles -fno-common -fno-builtin -march=armv8-a -I. -c main_neuro.c -o main_neuro.o
if errorlevel 1 goto error

echo.
echo [*] Linking...
%GCC% -T linker.ld -o kernel_neuro.elf boot.o mmu.o vectors.o main_neuro.o hal\uart.o hal\gicv3.o hal\gmac.o hal\exceptions.o core\heartbeat.o core\slab.o core\chaos.o core\logger.o neuro\neuro_sync.o neuro\telemetry.o neuro\weight_validation.o -ffreestanding -nostdlib
if errorlevel 1 goto error

echo.
echo [*] Creating binary...
%OBJCOPY% -O binary kernel_neuro.elf kernel_neuro.bin
if errorlevel 1 goto error

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
