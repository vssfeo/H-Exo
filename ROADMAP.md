# H-Exo Omni-Core: Testing & Development Roadmap

## Current Status (Phase 0.5 Complete ✅)

**Achievements:**
- ✅ Neural Arbitrator (Neuro-Sync) operational
- ✅ TinyML inference engine (6→8→4 network)
- ✅ Fixed-point arithmetic (Q16.16)
- ✅ Real-time telemetry collection
- ✅ Emergency beacon system designed
- ✅ Boot integrity test framework
- ✅ Automated YMODEM deployment
- ✅ **CRC32 validation for neural weights**
- ✅ **Heartbeat stability test with jitter measurement**
- ✅ **JSON structured logging**

**Working Kernel:** `kernel_neuro.bin` (12KB, 24 sectors)

## Immediate Priorities (Next 2 Weeks)

### 1. CRC Validation for Neural Weights ⚡ HIGH
**Goal:** Detect weight corruption during BSS clearing or deployment

**Implementation:**
```c
// neuro/weight_validation.h
u32 compute_weights_crc32(const neural_weights_t* weights);
bool validate_weights_integrity(neuro_sync_t* ns);

// At boot in main_neuro.c
u32 expected_crc = 0xDEADBEEF;  // Computed offline
u32 actual_crc = compute_weights_crc32(ns->weights);
if (actual_crc != expected_crc) {
    uart_puts("FATAL: Neural weights corrupted!\r\n");
    panic();
}
```

**Files to create:**
- `neuro/weight_validation.h`
- `neuro/weight_validation.c`
- `tools/compute_weights_crc.py` (offline calculator)

**Estimated time:** 2 hours

---

### 2. Heartbeat Stability Test 🔄 HIGH
**Goal:** Measure kernel stability and jitter under load

**Implementation:**
```c
// In main_neuro.c - add heartbeat mode
void heartbeat_mode(void) {
    u64 last_beat = read_cycle_counter();
    u32 beat_count = 0;
    
    while (1) {
        u64 now = read_cycle_counter();
        u64 delta = now - last_beat;
        
        if (delta >= 150000000) {  // 100ms at 1.5GHz
            uart_puts("BEAT ");
            uart_put_hex(beat_count++);
            uart_puts("\r\n");
            last_beat = now;
        }
    }
}
```

**PowerShell test:**
```powershell
# test_heartbeat.ps1
$beats = @()
while ($true) {
    $line = $port.ReadLine()
    if ($line -match "BEAT (\d+)") {
        $beats += [DateTime]::Now
        if ($beats.Count > 10) {
            $jitter = Measure-Jitter $beats
            if ($jitter -gt 5) {
                Write-Warning "Jitter: $jitter%"
            }
        }
    }
}
```

**Estimated time:** 3 hours

---

### 3. Performance Benchmarking Suite 📊 MEDIUM
**Goal:** Track performance metrics across builds

**Metrics to track:**
- Boot time (power-on → "Operational")
- Neural inference latency (min/max/avg)
- Memory footprint
- Cache hit rate
- UART throughput

**Implementation:**
```c
// core/benchmark.h
typedef struct {
    u64 boot_cycles;
    u32 inference_min_us;
    u32 inference_max_us;
    u32 inference_avg_us;
    u32 cache_hit_rate;
} benchmark_results_t;

void benchmark_init(void);
void benchmark_record_boot(void);
void benchmark_record_inference(u32 latency_us);
void benchmark_print_results(void);
```

**Output format (JSON for automation):**
```json
{
  "timestamp": "2026-03-25T22:00:00Z",
  "kernel_version": "v0.3",
  "boot_time_ms": 234,
  "inference_latency_us": {"min": 750, "max": 850, "avg": 800},
  "memory_kb": 12,
  "cache_hit_rate": 94.2
}
```

**Estimated time:** 4 hours

---

### 4. Structured JSON Logging 📝 MEDIUM
**Goal:** Enable automated test result parsing

**Implementation:**
```c
// core/logger.h
typedef enum {
    LOG_INFO,
    LOG_WARN,
    LOG_ERROR,
    LOG_PERF
} log_level_t;

void log_json(log_level_t level, const char* component, const char* message);
void log_perf(const char* metric, u64 value);

// Usage
log_json(LOG_INFO, "MMU", "Enabled");
log_perf("boot_time_ms", 234);
```

**Output:**
```json
{"level":"INFO","component":"MMU","message":"Enabled"}
{"level":"PERF","metric":"boot_time_ms","value":234}
```

**PowerShell parsing:**
```powershell
$logs = $output | Where-Object { $_ -match '^\{' } | ConvertFrom-Json
$perf_metrics = $logs | Where-Object { $_.level -eq "PERF" }
```

**Estimated time:** 3 hours

---

### 5. Automated Regression Testing 🤖 HIGH
**Goal:** Detect performance regressions automatically

**Framework:**
```powershell
# regression_test.ps1
$baseline = Get-Content baseline.json | ConvertFrom-Json
$current = Run-BenchmarkSuite

$regressions = @()
if ($current.boot_time_ms > $baseline.boot_time_ms * 1.1) {
    $regressions += "Boot time: +10%"
}
if ($current.kernel_size_bytes > $baseline.kernel_size_bytes * 1.05) {
    $regressions += "Kernel size: +5%"
}

if ($regressions.Count -gt 0) {
    Write-Error "REGRESSIONS DETECTED:"
    $regressions | ForEach-Object { Write-Error "  $_" }
    exit 1
}
```

