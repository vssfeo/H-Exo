param(
    [string]$PortName = "COM3",
    [int]$BaudRate = 1500000,
    [string]$TftpServerIp = "",
    [string]$BoardIp = "192.168.1.10",
    # U-Boot index from "mmc list" for the medium you actually boot from (SD vs eMMC differ by board).
    # If only SD works, use the line that shows (SD) — often 1 on NanoPi M4, but always confirm in U-Boot.
    [int]$MmcDev = 1,
    [string]$IdbFile = "idbloader.img",
    [string]$ItbFile = "u-boot.itb",
    [string]$TrustFile = "trust.img",
    # Override LBA for u-boot.itb (hex string, e.g. "0x2000").
    # LibreELEC/Armbian SPL scans 0x2000 first for FIT; standard Rockchip is 0x4000.
    [string]$ItbLba = "",
    # mmc write LBA for trust.img (hex). SPL often prints "Trust Addr:0x4000" but that is SPL-relative;
    # on NanoPi M4 / Armbian (FwPartOffset 0x2000) the matching U-Boot LBA is usually 0x6000 (default).
    # See recover_sdcard.ps1 comments. Override only if your layout differs.
    [string]$TrustLba = "",
    [switch]$ForceWrite,
    [switch]$OnlyUbootItb,
    [switch]$TrustOnly,
    [switch]$SkipUartReset,
    [switch]$SkipPing,
    [switch]$RequirePing
)

$ErrorActionPreference = "Stop"

function Resolve-FlashFile {
    param([Parameter(Mandatory=$true)][string]$Name)
    $candidates = @(
        (Join-Path "C:\tftpboot" $Name),
        (Join-Path $PSScriptRoot $Name),
        $Name
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { return (Resolve-Path $c).Path }
    }
    return $null
}

function Get-FlashImageInfo {
    param([Parameter(Mandatory=$true)][string]$Path)
    $fs = [System.IO.File]::OpenRead($Path)
    try {
        $buf = New-Object byte[] 16
        $null = $fs.Read($buf, 0, $buf.Length)
    } finally { $fs.Dispose() }
    $sig8  = [System.Text.Encoding]::ASCII.GetString($buf, 0, 8)
    $sig4  = [System.Text.Encoding]::ASCII.GetString($buf, 0, 4)
    $format = "UNKNOWN"
    if ($sig8 -eq "LOADER  ")                                            { $format = "LOADER" }
    elseif ($sig4 -eq "BL3X")                                            { $format = "BL3X"   }
    elseif ($buf[0] -eq 0xD0 -and $buf[1] -eq 0x0D -and
            $buf[2] -eq 0xFE -and $buf[3] -eq 0xED)                     { $format = "FIT"    }
    $len = (Get-Item $Path).Length
    [pscustomobject]@{
        Path        = $Path
        Length      = $len
        Format      = $format
        SectorCount = [int][Math]::Ceiling($len / 512.0)
    }
}

function Get-LocalIp {
    $ip = (Get-NetIPAddress -AddressFamily IPv4 |
        Where-Object { $_.IPAddress -match "^192\.168\." -and $_.IPAddress -ne "127.0.0.1" } |
        Select-Object -First 1).IPAddress
    return $ip
}

if (-not $TftpServerIp) { $TftpServerIp = Get-LocalIp }
if (-not $TftpServerIp) { throw "Cannot determine server IP. Pass -TftpServerIp explicitly." }

$idbLocal   = Resolve-FlashFile $IdbFile
$itbLocal   = Resolve-FlashFile $ItbFile
$trustLocal = Resolve-FlashFile $TrustFile
$idbInfo    = $null; $itbInfo = $null; $trustInfo = $null
if ($idbLocal)   { $idbInfo   = Get-FlashImageInfo $idbLocal   }
if ($itbLocal)   { $itbInfo   = Get-FlashImageInfo $itbLocal   }
if ($trustLocal) { $trustInfo = Get-FlashImageInfo $trustLocal }

