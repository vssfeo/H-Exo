# auto_test.ps1 - Полностью автоматизированный тест
# Вы нажимаете RESET -> я делаю всё остальное

param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [int]$TestDuration = 60
)

$ErrorActionPreference = "Stop"

# Логгер
$logBuffer = [System.Collections.ArrayList]::new()
function Write-TestLog {
    param([string]$Message, [string]$Level = "INFO")
    $timestamp = Get-Date -Format "HH:mm:ss.fff"
    $logEntry = "[$timestamp] [$Level] $Message"
    [void]$logBuffer.Add($logEntry)
    
    $color = switch ($Level) {
        "ERROR" { "Red" }
        "WARN"  { "Yellow" }
        "SUCCESS" { "Green" }
        "TEST"  { "Cyan" }
        default { "White" }
    }
    Write-Host $logEntry -ForegroundColor $color
}

# Загружаем baseline для регрессионного сравнения
$baselinePath = Join-Path $PSScriptRoot "baseline.json"
$baseline = $null
if (Test-Path $baselinePath) {
    $baseline = Get-Content $baselinePath | ConvertFrom-Json
    Write-Host "[+] Baseline loaded: v$($baseline.version) ($($baseline.metrics.kernel_size_bytes) bytes)" -ForegroundColor Cyan
}

# Парсеры
function Parse-Heartbeat {
    param([string]$line)
    if ($line -match "BEAT #([0-9A-F]+)\s+\|\s+Cycles:\s+([0-9A-F]+)\s+\|\s+Jitter:\s+([0-9A-F]+)%") {
        return @{
            BeatNum = [Convert]::ToInt64($matches[1], 16)
            Cycles = [Convert]::ToInt64($matches[2], 16)
            Jitter = [Convert]::ToInt32($matches[3], 16)
        }
    }
    return $null
}

