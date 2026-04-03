# Check all COM ports in the system
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  COM Ports Diagnostics" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Get all COM ports from Device Manager
Write-Host "[*] Checking Device Manager for COM ports..." -ForegroundColor Yellow
$comPorts = Get-CimInstance -ClassName Win32_PnPEntity | Where-Object {
    $_.Name -match "COM\d+"
}

if ($comPorts) {
    Write-Host "[+] Found COM ports:" -ForegroundColor Green
    foreach ($port in $comPorts) {
        $portName = if ($port.Name -match "(COM\d+)") { $matches[1] } else { "Unknown" }
        $status = if ($port.Status -eq "OK") { "OK" } else { $port.Status }
        Write-Host "    $portName - $($port.Name) - Status: $status" -ForegroundColor White
    }
} else {
    Write-Host "[!] No COM ports found in Device Manager." -ForegroundColor Red
    Write-Host "    Check if USB-to-Serial adapter is connected." -ForegroundColor Red
}

Write-Host ""
Write-Host "[*] Attempting to open each detected COM port..." -ForegroundColor Yellow
Write-Host ""

# Try to open each COM port
$availablePorts = [System.IO.Ports.SerialPort]::GetPortNames()
if ($availablePorts.Count -eq 0) {
    Write-Host "[!] No COM ports available to open." -ForegroundColor Red
} else {
    foreach ($portName in $availablePorts) {
        Write-Host "Testing $portName..." -ForegroundColor Cyan
        
        try {
            $port = New-Object System.IO.Ports.SerialPort
            $port.PortName = $portName
            $port.BaudRate = 115200
            $port.Open()
            
            Write-Host "  [+] $portName opened successfully at 115200 baud" -ForegroundColor Green
            
            # Try to read for 2 seconds
            $port.DiscardInBuffer()
            $deadline = [DateTime]::Now.AddSeconds(2)
            $receivedData = $false
            
            while ([DateTime]::Now -lt $deadline) {
                if ($port.BytesToRead -gt 0) {
                    $chunk = $port.ReadExisting()
                    Write-Host "  [+] $portName is receiving data!" -ForegroundColor Green
                    Write-Host "      First 100 chars: $($chunk.Substring(0, [Math]::Min(100, $chunk.Length)))" -ForegroundColor White
                    $receivedData = $true
                    break
                }
                Start-Sleep -Milliseconds 100
            }
            
            if (-not $receivedData) {
                Write-Host "  [!] $portName is silent (no data received in 2 sec)" -ForegroundColor Yellow
            }
            
            $port.Close()
            $port.Dispose()
            
        } catch {
            Write-Host "  [!] $portName - Error: $($_.Exception.Message)" -ForegroundColor Red
        }
        
        Write-Host ""
    }
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Diagnostics complete." -ForegroundColor Cyan
Write-Host ""
Write-Host "Next steps:" -ForegroundColor Yellow
Write-Host "1. If COM3 is NOT listed above, the board is not connected or driver is missing" -ForegroundColor White
Write-Host "2. If COM3 is listed but silent, check:" -ForegroundColor White
Write-Host "   - Board power (LED should be on)" -ForegroundColor White
Write-Host "   - USB cable connection" -ForegroundColor White
Write-Host "   - Try power cycling the board while this script runs" -ForegroundColor White
Write-Host "3. If another COM port shows data, use that port instead of COM3" -ForegroundColor White
Write-Host "========================================" -ForegroundColor Cyan