Write-Host "=== U-Boot Bootloader Flasher ===" -ForegroundColor Cyan
Write-Host "Port: $PortName @ $BaudRate"        -ForegroundColor Cyan
Write-Host "TFTP server IP: $TftpServerIp"      -ForegroundColor Cyan
Write-Host "Board IP: $BoardIp"                 -ForegroundColor Cyan
Write-Host "MMC device: $MmcDev"                -ForegroundColor Cyan
Write-Host "  Boot medium: use the same mmc index you boot from. In U-Boot: mmc list → e.g. (SD) = your card." -ForegroundColor DarkYellow
Write-Host "  SPL errors on eMMC (voltage select) are normal if the board only uses SD; they do not block SD boot." -ForegroundColor DarkGray
Write-Host "Files: $IdbFile, $ItbFile, $TrustFile" -ForegroundColor Cyan
if ($idbInfo)   { Write-Host ("  {0}: {1} bytes, format={2}, sectors=0x{3:X}" -f $IdbFile,   $idbInfo.Length,   $idbInfo.Format,   $idbInfo.SectorCount)   -ForegroundColor DarkCyan }
if ($itbInfo)   { Write-Host ("  {0}: {1} bytes, format={2}, sectors=0x{3:X}" -f $ItbFile,   $itbInfo.Length,   $itbInfo.Format,   $itbInfo.SectorCount)   -ForegroundColor DarkCyan }
if ($trustInfo) { Write-Host ("  {0}: {1} bytes, format={2}, sectors=0x{3:X}" -f $TrustFile, $trustInfo.Length, $trustInfo.Format, $trustInfo.SectorCount) -ForegroundColor DarkCyan }

$flashModeStr = if ($OnlyUbootItb) { "ONLY u-boot.itb (safe)" } else { "idbloader + u-boot.itb" }
Write-Host ("Flash mode: " + $flashModeStr) -ForegroundColor Cyan
if ($TrustOnly) { Write-Host "Trust mode: ONLY trust (BL3X slot)" -ForegroundColor Cyan }
$writeStr = if ($ForceWrite) { "WRITE (dangerous)" } else { "DRY-RUN (no mmc write)" }
Write-Host ("Mode: " + $writeStr) -ForegroundColor Cyan

if ($ForceWrite) {
    Write-Host ""
    Write-Host "WARNING: images MUST match board memory type." -ForegroundColor Red
    Write-Host "  NanoPi M4 (most revisions): DDR3 in SPL (log shows DDR3 933MHz)." -ForegroundColor Yellow
    Write-Host "  If SPL shows LPDDR3 -> board will not boot after flash." -ForegroundColor Yellow
    Write-Host ""
}

if ($SkipPing)      { Write-Host "Ping: skipped (-SkipPing)"            -ForegroundColor Cyan }
elseif ($RequirePing) { Write-Host "Ping: required (-RequirePing)"       -ForegroundColor Cyan }
else                  { Write-Host "Ping: soft (ICMP fail does not stop script)" -ForegroundColor Cyan }
Write-Host ""

if (-not $OnlyUbootItb -and -not $TrustOnly -and -not $idbInfo) {
    throw "Local file not found for $IdbFile. Required for format/size check before flash."
}
if (-not $TrustOnly -and -not $itbInfo) {
    throw "Local file not found for $ItbFile. Required for format/size check before flash."
}
if ($TrustOnly -and -not $trustInfo) {
    throw "Local file not found for $TrustFile. Required for trust/BL3X flash."
}