# Результаты теста
$testResults = @{
    StartTime    = Get-Date
    BootSuccess  = $false
    Beacons      = @{}
    Heartbeat    = @{ Beats = 0; MaxJitter = 0; MinCycles = 0; MaxCycles = 0; Jitters = @() }
    Chaos        = @{ Active = $false; Injections = 0 }
    Perf         = @{ BootTimeUs = 0; InferenceUs = 0 }
    Sched        = @{ Hint = ''; Power = ''; Stability = 0; SessionStability = 0 }
    Crc          = @{ Actual = ''; Expected = ''; Match = $false }
    Errors       = @()
    KernelSizeBytes = if (Test-Path "C:\tftpboot\kernel_neuro.bin") { (Get-Item "C:\tftpboot\kernel_neuro.bin").Length } else { 0 }
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo AUTO TEST v2.0" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""
Write-Host "[!] Подготовка:" -ForegroundColor Yellow
Write-Host "    1. Запустите TFTP сервер в другом окне:" -ForegroundColor Gray
Write-Host "       .\tftp_server_robust.ps1" -ForegroundColor Cyan
Write-Host "    2. Нажмите RESET на плате" -ForegroundColor Yellow
Write-Host "    Я автоматически сделаю всё остальное..." -ForegroundColor Green
Write-Host ""

Read-Host "Нажмите Enter для начала..."

# Открываем порт
$serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout = 100
$serial.WriteTimeout = 1000
$serial.Open()

Write-TestLog "Serial port opened" "INFO"

try {
    Write-Host "`n=== ОЖИДАНИЕ RESET ===" -ForegroundColor Cyan
    Write-TestLog "Waiting for board..." "TEST"
    
    $buffer = ""
    $phase = "WAITING"
    $phaseStart = Get-Date
    
    while ($true) {
        try {
            if ($serial.BytesToRead -gt 0) {
                $ch = [char]$serial.ReadChar()
                $buffer += $ch
                if ($buffer.Length -gt 5000) { $buffer = $buffer.Substring(3000) }
                
                switch ($phase) {
                    "WAITING" {
                        if ($buffer -match "U-Boot 2022|DDR Version") {
                            Write-TestLog "Board detected!" "SUCCESS"
                            $phase = "UBOOT"
                            $buffer = ""
                            $pxeInterrupted = $false
                        }
                    }
                    "UBOOT" {
                        # Прерываем PXE только ОДИН раз
                        if (-not $pxeInterrupted -and $buffer -match "BOOTP|DHCP") {
                            $serial.Write([char]3)
                            Write-TestLog "Interrupted PXE boot" "INFO"
                            $pxeInterrupted = $true
                            Start-Sleep -Milliseconds 500
                        }
                        if ($buffer -match "=>\s*$") {
                            Write-TestLog "U-Boot ready" "SUCCESS"
                            $phase = "CONFIGURE"
                        }
                    }
                    "CONFIGURE" {
                        $localIP = (Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -match "^192\.168\." } | Select-Object -First 1).IPAddress

                        Write-TestLog "Configuring network ($localIP)..." "TEST"
                        $serial.DiscardInBuffer()
                        $serial.Write("setenv ipaddr 192.168.1.10`r")
                        Start-Sleep -Milliseconds 600
                        $serial.DiscardInBuffer()
                        $serial.Write("setenv serverip $localIP`r")
                        Start-Sleep -Milliseconds 600
                        $serial.DiscardInBuffer()
                        $serial.Write("setenv bootfile kernel_neuro.bin`r")
                        Start-Sleep -Milliseconds 600

                        Write-TestLog "Starting TFTP..." "TEST"
                        $serial.DiscardInBuffer()
                        $serial.Write("tftp 0x02080000 kernel_neuro.bin`r")
                        $phase = "TFTP"
                        $phaseStart = Get-Date
                        $buffer = ""
                    }
                    "TFTP" {
                        if ($buffer -match "Bytes transferred") {
                            Write-TestLog "TFTP complete!" "SUCCESS"
                            $serial.Write("go 0x02080000`r")
                            $phase = "BOOTING"
                            $buffer = ""
                        }
                        if ($buffer -match "Retry count exceeded") {
                            throw "TFTP failed - server not responding"
                        }
                        if ($buffer -match "Could not initialize PHY|TIMEOUT") {
                            throw "PHY TIMEOUT - проверьте Ethernet кабель NanoPi M4"
                        }
                        if (((Get-Date) - $phaseStart).TotalSeconds -gt 60) {
                            throw "TFTP timeout after 60 seconds"
                        }
                    }
                    "BOOTING" {
                        if ($buffer -match "\[BEACON\] (A|B|C|D|E)") {
                            $beacon = $matches[1]
                            $testResults.Beacons[$beacon] = $true
                            Write-TestLog "[BEACON $beacon]" "TEST"
                        }

                        if ($buffer -match "Actual CRC:\s+0x([0-9A-Fa-f]+)") {
                            $testResults.Crc.Actual = "0x$($matches[1])"
                        }
                        if ($buffer -match "Expected CRC:\s+0x([0-9A-Fa-f]+)") {
                            $testResults.Crc.Expected = "0x$($matches[1])"
                        }
                        if ($buffer -match "\[OK\] Neural weights integrity verified") {
                            $testResults.Crc.Match = $true
                            Write-TestLog "CRC verified: $($testResults.Crc.Actual)" "SUCCESS"
                        }
                        if ($buffer -match "\[WARN\] Neural weights CRC mismatch") {
                            $testResults.Crc.Match = $false
                            $testResults.Errors += "CRC mismatch: actual=$($testResults.Crc.Actual) expected=$($testResults.Crc.Expected)"
                            Write-TestLog "CRC MISMATCH! actual=$($testResults.Crc.Actual)" "FAIL"
                        }

                        if ($buffer -match "\[PERF\] boot_time_us=0x([0-9A-Fa-f]+)") {
                            $testResults.Perf.BootTimeUs = [Convert]::ToInt64($matches[1], 16)
                            Write-TestLog "boot_time=$($testResults.Perf.BootTimeUs) us" "TEST"
                        }
                        if ($buffer -match "\[PERF\] inference_us=0x([0-9A-Fa-f]+)") {
                            $testResults.Perf.InferenceUs = [Convert]::ToInt64($matches[1], 16)
                            Write-TestLog "inference=$($testResults.Perf.InferenceUs) us" "TEST"
                        }
                        if ($buffer -match "\[SCHED\] hint=(\S+) power=(\S+) trust=0x[0-9A-Fa-f]+ stability=0x([0-9A-Fa-f]+)") {
                            $testResults.Sched.Hint  = $matches[1]
                            $testResults.Sched.Power = $matches[2]
                            $testResults.Sched.Stability = [Convert]::ToInt32($matches[3], 16)
                            Write-TestLog "sched hint=$($testResults.Sched.Hint) stability=$($testResults.Sched.Stability)" "TEST"
                        }

                        if ($buffer -match "Operational") {
                            $testResults.BootSuccess = $true
                            Write-TestLog "KERNEL OPERATIONAL!" "SUCCESS"
                            $phase = "TESTING"
                            $phaseStart = Get-Date
                            $buffer = ""
                        }

                        if ($buffer -match "Synchronous Exception") {
                            throw "Kernel crashed"
                        }
                    }
                    "TESTING" {
                        $hb = Parse-Heartbeat -line $buffer
                        if ($hb) {
                            $testResults.Heartbeat.Beats = $hb.BeatNum
                            $testResults.Heartbeat.Jitters += $hb.Jitter
                            if ($hb.Jitter -gt $testResults.Heartbeat.MaxJitter) {
                                $testResults.Heartbeat.MaxJitter = $hb.Jitter
                            }
                            if ($testResults.Heartbeat.MinCycles -eq 0 -or $hb.Cycles -lt $testResults.Heartbeat.MinCycles) {
                                $testResults.Heartbeat.MinCycles = $hb.Cycles
                            }
                            if ($hb.Cycles -gt $testResults.Heartbeat.MaxCycles) {
                                $testResults.Heartbeat.MaxCycles = $hb.Cycles
                            }
                        }
                        
                        if ($buffer -match "\[CHAOS\]") {
                            $testResults.Chaos.Injections++
                            $testResults.Chaos.Active = $true
                        }
                        
                        if ($buffer -match "stability=0x([0-9A-Fa-f]+)") {
                            $testResults.Sched.SessionStability = [Convert]::ToInt32($matches[1], 16)
                        }

                        if (((Get-Date) - $phaseStart).TotalSeconds -gt $TestDuration) {
                            Write-TestLog "Test complete - sending 'q' to kernel" "TEST"
                            $serial.Write("q")
                            # Collect [SCHED] session summary before exiting
                            $endStart = Get-Date
                            while (((Get-Date) - $endStart).TotalMilliseconds -lt 1000) {
                                try {
                                    if ($serial.BytesToRead -gt 0) {
                                        $ch = [char]$serial.ReadChar()
                                        $buffer += $ch
                                        Write-Host -NoNewline $ch
                                        if ($buffer -match "stability=0x([0-9A-Fa-f]+)") {
                                            $testResults.Sched.SessionStability = [Convert]::ToInt32($matches[1], 16)
                                            Write-TestLog "session_stability=$($testResults.Sched.SessionStability)" "TEST"
                                        }
                                    } else { Start-Sleep -Milliseconds 10 }
                                } catch { break }
                            }
                            break
                        }
                    }
                }
            }
            Start-Sleep -Milliseconds 10
        } catch [System.TimeoutException] {
            continue
        }
    }
    
} finally {
    $serial.Close()
}

