# check_tftp_port.ps1 - Script to check TFTP port availability

Write-Host "=== TFTP Port (69) Availability Check ===" -ForegroundColor Green
Write-Host ""

# Check if port 69 is listening
Write-Host "Checking if port 69 is currently in use..." -ForegroundColor Yellow

try {
    $portCheck = Get-NetTCPConnection -LocalPort 69 -ErrorAction SilentlyContinue -WarningAction SilentlyContinue
    if ($portCheck) {
        Write-Host "  [INFO] Port 69 is being used by:" -ForegroundColor Cyan
        foreach ($conn in $portCheck) {
            Write-Host "    Process: $($conn.OwningProcess), State: $($conn.State)" -ForegroundColor Cyan
        }
    } else {
        Write-Host "  [OK] Port 69 is not used by TCP connections" -ForegroundColor Green
        Write-Host "  TFTP typically uses UDP, so this is normal" -ForegroundColor Cyan
    }
} catch {
    Write-Host "  [INFO] Could not check TCP port 69" -ForegroundColor Cyan
}

Write-Host ""

# Check if we can bind to port 69
Write-Host "Checking if we can bind to port 69..." -ForegroundColor Yellow

try {
    $udp = New-Object System.Net.Sockets.UdpClient(69)
    Write-Host "  [OK] Successfully bound to UDP port 69" -ForegroundColor Green
    $udp.Close()
} catch {
    Write-Host "  [WARNING] Could not bind to UDP port 69: $($_.Exception.Message)" -ForegroundColor Yellow
    Write-Host "  This might indicate that another TFTP server is running" -ForegroundColor Cyan
}

Write-Host ""
Write-Host "=== Port Check Complete ===" -ForegroundColor Green