if ($OnlyUbootItb -and -not $TrustOnly -and $itbInfo -and $itbInfo.Format -eq "FIT") {
    if ($ItbLba) {
        # Explicit LBA override: user knows what they're doing (e.g. 0x2000 for LibreELEC SPL FIT slot)
        Write-Host ("[!] FIT image at explicit LBA 0x{0:X} — BL31 will be embedded from FIT." -f [Convert]::ToInt32($ItbLba, 16)) -ForegroundColor Yellow
    } else {
        $msg = "Detected plain FIT ($ItbFile). On legacy Rockchip chain this does NOT update LOADER/BL31 slot. Pass -ItbLba 0x2000 to target LibreELEC FIT slot."
        if ($ForceWrite) { throw $msg }
        else             { Write-Host "[!] $msg" -ForegroundColor Yellow }
    }
}

$idbWriteLba   = 0x40
$itbWriteLba   = 0x4000
$trustWriteLba = 0x6000
if ($itbInfo -and $itbInfo.Format -eq "BL3X") { $itbWriteLba = 0x6000 }
if ($ItbLba) {
    $itbWriteLba = [Convert]::ToInt32($ItbLba, 16)
    Write-Host ("[*] LBA override: u-boot.itb will be written at LBA 0x{0:X}" -f $itbWriteLba) -ForegroundColor Cyan
}
if ($TrustLba) {
    $trustWriteLba = [Convert]::ToInt32($TrustLba, 16)
    Write-Host ("[*] LBA override: trust.img will be written at LBA 0x{0:X}" -f $trustWriteLba) -ForegroundColor Cyan
}

if ($TrustOnly -and $trustInfo) {
    Write-Host ""
    if ($trustInfo.Length -gt 8MB) {
        Write-Host "WARNING: Very large trust images usually include BL32/OP-TEE and often break Armbian SPL:" -ForegroundColor Red
        Write-Host "  SPL may DMA-timeout or fail LoadTrustBL when probing BL32. See docs/rk3399/firmware-handoff-debug.md" -ForegroundColor Red
        Write-Host "  Prefer trust with BL31-only (e.g. trust-armbian-no-optee.img) for this board unless you know you need OP-TEE." -ForegroundColor Yellow
        Write-Host ""
    }
    Write-Host "TFTP: board loads $TrustFile from TFTP root (e.g. C:\tftpboot\$TrustFile)." -ForegroundColor Yellow
    Write-Host "  If you see 'Retry count exceeded' / lines of 'T': server not reachable or firewall blocked UDP." -ForegroundColor Yellow
    Write-Host "  Windows: allow inbound UDP 69, or allow your TFTP app on the Private profile; ping can fail while TFTP still works." -ForegroundColor Yellow
    if ($trustInfo.Length -gt 4MB) {
        Write-Host ("  Large image ({0} bytes) — transfer can take a minute; keep TFTP server running." -f $trustInfo.Length) -ForegroundColor DarkYellow
    }
    Write-Host ("  Trust mmc write LBA: 0x{0:X} (override with -TrustLba if needed)." -f $trustWriteLba) -ForegroundColor DarkCyan
    Write-Host ""
}

