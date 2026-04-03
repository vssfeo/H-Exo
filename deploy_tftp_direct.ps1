# Deploy kernel via TFTP directly to RAM and boot (bypass SD card)
param(
    [switch]$AssumePrompt
)

$ErrorActionPreference = "Stop"
$port = "COM3"
$baud = 1500000

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo TFTP Direct RAM Boot" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

# Open serial port with retry
Write-Host "[*] Opening $port at $baud baud..." -ForegroundColor Yellow

$serial = $null
$maxRetries = 5
for ($i = 0; $i -lt $maxRetries; $i++) {
    try {
        $serial = New-Object System.IO.Ports.SerialPort($port, $baud, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
        $serial.ReadTimeout = 500
        $serial.WriteTimeout = 500
        $serial.Open()
        Write-Host "[+] Port opened successfully at $baud." -ForegroundColor Green
        break
    } catch {
        if ($i -eq $maxRetries - 1) {
            Write-Host "[ERROR] Cannot open $port for $($maxRetries * 5) sec. Port is busy." -ForegroundColor Red
            exit 1
        }
        Start-Sleep -Seconds 5
    }
}

try {
    if (-not $AssumePrompt) {
        Write-Host "[*] REBOOT BOARD NOW (power cycle). Waiting U-Boot prompt at $baud..." -ForegroundColor Yellow
        
        $buffer = ""
        $timeout = 60
        $start = Get-Date
        
        while (((Get-Date) - $start).TotalSeconds -lt $timeout) {
            try {
                $char = $serial.ReadChar()
                $buffer += [char]$char
                Write-Host -NoNewline ([char]$char)
                
                # Send break on PXE boot attempts
                if ($buffer -match "BOOTP|DHCP|TFTP|pxelinux") {
                    $serial.Write([char]3)
                }
                
                # Detect U-Boot prompt
                if ($buffer -match "=>") {
                    Write-Host ""
                    Write-Host "[+] U-Boot prompt detected at $baud baud." -ForegroundColor Green
                    Start-Sleep -Milliseconds 500
                    break
                }
            } catch {
                Start-Sleep -Milliseconds 10
            }
        }
    } else {
        Write-Host "[*] Assuming U-Boot prompt is ready..." -ForegroundColor Yellow
        Start-Sleep -Seconds 1
    }
    
    # Send U-Boot commands
    function Send-UBootCommand {
        param([string]$cmd, [int]$timeout = 30)
        
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
    
    # Configure network
    Send-UBootCommand "setenv ipaddr 192.168.1.10" | Out-Null
    Send-UBootCommand "setenv serverip 192.168.1.166" | Out-Null
    Send-UBootCommand "setenv bootfile kernel_neuro.bin" | Out-Null
    
    # Load kernel directly to 0x02080000 via TFTP
    Write-Host ""
    Write-Host "[*] Loading kernel directly to RAM via TFTP..." -ForegroundColor Yellow
    $result = Send-UBootCommand "tftp 0x02080000 kernel_neuro.bin" 120
    
    if (-not $result) {
        Write-Host "[ERROR] TFTP transfer failed!" -ForegroundColor Red
        exit 1
    }
    
    # Boot kernel
    Write-Host ""
    Write-Host "[*] Booting kernel from RAM..." -ForegroundColor Yellow
    Send-UBootCommand "go 0x02080000" 5 | Out-Null
    
    Write-Host ""
    Write-Host "[*] Kernel started! Monitoring for beacons..." -ForegroundColor Green
    Write-Host ""
    
    # Monitor output for boot beacons
    Write-Host "[*] Monitoring output for 90 sec..." -ForegroundColor Yellow
    $start = Get-Date
    $buffer = ""
    
    while (((Get-Date) - $start).TotalSeconds -lt 90) {
        try {
            $char = $serial.ReadChar()
            Write-Host -NoNewline ([char]$char)
            $buffer += [char]$char
            
            # Highlight beacons
            if ($buffer -match "BEAT") {
                Write-Host "" -ForegroundColor Green
            }
        } catch {
            Start-Sleep -Milliseconds 10
        }
    }
    
} finally {
    if ($serial -and $serial.IsOpen) {
        $serial.Close()
        Write-Host ""
        Write-Host "[!] Port closed." -ForegroundColor Yellow
    }
}
