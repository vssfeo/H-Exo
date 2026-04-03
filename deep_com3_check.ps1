# Deep COM3 diagnostics and repair
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Deep COM3 Diagnostics & Repair" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Step 1: Kill ALL processes that might hold COM ports
Write-Host "[1] Killing all PowerShell processes except current..." -ForegroundColor Yellow
Get-Process | Where-Object {
    ($_.ProcessName -eq "pwsh" -or $_.ProcessName -eq "powershell") -and $_.Id -ne $PID
} | ForEach-Object {
    Write-Host "    Killing PID $($_.Id) - $($_.ProcessName)" -ForegroundColor Gray
    Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
}

# Kill PuTTY, TeraTerm, etc.
$terminalApps = @("putty", "teraterm", "minicom", "screen", "cu")
foreach ($app in $terminalApps) {
    Get-Process -Name $app -ErrorAction SilentlyContinue | ForEach-Object {
        Write-Host "    Killing $($_.ProcessName) PID $($_.Id)" -ForegroundColor Gray
        Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue
    }
}

Start-Sleep -Seconds 2
Write-Host "[+] Process cleanup complete." -ForegroundColor Green
Write-Host ""

# Step 2: Check COM3 driver status
Write-Host "[2] Checking COM3 driver status..." -ForegroundColor Yellow
$com3Device = Get-CimInstance -ClassName Win32_PnPEntity | Where-Object {
    $_.Name -match "COM3"
}

if ($com3Device) {
    Write-Host "    Device: $($com3Device.Name)" -ForegroundColor White
    Write-Host "    Status: $($com3Device.Status)" -ForegroundColor White
    Write-Host "    DeviceID: $($com3Device.DeviceID)" -ForegroundColor Gray
    
    if ($com3Device.Status -ne "OK") {
        Write-Host "[!] COM3 device status is not OK. Attempting to restart..." -ForegroundColor Red
        # Disable and re-enable device
        $deviceId = $com3Device.DeviceID
        pnputil /restart-device "$deviceId" 2>&1 | Out-Null
        Start-Sleep -Seconds 3
        Write-Host "[+] Device restarted." -ForegroundColor Green
    }
} else {
    Write-Host "[!] COM3 device not found in Device Manager!" -ForegroundColor Red
}
Write-Host ""

# Step 3: Try multiple baud rates
Write-Host "[3] Testing COM3 at multiple baud rates..." -ForegroundColor Yellow
$baudRates = @(1500000, 115200, 921600, 460800, 230400, 57600, 9600)

foreach ($baud in $baudRates) {
    Write-Host "    Testing $baud baud..." -ForegroundColor Cyan
    
    try {
        $port = New-Object System.IO.Ports.SerialPort
        $port.PortName = "COM3"
        $port.BaudRate = $baud
        $port.DataBits = 8
        $port.Parity = [System.IO.Ports.Parity]::None
        $port.StopBits = [System.IO.Ports.StopBits]::One
        $port.Handshake = [System.IO.Ports.Handshake]::None
        $port.ReadTimeout = 500
        $port.WriteTimeout = 500
        $port.DtrEnable = $true
        $port.RtsEnable = $true
        
        $port.Open()
        $port.DiscardInBuffer()
        $port.DiscardOutBuffer()
        
        # Send multiple Enter keys
        for ($i = 0; $i -lt 5; $i++) {
            $port.Write([char]13)
            Start-Sleep -Milliseconds 100
        }
        
        # Wait for response
        $deadline = [DateTime]::Now.AddSeconds(3)
        $gotData = $false
        
        while ([DateTime]::Now -lt $deadline) {
            if ($port.BytesToRead -gt 0) {
                $data = $port.ReadExisting()
                Write-Host "      [+] GOT DATA at $baud baud!" -ForegroundColor Green
                Write-Host "      Data: $($data.Substring(0, [Math]::Min(200, $data.Length)))" -ForegroundColor White
                $gotData = $true
                $port.Close()
                $port.Dispose()
                
                Write-Host ""
                Write-Host "========================================" -ForegroundColor Green
                Write-Host "SUCCESS: COM3 is working at $baud baud!" -ForegroundColor Green
                Write-Host "========================================" -ForegroundColor Green
                Write-Host ""
                Write-Host "Run deploy script with: .\deploy_tftp_and_boot.ps1 -BaudRate $baud -AssumePrompt" -ForegroundColor Yellow
                exit 0
            }
            Start-Sleep -Milliseconds 50
        }
        
        $port.Close()
        $port.Dispose()
        
        if (-not $gotData) {
            Write-Host "      [!] No data at $baud" -ForegroundColor Gray
        }
        
    } catch {
        Write-Host "      [!] Error: $($_.Exception.Message)" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "========================================" -ForegroundColor Red
Write-Host "FAILED: No data received at any baud rate" -ForegroundColor Red
Write-Host "========================================" -ForegroundColor Red
Write-Host ""
Write-Host "Possible causes:" -ForegroundColor Yellow
Write-Host "1. Board is not powered (check power LED)" -ForegroundColor White
Write-Host "2. Board is stuck/frozen (try hard reset)" -ForegroundColor White
Write-Host "3. USB cable is damaged or loose" -ForegroundColor White
Write-Host "4. TX/RX lines are swapped in cable" -ForegroundColor White
Write-Host "5. Board UART2 is not configured correctly" -ForegroundColor White
Write-Host ""
Write-Host "Try these steps:" -ForegroundColor Yellow
Write-Host "1. Unplug and replug USB cable" -ForegroundColor White
Write-Host "2. Power cycle the board (remove power, wait 5 sec, reconnect)" -ForegroundColor White
Write-Host "3. Run this script again immediately after power-on" -ForegroundColor White
Write-Host "4. Check if board boots normally (LEDs, etc.)" -ForegroundColor White
Write-Host ""
