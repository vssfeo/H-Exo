# Automation for TFTP transfer and Kernel Boot
param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [string]$ServerIP = "192.168.1.166",
    [string]$DeviceIP = "192.168.1.10",
    [int]$OpenRetrySeconds = 25,
    [int]$MonitorSeconds = 90,
    [switch]$AssumePrompt
)

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo TFTP Deploy & Boot Automation" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

$port = $null

function Open-PortWithRetry {
    param(
        [string]$Name,
        [int]$Rate,
        [int]$RetrySeconds
    )

    $deadline = [DateTime]::Now.AddSeconds($RetrySeconds)
    while ([DateTime]::Now -lt $deadline) {
        try {
            $p = New-Object System.IO.Ports.SerialPort $Name, $Rate, None, 8, One
            $p.Handshake = [System.IO.Ports.Handshake]::None
            $p.ReadTimeout = 1500
            $p.WriteTimeout = 1000
            $p.Open()
            return $p
        } catch {
            Start-Sleep -Milliseconds 400
        }
    }

    throw "Cannot open $Name for $RetrySeconds sec. Port is busy."
}

function Read-UntilPrompt {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [int]$TimeoutSeconds
    )

    $buf = ""
    $lastKick = [DateTime]::MinValue
    $lastBurst = [DateTime]::MinValue
    $autobootSeen = $false
    $deadline = [DateTime]::Now.AddSeconds($TimeoutSeconds)
    while ([DateTime]::Now -lt $deadline) {
        if ((([DateTime]::Now - $lastKick).TotalMilliseconds -ge 220)) {
            $Serial.Write([char]13)
            $lastKick = [DateTime]::Now
        }

        if ((([DateTime]::Now - $lastBurst).TotalSeconds -ge 2)) {
            $Serial.Write([char]3)
            Start-Sleep -Milliseconds 60
            $Serial.Write([char]13)
            $lastBurst = [DateTime]::Now
        }

        if ($Serial.BytesToRead -gt 0) {
            $chunk = $Serial.ReadExisting()
            $buf += $chunk
            Write-Host $chunk -NoNewline

            if ($buf -match "Hit any key to stop autoboot") {
                $autobootSeen = $true
                for ($i = 0; $i -lt 8; $i++) {
                    $Serial.Write([char]13)
                    Start-Sleep -Milliseconds 70
                }
            }

            if ($buf -match "Executing script" -or $buf -match "Boot script loaded" -or $buf -match "Retrieving file:" -or $buf -match "pxelinux") {
                for ($i = 0; $i -lt 5; $i++) {
                    $Serial.Write([char]3)
                    Start-Sleep -Milliseconds 50
                }
                $Serial.Write([char]13)
            }

            if ($buf -match "=>" -or $buf -match "U-Boot>") {
                return $true
            }

            if ($autobootSeen -and $buf -match "U-Boot") {
                $Serial.Write([char]13)
            }
        }
        Start-Sleep -Milliseconds 25
    }

    return $false
}

function Probe-UbootPrompt {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [int]$TimeoutSeconds
    )

    $buf = ""
    $deadline = [DateTime]::Now.AddSeconds($TimeoutSeconds)
    while ([DateTime]::Now -lt $deadline) {
        $Serial.Write([char]13)
        Start-Sleep -Milliseconds 120

        if ($Serial.BytesToRead -gt 0) {
            $chunk = $Serial.ReadExisting()
            $buf += $chunk
            Write-Host $chunk -NoNewline
            if ($buf -match "=>" -or $buf -match "U-Boot>") {
                return $true
            }
        }
        Start-Sleep -Milliseconds 120
    }

    return $false
}

function Send-UbootCommand {
    param(
        [System.IO.Ports.SerialPort]$Serial,
        [string]$Command,
        [int]$TimeoutSeconds
    )

    Write-Host "`n[>] $Command" -ForegroundColor White
    $Serial.WriteLine($Command)

    $buf = ""
    $deadline = [DateTime]::Now.AddSeconds($TimeoutSeconds)
    while ([DateTime]::Now -lt $deadline) {
        if ($Serial.BytesToRead -gt 0) {
            $chunk = $Serial.ReadExisting()
            $buf += $chunk
            Write-Host $chunk -NoNewline
            if ($buf -match "=>" -or $buf -match "U-Boot>") {
                return $true
            }
        }
        Start-Sleep -Milliseconds 30
    }

    return $false
}

