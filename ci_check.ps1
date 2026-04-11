# ci_check.ps1 - H-Exo Omni-Core CI Build Verification
# Usage: .\ci_check.ps1 [-SkipBuild] [-Verbose]
# Exit code: 0 = PASS, 1 = FAIL

param(
    [switch]$SkipBuild,
    [switch]$Verbose
)

$ErrorActionPreference = "Stop"
$failures = @()
$warnings = @()

function Write-CI {
    param([string]$msg, [string]$status = "INFO")
    $color = switch ($status) {
        "PASS"  { "Green"  }
        "FAIL"  { "Red"    }
        "WARN"  { "Yellow" }
        "INFO"  { "Cyan"   }
        default { "White"  }
    }
    Write-Host "[$status] $msg" -ForegroundColor $color
}

$root = $PSScriptRoot

Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  H-Exo CI Check" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan
Write-Host ""

# --- Load baseline ---
$baselinePath = Join-Path $root "baseline.json"
if (-not (Test-Path $baselinePath)) {
    Write-CI "baseline.json not found - cannot check regression thresholds" "WARN"
    $baseline = $null
} else {
    $baseline = Get-Content $baselinePath | ConvertFrom-Json
    Write-CI "Baseline: $($baseline.version) - $($baseline.description)" "INFO"
}

# --- Step 1: Build ---
if (-not $SkipBuild) {
    Write-Host ""
    Write-CI "Building kernel..." "INFO"
    $buildOutput = & cmd /c "cd /d `"$root`" && .\build.bat 2>&1"
    $buildExit = $LASTEXITCODE

    if ($buildExit -ne 0) {
        Write-CI "Build FAILED (exit $buildExit)" "FAIL"
        if ($Verbose) { $buildOutput | ForEach-Object { Write-Host $_ } }
        $failures += "Build failed with exit code $buildExit"
    } else {
        Write-CI "Build succeeded" "PASS"
        if ($Verbose) { $buildOutput | ForEach-Object { Write-Host $_ -ForegroundColor DarkGray } }
    }
} else {
    Write-CI "Build skipped (-SkipBuild)" "WARN"
}

# --- Step 2: Binary exists ---
$binPath = Join-Path $root "kernel_neuro.bin"
if (-not (Test-Path $binPath)) {
    Write-CI "kernel_neuro.bin not found" "FAIL"
    $failures += "kernel_neuro.bin missing"
    Write-Host "`n[FAIL] CI check failed" -ForegroundColor Red
    exit 1
}

$binSize = (Get-Item $binPath).Length
Write-CI "kernel_neuro.bin: $binSize bytes" "INFO"

# --- Step 3: Size regression ---
if ($baseline) {
    $maxSize = $baseline.regression_thresholds.max_kernel_size_bytes
    if ($binSize -le $maxSize) {
        Write-CI "Size $binSize <= $maxSize bytes" "PASS"
    } else {
        Write-CI "Size $binSize > $maxSize bytes (regression!)" "FAIL"
        $failures += "Kernel size $binSize bytes exceeds max $maxSize"
    }

    # Percentage increase from baseline
    $baseSize = $baseline.metrics.kernel_size_bytes
    if ($baseSize -gt 0) {
        $pct = [math]::Round((($binSize - $baseSize) * 100.0) / $baseSize, 1)
        $maxPct = $baseline.regression_thresholds.max_kernel_size_increase_percent
        if ($pct -le $maxPct) {
            Write-CI "Size delta: ${pct}% (max ${maxPct}%)" "PASS"
        } else {
            Write-CI "Size delta: ${pct}% > ${maxPct}% (regression!)" "FAIL"
            $failures += "Size increase ${pct}% exceeds max ${maxPct}%"
        }
    }
} else {
    Write-CI "No baseline - skipping size regression" "WARN"
    $warnings += "No baseline.json found"
}

# --- Step 4: Binary header check (first 4 bytes = valid AArch64 instruction) ---
$bytes = [System.IO.File]::ReadAllBytes($binPath)
if ($bytes.Length -ge 4) {
    $word0 = [BitConverter]::ToUInt32($bytes, 0)
    # Valid AArch64: branch (0x14xxxxxx), NOP (0xD503201F), or any instruction with bits[28:25] != 0
    $isValidArm64 = ($word0 -ne 0x00000000) -and ($word0 -ne 0xFFFFFFFF)
    if ($isValidArm64) {
        Write-CI "First instruction 0x$($word0.ToString('X8')) looks valid" "PASS"
    } else {
        Write-CI "First instruction 0x$($word0.ToString('X8')) suspicious (zero/all-ones)" "WARN"
        $warnings += "First instruction may be invalid: 0x$($word0.ToString('X8'))"
    }
} else {
    Write-CI "Binary too small to validate header" "FAIL"
    $failures += "Binary size < 4 bytes"
}

