# analyze_kernel_size.ps1 - Script to analyze kernel size and sections

Write-Host "=== Kernel Size Analysis ===" -ForegroundColor Green
Write-Host ""

# Get file sizes
$binSize = (Get-Item "kernel_neuro.bin").Length
$elfSize = (Get-Item "kernel_neuro.elf").Length

Write-Host "Binary Size: $binSize bytes ($([Math]::Round($binSize/1024, 2)) KB)" -ForegroundColor Cyan
Write-Host "ELF Size: $elfSize bytes ($([Math]::Round($elfSize/1024, 2)) KB)" -ForegroundColor Cyan
Write-Host ""

# Try to analyze ELF sections if objdump is available
try {
    # Check if objdump is available
    $objdumpPath = "C:\gcc-arm\bin\aarch64-none-elf-objdump.exe"
    if (Test-Path $objdumpPath) {
        Write-Host "Analyzing ELF sections..." -ForegroundColor Yellow
        
        # Get section sizes
        $sectionInfo = & $objdumpPath -h "kernel_neuro.elf" 2>$null
        Write-Host $sectionInfo
        
        Write-Host "" 
        Write-Host "Analyzing symbols..." -ForegroundColor Yellow
        
        # Get largest symbols
        $symbolInfo = & $objdumpPath -t "kernel_neuro.elf" 2>$null | Select-String -Pattern "[0-9a-f]{8,}" | Sort-Object { [int64]($_ -split '\s+')[3] } -Descending | Select-Object -First 20
        Write-Host $symbolInfo
    } else {
        Write-Host "objdump not found at $objdumpPath" -ForegroundColor Red
        Write-Host "Install GCC ARM toolchain to get detailed analysis" -ForegroundColor Yellow
    }
} catch {
    Write-Host "Could not analyze ELF file: $_" -ForegroundColor Red
}

Write-Host "" 
Write-Host "=== Size Optimization Opportunities ===" -ForegroundColor Green
Write-Host "1. Neural network weights: ~400-500 bytes (critical, hard to reduce)" -ForegroundColor Cyan
Write-Host "2. Exception vector table: 2KB (aligned, can't reduce)" -ForegroundColor Cyan
Write-Host "3. Boot code: ~1-2KB (can be optimized)" -ForegroundColor Cyan
Write-Host "4. MMU setup: ~2KB (can be optimized)" -ForegroundColor Cyan
Write-Host "5. String literals and debug output: ~2-3KB (can be reduced)" -ForegroundColor Cyan
Write-Host "6. Unused functions and dead code: 1-2KB (can be removed)" -ForegroundColor Cyan
Write-Host "7. Stack size: 64KB (can be reduced to 16KB for this application)" -ForegroundColor Cyan
Write-Host "" 
Write-Host "Potential size reduction: 20-30KB" -ForegroundColor Green