try {
    $baudCandidates = @($BaudRate, 115200) | Select-Object -Unique
    $promptDetected = $false

    foreach ($candidateBaud in $baudCandidates) {
        Write-Host "[*] Opening $PortName at $candidateBaud baud..." -ForegroundColor Yellow
        $port = Open-PortWithRetry -Name $PortName -Rate $candidateBaud -RetrySeconds $OpenRetrySeconds
        Write-Host "[+] Port opened successfully at $candidateBaud." -ForegroundColor Green

        $port.DiscardInBuffer()
        $port.DiscardOutBuffer()

        if ($AssumePrompt) {
            Write-Host "[*] AssumePrompt mode: probing for existing U-Boot prompt at $candidateBaud..." -ForegroundColor Yellow
            $promptDetected = Probe-UbootPrompt -Serial $port -TimeoutSeconds 12
        } else {
            Write-Host "[*] REBOOT BOARD NOW (power cycle). Waiting U-Boot prompt at $candidateBaud..." -ForegroundColor Yellow
            $promptDetected = Read-UntilPrompt -Serial $port -TimeoutSeconds 75
        }

        if ($promptDetected) {
            Write-Host "`n[+] U-Boot prompt detected at $candidateBaud baud." -ForegroundColor Green
            break
        }

        Write-Host "`n[!] Prompt not detected at $candidateBaud baud, trying next speed..." -ForegroundColor Yellow
        $port.Close()
        $port.Dispose()
        $port = $null
    }

    if (-not $promptDetected) {
        throw "U-Boot prompt not detected on 1500000/115200. Manually stop at autoboot once, then run: .\\deploy_tftp_and_boot.ps1 -AssumePrompt"
    }

    $commands = @(
        "setenv ipaddr $DeviceIP",
        "setenv serverip $ServerIP",
        "setenv bootfile kernel_neuro.bin",
        "setenv loadaddr 0x42000000",
        "setenv sdwrite 'tftp `${loadaddr} `${bootfile}; mmc dev 0; mmc write `${loadaddr} 0x800 0x1000'",
        "saveenv",
        "run sdwrite",
        "mmc dev 0",
        "mmc read 0x02080000 0x800 0x28",
        "go 0x02080000"
    )

    foreach ($cmd in $commands) {
        $timeoutSec = 15
        if ($cmd -eq "run sdwrite") { $timeoutSec = 80 }
        if ($cmd -match "go 0x") { $timeoutSec = 3 }

        $ok = Send-UbootCommand -Serial $port -Command $cmd -TimeoutSeconds $timeoutSec

        if (-not $ok -and -not ($cmd -match "go 0x")) {
            throw "Command timeout: $cmd"
        }

        if ($cmd -match "go 0x") {
            Write-Host "`n[*] Kernel started! Monitoring for beacons..." -ForegroundColor Yellow
            break
        }
    }

    Write-Host "`n[*] Monitoring output for $MonitorSeconds sec..." -ForegroundColor Cyan
    $monitorDeadline = [DateTime]::Now.AddSeconds($MonitorSeconds)
    while ([DateTime]::Now -lt $monitorDeadline) {
        if ($port.BytesToRead -gt 0) {
            Write-Host $port.ReadExisting() -NoNewline
        }
        Start-Sleep -Milliseconds 20
    }

    Write-Host "`n[+] Monitor window ended." -ForegroundColor Green
    exit 0

} catch {
    Write-Host "`n[ERROR] $($_.Exception.Message)" -ForegroundColor Red
    exit 1
} finally {
    if ($port -and $port.IsOpen) {
        $port.Close()
        $port.Dispose()
        Write-Host "`n[!] Port closed." -ForegroundColor Yellow
    }
}
