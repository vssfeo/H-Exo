# test_universal.ps1 - Универсальный тест H-Exo Omni-Core
# Проверяет все режимы: Neural Arbitrator, Heartbeat, Chaos, Telemetry

param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [int]$TestDuration = 120,  # Общее время теста в секундах
    [switch]$AutoDeploy = $true
)

$ErrorActionPreference = "Stop"

# Конфигурация теста
$TEST_CONFIG = @{
    NeuralArbitratorIterations = 5
    HeartbeatDuration = 30
    ChaosEnabled = $true
    CollectTelemetry = $true
}

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Universal Test Suite" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "Port: $PortName @ $BaudRate baud" -ForegroundColor Gray
Write-Host "Duration: $TestDuration seconds" -ForegroundColor Gray
Write-Host ""

# Статистика теста
$testResults = @{
    StartTime = Get-Date
    NeuralArbitrator = @{ Passed = $false; Inferences = 0 }
    Heartbeat = @{ Passed = $false; Beats = 0; MaxJitter = 0; AvgJitter = 0 }
    Chaos = @{ Passed = $false; Injections = 0 }
    Telemetry = @{ Passed = $false; Samples = @() }
    Errors = @()
}

# Открываем порт
$serial = $null
try {
    $serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 100
    $serial.WriteTimeout = 1000
    $serial.Open()
    Write-Host "[+] Serial port opened" -ForegroundColor Green
} catch {
    Write-Error "Failed to open $PortName : $_"
    exit 1
}

# Функция для чтения с таймаутом
function Read-SerialWithTimeout {
    param([int]$TimeoutMs = 5000)
    $buffer = ""
    $start = Get-Date
    while (((Get-Date) - $start).TotalMilliseconds -lt $TimeoutMs) {
        if ($serial.BytesToRead -gt 0) {
            try {
                $ch = [char]$serial.ReadChar()
                $buffer += $ch
                Write-Host -NoNewline $ch
            } catch {}
        }
        Start-Sleep -Milliseconds 10
    }
    return $buffer
}

# Функция для отправки команды
function Send-Command {
    param([string]$Command, [int]$WaitMs = 500)
    $serial.Write($Command)
    Write-Host "`n[>] Sent: '$Command'" -ForegroundColor Green
    Start-Sleep -Milliseconds $WaitMs
}

# Функция парсинга heartbeat
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

# Функция парсинга telemetry
function Parse-Telemetry {
    param([string]$line)
    $telemetry = @{}
    
    # CPU Load
    if ($line -match "CPU Load:\s+(\d+)%") {
        $telemetry.CPULoad = [int]$matches[1]
    }
    # L2 Latency
    if ($line -match "L2 Latency:\s+(\d+)\s*us") {
        $telemetry.L2Latency = [int]$matches[1]
    }
    # Memory Pressure
    if ($line -match "Memory:\s+(\d+)%") {
        $telemetry.MemoryPressure = [int]$matches[1]
    }
    # Thermal
    if ($line -match "Thermal:\s+(\d+)%") {
        $telemetry.Thermal = [int]$matches[1]
    }
    # Inference Result
    if ($line -match "Task Priority:\s+(\d+)") {
        $telemetry.TaskPriority = [int]$matches[1]
    }
    if ($line -match "Migration Hint:\s+(\w+)") {
        $telemetry.MigrationHint = $matches[1]
    }
    
    return $telemetry
}

