# Fully automatic TFTP deployment: interrupt PXE boot → TFTP transfer → boot kernel
# All in one session without reboot

$ErrorActionPreference = "Stop"
$port = "COM3"
$baud = 1500000

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Fully Automatic TFTP Deploy" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

Write-Host "[*] Opening $port at $baud baud..." -ForegroundColor Yellow

$serial = $null
try {
    $serial = New-Object System.IO.Ports.SerialPort($port, $baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 500
    $serial.WriteTimeout = 500
    $serial.Open()
    Write-Host "[+] Port opened successfully." -ForegroundColor Green
} catch {
    Write-Host "[ERROR] Cannot open $port" -ForegroundColor Red
    exit 1
}

try {
    Write-Host "[*] REBOOT BOARD NOW (power cycle)..." -ForegroundColor Yellow
    Write-Host "[*] Waiting for PXE boot to interrupt..." -ForegroundColor Yellow
    
    $buffer = ""
    $pxeDetected = $false
    $abortSeen = $false
    $promptReceived = $false
    $timeout = 90
    $start = Get-Date
    $lastCtrlC = Get-Date
    
    # STEP 1: Interrupt PXE boot and get U-Boot prompt
    while (((Get-Date) - $start).TotalSeconds -lt $timeout) {
        try {
            $char = $serial.ReadChar()
            $buffer += [char]$char
            Write-Host -NoNewline ([char]$char)
            
            if ($buffer -match "BOOTP|DHCP|TFTP|pxelinux|Loading:|Retrieving") {
                if (-not $pxeDetected) {
                    Write-Host ""
                    Write-Host "[*] PXE boot detected! Interrupting..." -ForegroundColor Yellow
                    $pxeDetected = $true
                }
                if (((Get-Date) - $lastCtrlC).TotalMilliseconds -gt 200) {
                    $serial.Write([char]3)
                    $lastCtrlC = Get-Date
                }
            }
            
            if ($buffer -match "Abort") {
                if (-not $abortSeen) {
                    Write-Host ""
                    Write-Host "[+] PXE boot aborted! Waiting for prompt..." -ForegroundColor Green
                    $abortSeen = $true
                }
            }
            
            if ($buffer -match "=>" -and $abortSeen) {
                Write-Host ""
                Write-Host "[+] U-Boot prompt captured!" -ForegroundColor Green
                $promptReceived = $true
                Start-Sleep -Milliseconds 500
                break
            }
            
            if ($buffer.Length -gt 2000) {
                $buffer = $buffer.Substring($buffer.Length - 1000)
            }
            
        } catch {
            Start-Sleep -Milliseconds 10
        }
    }
    
    if (-not $promptReceived) {
        Write-Host ""
        Write-Host "[ERROR] Failed to get U-Boot prompt" -ForegroundColor Red
        exit 1
    }
    
    # STEP 2: Configure network and perform TFTP transfer
    function Send-Command {
        param([string]$cmd, [int]$timeout = 120)
        
        Write-Host "[>] $cmd" -ForegroundColor Cyan
        $serial.WriteLine($cmd)
        
        $buffer = ""
        $start = Get-Date
        
        while (((Get-Date) - $start).TotalSeconds -lt $timeout) {
            try {
                $char = $serial.ReadChar()
                $buffer += [char]$char
                Write-Host -NoNewline ([char]$char)
                
                if ($buffer -match "=>") {
                    return $true
                }
            } catch {
                Start-Sleep -Milliseconds 10
            }
        }
        
        Write-Host ""
        Write-Host "[ERROR] Command timeout: $cmd" -ForegroundColor Red
        return $false
    }
    
    Write-Host ""
    Write-Host "[*] Configuring network..." -ForegroundColor Yellow
    Send-Command "setenv ipaddr 192.168.1.10" | Out-Null
    Send-Command "setenv serverip 192.168.1.166" | Out-Null
    
    Write-Host ""
    Write-Host "[*] Loading kernel via TFTP to RAM..." -ForegroundColor Yellow
    $result = Send-Command "tftp 0x02080000 kernel_neuro.bin" 120
    
    if (-not $result) {
        Write-Host "[ERROR] TFTP transfer failed!" -ForegroundColor Red
        exit 1
    }
    
    Write-Host ""
    Write-Host "[*] Waiting for TFTP to complete..." -ForegroundColor Yellow
    Start-Sleep -Seconds 2
    
    Write-Host ""
    Write-Host "[*] Booting kernel from RAM..." -ForegroundColor Yellow
    $serial.WriteLine("go 0x02080000")
    Start-Sleep -Milliseconds 500
    
    Write-Host ""
    Write-Host "[+] Kernel started! Monitoring output..." -ForegroundColor Green
    Write-Host ""
    
    # Monitor kernel output
    $start = Get-Date
    Write-Host ""
    Write-Host "Monitoring kernel output for 180 seconds..." -ForegroundColor Cyan
    Write-Host "Expecting boot beacons (A, B, C) and BEAT messages..." -ForegroundColor Cyan
    Write-Host ""
    
    while (((Get-Date) - $start).TotalSeconds -lt 180) {
        try {
            $char = $serial.ReadChar()
            Write-Host -NoNewline ([char]$char)
        } catch {
            Start-Sleep -Milliseconds 10
        }
    }
    
    Write-Host ""
    Write-Host "[*] Monitoring complete." -ForegroundColor Yellow
    
} finally {
    if ($serial -and $serial.IsOpen) {
        $serial.Close()
        Write-Host ""
        Write-Host "[!] Port closed." -ForegroundColor Yellow
    }
}
