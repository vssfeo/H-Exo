# Interactive serial shell
param(
    [int]$BaudRate = 1500000
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Interactive Serial Shell" -ForegroundColor Cyan
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
    $port.ReadTimeout = 100
    $port.WriteTimeout = 500
    $port.DtrEnable = $true
    $port.RtsEnable = $true
    
    $port.Open()
    Write-Host "[+] COM3 opened at $BaudRate baud" -ForegroundColor Green
    Write-Host "[*] Type commands and press Enter. Type 'exit' to quit." -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
    
    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()
    
    # Send initial Enter to get prompt
    $port.WriteLine("")
    Start-Sleep -Milliseconds 500
    
    # Background job to read from serial
    $readJob = Start-Job -ScriptBlock {
        param($portName, $baudRate)
        $p = New-Object System.IO.Ports.SerialPort
        $p.PortName = $portName
        $p.BaudRate = $baudRate
        $p.DataBits = 8
        $p.Parity = [System.IO.Ports.Parity]::None
        $p.StopBits = [System.IO.Ports.StopBits]::One
        $p.Handshake = [System.IO.Ports.Handshake]::None
        $p.ReadTimeout = 100
        $p.Open()
        
        while ($true) {
            try {
                if ($p.BytesToRead -gt 0) {
                    $data = $p.ReadExisting()
                    Write-Output $data
                }
            } catch {}
            Start-Sleep -Milliseconds 50
        }
    } -ArgumentList "COM3", $BaudRate
    
    Write-Host "Quick commands:" -ForegroundColor Yellow
    Write-Host "  rm /boot/boot.scr     - Remove boot script" -ForegroundColor White
    Write-Host "  ls -la /boot/         - List boot directory" -ForegroundColor White
    Write-Host "  reboot                - Reboot board" -ForegroundColor White
    Write-Host ""
    
    while ($true) {
        # Check for output from serial
        $jobOutput = Receive-Job -Job $readJob -ErrorAction SilentlyContinue
        if ($jobOutput) {
            Write-Host $jobOutput -NoNewline -ForegroundColor Gray
        }
        
        # Check for user input
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            
            if ($key.Key -eq 'Enter') {
                $port.WriteLine("")
                Write-Host ""
            } elseif ($key.Key -eq 'Backspace') {
                Write-Host "`b `b" -NoNewline
            } else {
                $port.Write($key.KeyChar)
                Write-Host $key.KeyChar -NoNewline -ForegroundColor White
            }
        }
        
        Start-Sleep -Milliseconds 10
    }
    
} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($readJob) {
        Stop-Job -Job $readJob -ErrorAction SilentlyContinue
        Remove-Job -Job $readJob -Force -ErrorAction SilentlyContinue
    }
    if ($port -and $port.IsOpen) {
        $port.Close()
        $port.Dispose()
        Write-Host "`n[!] Port closed." -ForegroundColor Yellow
    }
}
