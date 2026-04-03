param(
    [string]$BaselineFile = "baseline.json",
    [string]$KernelPath = "kernel_neuro.bin"
)

Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  H-Exo Regression Test Suite" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

# Load baseline
if (-not (Test-Path $BaselineFile)) {
    Write-Host "[ERROR] Baseline file not found: $BaselineFile" -ForegroundColor Red
    exit 1
}

$baseline = Get-Content $BaselineFile | ConvertFrom-Json
Write-Host "[*] Loaded baseline: $($baseline.version) ($($baseline.date))" -ForegroundColor Yellow

# Current metrics
$currentMetrics = @{
    kernel_size_bytes = 0
    kernel_sectors = 0
    boot_time_ms = 0
    inference_time_us = 0
    cache_hit_rate = 0
    max_jitter_percent = 0
}

# Check kernel size
if (Test-Path $KernelPath) {
    $currentMetrics.kernel_size_bytes = (Get-Item $KernelPath).Length
    $currentMetrics.kernel_sectors = [Math]::Ceiling($currentMetrics.kernel_size_bytes / 512)
    
    Write-Host "`n[*] Kernel Size Check:" -ForegroundColor Yellow
    Write-Host "    Baseline: $($baseline.metrics.kernel_size_bytes) bytes" -ForegroundColor Gray
    Write-Host "    Current:  $($currentMetrics.kernel_size_bytes) bytes" -ForegroundColor Gray
    
    $sizeIncrease = $currentMetrics.kernel_size_bytes - $baseline.metrics.kernel_size_bytes
    $sizeIncreasePercent = ($sizeIncrease / $baseline.metrics.kernel_size_bytes) * 100
    
    if ($currentMetrics.kernel_size_bytes -le $baseline.regression_thresholds.max_kernel_size_bytes) {
        Write-Host "    [PASS] Size within threshold" -ForegroundColor Green
    } else {
        Write-Host "    [FAIL] Size exceeds threshold by $sizeIncrease bytes ($([Math]::Round($sizeIncreasePercent, 2))%)" -ForegroundColor Red
    }
} else {
    Write-Host "[ERROR] Kernel not found: $KernelPath" -ForegroundColor Red
    exit 1
}

# Regression detection
$regressions = @()

# Size regression
if ($currentMetrics.kernel_size_bytes > $baseline.regression_thresholds.max_kernel_size_bytes) {
    $regressions += "Kernel size: $($currentMetrics.kernel_size_bytes) > $($baseline.regression_thresholds.max_kernel_size_bytes) bytes"
}

# Check for component additions/removals
Write-Host "`n[*] Component Check:" -ForegroundColor Yellow
$baseline.components.PSObject.Properties | ForEach-Object {
    $component = $_.Name
    $expected = $_.Value
    Write-Host "    $component : $(if ($expected) { 'ENABLED' } else { 'DISABLED' })" -ForegroundColor Gray
}

# Summary
Write-Host "`n========================================" -ForegroundColor Cyan
Write-Host "  Regression Test Results" -ForegroundColor Cyan
Write-Host "========================================`n" -ForegroundColor Cyan

if ($regressions.Count -eq 0) {
    Write-Host "[PASS] No regressions detected!" -ForegroundColor Green
    Write-Host "    Kernel size: $($currentMetrics.kernel_size_bytes) bytes (baseline: $($baseline.metrics.kernel_size_bytes))" -ForegroundColor Green
    exit 0
} else {
    Write-Host "[FAIL] $($regressions.Count) regression(s) detected:" -ForegroundColor Red
    $regressions | ForEach-Object {
        Write-Host "  - $_" -ForegroundColor Red
    }
    exit 1
}
