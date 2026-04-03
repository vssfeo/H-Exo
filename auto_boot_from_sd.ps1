param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Auto Boot from SD" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

Write-Host "[*] REBOOT THE BOARD NOW!" -ForegroundColor Red
Write-Host "[*] Script will automatically interrupt autoboot and load kernel from SD" -ForegroundColor Yellow
Write-Host ""

$port = New-Object System.IO.Ports.SerialPort $PortName, $BaudRate, None, 8, One
$port.Handshake = [System.IO.Ports.Handshake]::None
$port.ReadTimeout = 1000
$port.WriteTimeout = 1000

try {
    $port.Open()
    Write-Host "[+] Serial port opened at $BaudRate baud" -ForegroundColor Green
    
    $buffer = ""
    $promptDetected = $false
    $commandsSent = $false
    
    while ($true) {
        if ($port.BytesToRead -gt 0) {
            $data = $port.ReadExisting()
            $buffer += $data
            Write-Host $data -NoNewline
            
            # Aggressively interrupt autoboot
            if ($buffer -match "Hit any key to stop autoboot" -or $buffer -match "autoboot") {
                Write-Host "`n[*] Interrupting autoboot..." -ForegroundColor Yellow
                for ($i = 0; $i -lt 20; $i++) {
                    $port.Write([char]3)  # Ctrl+C
                    $port.Write(" ")      # Space
                    Start-Sleep -Milliseconds 50
                }
            }
            
            # Detect U-Boot prompt
            if ($buffer -match "=>\s*$" -and -not $commandsSent) {
                $promptDetected = $true
                Write-Host "`n[+] U-Boot prompt detected!" -ForegroundColor Green
                Write-Host "[*] Sending boot commands..." -ForegroundColor Yellow
                
                Start-Sleep -Milliseconds 500
                
                # Send commands
                $port.WriteLine("mmc dev 1")
                Write-Host "[CMD] mmc dev 1" -ForegroundColor Cyan
                Start-Sleep -Seconds 1
                
                $port.WriteLine("mmc read 0x02080000 500000 28")
                Write-Host "[CMD] mmc read 0x02080000 500000 28" -ForegroundColor Cyan
                Start-Sleep -Seconds 2
                
                $port.WriteLine("go 0x02080000")
                Write-Host "[CMD] go 0x02080000" -ForegroundColor Cyan
                Start-Sleep -Seconds 1
                
                $commandsSent = $true
                Write-Host "[+] Boot commands sent!" -ForegroundColor Green
                Write-Host "[*] Kernel should be booting..." -ForegroundColor Yellow
                
                # Switch to 115200 for kernel
                Write-Host "[*] Switching to 115200 baud for kernel output..." -ForegroundColor Yellow
                $port.Close()
                Start-Sleep -Milliseconds 500
                
                $port = New-Object System.IO.Ports.SerialPort $PortName, 115200, None, 8, One
                $port.ReadTimeout = 1000
                $port.Open()
                Write-Host "[+] Now at 115200 baud" -ForegroundColor Green
                Write-Host ""
                $buffer = ""
            }
            
            # Detect kernel boot
            if ($buffer -match "H-Exo Omni-Core") {
                Write-Host "`n[!] H-Exo kernel detected!" -ForegroundColor Green
            }
            
            # Detect menu
            if ($buffer -match "Select mode") {
                Write-Host "`n[!] Menu detected - send '2' for Heartbeat Mode" -ForegroundColor Yellow
            }
            
            # Detect heartbeat
            if ($buffer -match "BEAT") {
                Write-Host "`n[!] HEARTBEAT ACTIVE!" -ForegroundColor Green
            }
        }
        Start-Sleep -Milliseconds 50
    }
    
} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
} finally {
    if ($null -ne $port -and $port.IsOpen) {
        $port.Close()
    }
}