**Baseline storage:**
```json
{
  "version": "v0.3",
  "date": "2026-03-25",
  "boot_time_ms": 234,
  "kernel_size_bytes": 12288,
  "inference_time_us": 800,
  "cache_hit_rate": 94.2
}
```

**Estimated time:** 4 hours

---

## Medium-Term Goals (1-2 Months)

### 6. Hardware-in-the-Loop (HIL) Testing
**Goal:** Automated stress testing with real hardware

**Scenarios:**
- **Thermal stress:** Run at max CPU frequency for 1 hour
- **Memory stress:** Allocate/deallocate in heap repeatedly
- **Neural stress:** 10,000 consecutive inferences
- **Power cycling:** Automated reboot 100 times

**Tools needed:**
- USB relay for power control
- Temperature monitoring via TSADC
- Automated test orchestration

**Estimated time:** 1 week

---

### 7. CI/CD Pipeline
**Goal:** Automated build & test on every commit

**GitHub Actions workflow:**
```yaml
name: H-Exo CI/CD
on: [push, pull_request]
jobs:
  build-and-test:
    runs-on: self-hosted  # Machine with NanoPi connected
    steps:
      - uses: actions/checkout@v2
      - name: Build kernel
        run: make -f Makefile.neuro
      - name: Run tests
        run: |
          .\test_boot_integrity.ps1
          .\test_heartbeat.ps1
          .\regression_test.ps1
      - name: Upload artifacts
        uses: actions/upload-artifact@v2
        with:
          name: kernel-${{ github.sha }}
          path: kernel_neuro.bin
```

**Estimated time:** 1 week

---

### 8. Visual Dashboard
**Goal:** Real-time monitoring and historical tracking

**Stack:**
- **Backend:** Node.js + Express
- **Frontend:** React + Chart.js
- **Database:** SQLite for metrics history
- **WebSocket:** Real-time UART streaming

**Features:**
- Live UART console
- Performance graphs (boot time, inference latency)
- Test history timeline
- Regression alerts

**Estimated time:** 2 weeks

---

### 9. Fuzzing Infrastructure
**Goal:** Discover edge cases and crashes

**Implementation:**
```c
// test/fuzzer.c
void fuzz_neural_arbitrator(u32 iterations) {
    for (u32 i = 0; i < iterations; i++) {
        telemetry_t input = {
            .cpu_load = rand() % 100,
            .l2_latency_us = rand() % 1000,
            .memory_pressure = rand() % 100,
            .thermal_state = rand() % 100,
            .packet_rate = rand() % 10000,
            .node_count = rand() % 100
        };
        
        inference_result_t output;
        result_t res = neuro_sync_inference(&ns, &input, &output);
        
        if (res != OK) {
            log_json(LOG_ERROR, "FUZZER", "Inference crashed");
            dump_registers();
            panic("Fuzzing detected crash");
        }
    }
    log_json(LOG_INFO, "FUZZER", "All iterations passed");
}
```

**Estimated time:** 1 week

---

## Long-Term Vision (3-6 Months)

### 10. Multi-Board Testing
**Goal:** Test distributed mesh with multiple NanoPi M4 boards

**Setup:**
- 3-5 NanoPi M4 boards
- Ethernet switch for L2 mesh
- Automated power control
- Synchronized testing

**Estimated time:** 1 month

---

### 11. Fix Optimized Kernel Issues
**Goal:** Debug and deploy boot_optimized.s with emergency beacons

**Current issue:** UART2 not initialized by U-Boot, causing Synchronous Abort

**Solutions to try:**
1. Initialize UART2 in assembly before DEBUG_PUTC
2. Use different peripheral (GPIO LED) for beacons
3. Map UART2 in early page tables before access

**Estimated time:** 1 week

---

## Priority Matrix

| Task | Priority | Effort | Impact | Status |
|------|----------|--------|--------|--------|
| CRC Validation | HIGH | 2h | HIGH | Pending |
| Heartbeat Test | HIGH | 3h | HIGH | Pending |
| Regression Framework | HIGH | 4h | HIGH | Pending |
| Performance Benchmarking | MEDIUM | 4h | HIGH | Pending |
| JSON Logging | MEDIUM | 3h | MEDIUM | Pending |
| HIL Testing | MEDIUM | 1w | MEDIUM | Pending |
| CI/CD Pipeline | MEDIUM | 1w | HIGH | Pending |
| Visual Dashboard | LOW | 2w | MEDIUM | Pending |
| Fuzzing | LOW | 1w | MEDIUM | Pending |
| Multi-Board Testing | LOW | 1m | HIGH | Pending |
| Fix Optimized Kernel | MEDIUM | 1w | MEDIUM | Pending |

---

## Execution Plan (Next 7 Days)

**Day 1-2:** CRC Validation + Heartbeat Test
**Day 3-4:** Performance Benchmarking Suite
**Day 5:** JSON Logging
**Day 6-7:** Regression Testing Framework

**Total estimated time:** ~20 hours of focused development

---

## Success Metrics

- ✅ All tests pass automatically
- ✅ Zero regressions detected
- ✅ Boot time < 250ms
- ✅ Neural inference < 1ms
- ✅ 100% uptime in 1-hour stress test
- ✅ CI/CD pipeline green on every commit

---

## Notes

**Current blockers:**
- Optimized kernel UART initialization (non-critical)
- No automated power cycling yet (manual reboot required)

**Dependencies:**
- None - all work can proceed independently

**Risk mitigation:**
- Keep working kernel (kernel_neuro.bin) as fallback
- Incremental testing after each feature
- Version control for all changes
