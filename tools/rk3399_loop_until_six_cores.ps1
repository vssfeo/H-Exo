#Requires -Version 7
<#
.SYNOPSIS
  Цикл: прошить trust в SD через UART+TFTP (карта в NanoPi) → загрузить ядро по TFTP → go → ждать в UART успех SMP (6 ядер RK3399).

.DESCRIPTION
  Прямая запись на карту в ридере ПК здесь НЕ используется — карта должна стоять в плате; U-Boot пишет mmc.
  Условие успеха: строка ядра H-Exo "[OK] SMP: 0000000000000006 cores online" (uart_put_hex для u64).

  Ограничения: нужны питание платы, Ethernet, TFTP (UDP 69), COM. Скрипт не чинит железо бесконечно — есть -MaxCycles.

.PARAMETER ExpectedSmpHex
  16 hex-цифр (как печатает uart_put_hex). Для 6 ядер: 0000000000000006.
#>
param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [string]$TrustFile = "trust.img",
    [int]$MmcDev = 1,
    [int]$MaxCycles = 30,
    [int]$SleepBetweenSec = 25,
    [int]$UbootPromptWaitSec = 150,
    [int]$PostGoCaptureSec = 150,
    [string]$KernelFile = "kernel_neuro.bin",
    [string]$TftpDir = "C:\tftpboot",
    [string]$BoardIp = "192.168.1.10",
    [string]$ExpectedSmpHex = "0000000000000006",
    [switch]$NoFlashTrust,
    [switch]$NoKernelBoot,
    # SPL CheckImage Fail = неверный хеш BL31 в trust-контейнере; повторная прошивка тем же файлом не поможет.
    [switch]$IgnoreSplCheckImageFail
)

$ErrorActionPreference = "Continue"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$flashScript = Join-Path $repoRoot 'flash_bootloader_uboot.ps1'
$tftpStarter = Join-Path $repoRoot 'start_tftp_server_bg.ps1'

function Get-LocalIpArmbianStyle {
    $ip = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -match '^192\.168\.1\.' -and $_.PrefixOrigin -eq 'Dhcp' } |
        Select-Object -First 1).IPAddress
    if (-not $ip) {
        $ip = (Get-NetIPAddress -AddressFamily IPv4 |
            Where-Object { $_.IPAddress -match '^192\.168\.' } |
            Select-Object -First 1).IPAddress
    }
    return $ip
}

function Resolve-TrustPath {
    foreach ($c in @(
            (Join-Path 'C:\tftpboot' $TrustFile),
            (Join-Path $repoRoot $TrustFile),
            (Join-Path $repoRoot 'trust-bl31only-v2.14.img'),
            $TrustFile
        )) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    throw "Не найден trust: $TrustFile (положи в C:\tftpboot или корень репо)."
}

function Ensure-Tftp {
    $udp = Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue
    if ($udp) { return }
    if (Test-Path -LiteralPath $tftpStarter) {
        Write-Host "[*] Запуск фонового TFTP..." -ForegroundColor Yellow
        & $tftpStarter
        Start-Sleep -Seconds 2
    }
}

function Wait-UbootPrompt {
    param($serial, [int]$TimeoutSec, [switch]$BreakAutoboot)
    $buf = ""
    $start = Get-Date
    $lastBreak = Get-Date
    while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSec) {
        if ($BreakAutoboot -and ((Get-Date) - $lastBreak).TotalMilliseconds -gt 150) {
            try { $serial.Write(" "); $serial.Write("`r") } catch { }
            $lastBreak = Get-Date
        }
        try {
            while ($serial.BytesToRead -gt 0) {
                $ch = [char]$serial.ReadChar()
                $buf += $ch
                Write-Host -NoNewline $ch
                if ($buf.Length -gt 25000) { $buf = $buf.Substring($buf.Length - 12000) }
            }
            if ($buf -match '=>\s*$') { return @{ Ok = $true; Captured = $buf } }
        }
        catch { Start-Sleep -Milliseconds 30 }
        Start-Sleep -Milliseconds 30
    }
    return @{ Ok = $false; Captured = $buf }
}