try {
    # Если нужно автоматически развернуть ядро
    if ($AutoDeploy) {
        Write-Host "[*] Auto-deploy not implemented - kernel should be loaded" -ForegroundColor Yellow
        Write-Host "[*] Waiting for kernel boot..." -ForegroundColor Yellow
        Start-Sleep -Seconds 3
    }
    
    # Ждем загрузки ядра
    Write-Host "`n=== PHASE 1: Kernel Boot Detection ===" -ForegroundColor Cyan
    $bootDetected = $false
    $bootBuffer = ""
    $bootTimeout = 30
    $bootStart = Get-Date
    
    while (-not $bootDetected -and ((Get-Date) - $bootStart).TotalSeconds -lt $bootTimeout) {
        $line = Read-SerialWithTimeout -TimeoutMs 1000
        $bootBuffer += $line
        
        if ($bootBuffer -match "H-Exo Omni-Core: Operational") {
            $bootDetected = $true
            Write-Host "`n[+] Kernel booted successfully!" -ForegroundColor Green
        }
    }
    
    if (-not $bootDetected) {
        Write-Warning "Kernel boot not confirmed, but continuing..."
    }
    
    # Даем время стабилизироваться
    Start-Sleep -Seconds 2
    
    # === PHASE 2: Neural Arbitrator Test ===
    Write-Host "`n=== PHASE 2: Neural Arbitrator Test ===" -ForegroundColor Cyan
    Write-Host "[*] Running $TEST_CONFIG.NeuralArbitratorIterations inferences..." -ForegroundColor Yellow
    
    # Отправляем SPACE для запуска inference
    for ($i = 0; $i -lt $TEST_CONFIG.NeuralArbitratorIterations; $i++) {
        Send-Command " " 1000
        
        # Собираем вывод
        $infBuffer = ""
        $infStart = Get-Date
        while (((Get-Date) - $infStart).TotalSeconds -lt 2) {
            $line = Read-SerialWithTimeout -TimeoutMs 100
            $infBuffer += $line
        }
        
        # Парсим результаты
        if ($infBuffer -match "NEURAL INFERENCE|Task Priority") {
            $testResults.NeuralArbitrator.Inferences++
            $telemetry = Parse-Telemetry -line $infBuffer
            if ($telemetry.Count -gt 0) {
                $testResults.Telemetry.Samples += $telemetry
            }
        }
    }
    
    if ($testResults.NeuralArbitrator.Inferences -gt 0) {
        $testResults.NeuralArbitrator.Passed = $true
        Write-Host "[+] Neural Arbitrator: PASSED ($($testResults.NeuralArbitrator.Inferences) inferences)" -ForegroundColor Green
    } else {
        Write-Host "[-] Neural Arbitrator: FAILED" -ForegroundColor Red
    }
    
    # === PHASE 3: Heartbeat Test ===
    Write-Host "`n=== PHASE 3: Heartbeat Stability Test ===" -ForegroundColor Cyan
    Write-Host "[*] Starting heartbeat test for $TEST_CONFIG.HeartbeatDuration seconds..." -ForegroundColor Yellow
    
    # Отправляем '2' для heartbeat mode
    Send-Command "2" 500
    
    $heartbeatStart = Get-Date
    $jitterValues = @()
    $beatCount = 0
    
    while (((Get-Date) - $heartbeatStart).TotalSeconds -lt $TEST_CONFIG.HeartbeatDuration) {
        $line = Read-SerialWithTimeout -TimeoutMs 100
        
        $hb = Parse-Heartbeat -line $line
        if ($hb) {
            $beatCount++
            $jitterValues += $hb.Jitter
            $testResults.Heartbeat.Beats = $hb.BeatNum
        }
        
        # Проверяем chaos injections
        if ($line -match "\[CHAOS\] Injecting instability") {
            $testResults.Chaos.Injections++
        }
        
        # Выход из heartbeat по 'q'
        if ([Console]::KeyAvailable) {
            $key = [Console]::ReadKey($true)
            if ($key.KeyChar -eq 'q') {
                Send-Command "q" 100
                break
            }
            if ($key.KeyChar -eq 'C' -and $TEST_CONFIG.ChaosEnabled) {
                Send-Command "C" 100
                Write-Host "`n[*] Chaos mode enabled" -ForegroundColor Magenta
            }
        }
    }
    
    # Вычисляем статистику jitter
    if ($jitterValues.Count -gt 0) {
        $testResults.Heartbeat.MaxJitter = ($jitterValues | Measure-Object -Maximum).Maximum
        $testResults.Heartbeat.AvgJitter = ($jitterValues | Measure-Object -Average).Average
        
        # PASS если jitter < 5%
        $testResults.Heartbeat.Passed = ($testResults.Heartbeat.MaxJitter -le 5)
        
        Write-Host "`n--- Heartbeat Statistics ---" -ForegroundColor Cyan
        Write-Host "Total beats: $($testResults.Heartbeat.Beats)" -ForegroundColor White
        Write-Host "Max jitter: $($testResults.Heartbeat.MaxJitter)%" -ForegroundColor $(if ($testResults.Heartbeat.MaxJitter -le 5) { "Green" } else { "Red" })
        Write-Host "Avg jitter: $([Math]::Round($testResults.Heartbeat.AvgJitter, 2))%" -ForegroundColor White
        Write-Host "Chaos injections: $($testResults.Chaos.Injections)" -ForegroundColor White
        
        if ($testResults.Heartbeat.Passed) {
            Write-Host "[+] Heartbeat: PASSED" -ForegroundColor Green
        } else {
            Write-Host "[-] Heartbeat: FAILED (jitter > 5%)" -ForegroundColor Red
        }
    } else {
        Write-Host "[-] Heartbeat: NO DATA" -ForegroundColor Red
    }
    
    if ($testResults.Chaos.Injections -gt 0) {
        $testResults.Chaos.Passed = $true
    }
    
    # === PHASE 4: Echo Mode Test ===
    Write-Host "`n=== PHASE 4: Echo Mode Test ===" -ForegroundColor Cyan
    Write-Host "[*] Testing echo mode..." -ForegroundColor Yellow
    
    # Отправляем 'q' для выхода в echo mode
    Send-Command "q" 500
    
    # Тестируем echo
    $testString = "TEST123"
    foreach ($char in $testString.ToCharArray()) {
        Send-Command $char 50
    }
    
    Start-Sleep -Milliseconds 500
    $echoBuffer = ""
    $echoStart = Get-Date
    while (((Get-Date) - $echoStart).TotalMilliseconds -lt 1000) {
        $line = Read-SerialWithTimeout -TimeoutMs 100
        $echoBuffer += $line
    }
    
    if ($echoBuffer -match $testString) {
        Write-Host "[+] Echo mode: PASSED" -ForegroundColor Green
    } else {
        Write-Host "[-] Echo mode: FAILED (expected '$testString')" -ForegroundColor Red
    }
    
} catch {
    Write-Error "Test error: $_"
    $testResults.Errors += $_.ToString()
} finally {
    if ($serial -and $serial.IsOpen) {
        $serial.Close()
    }
}

