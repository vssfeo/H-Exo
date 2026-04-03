# H-Exo Omni-Core: Testing System Documentation

## Overview

Comprehensive testing framework for H-Exo bare-metal kernel development on RK3399 NanoPi M4.

## Emergency Beacon System

### Purpose
Low-level debugging system that outputs characters directly to UART2 hardware registers, bypassing all software layers (MMU, stack, C runtime).

### Beacon Sequence

| Beacon | Location | Meaning | Failure Diagnosis |
|--------|----------|---------|-------------------|
| `1` | `_start` entry | Hardware under control | CPU not executing code |
| `2` | Before `eret` EL3→EL2 | About to transition | Stuck in EL3 setup |
| `3` | After EL2 entry | Successfully at EL2 | EL3→EL2 transition crashed |
| `4` | EL1 entry | At EL1, before MMU | EL2→EL1 transition crashed |
| `5` | After `mmu_enable` | MMU working | Page table error |

### Usage

```powershell
# Run boot integrity test
.\test_boot_integrity.ps1

# Expected output:
# 12345
# [OK] Hardware: RK3399...
```

### Failure Analysis

**Scenario 1: Only `1` appears**
- Problem: Exception level transition failing
- Check: SPSR_EL3, SCR_EL3 configuration

**Scenario 2: `12` but no `3`**
- Problem: EL3→EL2 `eret` instruction crash
- Check: ELR_EL3 points to valid code

**Scenario 3: `1234` but no `5`**
- Problem: MMU initialization or page table error
- Check: TCR_EL1, TTBR0_EL1, page table alignment

**Scenario 4: `12345` but no kernel messages**
- Problem: C runtime initialization (BSS clearing, stack)
- Check: Linker script, BSS overlap with .rodata

## Test Scripts

### 1. `test_boot_integrity.ps1`
**Purpose:** Validate boot sequence with beacon detection

**Features:**
- Automated beacon sequence validation
- Kernel message verification
- Failure point diagnosis
- Performance metrics

**Usage:**
```powershell
.\test_boot_integrity.ps1 -PortName COM3 -BaudRate 1500000
```

### 2. `test_neuro.ps1`
**Purpose:** Test Neural Arbitrator (Neuro-Sync) functionality

**Features:**
- TinyML inference validation
- Telemetry collection test
- Interactive demo mode

**Usage:**
```powershell
.\test_neuro.ps1
# Press SPACE to trigger inference
# Press 'q' to exit demo
```

### 3. `auto_test.ps1`
**Purpose:** Basic kernel boot test (legacy)

**Usage:**
```powershell
.\auto_test.ps1
```

## Linker Script Validation

### Critical Sections

```
.text       → Code (executable)
.rodata     → Neural network weights (READ-ONLY, must not overlap BSS)
.data       → Initialized data
.page_tables → MMU page tables (4KB aligned)
.bss        → Zero-initialized (cleared at boot)
```

### Alignment Requirements

- **Page tables:** 4KB (0x1000) alignment
- **Exception vectors:** 2KB (0x800) alignment
- **.rodata:** 64-byte (cache line) alignment

### Overlap Detection

```bash
# Check for overlaps
aarch64-none-elf-nm -n kernel_optimized.elf | grep -E "(rodata|bss)"
```

## Neural Arbitrator Testing

### Weight Integrity Check

Add to `main_neuro.c`:

```c
// CRC32 checksum of neural weights
u32 compute_weights_crc(void) {
    u32 crc = 0xFFFFFFFF;
    u8* data = (u8*)&default_weights;
    usize len = sizeof(default_weights);
    
    for (usize i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
        }
    }
    return ~crc;
}

// At boot
u32 crc = compute_weights_crc();
uart_puts("Weights CRC: 0x");
uart_put_hex(crc);
// Expected: 0xXXXXXXXX (compare with offline calculation)
```

### Fuzzing Test

```c
void fuzz_neural_arbitrator(void) {
    for (int i = 0; i < 10000; i++) {
        telemetry_t random_input = {
            .cpu_load = rand() % 100,
            .l2_latency_us = rand() % 1000,
            // ...
        };
        
        inference_result_t output;
        if (neuro_sync_inference(&ns, &random_input, &output) != OK) {
            panic("Neural inference crashed");
        }
    }
}
```

## Performance Benchmarking

### Boot Time Measurement

```powershell
# In test script
$bootStart = [DateTime]::Now
# ... wait for "Operational" message ...
$bootEnd = [DateTime]::Now
$bootTime = ($bootEnd - $bootStart).TotalMilliseconds
Write-Host "Boot time: $bootTime ms"
```

### Neural Inference Latency

```c
// In kernel
u64 start = read_cycle_counter();
neuro_sync_inference(&ns, &input, &output);
u64 end = read_cycle_counter();
u64 cycles = end - start;
u32 us = cycles / 1500;  // 1.5GHz CPU
uart_puts("Inference: ");
uart_put_hex(us);
uart_puts(" us\r\n");
```

## Regression Testing

### Baseline Metrics

Create `baseline.json`:

```json
{
    "boot_time_ms": 234,
    "kernel_size_bytes": 36864,
    "inference_time_us": 800,
    "beacons_detected": 5
}
```

### Automated Comparison

```powershell
$baseline = Get-Content baseline.json | ConvertFrom-Json
if ($current.boot_time_ms > $baseline.boot_time_ms * 1.1) {
    Write-Warning "Boot time regression: +10%"
}
```

## Hardware-in-the-Loop (HIL) Testing

### Stress Test Scenario

```powershell
function Test-StressLoad {
    # 1. Boot kernel
    .\test_neuro.ps1
    
    # 2. Send 1000 inference commands
    for ($i = 0; $i -lt 1000; $i++) {
        Send-SerialCommand " "
        Start-Sleep -Milliseconds 10
    }
    
    # 3. Verify no crashes
    $output = Read-SerialOutput
    return ($output -notmatch "panic|exception")
}
```

## CI/CD Integration

### GitHub Actions Example

```yaml
name: H-Exo Build & Test
on: [push]
jobs:
  test:
    runs-on: self-hosted
    steps:
      - uses: actions/checkout@v2
      - name: Build
        run: make -f Makefile.neuro
      - name: Boot Integrity Test
        run: .\test_boot_integrity.ps1
      - name: Upload Results
        uses: actions/upload-artifact@v2
        with:
          name: test-results
          path: test_results.json
```

## Troubleshooting

### Common Issues

**Issue:** Beacons not appearing
- **Cause:** UART not initialized by bootloader
- **Fix:** Check U-Boot UART2 configuration

**Issue:** Beacon `5` missing, kernel silent
- **Cause:** Page table misconfiguration
- **Fix:** Verify UART2 (0xFF1A0000) mapped as Device memory

**Issue:** Random characters instead of beacons
- **Cause:** Baud rate mismatch
- **Fix:** Ensure 1,500,000 baud on both sides

## Best Practices

1. **Always run boot integrity test** after linker script changes
2. **Verify beacon sequence** before debugging C code
3. **Check .rodata alignment** when adding neural network weights
4. **Use structured logging** (JSON) for automated parsing
5. **Maintain baseline metrics** for regression detection

## Future Enhancements

- [ ] Automated power cycling via USB relay
- [ ] Real-time performance dashboard
- [ ] Continuous fuzzing infrastructure
- [ ] Multi-board parallel testing
- [ ] Thermal stress testing
