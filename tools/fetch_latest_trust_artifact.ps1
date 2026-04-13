#Requires -Version 7
<#
.SYNOPSIS
  Скачивает trust.img из последнего успешного прогона workflow RK3399 BL31 на GitHub (без ручного браузера).

.DESCRIPTION
  Требуется один из вариантов аутентификации GitHub API:
    - установленный и залогиненный `gh` (gh auth login — один раз);
    - или переменная окружения GITHUB_TOKEN / GH_TOKEN (classic: repo для приватного репо).

  Не прошивает плату и не трогает диск — только кладёт файлы в -OutDir.

.PARAMETER OutDir
  Куда положить trust.img (по умолчанию C:\tftpboot).
#>
param(
    [string]$Owner = "",
    [string]$Repo = "",
    [string]$WorkflowFile = "rk3399-bl31-trust.yml",
    [string]$OutDir = "C:\tftpboot",
    [switch]$WhatIf
)

$ErrorActionPreference = "Stop"

function Get-RepoFromGit {
    $remote = ""
    try {
        Push-Location (Resolve-Path (Join-Path $PSScriptRoot '..'))
        $remote = (git remote get-url origin 2>$null)
    }
    finally { Pop-Location }
    if (-not $remote) { return $null, $null }
    if ($remote -match 'github\.com[:/]([^/]+)/([^/.]+)(\.git)?$') {
        return $Matches[1], ($Matches[2] -replace '\.git$', '')
    }
    return $null, $null
}

if (-not $Owner -or -not $Repo) {
    $o, $r = Get-RepoFromGit
    if (-not $o) { throw "Укажи -Owner и -Repo или запусти из клона с origin на github.com." }
    $Owner = $o
    $Repo = $r
}

$token = $env:GITHUB_TOKEN
if (-not $token) { $token = $env:GH_TOKEN }
$headers = @{
    Accept                 = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}
if ($token) { $headers['Authorization'] = "Bearer $token" }

function Get-LatestSuccessRunId {
    param([hashtable]$Hdr)
    if (Get-Command gh -ErrorAction SilentlyContinue) {
        $json = gh run list --repo "${Owner}/${Repo}" --workflow $WorkflowFile --status success -L 1 --json databaseId 2>$null
        if ($LASTEXITCODE -eq 0 -and $json) {
            $row = @($json | ConvertFrom-Json)[0]
            if ($row -and $row.databaseId) { return [long]$row.databaseId }
        }
    }
    $uri = "https://api.github.com/repos/$Owner/$Repo/actions/workflows/$WorkflowFile/runs?per_page=15&status=completed"
    $resp = Invoke-RestMethod -Uri $uri -Headers $Hdr -Method Get
    $run = $resp.workflow_runs | Where-Object { $_.conclusion -eq 'success' } | Select-Object -First 1
    if (-not $run) { throw "Нет успешных прогонов для $WorkflowFile." }
    return [long]$run.id
}

function Download-ArtifactsViaGh {
    param([long]$RunId, [string]$Dest)
    if (-not (Get-Command gh -ErrorAction SilentlyContinue)) { return $false }
    $env:GH_TOKEN = $token
    New-Item -ItemType Directory -Path $Dest -Force | Out-Null
    $tmp = Join-Path $env:TEMP ("gh-art-{0}" -f [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $tmp -Force | Out-Null
    try {
        & gh run download $RunId --repo "${Owner}/${Repo}" -D $tmp 2>&1 | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "gh run download failed: $LASTEXITCODE" }
        $trust = Get-ChildItem -Path $tmp -Recurse -Filter 'trust.img' -File | Select-Object -First 1
        if (-not $trust) { throw "В артефакте нет trust.img" }
        if ($WhatIf) {
            Write-Host "WhatIf: скопировать $($trust.FullName) -> $OutDir\trust.img"
            return $true
        }
        New-Item -ItemType Directory -Path $Dest -Force | Out-Null
        Copy-Item -LiteralPath $trust.FullName -Destination (Join-Path $Dest 'trust.img') -Force
        Write-Host "[OK] trust.img -> $(Join-Path $Dest 'trust.img')" -ForegroundColor Green
        (Get-FileHash -Algorithm SHA256 (Join-Path $Dest 'trust.img')).Hash
        return $true
    }
    finally {
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host "Repo: $Owner/$Repo  workflow: $WorkflowFile" -ForegroundColor Cyan

if (-not $token -and (Get-Command gh -ErrorAction SilentlyContinue)) {
    try {
        $token = (gh auth token 2>$null).Trim()
        if ($token) { $headers['Authorization'] = "Bearer $token" }
    }
    catch { }
}

if (-not $headers.Authorization) {
    throw "Нужен доступ к API: `gh auth login` или задай GITHUB_TOKEN/GH_TOKEN (для приватного репо — scope repo)."
}

$runId = Get-LatestSuccessRunId -Hdr $headers
Write-Host "Последний успешный run id: $runId" -ForegroundColor DarkGray

if (Download-ArtifactsViaGh -RunId $runId -Dest $OutDir) {
    exit 0
}

# Fallback: REST list artifacts + download zip (без gh)
$arts = Invoke-RestMethod -Uri "https://api.github.com/repos/$Owner/$Repo/actions/runs/$runId/artifacts" -Headers $headers
$art = $arts.artifacts | Where-Object { $_.name -like 'rk3399-trust-tfa-*' } | Select-Object -First 1
if (-not $art) { $art = $arts.artifacts | Select-Object -First 1 }
if (-not $art) { throw "У run $runId нет артефактов." }

$zipUrl = $art.archive_download_url
$zipPath = Join-Path $env:TEMP ("trust-art-{0}.zip" -f $runId)
Invoke-WebRequest -Uri $zipUrl -Headers $headers -OutFile $zipPath
$expand = Join-Path $env:TEMP ("trust-exp-{0}" -f $runId)
if (Test-Path $expand) { Remove-Item $expand -Recurse -Force }
Expand-Archive -Path $zipPath -DestinationPath $expand -Force
$trust = Get-ChildItem -Path $expand -Recurse -Filter 'trust.img' | Select-Object -First 1
if (-not $trust) { throw "В zip нет trust.img" }
if ($WhatIf) {
    Write-Host "WhatIf: $($trust.FullName) -> $OutDir\trust.img"
    exit 0
}
New-Item -ItemType Directory -Path $OutDir -Force | Out-Null
Copy-Item -LiteralPath $trust.FullName -Destination (Join-Path $OutDir 'trust.img') -Force
Write-Host "[OK] trust.img -> $(Join-Path $OutDir 'trust.img')" -ForegroundColor Green
Write-Host "SHA256: $((Get-FileHash -Algorithm SHA256 (Join-Path $OutDir 'trust.img')).Hash)" -ForegroundColor DarkGray
Remove-Item $zipPath -Force -ErrorAction SilentlyContinue
Remove-Item $expand -Recurse -Force -ErrorAction SilentlyContinue
