# test_optimized_kernel.ps1 - Test script for optimized kernel

Write-Host "=== H-Exo Optimized Kernel Testing ===" -ForegroundColor Green
Write-Host ""

# Test configuration
$testConfigs = @(
    @{
        Name = "Baseline"
        Makefile = "Makefile.neuro"
        Source = "main_neuro.c"
        Binary = "kernel_neuro.bin"
    },
    @{
        Name = "Optimized"
        Makefile = "Makefile.ultra_optimized"
        Source = "main_neuro_minimal.c"
        Binary = "kernel_neuro_ultra.bin"
    },
    @{
        Name = "Micro"
        Makefile = "Makefile.ultra_optimized"
        Source = "main_micro_kernel.c"
        Binary = "kernel_micro.bin"
    }
)

# Function to perform comprehensive tests
ection Test-Kernel {
    param(
        [hashtable]$Config
    )
    
    Write-Host "`n=== Testing $($Config.Name) Kernel ===" -ForegroundColor Cyan
    
    # Check if files exist
    if (-not (Test-Path $Config.Makefile)) {
        Write-Host "[ERROR] Makefile $($Config.Makefile) not found" -ForegroundColor Red
        return $false
    }
    
    # Clean build
    Write-Host "Cleaning previous build..." -ForegroundColor Gray
    & make -f $Config.Makefile clean 2>$null
    
    # Build
    Write-Host "Building kernel..." -ForegroundColor Gray
    try {
        $buildOutput = & make -f $Config.Makefile all 2>&1
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[ERROR] Build failed for $($Config.Name)" -ForegroundColor Red
            Write-Host $buildOutput -ForegroundColor Red
            return $false
        }
    } catch {
        Write-Host "[ERROR] Build exception: $_" -ForegroundColor Red
        return $false
    }
    
    # Check binary
    if (Test-Path $Config.Binary) {
        $size = (Get-Item $Config.Binary).Length
        Write-Host "[OK] Build successful" -ForegroundColor Green
        Write-Host "[INFO] Size: $size bytes ($([Math]::Round($size/1024, 2)) KB)" -ForegroundColor Cyan
        
        # Additional checks
        if ($size -gt 32768) {
            Write-Host "[WARNING] Kernel larger than 32KB" -ForegroundColor Yellow
        }
        
        return @{
            Success = $true
            Size = $size
        }
    } else {
        Write-Host "[ERROR] Binary $($Config.Binary) not created" -ForegroundColor Red
        return $false
    }
}

# Function to compare results
ection Compare-Results {
    param(
        [array]$Results
    )
    
    Write-Host "`n=== COMPARISON RESULTS ===" -ForegroundColor Green
    
    $baselineResult = $Results | Where-Object { $_.Config.Name -eq "Baseline" }
    if ($baselineResult) {
        Write-Host "Baseline size: $($baselineResult.Result.Size) bytes" -ForegroundColor Gray
        
        foreach ($result in $Results) {
            if ($result.Config.Name -ne "Baseline") {
                $savings = $baselineResult.Result.Size - $result.Result.Size
                $percent = [Math]::Round((($savings / $baselineResult.Result.Size) * 100), 2)
                Write-Host "$($result.Config.Name): $($result.Result.Size) bytes ($percent% smaller)" -ForegroundColor Cyan
            }
        }
    }
}

# Main test process
Write-Host "Starting comprehensive kernel testing..." -ForegroundColor Yellow

$results = @()

foreach ($config in $testConfigs) {
    $result = Test-Kernel -Config $config
    if ($result) {
        $results += @{
            Config = $config
            Result = $result
        }
        Write-Host "[PASS] $($config.Name) test completed`n" -ForegroundColor Green
    } else {
        Write-Host "[FAIL] $($config.Name) test failed`n" -ForegroundColor Red
    }
}

# Show comparison
Compare-Results -Results $results

# Final recommendations
Write-Host "`n=== RECOMMENDATIONS ===" -ForegroundColor Green
if ($results.Count -gt 0) {
    $smallest = $results | Where-Object { $_.Result.Success } | Sort-Object { $_.Result.Size } | Select-Object -First 1
    if ($smallest) {
        Write-Host "Recommended build: $($smallest.Config.Name) ($($smallest.Result.Size) bytes)" -ForegroundColor Green
    }
}

Write-Host "`n=== TESTING COMPLETE ===" -ForegroundColor Green