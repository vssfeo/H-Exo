#Requires -Version 7
<#
.SYNOPSIS
  Автоматическая цепочка: выбор SD → бэкап → запись trust (LBA 0x6000) → readback SHA256 → при провале откат из boot-backups → опционально UART reset и просмотр лога.

.DESCRIPTION
  НЕВОЗМОЖНО ИЗ КОДА (никакой «полной автономии до конца проекта»):
  - вынуть/вставить microSD, нажать питание, удерживать recovery;
  - обойти UAC Windows без подтверждения (сырой диск требует админа);
  - работать 24/7 без запущенного процесса/агента — скрипт делает один прогон, пока ты его не запустишь снова.

  ЧТО ДЕЛАЕТ АВТОМАТИЧЕСКИ за один запуск (админ + SD в USB-ридере):
  - выбор диска (одна USB/removable 1–128 GiB) или -DiskNumber;
  - если слот уже = эталонному trust — пропуск записи;
  - иначе write_rk3399_trust_to_sd (с бэкапом) → readback SHA256;
  - при несовпадении — откат последнего бэкапа и следующий раунд (до MaxRounds);
  - затем ожидание UART, при появлении U-Boot — reset, сбор лога на признаки ошибки SPL.

.PARAMETER TrustPath
  Образ trust.img (по умолчанию: C:\tftpboot\trust.img, иначе .\trust.img в корне репо).

.PARAMETER DiskNumber
  Явный номер диска (Get-Disk). Если -1 — авто: ровно одна USB/removable 1–128 GiB.

.PARAMETER MaxRounds
  Сколько циклов «запись → проверка → при провале откат последним бэкапом».

.PARAMETER InsertWaitSec
  После успешной верификации на ПК: сколько секунд ждать вставки SD в плату и появления UART (питание включите сами).

.PARAMETER UartWatchSec
  После reset: сколько секунд собирать вывод UART для эвристики успех/ошибка SPL.

.PARAMETER SkipUart
  Не открывать COM (только запись и проверка на ридере).

.EXAMPLE
  sudo pwsh .\tools\rk3399_trust_autonomous.ps1
#>
param(
    [string]$TrustPath = "",
    [int]$DiskNumber = -1,
    [string]$StartLba = "0x6000",
    [int]$MaxRounds = 3,
    [int]$InsertWaitSec = 180,
    [int]$UartWatchSec = 35,
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [switch]$SkipUart
)

$ErrorActionPreference = "Stop"

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Host "[!] Нужны права администратора (сырой диск + бэкап)." -ForegroundColor Red
    Write-Host "    Запуск: правый клик PowerShell 7 → Запуск от имени администратора, затем:" -ForegroundColor Yellow
    Write-Host "    Set-Location '$PSScriptRoot\..'; .\tools\rk3399_trust_autonomous.ps1" -ForegroundColor Gray
    exit 1
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$toolsDir = $PSScriptRoot
$backupDir = Join-Path $repoRoot 'boot-backups'

function Resolve-TrustSource {
    if ($TrustPath) {
        return (Resolve-Path -LiteralPath $TrustPath).Path
    }
    foreach ($c in @(
            'C:\tftpboot\trust.img',
            (Join-Path $repoRoot 'trust.img'),
            (Join-Path $repoRoot 'trust-bl31only-v2.14.img')
        )) {
        if (Test-Path -LiteralPath $c) { return (Resolve-Path -LiteralPath $c).Path }
    }
    throw "Не найден trust.img: укажи -TrustPath или положи файл в C:\tftpboot\ или в корень репо."
}

