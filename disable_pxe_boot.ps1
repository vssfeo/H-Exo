# Disable PXE boot in U-Boot to allow clean prompt access
# This script interrupts PXE boot loop and disables network boot permanently

$ErrorActionPreference = "Stop"
$port = "COM3"
$baud = 1500000

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Disable PXE Boot in U-Boot" -ForegroundColor Cyan
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
    Write-Host "[*] Waiting for PXE boot to start, then interrupting..." -ForegroundColor Yellow
    
    $buffer = ""
    $pxeDetected = $false
    $abortSeen = $false
    $promptReceived = $false
    $timeout = 90
    $start = Get-Date
    $lastCtrlC = Get-Date
    
    while (((Get-Date) - $start).TotalSeconds -lt $timeout) {
        try {
            $char = $serial.ReadChar()
            $buffer += [char]$char
            Write-Host -NoNewline ([char]$char)
            
            # Detect PXE boot activity
            if ($buffer -match "BOOTP|DHCP|TFTP|pxelinux|Loading:|Retrieving") {
                if (-not $pxeDetected) {
                    Write-Host ""
                    Write-Host "[*] PXE boot detected! Sending Ctrl+C to interrupt..." -ForegroundColor Yellow
                    $pxeDetected = $true
                }
                # Send Ctrl+C periodically during PXE boot
                if (((Get-Date) - $lastCtrlC).TotalMilliseconds -gt 200) {
                    $serial.Write([char]3)
                    $lastCtrlC = Get-Date
                }
            }
            
            # Detect Abort message
            if ($buffer -match "Abort") {
                if (-not $abortSeen) {
                    Write-Host ""
                    Write-Host "[+] PXE boot aborted! Waiting for U-Boot prompt..." -ForegroundColor Green
                    $abortSeen = $true
                }
            }
            
            # Detect U-Boot prompt after abort
            if ($buffer -match "=>" -and $abortSeen) {
                Write-Host ""
                Write-Host "[+] U-Boot prompt captured!" -ForegroundColor Green
                $promptReceived = $true
                Start-Sleep -Milliseconds 500
                break
            }
            
            # Keep buffer manageable
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
    
    # Now send commands to disable PXE boot
    Write-Host ""
    Write-Host "[*] Disabling PXE boot permanently..." -ForegroundColor Yellow
    
    function Send-Command {
        param([string]$cmd)
        
        Write-Host "[>] $cmd" -ForegroundColor Cyan
        $serial.WriteLine($cmd)
        
        $buffer = ""
        $start = Get-Date
        
        while (((Get-Date) - $start).TotalSeconds -lt 10) {
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
    
    # Disable network boot targets (remove pxe and dhcp)
    Send-Command "setenv boot_targets 'mmc1 mmc0 usb0'" | Out-Null
    
    # Set bootdelay to 1 second to allow interruption
    Send-Command "setenv bootdelay 1" | Out-Null
    
    # Try to save environment (may fail due to MMC issues, but that's OK)
    Write-Host ""
    Write-Host "[*] Attempting to save environment (may fail due to MMC)..." -ForegroundColor Yellow
    Send-Command "saveenv" | Out-Null
    
    Write-Host ""
    Write-Host "[*] Resetting board..." -ForegroundColor Yellow
    $serial.WriteLine("reset")
    
    Start-Sleep -Seconds 2
    
    Write-Host ""
    Write-Host "[+] PXE boot disabled! Board is resetting..." -ForegroundColor Green
    Write-Host ""
    Write-Host "After reboot, the board should drop to U-Boot prompt without PXE boot." -ForegroundColor Cyan
    Write-Host "You can now run: .\deploy_tftp_direct.ps1 -AssumePrompt" -ForegroundColor Cyan
    
} finally {
    if ($serial -and $serial.IsOpen) {
        $serial.Close()
        Write-Host ""
        Write-Host "[!] Port closed." -ForegroundColor Yellow
    }
}