# --- Step 5: Sector alignment (U-Boot loads in 512-byte sectors) ---
# Note: non-alignment is a warning only; U-Boot pads with zeros
$sectors = [math]::Ceiling($binSize / 512)
$aligned  = $sectors * 512
if ($binSize % 512 -eq 0) {
    Write-CI "Sector-aligned: $sectors sectors" "PASS"
} else {
    Write-CI "Not sector-aligned: $binSize bytes = $sectors sectors ($aligned padded)" "WARN"
    $warnings += "Binary not 512-byte aligned"
}

# --- Step 6: Neural weights CRC verification ---
$elfPath = Join-Path $root "kernel_neuro.elf"
$nmTool  = "C:\gcc-arm\bin\aarch64-none-elf-nm.exe"
if ((Test-Path $elfPath) -and (Test-Path $nmTool)) {
    $nmOut = & $nmTool $elfPath 2>$null | Select-String "default_weights"
    if ($nmOut -match "([0-9a-f]+)\s+r\s+default_weights") {
        $wAddr  = [Convert]::ToInt64($matches[1], 16)
        $wOff   = [int]($wAddr - 0x02080000)
        $wSize  = 368  # sizeof(neural_weights_t): 6*8 + 8 + 8*4 + 4 = 92 i32 = 368 bytes
        if ($wOff -ge 0 -and ($wOff + $wSize) -le $bytes.Length) {
            # CRC32 (IEEE 802.3) - [uint32] ensures logical (not arithmetic) right-shift
            $poly = [uint32]3988292384  # 0xEDB88320
            $tbl = 0..255 | ForEach-Object {
                [uint32]$c = [uint32]$_
                1..8 | ForEach-Object { if ($c -band 1) { $c = ($c -shr 1) -bxor $poly } else { $c = $c -shr 1 } }
                $c
            }
            [uint32]$crc = [uint32]4294967295
            for ($i = $wOff; $i -lt ($wOff + $wSize); $i++) {
                [uint32]$idx = ($crc -bxor [uint32]$bytes[$i]) -band 255
                $crc = ($crc -shr 8) -bxor $tbl[$idx]
            }
            $computedCrc = "0x$((-bnot $crc).ToString('X8'))"

            if ($baseline -and $baseline.regression_thresholds.crc_weights_expected) {
                $expectedCrc = $baseline.regression_thresholds.crc_weights_expected
                if ($computedCrc -eq $expectedCrc) {
                    Write-CI "Weights CRC $computedCrc matches baseline" "PASS"
                } else {
                    Write-CI "Weights CRC $computedCrc != baseline $expectedCrc" "FAIL"
                    $failures += "Weights CRC mismatch: $computedCrc vs $expectedCrc"
                }
            } else {
                Write-CI "Weights CRC $computedCrc (no baseline reference)" "INFO"
            }
        } else {
            Write-CI "Weights offset 0x$($wOff.ToString('X')) out of binary range" "WARN"
            $warnings += "Could not verify weights CRC (offset out of range)"
        }
    } else {
        Write-CI "default_weights symbol not found in ELF" "WARN"
        $warnings += "default_weights symbol not found"
    }
} else {
    Write-CI "Skipping CRC check (ELF or nm not found)" "WARN"
    $warnings += "CRC check skipped"
}

# --- Report ---
Write-Host ""
Write-Host "========================================" -ForegroundColor Cyan
Write-Host "  CI RESULTS" -ForegroundColor Cyan
Write-Host "========================================" -ForegroundColor Cyan

if ($warnings.Count -gt 0) {
    Write-Host "Warnings:" -ForegroundColor Yellow
    $warnings | ForEach-Object { Write-Host "  - $_" -ForegroundColor Yellow }
}

if ($failures.Count -eq 0) {
    Write-Host ""
    Write-Host "  PASS ($($warnings.Count) warnings)" -ForegroundColor Green
    Write-Host "========================================" -ForegroundColor Green
    exit 0
} else {
    Write-Host "Failures:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "  - $_" -ForegroundColor Red }
    Write-Host ""
    Write-Host "  FAIL ($($failures.Count) failures, $($warnings.Count) warnings)" -ForegroundColor Red
    Write-Host "========================================" -ForegroundColor Red
    exit 1
}
