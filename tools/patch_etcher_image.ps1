<#
.SYNOPSIS
Overlays SPL+TPL boot firmware onto a full Armbian .img file for balenaEtcher flashing.

.DESCRIPTION
Windows raw SD writes are unreliable (USB write cache). balenaEtcher works
correctly but writes from LBA 0. This script takes a KNOWN-GOOD Armbian .img,
overlays idbloader.img (TPL+SPL) at LBA 0x40 and u-boot.itb (U-Boot+BL31+DTB)
at LBA 0x4000, producing a ready-to-flash .img file.

Boot flow: BootROM -> TPL (DDR init) -> SPL -> BL31 -> U-Boot proper.
NO miniloader/Boot1 needed — pure mainline SPL+TPL.

.PARAMETER ArmbianPath
Path to the full Armbian .img file.

.PARAMETER IdbLoaderPath
Path to idbloader.img (TPL+SPL from U-Boot build).

.PARAMETER UBootItbPath
Path to u-boot.itb (FIT: U-Boot + BL31 + DTB from U-Boot build).

.PARAMETER OutputPath
Output .img file path.

.EXAMPLE
.\patch_etcher_image.ps1 -ArmbianPath C:\tftpboot\Armbian_community_26.2.0-trunk.668_Nanopim4_trixie_current_6.18.20_minimal.img -IdbLoaderPath C:\tftpboot\idbloader.img -UBootItbPath C:\tftpboot\u-boot.itb
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$ArmbianPath,

    [Parameter(Mandatory=$true)]
    [string]$IdbLoaderPath,

    [Parameter(Mandatory=$true)]
    [string]$UBootItbPath,

    [string]$OutputPath = ""
)

$ErrorActionPreference = "Stop"

$srcImg = (Resolve-Path -LiteralPath $ArmbianPath).Path
$idbFile = (Resolve-Path -LiteralPath $IdbLoaderPath).Path
$itbFile = (Resolve-Path -LiteralPath $UBootItbPath).Path

if (-not $OutputPath) {
    $dir = Split-Path $srcImg -Parent
    $base = [IO.Path]::GetFileNameWithoutExtension($srcImg)
    $OutputPath = Join-Path $dir "$base-spl-tpl.img"
}

$idbLba = 0x40       # BootROM loads idbloader from sector 64
$itbLba = 0x4000     # SPL loads u-boot.itb from here
$idbOff = $idbLba * 512
$itbOff = $itbLba * 512

$idbBytes = [IO.File]::ReadAllBytes($idbFile)
$idbLen = $idbBytes.Length
$itbBytes = [IO.File]::ReadAllBytes($itbFile)
$itbLen = $itbBytes.Length

$srcLen = (Get-Item -LiteralPath $srcImg).Length

Write-Host "=== Patch Armbian Image (SPL+TPL flow) ===" -ForegroundColor Cyan
Write-Host "Source:     $srcImg ($([math]::Round($srcLen/1GB, 2)) GiB)"
Write-Host "idbloader:  $idbFile ($idbLen bytes) -> LBA 0x$([Convert]::ToString($idbLba, 16)) (BootROM loads TPL+SPL)"
Write-Host "u-boot.itb: $itbFile ($itbLen bytes) -> LBA 0x$([Convert]::ToString($itbLba, 16)) (SPL loads U-Boot+BL31)"
Write-Host "Output:     $OutputPath"
Write-Host ""

if ($idbOff + $idbLen -gt $srcLen) {
    throw "idbloader offset + size exceeds Armbian image size. Image too small?"
}
if ($itbOff + $itbLen -gt $srcLen) {
    throw "u-boot.itb offset + size exceeds Armbian image size. Image too small?"
}

