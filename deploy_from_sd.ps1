# Deploy kernel from SD card without TFTP
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Deploy kernel_neuro.bin from SD Card" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Copy kernel to SD card
Write-Host "[1] Insert SD card into PC and press Enter..." -ForegroundColor Yellow
Read-Host

$drives = Get-Volume | Where-Object { 
    $_.DriveType -eq 'Removable' -and $_.DriveLetter 
}

if ($drives.Count -eq 0) {
    Write-Host "[!] No removable drives found." -ForegroundColor Red
    exit 1
}

Write-Host "[+] Found removable drives:" -ForegroundColor Green
foreach ($drive in $drives) {
    Write-Host "    $($drive.DriveLetter): - $($drive.FileSystemLabel)" -ForegroundColor White
}

if ($drives.Count -gt 1) {
    $driveLetter = Read-Host "Enter drive letter (e.g., D)"
} else {
    $driveLetter = $drives[0].DriveLetter
}

$bootDir = "${driveLetter}:\boot"
if (-not (Test-Path $bootDir)) {
    $bootDir = "${driveLetter}:\"
}

$kernelSrc = ".\kernel_neuro.bin"
$kernelDst = Join-Path $bootDir "kernel_neuro.bin"

if (-not (Test-Path $kernelSrc)) {
    Write-Host "[!] kernel_neuro.bin not found in current directory!" -ForegroundColor Red
    exit 1
}

Write-Host "[*] Copying kernel_neuro.bin to $bootDir..." -ForegroundColor Yellow
Copy-Item $kernelSrc $kernelDst -Force

$srcHash = (Get-FileHash $kernelSrc -Algorithm SHA256).Hash
$dstHash = (Get-FileHash $kernelDst -Algorithm SHA256).Hash

if ($srcHash -eq $dstHash) {
    Write-Host "[+] Copy verified! SHA256 match." -ForegroundColor Green
} else {
    Write-Host "[!] Copy failed! Hash mismatch." -ForegroundColor Red
    exit 1
}

Write-Host ""
Write-Host "[+] kernel_neuro.bin copied to SD card successfully!" -ForegroundColor Green
Write-Host ""
Write-Host "[2] Eject SD card from PC and insert into NanoPi M4" -ForegroundColor Yellow
Write-Host "[3] Power cycle the board" -ForegroundColor Yellow
Write-Host "[4] Press Enter when ready to connect via serial..." -ForegroundColor Yellow
Read-Host

# Step 2: Connect and send U-Boot commands
Write-Host ""
Write-Host "[*] Opening COM3 at 1500000 baud..." -ForegroundColor Yellow

Get-Process | Where-Object {
    ($_.ProcessName -eq "pwsh" -or $_.ProcessName -eq "powershell") -and $_.Id -ne $PID
} | Stop-Process -Force -ErrorAction SilentlyContinue

Start-Sleep -Seconds 1

try {
    $port = New-Object System.IO.Ports.SerialPort
    $port.PortName = "COM3"
    $port.BaudRate = 1500000
    $port.DataBits = 8
    $port.Parity = [System.IO.Ports.Parity]::None
    $port.StopBits = [System.IO.Ports.StopBits]::One
    $port.Handshake = [System.IO.Ports.Handshake]::None
    $port.ReadTimeout = 500
    $port.WriteTimeout = 500
    $port.DtrEnable = $true
    $port.RtsEnable = $true
    
    $port.Open()
    Write-Host "[+] COM3 opened" -ForegroundColor Green
    
    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()
    
    Write-Host "[*] Waiting for U-Boot prompt (press Enter on board if needed)..." -ForegroundColor Yellow
    Write-Host ""
    
    # Wait for prompt
    $buf = ""
    $deadline = [DateTime]::Now.AddSeconds(30)
    $promptFound = $false
    
    while ([DateTime]::Now -lt $deadline) {
        $port.WriteLine("")
        Start-Sleep -Milliseconds 500
        
        if ($port.BytesToRead -gt 0) {
            $chunk = $port.ReadExisting()
            $buf += $chunk
            Write-Host $chunk -NoNewline -ForegroundColor Gray
            
            if ($buf -match "=>" -or $buf -match "U-Boot>") {
                $promptFound = $true
                break
            }
        }
    }
    
    if (-not $promptFound) {
        Write-Host "`n[!] U-Boot prompt not detected. Manually interrupt autoboot and run:" -ForegroundColor Red
        Write-Host "    fatload mmc 1:1 0x02080000 /boot/kernel_neuro.bin" -ForegroundColor White
        Write-Host "    go 0x02080000" -ForegroundColor White
        exit 1
    }
    
    Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
    Write-Host ""
    
    # Send commands
    $commands = @(
        "fatload mmc 1:1 0x02080000 /boot/kernel_neuro.bin",
        "go 0x02080000"
    )
    
    foreach ($cmd in $commands) {
        Write-Host "[>] $cmd" -ForegroundColor Yellow
        $port.WriteLine($cmd)
        Start-Sleep -Seconds 2
        
        $deadline = [DateTime]::Now.AddSeconds(5)
        while ([DateTime]::Now -lt $deadline) {
            if ($port.BytesToRead -gt 0) {
                $response = $port.ReadExisting()
                Write-Host $response -NoNewline -ForegroundColor White
            }
            Start-Sleep -Milliseconds 100
        }
        Write-Host ""
        
        if ($cmd -match "go 0x") {
            Write-Host "[+] Kernel started! Monitoring for beacons..." -ForegroundColor Green
            break
        }
    }
    
    # Monitor output
    Write-Host ""
    Write-Host "=== KERNEL OUTPUT ===" -ForegroundColor Cyan
    Write-Host ""
    
    $monitorDeadline = [DateTime]::Now.AddSeconds(60)
    while ([DateTime]::Now -lt $monitorDeadline) {
        if ($port.BytesToRead -gt 0) {
            Write-Host $port.ReadExisting() -NoNewline -ForegroundColor White
        }
        Start-Sleep -Milliseconds 20
    }
    
    Write-Host ""
    Write-Host ""
    Write-Host "[+] Monitoring complete." -ForegroundColor Green
    
} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port -and $port.IsOpen) {
        $port.Close()
        $port.Dispose()
        Write-Host "[!] Port closed." -ForegroundColor Yellow
    }
}