$serial = New-Object System.IO.Ports.SerialPort($PortName, $BaudRate,
    [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
$serial.ReadTimeout  = 200
$serial.WriteTimeout = 1000
$serial.NewLine      = "`r"
$serial.Open()
$script:LastUbootOutput = ""

function Read-UntilPrompt {
    param([int]$TimeoutSec = 20, [switch]$BreakAutoboot)
    $buf = ""; $start = Get-Date; $lastBreak = Get-Date
    while (((Get-Date) - $start).TotalSeconds -lt $TimeoutSec) {
        if ($BreakAutoboot -and ((Get-Date) - $lastBreak).TotalMilliseconds -gt 120) {
            try { $serial.Write(" "); $serial.Write("`r"); $serial.Write([char]3) } catch {}
            $lastBreak = Get-Date
        }
        try {
            $ch = [char]$serial.ReadChar()
            $buf += $ch
            Write-Host -NoNewline $ch
            if ($buf -match "=>\s*$") { $script:LastUbootOutput = $buf; return $true }
            if ($buf.Length -gt 12000) { $buf = $buf.Substring($buf.Length - 6000) }
        } catch { Start-Sleep -Milliseconds 20 }
    }
    $script:LastUbootOutput = $buf
    return $false
}

function Send-Uboot {
    param(
        [Parameter(Mandatory=$true)][string]$Cmd,
        [int]$TimeoutSec = 20,
        [switch]$PingSoftFail,
        [int]$TftpRetries = 3
    )
    $maxAttempts = if ($Cmd -like "tftp *") { $TftpRetries } else { 1 }
    for ($attempt = 1; $attempt -le $maxAttempts; $attempt++) {
    if ($attempt -gt 1) {
        Write-Host "[*] PHY not ready, waiting 8s before retry $attempt/$maxAttempts..." -ForegroundColor Yellow
        Start-Sleep -Seconds 8
    }
    Write-Host ""
    Write-Host "[>] $Cmd" -ForegroundColor Yellow
    $serial.DiscardInBuffer()
    $serial.Write($Cmd + "`r")
    $ok = Read-UntilPrompt -TimeoutSec $TimeoutSec
    $combined = $Cmd + "`n" + $script:LastUbootOutput
    # For ping with PingSoftFail, let the ping-specific handler decide; skip general throw
    $skipGeneralThrow = ($PingSoftFail -and $Cmd -like "ping *")
    $isPhyError = $combined -match "Could not initialize PHY|Waiting for PHY.*TIMEOUT"
    if ($Cmd -like "tftp *" -and $isPhyError -and $attempt -lt $maxAttempts) {
        Write-Host "[!] PHY timeout on TFTP attempt $attempt — will retry..." -ForegroundColor Yellow
        continue
    }
    if (-not $skipGeneralThrow -and $combined -match "Card did not respond to voltage select|Could not initialize PHY|Retry count exceeded|DMA reset timeout|TFTP error|ERROR|Bad device specification|No block device|Filename not found|Access violation") {
        throw "Command failed: $Cmd"
    }
    if ($Cmd -like "tftp *") {
        if ($combined -notmatch "(?i)Bytes transferred\s*=\s*[0-9]+") {
            throw "TFTP did not confirm transfer (no 'Bytes transferred'): $Cmd"
        }
    }
    if ($Cmd -like "ping *") {
        $pingBad  = $combined -match "(?i)ping failed|host .* is not alive|dead|no answer|Unknown command"
        $pingGood = $combined -match "(?i)is alive|bytes from"
        if ($pingBad -and -not $pingGood) {
            if ($PingSoftFail) {
                Write-Host ""
                Write-Host "[!] Ping failed (Windows firewall may block ICMP). Trying TFTP over UDP." -ForegroundColor Yellow
            } else {
                throw "Ping to TFTP server failed: $Cmd"
            }
        }
    }
    if (-not $ok) { throw "Timeout waiting for prompt after: $Cmd" }
    break  # success - exit retry loop
    } # end retry loop
}

function Send-UbootFireAndForget {
    param([Parameter(Mandatory=$true)][string]$Cmd, [int]$DrainMs = 1200)
    Write-Host ""
    Write-Host "[>] $Cmd" -ForegroundColor Yellow
    Write-Host "    (no prompt wait - command sent to UART)" -ForegroundColor DarkGray
    try { $serial.DiscardInBuffer(); $serial.Write($Cmd + "`r") } catch {}
    Start-Sleep -Milliseconds $DrainMs
}

function Probe-BoardState {
    # Quick non-blocking drain: detect kernel '> ' or U-Boot '=>'
    $buf = ""; $start = Get-Date
    while (((Get-Date) - $start).TotalSeconds -lt 3) {
        try { $ch = [char]$serial.ReadChar(); $buf += $ch } catch { break }
    }
    if ($buf -match ">\s*$") { return "kernel" }
    if ($buf -match "=>\s*$") { return "uboot" }
    return "unknown"
}

try {
    Write-Host "[*] Probing board state..." -ForegroundColor Green
    $serial.DiscardInBuffer()
    try { $serial.Write("`r") } catch {}
    Start-Sleep -Milliseconds 600
    $state = Probe-BoardState
    Write-Host "[*] Board state: $state" -ForegroundColor Cyan

    if ($state -eq "kernel") {
        Write-Host "[*] Kernel interactive mode detected — sending 'r' (PSCI SYSTEM_RESET)..." -ForegroundColor Yellow
        try { $serial.Write("r`r") } catch {}
        Start-Sleep -Milliseconds 800
        $serial.DiscardInBuffer()
    } elseif ($state -eq "uboot") {
        Write-Host "[+] Already at U-Boot prompt." -ForegroundColor Green
    } else {
        Write-Host "[*] State unknown — waiting for boot (power-cycle if needed)..." -ForegroundColor Yellow
    }

    if ($state -ne "uboot") {
        Write-Host "[*] Waiting for U-Boot prompt => (power-cycle board if not already booting)..." -ForegroundColor Green
        if (-not (Read-UntilPrompt -TimeoutSec 180 -BreakAutoboot)) {
            throw "Timed out waiting for U-Boot prompt. Check COM port, baud rate, and reboot board."
        }
    } else {
        Write-Host "[+] U-Boot prompt confirmed." -ForegroundColor Green
    }

    Send-Uboot "setenv ipaddr $BoardIp"
    Send-Uboot "setenv serverip $TftpServerIp"
    Send-Uboot "mmc list"
    Send-Uboot "mmc dev $MmcDev"
    Send-Uboot "mmc info" -TimeoutSec 15
    Write-Host "[*] Verify 'mmc info' capacity matches your boot microSD. If not, abort (Ctrl+C) and use -MmcDev 0 or 1." -ForegroundColor Yellow
    Write-Host "    On many NanoPi M4 builds the SD slot is mmc 0; eMMC (if dead) may still appear as mmc 1 — do not flash the wrong index." -ForegroundColor DarkYellow

    if (-not $SkipPing) {
        $soft = -not $RequirePing
        Send-Uboot "ping $TftpServerIp" -TimeoutSec 30 -PingSoftFail:$soft
    }

    if (-not $OnlyUbootItb -and -not $TrustOnly) {
        Send-Uboot "tftp 0x02000000 $IdbFile" -TimeoutSec 300
    } else {
        Write-Host ""
        Write-Host "[*] Skipping idbloader.img load" -ForegroundColor Yellow
    }

    if ($TrustOnly) {
        Send-Uboot "tftp 0x04000000 $TrustFile" -TimeoutSec 300
        # Verify loaded file: try sha256sum (newer U-Boot), fallback to md5sum, fallback to crc32
        $sha256local = [System.Security.Cryptography.SHA256]::Create()
        $localBytes  = [System.IO.File]::ReadAllBytes((Resolve-FlashFile $TrustFile))
        $localSHA    = ($sha256local.ComputeHash($localBytes) | ForEach-Object { $_.ToString("x2") }) -join ""
        $fileSize    = $localBytes.Length
        $verified    = $false
        # Try sha256sum (U-Boot 2020+)
        Send-Uboot ("sha256sum 0x04000000 0x{0:X}" -f $fileSize) -TimeoutSec 10
        if ($script:LastUbootOutput -match "([0-9a-fA-F]{64})") {
            $ubHash = $Matches[1].ToLower()
            if ($ubHash -eq $localSHA) {
                Write-Host "[OK] SHA256 verified: TFTP loaded correct file" -ForegroundColor Green; $verified = $true
            } else {
                Write-Host "[ERR] SHA256 MISMATCH — TFTP served stale/wrong file!" -ForegroundColor Red
                Write-Host "  U-Boot: $ubHash" -ForegroundColor Red
                Write-Host "  Local:  $localSHA" -ForegroundColor Red
                throw "TFTP served wrong file - aborting. Restart TFTP server and retry."
            }
        }
        if (-not $verified) {
            # Try md5sum
            $md5local = ([System.Security.Cryptography.MD5]::Create().ComputeHash($localBytes) | ForEach-Object { $_.ToString("x2") }) -join ""
            Send-Uboot ("md5sum 0x04000000 0x{0:X}" -f $fileSize) -TimeoutSec 10
            if ($script:LastUbootOutput -match "([0-9a-fA-F]{32})") {
                $ubMd5 = $Matches[1].ToLower()
                if ($ubMd5 -eq $md5local) {
                    Write-Host "[OK] MD5 verified: TFTP loaded correct file" -ForegroundColor Green; $verified = $true
                } else {
                    Write-Host "[ERR] MD5 MISMATCH — TFTP served stale/wrong file!" -ForegroundColor Red
                    throw "TFTP served wrong file (MD5 mismatch). Restart TFTP server and retry."
                }
            }
        }
        if (-not $verified) {
            Write-Host "[WARN] No hash command available in U-Boot (sha256sum/md5sum not found)" -ForegroundColor Yellow
            Write-Host "[WARN] Cannot verify TFTP integrity — proceeding. Ensure TFTP server was restarted!" -ForegroundColor Yellow
            Write-Host "       Local SHA256: $localSHA" -ForegroundColor Yellow
        }
    } else {
        Send-Uboot "tftp 0x04000000 $ItbFile" -TimeoutSec 300
    }

    if ($ForceWrite) {
        Write-Host ""
        Write-Host "[!] ForceWrite enabled: writing to MMC" -ForegroundColor Yellow
        if (-not $OnlyUbootItb -and -not $TrustOnly) {
            Send-Uboot ("mmc write 0x02000000 0x{0:X} 0x{1:X}" -f $idbWriteLba, $idbInfo.SectorCount)
        } else {
            Write-Host "[*] ONLY mode: skipping mmc write for idbloader" -ForegroundColor Yellow
        }
        if ($TrustOnly) {
            Send-Uboot ("mmc write 0x04000000 0x{0:X} 0x{1:X}" -f $trustWriteLba, $trustInfo.SectorCount)
        } else {
            Send-Uboot ("mmc write 0x04000000 0x{0:X} 0x{1:X}" -f $itbWriteLba, $itbInfo.SectorCount)
        }
    } else {
        Write-Host ""
        Write-Host "[OK] DRY-RUN: checks passed, MMC write NOT performed." -ForegroundColor Green
        Write-Host "[*] To write, run with -ForceWrite"                    -ForegroundColor Yellow
    }

    $tail = ""
    try { while ($serial.BytesToRead -gt 0) { $tail += [char]$serial.ReadChar() } } catch {}
    if ($tail -match "Card did not respond to voltage select|Could not initialize PHY|Retry count exceeded|DMA reset timeout|ERROR") {
        throw "Flash commands failed. Check MMC device and TFTP network."
    }

    Write-Host ""
    if ($ForceWrite) {
        Write-Host "[OK] Bootloader written to MMC (dev $MmcDev)." -ForegroundColor Green
        if (-not $SkipUartReset) {
            Write-Host "[*] Sending reset via UART (do NOT type reset in PowerShell)" -ForegroundColor Cyan
            Send-UbootFireAndForget "reset" 1500
        } else {
            Write-Host "[*] Manual reset: type 'reset' in U-Boot console or power-cycle." -ForegroundColor Yellow
        }
    } else {
        Write-Host "[OK] DRY-RUN complete, nothing written." -ForegroundColor Green
    }
} finally {
    if ($serial -and $serial.IsOpen) {
        $serial.Close()
        Write-Host "[*] COM closed" -ForegroundColor DarkGray
    }
}