function Get-TargetDiskNumber {
    if ($DiskNumber -ge 0) {
        $d = Get-Disk -Number $DiskNumber
        if ($d.BusType -eq 'NVMe') { throw "Refusing DiskNumber $DiskNumber (NVMe)." }
        return $DiskNumber
    }
    $cand = @(Get-Disk | Where-Object {
            ($_.BusType -eq 'USB' -or $_.IsRemovable) -and $_.Size -gt 1GB -and $_.Size -lt 130GB -and $_.BusType -ne 'NVMe'
        })
    if ($cand.Count -eq 0) { throw "Нет кандидата SD: вставь USB-карту и повтори, или укажи -DiskNumber N (Get-Disk)." }
    if ($cand.Count -gt 1) {
        $cand | Format-Table Number, FriendlyName, @{N='GiB';E={[math]::Round($_.Size/1GB,2)}}, BusType -AutoSize
        throw "Несколько съёмных дисков — укажи явно -DiskNumber (НЕ системный NVMe)."
    }
    return [int]$cand[0].Number
}

function Get-LatestBackupPath {
    param([int]$N)
    if (-not (Test-Path -LiteralPath $backupDir)) { return $null }
    $f = Get-ChildItem -LiteralPath $backupDir -Filter "trust-dsk${N}-lba*.img" -File -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if ($f) { return $f.FullName }
    return $null
}

function Test-ReadbackMatches {
    param(
        [int]$DiskN,
        [string]$ExpectedHash,
        [string]$TempImg
    )
    & (Join-Path $toolsDir 'read_rk3399_trust_from_sd.ps1') -DiskNumber $DiskN -StartLba $StartLba -OutPath $TempImg -Quiet
    $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $TempImg).Hash
    return ($h.ToUpperInvariant() -eq $ExpectedHash.ToUpperInvariant())
}

function Invoke-UartPhase {
    param([int]$WaitBeforeResetSec, [int]$WatchSec)

    Write-Host "`n=== UART: вставь SD в M4, включи питание. Ожидание до $WaitBeforeResetSec с... ===" -ForegroundColor Yellow
    $serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate,
        [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 200
    $serial.WriteTimeout = 1000
    try {
        $serial.Open()
    }
    catch {
        Write-Host "[WARN] COM не открыть: $_ — пропуск UART." -ForegroundColor Yellow
        return
    }

    $deadline = (Get-Date).AddSeconds($WaitBeforeResetSec)
    $buf = ""
    while ((Get-Date) -lt $deadline) {
        try {
            while ($serial.BytesToRead -gt 0) {
                $buf += [char]$serial.ReadChar()
                if ($buf.Length -gt 20000) { $buf = $buf.Substring($buf.Length - 10000) }
            }
        }
        catch { }
        if ($buf -match "=>\s*$") {
            Write-Host "[*] U-Boot => — отправляю reset" -ForegroundColor Cyan
            try {
                $serial.DiscardInBuffer()
                $serial.Write("reset`r")
            }
            catch { }
            break
        }
        if ($buf -match "Hit any key to stop autoboot") {
            try { $serial.Write(" ") } catch { }
        }
        Start-Sleep -Milliseconds 400
    }

    Start-Sleep -Seconds 2
    $buf2 = $buf
    $end = (Get-Date).AddSeconds($WatchSec)
    while ((Get-Date) -lt $end) {
        try {
            while ($serial.BytesToRead -gt 0) {
                $buf2 += [char]$serial.ReadChar()
                if ($buf2.Length -gt 40000) { $buf2 = $buf2.Substring($buf2.Length - 20000) }
            }
        }
        catch { }
        Start-Sleep -Milliseconds 200
    }
    $serial.Close()

    Write-Host "`n--- UART (фрагмент, последние ~2k символов) ---" -ForegroundColor DarkGray
    $tail = if ($buf2.Length -gt 2000) { $buf2.Substring($buf2.Length - 2000) } else { $buf2 }
    Write-Host $tail

    if ($buf2 -match 'LoadTrustBL error|No find trust\.img') {
        Write-Host "`n[!] В логе есть ошибка загрузки trust (SPL). Проверь карту/питание/другой ридер; образ на ПК уже совпал с эталоном." -ForegroundColor Red
    }
    elseif ($buf2 -match 'U-Boot 20|NOTICE:\s+BL31|v2\.14') {
        Write-Host "`n[OK] По UART видны признаки загрузчика/BL31." -ForegroundColor Green
    }
}

# --- main ---
$src = Resolve-TrustSource
$refHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $src).Hash
$diskN = Get-TargetDiskNumber
$tempRb = Join-Path $env:TEMP ("rk3399-trust-rb-{0}.img" -f [Guid]::NewGuid().ToString('N').Substring(0, 8))