# Verify current boot regions in Armbian image
Write-Host "[*] Checking current boot regions in Armbian image..." -ForegroundColor Cyan
$checkStream = [IO.File]::Open($srcImg, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    foreach ($info in @(@{Lba=$idbLba; Off=$idbOff; Label="idbloader (TPL+SPL)"}, @{Lba=$itbLba; Off=$itbOff; Label="u-boot.itb (U-Boot+BL31)"})) {
        $null = $checkStream.Seek($info.Off, [IO.SeekOrigin]::Begin)
        $curMagic = New-Object byte[] 8
        $checkStream.Read($curMagic, 0, 8) | Out-Null
        $magicStr = [Text.Encoding]::ASCII.GetString($curMagic[0..3])
        $magicU32 = [BitConverter]::ToUInt32($curMagic, 0).ToString('X8')
        Write-Host "    Current magic at LBA 0x$([Convert]::ToString($info.Lba, 16)) [$($info.Label)]: '$magicStr' (0x$magicU32)" -ForegroundColor Yellow
    }
}
finally {
    $checkStream.Dispose()
}

# Copy Armbian image to output (file copy, not in-memory)
Write-Host "`n[*] Copying Armbian image to output..." -ForegroundColor Cyan
Copy-Item -LiteralPath $srcImg -Destination $OutputPath -Force
Write-Host "    Copied $([math]::Round($srcLen/1MB, 0)) MiB" -ForegroundColor DarkGray

# Overlay boot firmware at correct offsets
Write-Host "`n[*] Overlaying boot firmware..." -ForegroundColor Cyan
$outStream = [IO.File]::Open($OutputPath, [IO.FileMode]::Open, [IO.FileAccess]::Write, [IO.FileShare]::None)
try {
    # Write idbloader.img at LBA 0x40 (BootROM loads TPL+SPL from here)
    $null = $outStream.Seek($idbOff, [IO.SeekOrigin]::Begin)
    $outStream.Write($idbBytes, 0, $idbLen)
    Write-Host "    idbloader.img written at LBA 0x$([Convert]::ToString($idbLba, 16))" -ForegroundColor Green

    # Write u-boot.itb at LBA 0x4000 (SPL loads U-Boot+BL31 from here)
    $null = $outStream.Seek($itbOff, [IO.SeekOrigin]::Begin)
    $outStream.Write($itbBytes, 0, $itbLen)
    Write-Host "    u-boot.itb written at LBA 0x$([Convert]::ToString($itbLba, 16))" -ForegroundColor Green

    $outStream.Flush()
}
finally {
    $outStream.Dispose()
}

# Verify boot firmware regions
Write-Host "`n[*] Verifying patched image..." -ForegroundColor Cyan
$verifyStream = [IO.File]::Open($OutputPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
try {
    $verifyEntries = @(
        @{Lba=$idbLba; Off=$idbOff; Label="idbloader (TPL+SPL)"; Expected=$idbBytes; ELen=$idbLen},
        @{Lba=$itbLba; Off=$itbOff; Label="u-boot.itb (U-Boot+BL31)"; Expected=$itbBytes; ELen=$itbLen}
    )
    foreach ($info in $verifyEntries) {
        $null = $verifyStream.Seek($info.Off, [IO.SeekOrigin]::Begin)
        $verifyData = New-Object byte[] $info.ELen
        $verifyStream.Read($verifyData, 0, $info.ELen) | Out-Null
        $vMagic = [Text.Encoding]::ASCII.GetString($verifyData[0..3])
        Write-Host "    Magic at LBA 0x$([Convert]::ToString($info.Lba, 16)) [$($info.Label)]: '$vMagic'" -ForegroundColor Green

        $match = $true
        for ($i = 0; $i -lt $info.ELen; $i++) {
            if ($verifyData[$i] -ne $info.Expected[$i]) { $match = $false; break }
        }
        if ($match) {
            Write-Host "    [OK] Boot firmware at LBA 0x$([Convert]::ToString($info.Lba, 16)) matches source" -ForegroundColor Green
        } else {
            Write-Host "    [FAIL] Boot firmware at LBA 0x$([Convert]::ToString($info.Lba, 16)) MISMATCH at byte $i!" -ForegroundColor Red
        }
    }
}
finally {
    $verifyStream.Dispose()
}

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Flash with balenaEtcher:" -ForegroundColor Green
Write-Host "  $OutputPath" -ForegroundColor White
Write-Host "========================================" -ForegroundColor Cyan
