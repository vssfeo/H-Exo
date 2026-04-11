$ErrorActionPreference = "Stop"

$logDir = Join-Path $PSScriptRoot "logs"
$pidFile = Join-Path $logDir "tftp-server.pid"

$stopped = $false

if (Test-Path $pidFile) {
    $pidText = (Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1).Trim()
    if ($pidText) {
        $proc = Get-Process -Id ([int]$pidText) -ErrorAction SilentlyContinue
        if ($proc) {
            Stop-Process -Id $proc.Id -Force
            Write-Host "Stopped TFTP server PID=$($proc.Id)." -ForegroundColor Green
            $stopped = $true
        }
    }
    Remove-Item $pidFile -Force -ErrorAction SilentlyContinue
}

Get-NetUDPEndpoint -LocalPort 69 -ErrorAction SilentlyContinue |
    ForEach-Object {
        $proc = Get-Process -Id $_.OwningProcess -ErrorAction SilentlyContinue
        if ($proc -and $proc.ProcessName -match 'powershell|pwsh') {
            Stop-Process -Id $proc.Id -Force
            Write-Host "Stopped UDP:69 listener PID=$($proc.Id)." -ForegroundColor Green
            $stopped = $true
        }
    }

if (-not $stopped) {
    Write-Host "No background TFTP server found." -ForegroundColor Yellow
}