Write-Host "=== rk3399_trust_autonomous ===" -ForegroundColor Cyan
Write-Host "Trust: $src" -ForegroundColor Cyan
Write-Host "SHA256: $refHash" -ForegroundColor Cyan
Write-Host "DiskNumber: $diskN (PhysicalDrive$diskN)" -ForegroundColor Cyan
Write-Host "MaxRounds: $MaxRounds" -ForegroundColor Cyan

$round = 0
while ($round -lt $MaxRounds) {
    $round++
    Write-Host "`n--- Round $round / $MaxRounds ---" -ForegroundColor Magenta

    if (Test-ReadbackMatches -DiskN $diskN -ExpectedHash $refHash -TempImg $tempRb) {
        Write-Host "[OK] На SD уже лежит нужный trust (SHA256 совпал). Запись не нужна." -ForegroundColor Green
        if (-not $SkipUart) { Invoke-UartPhase -WaitBeforeResetSec $InsertWaitSec -WatchSec $UartWatchSec }
        Remove-Item -LiteralPath $tempRb -Force -ErrorAction SilentlyContinue
        exit 0
    }

    Write-Host "[*] Запись trust + встроенный бэкап предыдущего слота..." -ForegroundColor Yellow
    & (Join-Path $toolsDir 'write_rk3399_trust_to_sd.ps1') -TrustPath $src -DiskNumber $diskN -StartLba $StartLba -ForceWrite

    if (-not (Test-ReadbackMatches -DiskN $diskN -ExpectedHash $refHash -TempImg $tempRb)) {
        Write-Host "[!] Readback после записи НЕ совпал с эталоном." -ForegroundColor Red
        $bak = Get-LatestBackupPath -N $diskN
        if ($bak) {
            Write-Host "[*] Откат слота из бэкапа (состояние до последней записи): $bak" -ForegroundColor Yellow
            & (Join-Path $toolsDir 'write_rk3399_trust_to_sd.ps1') -TrustPath $bak -DiskNumber $diskN -StartLba $StartLba -ForceWrite
            & (Join-Path $toolsDir 'read_rk3399_trust_from_sd.ps1') -DiskNumber $diskN -StartLba $StartLba -OutPath $tempRb -Quiet | Out-Null
            $rh = (Get-FileHash -Algorithm SHA256 -LiteralPath $tempRb).Hash
            $bh = (Get-FileHash -Algorithm SHA256 -LiteralPath $bak).Hash
            if ($rh.ToUpperInvariant() -eq $bh.ToUpperInvariant()) {
                Write-Host "[OK] Откат проверен: readback = SHA256 бэкапа." -ForegroundColor Green
            }
            else {
                Write-Host "[!] После отката readback ($rh) != бэкап ($bh)." -ForegroundColor Red
            }
        }
        else {
            Write-Host "[!] Нет файла в boot-backups для отката." -ForegroundColor Red
        }
        continue
    }

    Write-Host "[OK] Запись и readback SHA256 совпадают с эталоном." -ForegroundColor Green
    if (-not $SkipUart) { Invoke-UartPhase -WaitBeforeResetSec $InsertWaitSec -WatchSec $UartWatchSec }
    Remove-Item -LiteralPath $tempRb -Force -ErrorAction SilentlyContinue
    exit 0
}

Remove-Item -LiteralPath $tempRb -Force -ErrorAction SilentlyContinue
Write-Host "`n[!] Исчерпаны раунды. Проверь диск/карту/адаптер." -ForegroundColor Red
exit 2