function Send-Cmd {
    param($serial, [string]$cmd, [int]$waitMs = 600)
    Write-Host "`n[>] $cmd" -ForegroundColor Cyan
    try { $serial.DiscardInBuffer(); $serial.Write($cmd + "`r") } catch { }
    Start-Sleep -Milliseconds $waitMs
}

function Boot-KernelViaTftpAndCapture {
    param($serial, [string]$serverIp, [int]$captureSec, [string]$smpHex)

    $kernelLeaf = [System.IO.Path]::GetFileName($KernelFile)
    Send-Cmd $serial "setenv ipaddr $BoardIp"
    Send-Cmd $serial "setenv serverip $serverIp"
    Send-Cmd $serial "setenv bootfile $kernelLeaf"

    $serial.DiscardInBuffer()
    $serial.Write("tftp 0x02080000 $kernelLeaf`r")
    $tftpBuf = ""
    $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt 90) {
        try {
            while ($serial.BytesToRead -gt 0) {
                $c = [char]$serial.ReadChar()
                $tftpBuf += $c
                Write-Host -NoNewline $c
                if ($tftpBuf.Length -gt 4000) { $tftpBuf = $tftpBuf.Substring($tftpBuf.Length - 2000) }
            }
        }
        catch { }
        if ($tftpBuf -match '(?i)Bytes transferred') { break }
        if ($tftpBuf -match 'Retry count exceeded|DMA reset timeout|Could not initialize PHY') {
            throw "TFTP failed (PHY/сеть/сервер)."
        }
        Start-Sleep -Milliseconds 40
    }
    if ($tftpBuf -notmatch '(?i)Bytes transferred') { throw "TFTP timeout" }

    Send-Cmd $serial "dcache off" 800
    # Armbian U-Boot 2022.x часто без команды "icache" — не шлём, чтобы не засорять лог.
    $serial.Write("go 0x02080000`r")

    $big = ""
    $t1 = Get-Date
    while (((Get-Date) - $t1).TotalSeconds -lt $captureSec) {
        try {
            while ($serial.BytesToRead -gt 0) {
                $c = [char]$serial.ReadChar()
                $big += $c
                Write-Host -NoNewline $c
                if ($big.Length -gt 200000) { $big = $big.Substring($big.Length - 100000) }
            }
        }
        catch { }
        $okPat = "\[OK\] SMP:\s*$smpHex\s+cores online"
        if ($big -match $okPat) { return @{ Ok = $true; Text = $big } }
        if ($big -match 'bl31\.bin_0:CheckImage Fail|CheckImage Fail') {
            return @{ Ok = $false; Text = $big; Reason = 'spl-checkimage' }
        }
        if ($big -match 'A72_FAIL_STAGE|still ON_PENDING after timeout|LoadTrustBL error|No find trust') {
            return @{ Ok = $false; Text = $big; Reason = 'fail-marker' }
        }
        Start-Sleep -Milliseconds 50
    }
    return @{ Ok = $false; Text = $big; Reason = 'timeout' }
}

$trustResolved = Resolve-TrustPath
$serverIp = Get-LocalIpArmbianStyle
if (-not $serverIp) { throw "Не найден IPv4 192.168.x.x для TFTP serverip." }

$kernelPath = Join-Path $TftpDir $KernelFile
if (-not $NoKernelBoot -and -not (Test-Path -LiteralPath $kernelPath)) {
    throw "Нет ядра: $kernelPath"
}

$successPattern = "\[OK\] SMP:\s*$ExpectedSmpHex\s+cores online"
Write-Host "=== rk3399_loop_until_six_cores ===" -ForegroundColor Cyan
Write-Host "Trust: $trustResolved" -ForegroundColor Cyan
Write-Host "Success UART regex: $successPattern" -ForegroundColor Cyan
Write-Host "MaxCycles: $MaxCycles  serverip: $serverIp  mmc dev: $MmcDev" -ForegroundColor Cyan
Write-Host ""

Ensure-Tftp

