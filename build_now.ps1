$GCC     = "C:\gcc-arm\bin\aarch64-none-elf-gcc.exe"
$OBJCOPY = "C:\gcc-arm\bin\aarch64-none-elf-objcopy.exe"
$CFLAGS  = @("-Wall","-O2","-ffreestanding","-nostdlib","-nostartfiles",
             "-fno-common","-fno-builtin","-march=armv8-a","-I.")

Remove-Item -Path "*.o","hal\*.o","neuro\*.o","core\*.o","kernel_neuro.*" -ErrorAction SilentlyContinue

$srcs = @(
    @{src="boot.s";                  obj="boot.o"},
    @{src="mmu.s";                   obj="mmu.o"},
    @{src="vectors.s";               obj="vectors.o"},
    @{src="hal\uart.c";              obj="hal\uart.o"},
    @{src="hal\cci.c";               obj="hal\cci.o"},
    @{src="hal\gicv3.c";             obj="hal\gicv3.o"},
    @{src="hal\gmac.c";              obj="hal\gmac.o"},
    @{src="hal\exceptions.c";        obj="hal\exceptions.o"},
    @{src="hal\net.c";               obj="hal\net.o"},
    @{src="core\heartbeat.c";        obj="core\heartbeat.o"},
    @{src="core\slab.c";             obj="core\slab.o"},
    @{src="core\chaos.c";            obj="core\chaos.o"},
    @{src="core\logger.c";           obj="core\logger.o"},
    @{src="core\smp.c";              obj="core\smp.o"},
    @{src="core\workqueue.c";        obj="core\workqueue.o"},
    @{src="neuro\neuro_sync.c";      obj="neuro\neuro_sync.o"},
    @{src="neuro\telemetry.c";       obj="neuro\telemetry.o"},
    @{src="neuro\weight_validation.c"; obj="neuro\weight_validation.o"},
    @{src="neuro\adaptive_scheduler.c"; obj="neuro\adaptive_scheduler.o"},
    @{src="main_neuro.c";            obj="main_neuro.o"}
)

$failed = $false
foreach ($s in $srcs) {
    Write-Host "  CC $($s.src)"
    & $GCC @CFLAGS -c $s.src -o $s.obj 2>&1 | Where-Object { $_ -match "error:" } | ForEach-Object { Write-Host "ERROR: $_" -ForegroundColor Red; $failed = $true }
}

if ($failed) { Write-Host "COMPILE ERRORS" -ForegroundColor Red; exit 1 }

$objs = $srcs | ForEach-Object { $_.obj }
Write-Host "  LD kernel_neuro.elf"
& $GCC -T linker.ld -o kernel_neuro.elf @objs @("-ffreestanding","-nostdlib") 2>&1
& $OBJCOPY -O binary kernel_neuro.elf kernel_neuro.bin
Copy-Item kernel_neuro.bin C:\tftpboot\kernel_neuro.bin -Force
$sz = (Get-Item kernel_neuro.bin).Length
Write-Host "BUILD OK: $sz bytes" -ForegroundColor Green
