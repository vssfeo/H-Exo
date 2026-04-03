# h-exo.ps1 - Единый скрипт для H-Exo Omni-Core
# Build + Deploy + Test в одном файле

param(
    [Parameter(Mandatory=$false)]
    [ValidateSet("build", "deploy", "test", "all", "menu")]
    [string]$Command = "menu",
    
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000
)

$ErrorActionPreference = "Stop"
$GCC = "C:\gcc-arm\bin\aarch64-none-elf-gcc.exe"
$OBJCOPY = "C:\gcc-arm\bin\aarch64-none-elf-objcopy.exe"

function Show-Menu {
    Clear-Host
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host "  H-Exo Omni-Core Control Center" -ForegroundColor Cyan
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
    Write-Host "  [1] Build kernel" -ForegroundColor White
    Write-Host "  [2] Deploy via TFTP" -ForegroundColor White
    Write-Host "  [3] Run tests" -ForegroundColor White
    Write-Host "  [4] Build + Deploy + Test (FULL)" -ForegroundColor Yellow
    Write-Host ""
    Write-Host "  [Q] Quit" -ForegroundColor Gray
    Write-Host ""
    
    $choice = Read-Host "Select option"
    switch ($choice) {
        "1" { Invoke-Build }
        "2" { Invoke-Deploy }
        "3" { Invoke-Test }
        "4" { Invoke-Full }
        "Q" { exit 0 }
        "q" { exit 0 }
        default { Show-Menu }
    }
}

function Invoke-Build {
    Write-Host "`n========================================" -ForegroundColor Green
    Write-Host "  BUILD KERNEL" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    
    if (-not (Test-Path $GCC)) {
        Write-Error "GCC not found: $GCC"
        return
    }
    
    # Clean
    Remove-Item -Path "*.o","hal\*.o","neuro\*.o","core\*.o","kernel_*.elf","kernel_*.bin" -ErrorAction SilentlyContinue
    
    $CFLAGS = @("-Wall","-O2","-ffreestanding","-nostdlib","-nostartfiles","-fno-common","-fno-builtin","-march=armv8-a","-I.")
    
    # Compile
    Write-Host "[*] Compiling..." -ForegroundColor Yellow
    & $GCC @CFLAGS -c boot.s -o boot.o
    & $GCC @CFLAGS -c mmu.s -o mmu.o
    & $GCC @CFLAGS -c vectors.s -o vectors.o
    & $GCC @CFLAGS -c hal\uart.c -o hal\uart.o
    & $GCC @CFLAGS -c hal\gicv3.c -o hal\gicv3.o
    & $GCC @CFLAGS -c hal\gmac.c -o hal\gmac.o
    & $GCC @CFLAGS -c hal\exceptions.c -o hal\exceptions.o
    & $GCC @CFLAGS -c core\heartbeat.c -o core\heartbeat.o
    & $GCC @CFLAGS -c core\slab.c -o core\slab.o
    & $GCC @CFLAGS -c core\chaos.c -o core\chaos.o
    & $GCC @CFLAGS -c core\logger.c -o core\logger.o
    & $GCC @CFLAGS -c neuro\neuro_sync.c -o neuro\neuro_sync.o
    & $GCC @CFLAGS -c neuro\telemetry.c -o neuro\telemetry.o
    & $GCC @CFLAGS -c neuro\weight_validation.c -o neuro\weight_validation.o
    & $GCC @CFLAGS -c main_neuro.c -o main_neuro.o
    
    # Link
    $OBJS = @("boot.o","mmu.o","vectors.o","main_neuro.o","hal\uart.o","hal\gicv3.o","hal\gmac.o","hal\exceptions.o","core\heartbeat.o","core\slab.o","core\chaos.o","core\logger.o","neuro\neuro_sync.o","neuro\telemetry.o","neuro\weight_validation.o")
    & $GCC -T linker.ld -o kernel_neuro.elf @OBJS @("-ffreestanding","-nostdlib")
    & $OBJCOPY -O binary kernel_neuro.elf kernel_neuro.bin
    
    # Copy to tftpboot
    Copy-Item kernel_neuro.bin C:\tftpboot\kernel_neuro.bin -Force
    
    $size = (Get-Item kernel_neuro.bin).Length
    Write-Host "`n[+] BUILD SUCCESS: $size bytes" -ForegroundColor Green
    
    Pause
    Show-Menu
}

