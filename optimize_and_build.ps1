# optimize_and_build.ps1 - Automated optimization and build script

Write-Host "=== H-Exo Kernel Optimization and Build ===" -ForegroundColor Green
Write-Host ""

# Check if required tools exist
$gccPath = "C:\gcc-arm\bin\aarch64-none-elf-gcc.exe"
if (-not (Test-Path $gccPath)) {
    Write-Host "[WARNING] GCC ARM toolchain not found at $gccPath" -ForegroundColor Yellow
    Write-Host "         Size optimization analysis will be limited" -ForegroundColor Yellow
}

# Function to build with different optimization levels
ection Build-Kernel {
    param(
        [string]$ConfigName,
        [string]$Makefile,
        [string]$SourceFile
    )
    
    Write-Host "`n=== Building $ConfigName ===" -ForegroundColor Cyan
    
    # Backup original files if needed
    if ($SourceFile -and (Test-Path $SourceFile)) {
        Copy-Item $SourceFile "$SourceFile.backup" -Force
    }
    
    try {
        # Build with specified makefile
        if (Test-Path $Makefile) {
            Write-Host "Using makefile: $Makefile" -ForegroundColor Gray
            & make -f $Makefile clean 2>$null
            & make -f $Makefile all 2>$null
            
            # Check if build succeeded
            $targetBin = "kernel_neuro_ultra.bin"
            if (Test-Path $targetBin) {
                $size = (Get-Item $targetBin).Length
                Write-Host "[$ConfigName] Build successful!" -ForegroundColor Green
                Write-Host "[$ConfigName] Size: $size bytes ($([Math]::Round($size/1024, 2)) KB)" -ForegroundColor Cyan
                return $size
            } else {
                Write-Host "[$ConfigName] Build failed" -ForegroundColor Red
                return $null
            }
        } else {
            Write-Host "Makefile $Makefile not found" -ForegroundColor Red
            return $null
        }
    } catch {
        Write-Host "[$ConfigName] Build error: $_" -ForegroundColor Red
        return $null
    } finally {
        # Restore original files
        if (Test-Path "$SourceFile.backup") {
            Move-Item "$SourceFile.backup" $SourceFile -Force
        }
    }
}

# Function to analyze size savings
ection Analyze-Savings {
    param(
        [int]$OriginalSize,
        [int]$OptimizedSize
    )
    
    if ($OriginalSize -and $OptimizedSize) {
        $savings = $OriginalSize - $OptimizedSize
        $percent = [Math]::Round((($savings / $OriginalSize) * 100), 2)
        Write-Host "`n=== SIZE OPTIMIZATION RESULTS ===" -ForegroundColor Green
        Write-Host "Original size: $OriginalSize bytes" -ForegroundColor Gray
        Write-Host "Optimized size: $OptimizedSize bytes" -ForegroundColor Gray
        Write-Host "Space saved: $savings bytes ($percent%)" -ForegroundColor Green
        
        if ($savings -gt 0) {
            Write-Host "`nBENEFITS:" -ForegroundColor Yellow
            Write-Host "1. Faster boot time (smaller image)" -ForegroundColor Cyan
            Write-Host "2. Reduced memory footprint" -ForegroundColor Cyan
            Write-Host "3. More space for other applications" -ForegroundColor Cyan
            Write-Host "4. Lower power consumption" -ForegroundColor Cyan
        }
    }
}

# Main optimization process
Write-Host "Starting optimization process..." -ForegroundColor Yellow

# 1. Build original kernel to get baseline
Write-Host "`n1. Building baseline kernel..." -ForegroundColor Yellow
$originalSize = Build-Kernel -ConfigName "Baseline" -Makefile "Makefile.neuro" -SourceFile "main_neuro.c"

# 2. Build ultra-optimized kernel
Write-Host "`n2. Building ultra-optimized kernel..." -ForegroundColor Yellow
$optimizedSize = Build-Kernel -ConfigName "Ultra-Optimized" -Makefile "Makefile.ultra_optimized" -SourceFile "main_neuro_minimal.c"

# 3. Analyze savings
Analyze-Savings -OriginalSize $originalSize -OptimizedSize $optimizedSize

# 4. Provide recommendations
Write-Host "`n=== RECOMMENDATIONS ===" -ForegroundColor Green
Write-Host "1. Use Makefile.ultra_optimized for production builds" -ForegroundColor Cyan
Write-Host "2. Enable MINIMAL_OUTPUT flag for maximum size reduction" -ForegroundColor Cyan
Write-Host "3. Consider using main_neuro_minimal.c instead of main_neuro.c" -ForegroundColor Cyan
Write-Host "4. Review unused features in neuro/ directory" -ForegroundColor Cyan
Write-Host "5. Remove debug strings and verbose output in production" -ForegroundColor Cyan

Write-Host "`n=== OPTIMIZATION COMPLETE ===" -ForegroundColor Green