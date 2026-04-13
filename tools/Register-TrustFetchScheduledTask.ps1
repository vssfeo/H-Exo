#Requires -Version 7
<#
.SYNOPSIS
  Регистрирует задачу планировщика Windows: периодически качает свежий trust.img с GitHub в C:\tftpboot.

.DESCRIPTION
  Один раз запускаешь от администратора — дальше обновление артефакта без открытия браузера.
  Прошивка SD/платы этим НЕ делается (см. rk3399_trust_autonomous.ps1 вручную или по другой задаче).

.PARAMETER TaskName
.PARAMETER DailyAt
  Локальное время суток "HH:mm".
#>
param(
    [string]$TaskName = "H-Exo-FetchTrustFromGitHub",
    [string]$DailyAt = "04:30",
    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"
if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "Запусти от администратора (регистрация Scheduled Task)."
}

if (-not $RepoRoot) {
    $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
}
$fetchScript = Join-Path $RepoRoot 'tools\fetch_latest_trust_artifact.ps1'
if (-not (Test-Path -LiteralPath $fetchScript)) { throw "Не найден: $fetchScript" }

$pwsh = (Get-Command pwsh -ErrorAction Stop).Source
$arg = "-NoProfile -ExecutionPolicy Bypass -File `"$fetchScript`" -OutDir C:\tftpboot"

Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue

$action = New-ScheduledTaskAction -Execute $pwsh -Argument $arg
$parts = $DailyAt -split ':'
$h = [int]$parts[0]; $m = [int]$parts[1]
$trigger = New-ScheduledTaskTrigger -Daily -At ([DateTime]::Today.AddHours($h).AddMinutes($m))
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -StartWhenAvailable
Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings `
    -Description "H-Exo: download latest rk3399 trust.img from GitHub Actions to C:\tftpboot" `
    -User $env:USERNAME -RunLevel Highest | Out-Null

Write-Host "[OK] Задача '$TaskName' — ежедневно в $DailyAt, pwsh -> fetch_latest_trust_artifact.ps1" -ForegroundColor Green
Write-Host "     Для приватного репо: в настройках задачи добавь переменную GITHUB_TOKEN или залогинь gh под этим пользователем." -ForegroundColor Yellow
Write-Host "     Проверка вручную: Start-ScheduledTask -TaskName '$TaskName'" -ForegroundColor DarkGray