function Invoke-Deploy {
    Write-Host "`n========================================" -ForegroundColor Green
    Write-Host "  DEPLOY VIA TFTP" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "Port: $PortName @ $BaudRate baud" -ForegroundColor Gray
    Write-Host ""
    
    $localIP = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -match "^192\.168\." } | Select-Object -First 1).IPAddress
    Write-Host "PC IP: $localIP" -ForegroundColor Cyan
    
    # Check kernel
    if (-not (Test-Path "C:\tftpboot\kernel_neuro.bin")) {
        Write-Error "Kernel not found in C:\tftpboot\"
        return
    }
    
    $serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 500
    $serial.WriteTimeout = 1000
    $serial.Open()
    
    try {
        Write-Host "`n[*] Power cycle the board now..." -ForegroundColor Yellow
        Write-Host "[*] Or press any key if already at U-Boot prompt..." -ForegroundColor Gray
        Write-Host "[*] Waiting for '=>' prompt..." -ForegroundColor Gray
        
        $buffer = ""
        $promptFound = $false
        $timeout = 180  # Увеличили до 3 минут
        $start = Get-Date
        $lastProgress = Get-Date
        $dataReceived = $false
        
        while (((Get-Date) - $start).TotalSeconds -lt $timeout -and -not $promptFound) {
            # Check for key press to skip waiting
            if ([Console]::KeyAvailable) {
                $null = [Console]::ReadKey($true)
                Write-Host "`n[!] Key pressed - assuming U-Boot is ready" -ForegroundColor Yellow
                $promptFound = $true
                break
            }
            
            try {
                $ch = [char]$serial.ReadChar()
                $dataReceived = $true
                $buffer += $ch
                
                # Debug: show first characters received
                if ($buffer.Length -le 100 -and $buffer.Length % 10 -eq 0) {
                    Write-Host "[DEBUG] Received: $($buffer.Substring([Math]::Max(0,$buffer.Length-20)))" -ForegroundColor DarkGray
                }
                
                # Show progress every 10 seconds
                if (((Get-Date) - $lastProgress).TotalSeconds -ge 10) {
                    $elapsed = [Math]::Round(((Get-Date) - $start).TotalSeconds)
                    Write-Host "[*] Waiting... ${elapsed}s (buffer: $($buffer.Length) chars)" -ForegroundColor Gray -NoNewline
                    Write-Host "`r" -NoNewline
                    $lastProgress = Get-Date
                }
                
                # Interrupt PXE boot
                if ($buffer -match "BOOTP|DHCP|pxelinux|Loading:") {
                    $serial.Write([char]3)
                    Write-Host "`n[*] Interrupted PXE boot" -ForegroundColor Yellow
                }
                
                # Look for U-Boot prompt (=> at end of line)
                if ($buffer -match "=>\s*$" -or $buffer -match "\n=>\s*$") {
                    $promptFound = $true
                    Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
                    Write-Host "[DEBUG] Last 100 chars: $($buffer.Substring([Math]::Max(0,$buffer.Length-100)))" -ForegroundColor DarkGray
                }
                
                if ($buffer.Length -gt 4000) { $buffer = $buffer.Substring(2000) }
            } catch [System.TimeoutException] {
                # Timeout is OK, continue waiting
                Start-Sleep -Milliseconds 10
            } catch {
                Start-Sleep -Milliseconds 10
            }
        }
        
        if (-not $promptFound) {
            if (-not $dataReceived) {
                Write-Error "No data received from $PortName. Check:`n1. Cable connected`n2. Correct COM port`n3. Board powered on`n4. Baud rate 1500000"
            } else {
                Write-Error "U-Boot prompt not found within ${timeout}s. Last buffer: $($buffer.Substring([Math]::Max(0,$buffer.Length-200)))"
            }
            return
        }
        
        Write-Host "`n[+] U-Boot ready" -ForegroundColor Green
        
        # Configure network
        Start-Sleep -Milliseconds 500
        $serial.WriteLine("setenv ipaddr 192.168.1.10")
        Start-Sleep -Milliseconds 200
        $serial.WriteLine("setenv serverip $localIP")
        Start-Sleep -Milliseconds 200
        $serial.WriteLine("setenv bootfile kernel_neuro.bin")
        Start-Sleep -Milliseconds 200
        
        Write-Host "[*] TFTP transfer..." -ForegroundColor Yellow
        $serial.WriteLine("tftp 0x02080000 kernel_neuro.bin")
        
        # Wait for TFTP
        Start-Sleep -Seconds 3
        
        Write-Host "[*] Booting kernel..." -ForegroundColor Yellow
        $serial.WriteLine("go 0x02080000")
        
        # Monitor boot
        Write-Host "`n========================================" -ForegroundColor Green
        Write-Host "  KERNEL RUNNING - MONITORING" -ForegroundColor Green
        Write-Host "========================================" -ForegroundColor Green
        Write-Host "Press Ctrl+C to stop`n" -ForegroundColor Gray
        
        while ($true) {
            try {
                if ($serial.BytesToRead -gt 0) {
                    $ch = [char]$serial.ReadChar()
                    Write-Host -NoNewline $ch
                }
                Start-Sleep -Milliseconds 10
            } catch {
                Start-Sleep -Milliseconds 10
            }
        }
    } finally {
        $serial.Close()
    }
}