# === REPORT ===
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  TEST REPORT" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

$regressions = @()

# Boot check
$bootOk = $testResults.BootSuccess
Write-Host "Boot:         $(if ($bootOk) { 'PASS' } else { 'FAIL' })" -ForegroundColor $(if ($bootOk) { "Green" } else { "Red" })

# Beacons check
$beaconsOk = ($testResults.Beacons.Count -eq 5)
Write-Host "Beacons:      $($testResults.Beacons.Count)/5 $(if ($beaconsOk) { 'PASS' } else { 'PARTIAL' })" -ForegroundColor $(if ($beaconsOk) { "Green" } else { "Yellow" })

# Heartbeat check
$jitterOk = $testResults.Heartbeat.MaxJitter -le 5
Write-Host "Heartbeat:    $($testResults.Heartbeat.Beats) beats, jitter $($testResults.Heartbeat.MaxJitter)% $(if ($jitterOk) { 'PASS' } else { 'FAIL' })" -ForegroundColor $(if ($jitterOk) { "Green" } else { "Red" })
if (-not $jitterOk) { $regressions += "Jitter $($testResults.Heartbeat.MaxJitter)% > 5%" }

# Kernel size regression
if ($baseline -and $testResults.KernelSizeBytes -gt 0) {
    $maxSize = $baseline.regression_thresholds.max_kernel_size_bytes
    $sizeOk  = $testResults.KernelSizeBytes -le $maxSize
    Write-Host "Kernel size:  $($testResults.KernelSizeBytes) bytes (max $maxSize) $(if ($sizeOk) { 'PASS' } else { 'FAIL' })" -ForegroundColor $(if ($sizeOk) { "Green" } else { "Red" })
    if (-not $sizeOk) { $regressions += "Kernel size $($testResults.KernelSizeBytes) > $maxSize" }
}

