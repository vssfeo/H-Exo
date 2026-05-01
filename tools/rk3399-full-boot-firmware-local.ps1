param(
    [string]$TfaTag = "v2.14.0",
    [string]$UBootRepo = "https://github.com/u-boot/u-boot.git",
    [string]$UBootBranch = "v2026.04",
    [ValidateSet("666", "800", "933")]
    [string]$DdrMhz = "800",
    [string]$OutputDir = ".\artifacts\rk3399-full-boot-local",
    [ValidateSet("auto", "wsl", "git-bash")]
    [string]$Backend = "auto"
)

$ErrorActionPreference = "Stop"

function Require-Command([string]$Name) {
    if (-not (Get-Command $Name -ErrorAction SilentlyContinue)) {
        throw "Missing required command: $Name"
    }
}

function Escape-BashSingle([string]$Value) {
    return $Value.Replace("'", "'\\''")
}

function Convert-WindowsPathToPosix([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if ($full -match '^[A-Za-z]:\\') {
        $drive = $full.Substring(0, 1).ToLowerInvariant()
        $rest = $full.Substring(2).Replace('\', '/')
        return "/$drive$rest"
    }
    return $full.Replace('\', '/')
}

function Find-GitBash {
    $candidates = @(
        "$env:ProgramFiles\\Git\\bin\\bash.exe",
        "$env:ProgramFiles\\Git\\usr\\bin\\bash.exe",
        "${env:ProgramFiles(x86)}\\Git\\bin\\bash.exe",
        "${env:ProgramFiles(x86)}\\Git\\usr\\bin\\bash.exe"
    )

    foreach ($p in $candidates) {
        if ($p -and (Test-Path $p)) {
            return $p
        }
    }

    $bashCmd = Get-Command bash -ErrorAction SilentlyContinue
    if ($bashCmd) {
        return $bashCmd.Source
    }

    return $null
}

function Resolve-Backend([string]$Requested) {
    $hasWsl = [bool](Get-Command wsl -ErrorAction SilentlyContinue)
    $gitBash = Find-GitBash

    switch ($Requested) {
        "wsl" {
            if (-not $hasWsl) {
                throw "Backend 'wsl' requested but 'wsl' command is not available."
            }
            return [pscustomobject]@{ Name = "wsl"; BashPath = $null }
        }
        "git-bash" {
            if (-not $gitBash) {
                throw "Backend 'git-bash' requested but Git Bash was not found."
            }
            return [pscustomobject]@{ Name = "git-bash"; BashPath = $gitBash }
        }
        default {
            if ($hasWsl) {
                return [pscustomobject]@{ Name = "wsl"; BashPath = $null }
            }
            if ($gitBash) {
                return [pscustomobject]@{ Name = "git-bash"; BashPath = $gitBash }
            }
            throw "No local Linux shell backend found. Install WSL (recommended) or Git for Windows (Git Bash)."
        }
    }
}

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = [IO.Path]::GetFullPath((Join-Path $scriptDir ".."))
$bashScriptWin = Join-Path $repoRoot "tools\rk3399-full-boot-firmware-local.sh"

if (-not (Test-Path $bashScriptWin)) {
    throw "Local bash pipeline not found: $bashScriptWin"
}

$outWin = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $outWin -Force | Out-Null

$backendSel = Resolve-Backend $Backend

Write-Host "[LOCAL PIPELINE] repo: $repoRoot" -ForegroundColor Cyan
Write-Host "[LOCAL PIPELINE] out : $outWin" -ForegroundColor Cyan
Write-Host "[LOCAL PIPELINE] backend: $($backendSel.Name)" -ForegroundColor Cyan

if ($backendSel.Name -eq "wsl") {
    $repoPosix = (wsl wslpath -a "$repoRoot").Trim()
    $outPosix = (wsl wslpath -a "$outWin").Trim()
    $scriptPosix = (wsl wslpath -a "$bashScriptWin").Trim()

    Write-Host "[LOCAL PIPELINE] WSL script: $scriptPosix" -ForegroundColor Cyan

    $cmd = @(
        "bash",
        "-lc",
        "chmod +x '$(Escape-BashSingle $scriptPosix)' && '$(Escape-BashSingle $scriptPosix)' '$(Escape-BashSingle $repoPosix)' '$(Escape-BashSingle $outPosix)' '$(Escape-BashSingle $TfaTag)' '$(Escape-BashSingle $UBootRepo)' '$(Escape-BashSingle $UBootBranch)' '$(Escape-BashSingle $DdrMhz)'"
    )

    & wsl @cmd
}
else {
    $repoPosix = Convert-WindowsPathToPosix $repoRoot
    $outPosix = Convert-WindowsPathToPosix $outWin
    $scriptPosix = Convert-WindowsPathToPosix $bashScriptWin

    Write-Host "[LOCAL PIPELINE] Git Bash: $($backendSel.BashPath)" -ForegroundColor Cyan
    Write-Host "[LOCAL PIPELINE] Script: $scriptPosix" -ForegroundColor Cyan

    # Build PATH with mingw/gcc for U-Boot host tools
    $machinePath = [System.Environment]::GetEnvironmentVariable("Path","Machine")
    $userPath = [System.Environment]::GetEnvironmentVariable("Path","User")
    $fullPath = "$machinePath;$userPath"
    # Convert to POSIX-style PATH for Git Bash (colon-separated, /c/ style)
    $posixPathParts = $fullPath.Split(';') | ForEach-Object {
        if ($_ -match '^([A-Za-z]):\\(.*)') { "/$($Matches[1].ToLower())/$($Matches[2] -replace '\\','/')" } else { $_ -replace '\\','/' }
    }
    $posixPath = $posixPathParts -join ':'

    # Append Windows PATH after Git Bash PATH so Unix find/chmod etc take precedence
    # but mingw gcc is still discoverable (no gcc in /usr/bin)
    $cmdLine = "export PATH=`$PATH:'$($posixPath -replace "'","'\\''")'; chmod +x '$(Escape-BashSingle $scriptPosix)' && '$(Escape-BashSingle $scriptPosix)' '$(Escape-BashSingle $repoPosix)' '$(Escape-BashSingle $outPosix)' '$(Escape-BashSingle $TfaTag)' '$(Escape-BashSingle $UBootRepo)' '$(Escape-BashSingle $UBootBranch)' '$(Escape-BashSingle $DdrMhz)'"
    & $backendSel.BashPath -lc $cmdLine
}

if ($LASTEXITCODE -ne 0) {
    throw "Local boot firmware pipeline failed with exit code $LASTEXITCODE"
}

Write-Host "[LOCAL PIPELINE] OK" -ForegroundColor Green
Write-Host "Artifacts:" -ForegroundColor Green
Get-ChildItem -Path $outWin -File | Select-Object Name, Length
