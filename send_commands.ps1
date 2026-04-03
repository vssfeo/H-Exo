# Send commands to already running Linux shell
param(
    [int]$BaudRate = 1500000
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Send Commands to Serial Console" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# Kill any process holding COM3
Get-Process | Where-Object {
    ($_.ProcessName -eq "pwsh" -or $_.ProcessName -eq "powershell") -and $_.Id -ne $PID
} | Stop-Process -Force -ErrorAction SilentlyContinue

Start-Sleep -Seconds 1

try {
    $port = New-Object System.IO.Ports.SerialPort
    $port.PortName = "COM3"
    $port.BaudRate = $BaudRate
    $port.DataBits = 8
    $port.Parity = [System.IO.Ports.Parity]::None
    $port.StopBits = [System.IO.Ports.StopBits]::One
    $port.Handshake = [System.IO.Ports.Handshake]::None
    $port.ReadTimeout = 500
    $port.WriteTimeout = 500
    $port.DtrEnable = $true
    $port.RtsEnable = $true
    
    $port.Open()
    Write-Host "[+] COM3 opened at $BaudRate baud" -ForegroundColor Green
    
    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()
    
    # Send commands
    $commands = @(
        "rm -f /boot/boot.scr",
        "ls -la /boot/boot.scr",
        "sync",
        "reboot"
    )
    
    foreach ($cmd in $commands) {
        Write-Host "[>] $cmd" -ForegroundColor Yellow
        $port.WriteLine($cmd)
        Start-Sleep -Seconds 1
        
        # Read response
        $deadline = [DateTime]::Now.AddSeconds(2)
        while ([DateTime]::Now -lt $deadline) {
            if ($port.BytesToRead -gt 0) {
                $response = $port.ReadExisting()
                Write-Host $response -NoNewline -ForegroundColor Gray
            }
            Start-Sleep -Milliseconds 100
        }
        Write-Host ""
    }
    
    Write-Host ""
    Write-Host "[+] Commands sent. Board should be rebooting..." -ForegroundColor Green
    Write-Host ""
    Write-Host "Wait 30 seconds, then run: .\deploy_tftp_and_boot.ps1" -ForegroundColor Yellow
    Write-Host ""
    
} catch {
    Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port -and $port.IsOpen) {
        $port.Close()
        $port.Dispose()
        Write-Host "[!] Port closed." -ForegroundColor Yellow
    }
}