function Invoke-Test {
    Write-Host "`n========================================" -ForegroundColor Green
    Write-Host "  TEST MODE" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    Write-Host "`n[!] Kernel should be running!" -ForegroundColor Yellow
    Write-Host "Press any key to start tests..."
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    
    $serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 100
    $serial.Open()
    
    try {
        $stats = @{ Beats = 0; MaxJitter = 0; Inferences = 0; Chaos = 0 }
        $start = Get-Date
        $testDuration = 60
        
        Write-Host "`n[*] Testing for $testDuration seconds..." -ForegroundColor Yellow
        Write-Host "[*] Press 'q' to quit, '2' for heartbeat, 'C' for chaos`n" -ForegroundColor Gray
        
        while (((Get-Date) - $start).TotalSeconds -lt $testDuration) {
            # Read serial
            if ($serial.BytesToRead -gt 0) {
                try {
                    $ch = [char]$serial.ReadChar()
                    Write-Host -NoNewline $ch
                } catch {}
            }
            
            # Handle input
            if ([Console]::KeyAvailable) {
                $key = [Console]::ReadKey($true)
                if ($key.Key -eq "C" -and $key.Modifiers -eq "Control") { break }
                if ($key.KeyChar) { $serial.Write($key.KeyChar) }
            }
            
            Start-Sleep -Milliseconds 10
        }
        
        Write-Host "`n`n[+] Test complete" -ForegroundColor Green
    } finally {
        $serial.Close()
    }
    
    Pause
    Show-Menu
}

function Invoke-Full {
    Invoke-Build
    Write-Host "`n[!] Build complete. Press any key to deploy..." -ForegroundColor Yellow
    $null = $Host.UI.RawUI.ReadKey("NoEcho,IncludeKeyDown")
    Invoke-Deploy
}

# Main
if ($Command -eq "menu") {
    Show-Menu
} else {
    switch ($Command) {
        "build" { Invoke-Build }
        "deploy" { Invoke-Deploy }
        "test" { Invoke-Test }
        "all" { Invoke-Full }
    }
}