# === TEST SUMMARY ===
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  TEST SUMMARY" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

$endTime = Get-Date
$duration = ($endTime - $testResults.StartTime).TotalSeconds

Write-Host "Total duration: $([Math]::Round($duration, 1)) seconds" -ForegroundColor White
Write-Host ""

# Результаты по модулям
$allPassed = $true

Write-Host "Neural Arbitrator: " -NoNewline
if ($testResults.NeuralArbitrator.Passed) {
    Write-Host "PASS" -ForegroundColor Green
} else {
    Write-Host "FAIL" -ForegroundColor Red
    $allPassed = $false
}

Write-Host "Heartbeat:         " -NoNewline
if ($testResults.Heartbeat.Passed) {
    Write-Host "PASS (max jitter: $($testResults.Heartbeat.MaxJitter)%)" -ForegroundColor Green
} else {
    Write-Host "FAIL (max jitter: $($testResults.Heartbeat.MaxJitter)%)" -ForegroundColor Red
    $allPassed = $false
}

Write-Host "Chaos Mode:        " -NoNewline
if ($testResults.Chaos.Passed) {
    Write-Host "PASS ($($testResults.Chaos.Injections) injections)" -ForegroundColor Green
} else {
    Write-Host "N/A" -ForegroundColor Yellow
}

Write-Host "Telemetry:         " -NoNewline
if ($testResults.Telemetry.Samples.Count -gt 0) {
    Write-Host "PASS ($($testResults.Telemetry.Samples.Count) samples)" -ForegroundColor Green
} else {
    Write-Host "N/A" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "========================================" -ForegroundColor $(if ($allPassed) { "Green" } else { "Red" })
if ($allPassed) {
    Write-Host "  ALL TESTS PASSED!" -ForegroundColor Green
} else {
    Write-Host "  SOME TESTS FAILED" -ForegroundColor Red
}
Write-Host "========================================" -ForegroundColor $(if ($allPassed) { "Green" } else { "Red" })

# JSON экспорт результатов
$jsonResults = $testResults | ConvertTo-Json -Depth 5
$resultsFile = "test_results_$(Get-Date -Format 'yyyyMMdd_HHmmss').json"
$jsonResults | Out-File $resultsFile
Write-Host "`nResults saved to: $resultsFile" -ForegroundColor Cyan
