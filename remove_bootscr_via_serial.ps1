# Remove boot.scr via serial console after Armbian boots
param(
    [int]$BaudRate = 1500000
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  Remove boot.scr via Serial Console" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "This script will:" -ForegroundColor Yellow
Write-Host "1. Wait for Armbian to boot" -ForegroundColor White
Write-Host "2. Login as root" -ForegroundColor White
Write-Host "3. Remove /boot/boot.scr" -ForegroundColor White
Write-Host "4. Reboot the board" -ForegroundColor White
Write-Host ""
Write-Host "POWER CYCLE THE BOARD NOW and wait for login prompt!" -ForegroundColor Red
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
    Write-Host "[*] Waiting for Armbian login prompt..." -ForegroundColor Yellow
    Write-Host "========================================" -ForegroundColor Cyan
    Write-Host ""
    
    $port.DiscardInBuffer()
    $port.DiscardOutBuffer()
    
    $buf = ""
    $loginSeen = $false
    $deadline = [DateTime]::Now.AddSeconds(180)
    
    # Wait for login prompt
    while ([DateTime]::Now -lt $deadline) {
        if ($port.BytesToRead -gt 0) {
            $chunk = $port.ReadExisting()
            $buf += $chunk
            Write-Host $chunk -NoNewline -ForegroundColor Gray
            
            if ($buf -match "login:" -and -not $loginSeen) {
                $loginSeen = $true
                Write-Host "`n`n[+] Login prompt detected!" -ForegroundColor Green
                Start-Sleep -Seconds 2
                
                # Send root login
                Write-Host "[*] Logging in as root..." -ForegroundColor Yellow
                $port.WriteLine("root")
                Start-Sleep -Seconds 3
                
                # Wait for password prompt or shell
                $passwordDeadline = [DateTime]::Now.AddSeconds(10)
                $passwordBuf = ""
                while ([DateTime]::Now -lt $passwordDeadline) {
                    if ($port.BytesToRead -gt 0) {
                        $chunk = $port.ReadExisting()
                        $passwordBuf += $chunk
                        Write-Host $chunk -NoNewline -ForegroundColor Gray
                    }
                    Start-Sleep -Milliseconds 100
                }
                
                # If password prompt, send default password
                if ($passwordBuf -match "Password:" -or $passwordBuf -match "password:") {
                    Write-Host "`n[*] Sending default password (1234)..." -ForegroundColor Yellow
                    $port.WriteLine("1234")
                    Start-Sleep -Seconds 3
                }
                
                # Wait for shell prompt
                Start-Sleep -Seconds 2
                
                # Remove boot.scr
                Write-Host "`n[*] Removing /boot/boot.scr..." -ForegroundColor Yellow
                $port.WriteLine("rm -f /boot/boot.scr")
                Start-Sleep -Seconds 1
                
                # Verify removal
                Write-Host "[*] Verifying removal..." -ForegroundColor Yellow
                $port.WriteLine("ls -la /boot/boot.scr")
                Start-Sleep -Seconds 1
                
                # Read response
                $verifyBuf = ""
                $verifyDeadline = [DateTime]::Now.AddSeconds(3)
                while ([DateTime]::Now -lt $verifyDeadline) {
                    if ($port.BytesToRead -gt 0) {
                        $chunk = $port.ReadExisting()
                        $verifyBuf += $chunk
                        Write-Host $chunk -NoNewline -ForegroundColor Gray
                    }
                    Start-Sleep -Milliseconds 100
                }
                
                if ($verifyBuf -match "No such file") {
                    Write-Host "`n[+] boot.scr successfully removed!" -ForegroundColor Green
                } else {
                    Write-Host "`n[!] boot.scr may still exist. Check manually." -ForegroundColor Yellow
                }
                
                # Reboot
                Write-Host "[*] Rebooting board..." -ForegroundColor Yellow
                $port.WriteLine("reboot")
                Start-Sleep -Seconds 2
                
                Write-Host "`n========================================" -ForegroundColor Green
                Write-Host "SUCCESS! Board is rebooting." -ForegroundColor Green
                Write-Host "========================================" -ForegroundColor Green
                Write-Host ""
                Write-Host "After reboot, run: .\deploy_tftp_and_boot.ps1" -ForegroundColor Yellow
                Write-Host "U-Boot should now have longer autoboot delay." -ForegroundColor Yellow
                Write-Host ""
                
                break
            }
        }
        Start-Sleep -Milliseconds 100
    }
    
    if (-not $loginSeen) {
        Write-Host "`n[!] Login prompt not detected within 180 seconds." -ForegroundColor Red
        Write-Host "    Board may not have booted properly." -ForegroundColor Red
    }
    
} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($port -and $port.IsOpen) {
        Start-Sleep -Seconds 3
        $port.Close()
        $port.Dispose()
        Write-Host "`n[!] Port closed." -ForegroundColor Yellow
    }
}
