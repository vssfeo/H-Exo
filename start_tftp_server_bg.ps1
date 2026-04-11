$ErrorActionPreference = "Stop"

$serverScript = Join-Path $PSScriptRoot "ps_tftp_server.ps1"
$logDir = Join-Path $PSScriptRoot "logs"
$pidFile = Join-Path $logDir "tftp-server.pid"
$outFile = Join-Path $logDir "tftp-server.out.log"
$errFile = Join-Path $logDir "tftp-server.err.log"

if (-not (Test-Path $serverScript)) {
    throw "Не найден серверный скрипт: $serverScript"
}

if (-not (Test-Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir -Force | Out-Null
}

$udp69 = Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue
if ($udp69) {
    Write-Host "TFTP server already listening on UDP 69." -ForegroundColor Green
    $udp69 | Select-Object -First 1 | ForEach-Object {
        Write-Host ("PID={0} LocalAddress={1}:69" -f $_.OwningProcess, $_.LocalAddress) -ForegroundColor DarkGreen
    }
    exit 0
}

$argList = @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$serverScript`""
)

$proc = Start-Process powershell `
    -ArgumentList $argList `
    -WindowStyle Hidden `
    -RedirectStandardOutput $outFile `
    -RedirectStandardError $errFile `
    -PassThru

Set-Content -Path $pidFile -Value $proc.Id -NoNewline
Start-Sleep -Seconds 2

$udp69 = Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue
if ($udp69) {
    Write-Host "TFTP server started in background." -ForegroundColor Green
    Write-Host ("PID={0}" -f $proc.Id) -ForegroundColor Cyan
    Write-Host ("stdout: {0}" -f $outFile) -ForegroundColor DarkGray
    Write-Host ("stderr: {0}" -f $errFile) -ForegroundColor DarkGray
    exit 0
}

try {
    $proc.Refresh()
} catch {}

if ($proc -and -not $proc.HasExited) {
    Write-Host "TFTP server process is running; UDP 69 visibility is delayed." -ForegroundColor Yellow
    Write-Host ("PID={0}" -f $proc.Id) -ForegroundColor Cyan
    Write-Host ("stdout: {0}" -f $outFile) -ForegroundColor DarkGray
    Write-Host ("stderr: {0}" -f $errFile) -ForegroundColor DarkGray
    exit 0
}

Write-Host "TFTP server failed to start." -ForegroundColor Red
Write-Host ("stdout: {0}" -f $outFile) -ForegroundColor DarkGray
Write-Host ("stderr: {0}" -f $errFile) -ForegroundColor DarkGray
exit 1