for ($cycle = 1; $cycle -le $MaxCycles; $cycle++) {
    Write-Host "`n########################################" -ForegroundColor Magenta
    Write-Host "  CYCLE $cycle / $MaxCycles" -ForegroundColor Magenta
    Write-Host "########################################`n" -ForegroundColor Magenta

    if (-not $NoFlashTrust) {
        Write-Host "[*] UART: прошивка trust (TrustOnly)..." -ForegroundColor Yellow
        try {
            & $flashScript -TrustOnly -ForceWrite -PortName $PortName -BaudRate $BaudRate `
                -TrustFile $trustResolved -MmcDev $MmcDev
        }
        catch {
            Write-Host "[!] flash_bootloader_uboot.ps1: $_" -ForegroundColor Red
            Start-Sleep -Seconds $SleepBetweenSec
            continue
        }
        Write-Host "[*] Пауза после reset ($SleepBetweenSec с)..." -ForegroundColor DarkGray
        Start-Sleep -Seconds $SleepBetweenSec
    }

    if ($NoKernelBoot) {
        Write-Host "[!] -NoKernelBoot: проверка 6 ядер без запуска ядра невозможна — пропуск." -ForegroundColor Yellow
        Start-Sleep -Seconds $SleepBetweenSec
        continue
    }

    $serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate,
        [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 250
    $serial.WriteTimeout = 1500
    try {
        $serial.Open()
    }
    catch {
        Write-Host "[!] COM $PortName : $_" -ForegroundColor Red
        Start-Sleep -Seconds $SleepBetweenSec
        continue
    }

    try {
        Write-Host "[*] Ожидание U-Boot => (до $UbootPromptWaitSec с)..." -ForegroundColor Yellow
        $null = $serial.DiscardInBuffer()
        try { $serial.Write("`r") } catch { }
        $waitR = Wait-UbootPrompt -serial $serial -TimeoutSec $UbootPromptWaitSec -BreakAutoboot
        if (-not $waitR.Ok) {
            Write-Host "[!] Нет приглашения U-Boot." -ForegroundColor Red
            Start-Sleep -Seconds $SleepBetweenSec
            continue
        }
        if (($waitR.Captured -match 'CheckImage Fail') -and -not $IgnoreSplCheckImageFail) {
            Write-Host "`n[!] SPL: CheckImage Fail на BL31 в trust — хеш в контейнере не совпадает с телом образа." -ForegroundColor Red
            Write-Host "    Повторная прошивка ЭТИМ же trust.img не загрузит TF-A BL31; см. docs/rk3399/spl-trust-checkimage.md" -ForegroundColor Yellow
            exit 4
        }

        try {
            $r = Boot-KernelViaTftpAndCapture -serial $serial -serverIp $serverIp -captureSec $PostGoCaptureSec -smpHex $ExpectedSmpHex
        }
        catch {
            Write-Host "[!] Загрузка ядра: $_" -ForegroundColor Red
            Start-Sleep -Seconds $SleepBetweenSec
            continue
        }

        if ($r.Ok) {
            Write-Host "`n[OK] Условие выполнено: все 6 ядер (SMP line matched)." -ForegroundColor Green -BackgroundColor Black
            exit 0
        }
        if ($r.Reason -eq 'spl-checkimage' -and -not $IgnoreSplCheckImageFail) {
            Write-Host "`n[!] В логе ядра/SPL: CheckImage Fail — см. docs/rk3399/spl-trust-checkimage.md" -ForegroundColor Red
            exit 4
        }
        Write-Host "`n[!] Цикл $cycle без успеха: $($r.Reason)" -ForegroundColor Red
        if ($r.Text -match '\[OK\] SMP:') {
            Write-Host "    (в логе есть другая строка SMP — сравни с ожидаемым $ExpectedSmpHex)" -ForegroundColor DarkYellow
        }
    }
    finally {
        if ($serial.IsOpen) { $serial.Close() }
    }

    Start-Sleep -Seconds $SleepBetweenSec
}

Write-Host "`n[!] Исчерпан MaxCycles=$MaxCycles — остановка." -ForegroundColor Red
exit 2