# CRC check
if ($testResults.Crc.Actual -ne '') {
    $crcColor = if ($testResults.Crc.Match) { "Green" } else { "Red" }
    $crcStatus = if ($testResults.Crc.Match) { 'PASS' } else { 'FAIL' }
    Write-Host "CRC:          $($testResults.Crc.Actual) $crcStatus" -ForegroundColor $crcColor
    if (-not $testResults.Crc.Match) {
        $regressions += "CRC mismatch: actual=$($testResults.Crc.Actual) expected=$($testResults.Crc.Expected)"
    }
}

# PERF metrics
if ($testResults.Perf.BootTimeUs -gt 0) {
    Write-Host "boot_time_us: $($testResults.Perf.BootTimeUs) us" -ForegroundColor Cyan
}
if ($testResults.Perf.InferenceUs -gt 0) {
    Write-Host "inference_us: $($testResults.Perf.InferenceUs) us" -ForegroundColor Cyan
}
if ($testResults.Sched.Hint -ne '') {
    $stabColor = if ($testResults.Sched.Stability -ge 80) { "Green" } elseif ($testResults.Sched.Stability -ge 50) { "Yellow" } else { "Red" }
    Write-Host "Sched:        hint=$($testResults.Sched.Hint) power=$($testResults.Sched.Power) stability=$($testResults.Sched.Stability)" -ForegroundColor $stabColor
}

# Chaos
Write-Host "Chaos:        $($testResults.Chaos.Injections) injections" -ForegroundColor White

# Min/Max cycles
if ($testResults.Heartbeat.MinCycles -gt 0) {
    $jitterCycles = $testResults.Heartbeat.MaxCycles - $testResults.Heartbeat.MinCycles
    Write-Host "Cycle spread: $jitterCycles cycles ($([math]::Round($jitterCycles / 24.0, 2)) us)" -ForegroundColor Cyan
    if ($baseline -and $jitterCycles -gt $baseline.regression_thresholds.max_jitter_cycles) {
        $regressions += "Jitter cycles $jitterCycles > $($baseline.regression_thresholds.max_jitter_cycles)"
    }
}

$passed = $bootOk -and $beaconsOk -and $jitterOk -and ($regressions.Count -eq 0)

if ($regressions.Count -gt 0) {
    Write-Host "`n[!] Regressions detected:" -ForegroundColor Red
    $regressions | ForEach-Object { Write-Host "    - $_" -ForegroundColor Red }
}

Write-Host "`n========================================" -ForegroundColor $(if ($passed) { "Green" } else { "Red" })
Write-Host $(if ($passed) { "  PASS" } else { "  FAIL" }) -ForegroundColor $(if ($passed) { "Green" } else { "Red" })
Write-Host "========================================" -ForegroundColor $(if ($passed) { "Green" } else { "Red" })

# JSON export
$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$jsonResult = @{
    timestamp      = (Get-Date -Format 'o')
    version        = if ($baseline) { $baseline.version } else { "unknown" }
    passed         = $passed
    regressions    = $regressions
    kernel_size    = $testResults.KernelSizeBytes
    beacons        = $testResults.Beacons.Count
    heartbeat_beats = $testResults.Heartbeat.Beats
    max_jitter_pct = $testResults.Heartbeat.MaxJitter
    cycle_spread   = ($testResults.Heartbeat.MaxCycles - $testResults.Heartbeat.MinCycles)
    boot_time_us       = $testResults.Perf.BootTimeUs
    inference_us       = $testResults.Perf.InferenceUs
    sched_hint         = $testResults.Sched.Hint
    sched_power        = $testResults.Sched.Power
    sched_stability    = $testResults.Sched.Stability
    session_stability  = $testResults.Sched.SessionStability
    crc_actual         = $testResults.Crc.Actual
    crc_expected       = $testResults.Crc.Expected
    crc_match          = $testResults.Crc.Match
    chaos_injections   = $testResults.Chaos.Injections
} | ConvertTo-Json

$jsonFile = "test_result_$timestamp.json"
$jsonResult | Out-File $jsonFile -Encoding UTF8
$logBuffer | Out-File "auto_test_$timestamp.log"
Write-Host "`nResults: $jsonFile" -ForegroundColor Cyan
Write-Host "Log:     auto_test_$timestamp.log" -ForegroundColor Cyan
