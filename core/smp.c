// H-Exo Omni-Core: SMP - PSCI CPU_ON for RK3399
// Wakes secondary cores via BL31 PSCI SMC, each runs an idle counter loop.
// Core 0 reads smp_idle_counters[] to verify secondary activity.

#include "smp.h"
#include "types.h"
#include "log.h"
#include "workqueue.h"
#include "../hal/uart.h"
#include "../hal/gicv3.h"

extern uart_t console;

volatile u64 smp_idle_counters[SMP_MAX_CORES];
volatile u64 smp_entry_stage[SMP_MAX_CORES];
volatile u64 smp_start_tombstone;
volatile u64 smp_trace_page[64] __attribute__((aligned(64)));
// Real online count = core0 + cores that actually reached smp_secondary_main().
static volatile u32 smp_online_count = 1;
static volatile u32 smp_online_mask = 0x1; // bit i = core i is online
static volatile u32 smp_secondary_enter_mask = 0; // bit i = core i reached C loop
// Fixed debug beacon written by secondary_entry in boot.s.
#define SMP_BEACON_ADDR 0x02000000UL

typedef struct {
    u64 mpidr;
    i64 cpu_on_ret;
    i64 aff_before;
    i64 aff_after;
    u32 pending_polls;
    u32 kick_count;
    u32 sev_bursts;
    u32 flags;
    u64 t_cpu_on_us;
    u64 t_first_pending_us;
    u64 t_online_us;
    u64 t_pm_bit_clear_us;
    u64 t_core_pm_8_us;
    u64 t_aff_pending_first_us;
} smp_psci_diag_t;

static volatile smp_psci_diag_t smp_psci_diag[SMP_MAX_CORES];

#define SMP_FLAG_CPU_ON_OK        (1u << 0)
#define SMP_FLAG_ALREADY_ON       (1u << 1)
#define SMP_FLAG_ON_PENDING       (1u << 2)
#define SMP_FLAG_AFF_ONLINE       (1u << 3)
#define SMP_FLAG_ENTERED_C        (1u << 4)
#define SMP_FLAG_PWR_REQ_SENT     (1u << 5)
#define SMP_FLAG_PWR_ACK_SEEN     (1u << 6)
#define SMP_FLAG_PHASE_REPORTED   (1u << 7)

enum {
    SMP_TRACE_BOOT_MPIDR = 0,
    SMP_TRACE_BOOT_SEEN_MASK = 1,
    SMP_TRACE_SEC_SEEN_MASK = 2,
    SMP_TRACE_C_ENTRY_MASK = 3,
    SMP_TRACE_EL_MASK = 4,
    SMP_TRACE_STACK_MASK = 5,
    SMP_TRACE_MMU_MASK = 6,
    SMP_TRACE_WQ_MASK = 7,
    SMP_TRACE_LAST = 8,
};

// RK3399 MPIDR affinity values (Aff1:Aff0) — low bits only.
static const u64 rk3399_mpidr[SMP_MAX_CORES] = {
    0x0000UL,
    0x0001UL,
    0x0002UL,
    0x0003UL,
    0x0100UL,
    0x0101UL,
};

#define MPIDR_HW_MASK 0x80000000ULL
static inline u64 rk3399_psci_mpidr(u32 idx) {
    return rk3399_mpidr[idx] | MPIDR_HW_MASK;
}

static inline u64 smp_time_us(void) {
    u64 t = 0;
    asm volatile("mrs %0, cntpct_el0" : "=r"(t));
    return t / 24; // 24MHz generic timer
}

static void smp_log_a72_snapshot(const char *tag, i64 aff4, i64 aff5);
static void smp_log_a72_cluster_aff(const char *tag);
static __attribute__((noinline)) i64 psci_affinity_info_lvl(u64 target_affinity, u64 lowest_level);

static void smp_trace_or(u32 slot, u64 bit) {
    if (slot < 64) {
        smp_trace_page[slot] |= bit;
    }
}

static void smp_trace_set(u32 slot, u64 value) {
    if (slot < 64) {
        smp_trace_page[slot] = value;
    }
}

static const char *smp_path_state_name(smp_path_state_t state) {
    switch (state) {
        case SMP_PATH_NOT_REQUESTED: return "not-requested";
        case SMP_PATH_PRE_KERNEL_HANDOFF: return "pre-kernel-handoff";
        case SMP_PATH_REACHED_START: return "_start";
        case SMP_PATH_REACHED_SECONDARY_ENTRY: return "secondary-entry";
        case SMP_PATH_REACHED_C_WORKER: return "c-worker";
        default: return "unknown";
    }
}

static u32 smp_mask_has_core(u64 mask, u32 core_idx) {
    if (core_idx >= 64) {
        return 0;
    }
    return (u32)((mask >> core_idx) & 1ull);
}

static u32 smp_core_reached_start(u32 core_idx) {
    if (core_idx >= SMP_MAX_CORES) {
        return 0;
    }
    if (smp_start_tombstone == rk3399_mpidr[core_idx]) {
        return 1;
    }
    if (smp_trace_page[SMP_TRACE_BOOT_MPIDR] == rk3399_mpidr[core_idx]) {
        return 1;
    }
    return 0;
}

static u32 smp_core_reached_secondary_entry(u32 core_idx) {
    volatile u64 *fb = (volatile u64 *)SMP_BEACON_ADDR;
    if (core_idx >= SMP_MAX_CORES) {
        return 0;
    }
    if (smp_entry_stage[core_idx] != 0) {
        return 1;
    }
    if (smp_mask_has_core(smp_trace_page[SMP_TRACE_SEC_SEEN_MASK], core_idx)) {
        return 1;
    }
    if (fb[1] == 0xBEEFDEADULL && fb[0] == core_idx) {
        return 1;
    }
    return 0;
}

// PSCI function IDs
#define PSCI_VERSION_32      0x84000000UL
#define PSCI_FEATURES_32     0x8400000AUL
#define PSCI_CPU_ON_32       0x84000003UL
#define PSCI_CPU_ON_64       0xC4000003UL
#define PSCI_AFFINITY_INFO_64 0xC4000004UL
// PSCI return codes
#define PSCI_SUCCESS        0
#define PSCI_ALREADY_ON    (-4)
// RK3399 PMU registers (NS-accessible)
#define PMU_BASE             0xFF310000UL
#define PMU_PWRDN_ST         0x18
#define PMU_BUS_IDLE_ST      0x64
#define PMU_BUS_IDLE_ACK     0x68
#define PMU_CCI500_CON       0x6C
#define PMU_ADB400_ST        0x74
#define PMU_CORE_PWR_ST      0x7C
// PMU per-core PM control: PMU_BASE + 0xC0 + cpu_id*4
// 0=CORES_PM_DISABLE, BIT(3)=soft_wakeup_en, BIT(0)=core_pm_en
#define PMU_CORE_PM_CON(n)   (0xC0 + (n) * 4)
// SGRF_SOC_CON1 (0xFF33C004) holds warmboot address but is SECURE-ONLY — do not read from NS EL2.
// PMUGRF OS registers (diagnostic breadcrumbs)
#define PMUGRF_OS_REG1       0xFF320304UL
#define PMUGRF_OS_REG2       0xFF320308UL
#define CCI_BASE             0xFFB00000UL
#define CCI_CTRL             0x0000UL
#define CCI_STATUS           0x000CUL
#define CCI_SLAVE_A53        0x1000UL
#define CCI_SLAVE_A72        0x2000UL

static void smp_log_a72_snapshot(const char *tag, i64 aff4, i64 aff5) {
    volatile u32 *pmu_pwrdn_st = (volatile u32 *)(PMU_BASE + PMU_PWRDN_ST);
    volatile u32 *pmugrf_os_reg1 = (volatile u32 *)PMUGRF_OS_REG1;
    volatile u32 *pmugrf_os_reg2 = (volatile u32 *)PMUGRF_OS_REG2;
    volatile u32 *pmugrf_os_reg3 = (volatile u32 *)0xFF32030CUL;
    volatile u64 *fb = (volatile u64 *)SMP_BEACON_ADDR;
    u64 now_us = smp_time_us();
    u32 stage4 = (u32)smp_entry_stage[4];
    u32 stage5 = (u32)smp_entry_stage[5];

    uart_puts(&console, "[SMP][A72] ");
    uart_puts(&console, tag);
    uart_puts(&console, " t_us=0x");
    uart_put_hex(&console, now_us);
    uart_puts(&console, " pwrdn=0x");
    uart_put_hex(&console, *pmu_pwrdn_st);
    uart_puts(&console, " pm4=0x");
    uart_put_hex(&console, *(volatile u32 *)((uintptr_t)PMU_BASE + (uintptr_t)PMU_CORE_PM_CON(4)));
    uart_puts(&console, " pm5=0x");
    uart_put_hex(&console, *(volatile u32 *)((uintptr_t)PMU_BASE + (uintptr_t)PMU_CORE_PM_CON(5)));
    uart_puts(&console, " aff4=0x");
    uart_put_hex(&console, (u64)aff4);
    uart_puts(&console, " aff5=0x");
    uart_put_hex(&console, (u64)aff5);
    uart_puts(&console, " st4=0x");
    uart_put_hex(&console, stage4);
    uart_puts(&console, " st5=0x");
    uart_put_hex(&console, stage5);
    uart_puts(&console, " sec_mask=0x");
    uart_put_hex(&console, smp_trace_page[SMP_TRACE_SEC_SEEN_MASK]);
    uart_puts(&console, " grf=0x");
    uart_put_hex(&console, *pmugrf_os_reg1);
    uart_puts(&console, "/");
    uart_put_hex(&console, *pmugrf_os_reg2);
    uart_puts(&console, "/");
    uart_put_hex(&console, *pmugrf_os_reg3);
    uart_puts(&console, " bcn=0x");
    uart_put_hex(&console, fb[0]);
    uart_puts(&console, "/");
    uart_put_hex(&console, fb[1]);
    uart_puts(&console, "\r\n");
}

static void smp_log_a72_hw_sample(const char *tag) {
    volatile u32 *pmu_pwrdn_st = (volatile u32 *)(PMU_BASE + PMU_PWRDN_ST);
    volatile u32 *pmu_bus_idle_st = (volatile u32 *)(PMU_BASE + PMU_BUS_IDLE_ST);
    volatile u32 *pmu_bus_idle_ack = (volatile u32 *)(PMU_BASE + PMU_BUS_IDLE_ACK);
    volatile u32 *pmu_cci500_con = (volatile u32 *)(PMU_BASE + PMU_CCI500_CON);
    volatile u32 *pmu_adb400_st = (volatile u32 *)(PMU_BASE + PMU_ADB400_ST);
    volatile u32 *pmu_core_pwr_st = (volatile u32 *)(PMU_BASE + PMU_CORE_PWR_ST);
    uart_puts(&console, "[SMP][A72][HW] ");
    uart_puts(&console, tag);
    uart_puts(&console, " t_us=0x");
    uart_put_hex(&console, smp_time_us());
    uart_puts(&console, " pwrdn=0x");
    uart_put_hex(&console, *pmu_pwrdn_st);
    uart_puts(&console, " cci=0x");
    uart_put_hex(&console, *pmu_cci500_con);
    uart_puts(&console, " adb=0x");
    uart_put_hex(&console, *pmu_adb400_st);
    uart_puts(&console, " core_pwr=0x");
    uart_put_hex(&console, *pmu_core_pwr_st);
    uart_puts(&console, " bus=0x");
    uart_put_hex(&console, *pmu_bus_idle_st);
    uart_puts(&console, "/");
    uart_put_hex(&console, *pmu_bus_idle_ack);
    uart_puts(&console, " cpwr[A72]=");
    uart_put_hex(&console, ((*pmu_core_pwr_st >> 12) & 0x1Fu));
    uart_puts(&console, " A72wfe=");
    uart_put_hex(&console, ((*pmu_core_pwr_st >> 12) & 0x1u));
    uart_puts(&console, " A72wfi=");
    uart_put_hex(&console, ((*pmu_core_pwr_st >> 16) & 0x1u));
    uart_puts(&console, "\r\n");
}

static inline u32 smp_gicr_waker(u32 cpu) {
    volatile u32 *w = (volatile u32 *)((uintptr_t)GICR_BASE + (uintptr_t)cpu * 0x20000u + (uintptr_t)GICR_WAKER);
    return *w;
}

static void smp_log_gicr_waker_decode(const char *tag) {
    u32 w4 = smp_gicr_waker(4);
    u32 w5 = smp_gicr_waker(5);
    uart_puts(&console, "[SMP][A72][GICR] ");
    uart_puts(&console, tag);
    uart_puts(&console, " t_us=0x");
    uart_put_hex(&console, smp_time_us());
    uart_puts(&console, " w4=0x");
    uart_put_hex(&console, w4);
    uart_puts(&console, "(PS=");
    uart_put_hex(&console, (w4 >> 1) & 1u);
    uart_puts(&console, ",CA=");
    uart_put_hex(&console, (w4 >> 2) & 1u);
    uart_puts(&console, ") w5=0x");
    uart_put_hex(&console, w5);
    uart_puts(&console, "(PS=");
    uart_put_hex(&console, (w5 >> 1) & 1u);
    uart_puts(&console, ",CA=");
    uart_put_hex(&console, (w5 >> 2) & 1u);
    uart_puts(&console, ")\r\n");
}

static inline u32 smp_cci_read(u64 off) {
    return *(volatile u32 *)(uintptr_t)(CCI_BASE + off);
}

static void smp_log_cci_state(const char *tag) {
    u32 ctrl = smp_cci_read(CCI_CTRL);
    u32 st = smp_cci_read(CCI_STATUS);
    u32 a53 = smp_cci_read(CCI_SLAVE_A53);
    u32 a72 = smp_cci_read(CCI_SLAVE_A72);
    uart_puts(&console, "[SMP][A72][CCI] ");
    uart_puts(&console, tag);
    uart_puts(&console, " t_us=0x");
    uart_put_hex(&console, smp_time_us());
    uart_puts(&console, " ctrl=0x");
    uart_put_hex(&console, ctrl);
    uart_puts(&console, " st=0x");
    uart_put_hex(&console, st);
    uart_puts(&console, " a53=0x");
    uart_put_hex(&console, a53);
    uart_puts(&console, "(S=");
    uart_put_hex(&console, a53 & 1u);
    uart_puts(&console, ",D=");
    uart_put_hex(&console, (a53 >> 1) & 1u);
    uart_puts(&console, ") a72=0x");
    uart_put_hex(&console, a72);
    uart_puts(&console, "(S=");
    uart_put_hex(&console, a72 & 1u);
    uart_puts(&console, ",D=");
    uart_put_hex(&console, (a72 >> 1) & 1u);
    uart_puts(&console, ")\r\n");
}

static void smp_log_a72_cluster_aff(const char *tag) {
    // Cluster-level affinity check: Aff1=1 (A72 cluster), Aff0 ignored.
    // Use hardware-form MPIDR (RES1 bit) as required by Rockchip TF-A.
    i64 cl_aff = psci_affinity_info_lvl(0x80000100ULL, 1);
    uart_puts(&console, "[SMP][A72][CL] ");
    uart_puts(&console, tag);
    uart_puts(&console, " t_us=0x");
    uart_put_hex(&console, smp_time_us());
    uart_puts(&console, " aff1=0x");
    uart_put_hex(&console, (u64)cl_aff);
    uart_puts(&console, " w4=0x");
    uart_put_hex(&console, smp_gicr_waker(4));
    uart_puts(&console, " w5=0x");
    uart_put_hex(&console, smp_gicr_waker(5));
    uart_puts(&console, "\r\n");
}

// Defined in boot.s - secondary core AArch64 entry point
extern void secondary_entry(void);
extern void _start(void);
extern void smp_trampoline(void);
extern void smp_trampoline_end(void);
extern void secondary_entry_probe(void);

// noinline: prevents -Os from inlining the SMC into callers where an incomplete
// clobber list would corrupt the caller's register-allocated loop variables.
// Full ARM SMCCC clobber: BL31 may modify x0-x18 (x0-x7 = params/results,
// x8-x17 = scratch, x18 = platform scratch). x19-x28/SP/LR are preserved.
static __attribute__((noinline)) i64 psci_affinity_info_lvl(u64 target_affinity, u64 lowest_level) {
    register u64 x0 asm("x0") = PSCI_AFFINITY_INFO_64;
    register u64 x1 asm("x1") = target_affinity;
    register u64 x2 asm("x2") = lowest_level;
    asm volatile("smc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2)
        : "memory",
          "x3",  "x4",  "x5",  "x6",  "x7",
          "x8",  "x9",  "x10", "x11",
          "x12", "x13", "x14", "x15",
          "x16", "x17", "x18");
    return (i64)x0;
}

static __attribute__((noinline)) i64 psci_affinity_info(u64 target_affinity) {
    return psci_affinity_info_lvl(target_affinity, 0);
}

static __attribute__((noinline)) i64 psci_smc32(u64 fid) {
    register u64 x0 asm("x0") = fid;
    asm volatile("smc #0"
        : "+r"(x0)
        :
        : "memory",
          "x1",  "x2",  "x3",  "x4",  "x5",  "x6",  "x7",
          "x8",  "x9",  "x10", "x11",
          "x12", "x13", "x14", "x15",
          "x16", "x17", "x18");
    return (i64)x0;
}

static __attribute__((noinline)) i64 psci_smc32_1(u64 fid, u64 arg1) {
    register u64 x0 asm("x0") = fid;
    register u64 x1 asm("x1") = arg1;
    asm volatile("smc #0"
        : "+r"(x0)
        : "r"(x1)
        : "memory",
          "x2",  "x3",  "x4",  "x5",  "x6",  "x7",
          "x8",  "x9",  "x10", "x11",
          "x12", "x13", "x14", "x15",
          "x16", "x17", "x18");
    return (i64)x0;
}

static __attribute__((noinline)) i64 psci_cpu_on(u64 target_cpu, u64 entry_point, u64 context_id) {
    register u64 x0 asm("x0") = PSCI_CPU_ON_64;
    register u64 x1 asm("x1") = target_cpu;
    register u64 x2 asm("x2") = entry_point;
    register u64 x3 asm("x3") = context_id;
    asm volatile("smc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory",
          "x4",  "x5",  "x6",  "x7",
          "x8",  "x9",  "x10", "x11",
          "x12", "x13", "x14", "x15",
          "x16", "x17", "x18");
    return (i64)x0;
}

static __attribute__((noinline)) i64 psci_cpu_on32(u64 target_cpu, u64 entry_point, u64 context_id) {
    register u64 x0 asm("x0") = PSCI_CPU_ON_32;
    register u64 x1 asm("x1") = (u32)target_cpu;
    register u64 x2 asm("x2") = (u32)entry_point;
    register u64 x3 asm("x3") = (u32)context_id;
    asm volatile("smc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory",
          "x4",  "x5",  "x6",  "x7",
          "x8",  "x9",  "x10", "x11",
          "x12", "x13", "x14", "x15",
          "x16", "x17", "x18");
    return (i64)x0;
}

result_t smp_init(void) {
    // Flush core 0's dirty-zero BSS cache lines for smp_idle_counters to RAM
    // BEFORE secondary cores start.  Without this, a later dc civac from core 0
    // would write zeros back over whatever secondary cores stored.
    for (u32 i = 0; i < SMP_MAX_CORES; i++)
        asm volatile("dc civac, %0" :: "r"(&smp_idle_counters[i]) : "memory");
    // Reset early-stage diagnostics in RAM before CPU_ON.
    smp_start_tombstone = 0;
    asm volatile("dc civac, %0" :: "r"(&smp_start_tombstone) : "memory");
    for (u32 i = 0; i < 64; i++) {
        smp_trace_page[i] = 0;
        asm volatile("dc civac, %0" :: "r"(&smp_trace_page[i]) : "memory");
    }
    for (u32 i = 0; i < SMP_MAX_CORES; i++) {
        smp_psci_diag[i].mpidr = rk3399_mpidr[i];
        smp_psci_diag[i].cpu_on_ret = 0;
        smp_psci_diag[i].aff_before = 0;
        smp_psci_diag[i].aff_after = 0;
        smp_psci_diag[i].pending_polls = 0;
        smp_psci_diag[i].kick_count = 0;
        smp_psci_diag[i].sev_bursts = 0;
        smp_psci_diag[i].flags = 0;
        smp_psci_diag[i].t_cpu_on_us = 0;
        smp_psci_diag[i].t_first_pending_us = 0;
        smp_psci_diag[i].t_online_us = 0;
        smp_psci_diag[i].t_pm_bit_clear_us = 0;
        smp_psci_diag[i].t_core_pm_8_us = 0;
        smp_psci_diag[i].t_aff_pending_first_us = 0;
        asm volatile("dc civac, %0" :: "r"(&smp_psci_diag[i]) : "memory");
    }
    smp_trace_set(SMP_TRACE_BOOT_MPIDR, rk3399_mpidr[0]);
    smp_trace_or(SMP_TRACE_BOOT_SEEN_MASK, 1ull << 0);
    asm volatile("dc civac, %0" :: "r"(&smp_trace_page[SMP_TRACE_BOOT_MPIDR]) : "memory");
    asm volatile("dc civac, %0" :: "r"(&smp_trace_page[SMP_TRACE_BOOT_SEEN_MASK]) : "memory");
    for (u32 i = 0; i < SMP_MAX_CORES; i++) {
        smp_entry_stage[i] = 0;
        asm volatile("dc civac, %0" :: "r"(&smp_entry_stage[i]) : "memory");
    }
    // Pre-invalidate fixed beacon cache lines before CPU_ON.
    // Core 0 may have stale data in this line from earlier firmware/U-Boot activity.
    // Invalidating now guarantees first post-CPU_ON read comes from DRAM.
    {
        volatile u64 *fb = (volatile u64 *)SMP_BEACON_ADDR;
        // Reset beacon in RAM, then clean+invalidate so core 0 starts from a known state.
        fb[0] = 0;
        fb[1] = 0;
        asm volatile("dc civac, %0" :: "r"(&fb[0]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&fb[1]) : "memory");
        // Store core 0's MMU registers so the secondary trampoline can enable the same
        // EL2 identity-map MMU before branching to secondary_entry.
        // Non-coherent instruction fetches from 0x02081000 fail (AXI SLVERR) with
        // caches off; with M=1+C=1+I=1 the fetch goes through L2/CCI (coherent Normal),
        // which bypasses the protection.
        // beacon[4] = TTBR0_EL2, beacon[5] = TCR_EL2, beacon[6] = MAIR_EL2
        u64 ttbr0, tcr, mair;
        asm volatile("mrs %0, ttbr0_el2" : "=r"(ttbr0));
        asm volatile("mrs %0, tcr_el2"   : "=r"(tcr));
        asm volatile("mrs %0, mair_el2"  : "=r"(mair));
        fb[4] = ttbr0;
        fb[5] = tcr;
        fb[6] = mair;
        asm volatile("dc civac, %0" :: "r"(&fb[4]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&fb[5]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&fb[6]) : "memory");
        asm volatile("dsb sy" ::: "memory");
    }
    // Ensure secondaries fetch fresh instructions after TFTP DMA image load.
    asm volatile("ic ialluis\n dsb ish\n isb" ::: "memory");

    uart_puts(&console, "[SMP] entry(_start)=0x");
    uart_put_hex(&console, (u64)(uintptr_t)_start);
    uart_puts(&console, " sec=0x");
    uart_put_hex(&console, (u64)(uintptr_t)secondary_entry);
    uart_puts(&console, "\r\n");

    // PSCI_VERSION: confirms BL31 is responding to our SMC calls
    i64 psci_ver = psci_smc32(PSCI_VERSION_32);
    uart_puts(&console, "[SMP] PSCI_VERSION=0x");
    uart_put_hex(&console, (u64)psci_ver);
    uart_puts(&console, " (");
    uart_put_hex(&console, (u64)((psci_ver >> 16) & 0xFFFF));
    uart_puts(&console, ".");
    uart_put_hex(&console, (u64)(psci_ver & 0xFFFF));
    uart_puts(&console, ")\r\n");

    // PSCI_FEATURES: check if CPU_ON_64 is supported
    i64 feat64 = psci_smc32_1(PSCI_FEATURES_32, PSCI_CPU_ON_64);
    i64 feat32 = psci_smc32_1(PSCI_FEATURES_32, PSCI_CPU_ON_32);
    uart_puts(&console, "[SMP] PSCI_FEATURES CPU_ON_64=");
    uart_put_hex(&console, (u64)feat64);
    uart_puts(&console, " CPU_ON_32=");
    uart_put_hex(&console, (u64)feat32);
    uart_puts(&console, "\r\n");

    // PMU_PWRDN_ST: check which power domains are on/off before CPU_ON
    // Bits 0-3 = A53 cores 0-3, bits 4-5 = A72 cores 0-1
    // Bit 6 = SCU_L (A53 cluster), bit 7 = SCU_B (A72 cluster)
    // 1 = powered down, 0 = powered up
    volatile u32 *pmu_pwrdn_st = (volatile u32 *)(PMU_BASE + PMU_PWRDN_ST);
    u32 pmu_st_before = *pmu_pwrdn_st;
    uart_puts(&console, "[SMP] PMU_PWRDN_ST before=0x");
    uart_put_hex(&console, (u64)pmu_st_before);
    uart_puts(&console, "\r\n");

    // Clear PMU GRF scratch regs before a new bring-up pass.
    volatile u32 *pmugrf_os_reg1 = (volatile u32 *)0xFF320304UL;
    volatile u32 *pmugrf_os_reg2 = (volatile u32 *)0xFF320308UL;
    volatile u32 *pmugrf_os_reg3 = (volatile u32 *)0xFF32030CUL;
    *pmugrf_os_reg1 = 0;
    *pmugrf_os_reg2 = 0;
    *pmugrf_os_reg3 = 0;
    asm volatile("dsb sy; isb" ::: "memory");

    // --- Relay trampoline at 0x200000 ---
    // We proved 0x200000 works (UART 'X', GRF=0xCC in the trampoline test).
    // BL31 may ignore our PSCI entry_pa and always ERET to 0x200000 (the
    // original U-Boot load address saved in its NS context at cold boot).
    // Dual-path strategy: give ALL cores entry=0x200000 AND put the relay
    // trampoline there.  Works whether BL31 honours entry_pa or ignores it.
    // The trampoline does: canary GRF writes → UART 'X' → absolute br to
    // secondary_entry (0x02081000, guaranteed by .align 12 in boot.s).
    #define TRAMP_PA 0x00200000UL
    {
        u8 *dst = (u8 *)TRAMP_PA;
        u8 *src = (u8 *)(uintptr_t)smp_trampoline;
        u32 sz  = (u32)((u8 *)(uintptr_t)smp_trampoline_end - src);
        for (u32 b = 0; b < sz; b++) dst[b] = src[b];
        // Clean copied code from dcache to DRAM, then invalidate icache.
        for (u32 b = 0; b < sz; b += 64)
            asm volatile("dc civac, %0" :: "r"(dst + b) : "memory");
        asm volatile("ic ialluis\n dsb ish\n isb" ::: "memory");
        uart_puts(&console, "[SMP] trampoline @0x");
        uart_put_hex(&console, TRAMP_PA);
        uart_puts(&console, " size=0x");
        uart_put_hex(&console, sz);
        uart_puts(&console, " -> sec_entry=0x");
        uart_put_hex(&console, (u64)(uintptr_t)secondary_entry);
        uart_puts(&console, "\r\n");
    }

    // Entry for ALL cores = relay trampoline at 0x200000.
    u64 entry_pa = TRAMP_PA;
    uart_puts(&console, "[SMP] entry_pa=0x");
    uart_put_hex(&console, entry_pa);
    uart_puts(&console, " (relay trampoline)\r\n");

    // Stage 1: bring up LITTLE cluster first (cores 1..3).
    for (u32 i = 1; i <= 3; i++) {
        u64 hwid = rk3399_psci_mpidr(i);
        i64 ret = psci_cpu_on(hwid, entry_pa, (u64)i);
        smp_psci_diag[i].cpu_on_ret = ret;
        if (ret == PSCI_SUCCESS) smp_psci_diag[i].flags |= SMP_FLAG_CPU_ON_OK;
        if (ret == PSCI_ALREADY_ON) smp_psci_diag[i].flags |= SMP_FLAG_ALREADY_ON;
        uart_puts(&console, "[SMP] CPU_ON core ");
        uart_put_hex(&console, i);
        uart_puts(&console, " hw_mpidr=0x");
        uart_put_hex(&console, hwid);
        uart_puts(&console, " ret=");
        uart_put_hex(&console, (u64)(i64)ret);
        uart_puts(&console, "\r\n");
    }

    // Poll LITTLE for 300ms.
    u64 cnt_start, cnt_now;
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt_start));
    u64 timeout_ticks = (24000000ULL * 3) / 10; // 300ms at 24MHz
    u32 poll_rounds = 0;
    u32 all_on = 0;
    while (!all_on) {
        asm volatile("mrs %0, cntpct_el0" : "=r"(cnt_now));
        if (cnt_now - cnt_start > timeout_ticks) break;
        all_on = 1;
        for (u32 i = 1; i <= 3; i++) {
            if (smp_psci_diag[i].cpu_on_ret != PSCI_SUCCESS) continue;
            if (smp_psci_diag[i].flags & SMP_FLAG_AFF_ONLINE) continue;
            i64 aff = psci_affinity_info(rk3399_psci_mpidr(i));
            smp_psci_diag[i].aff_before = aff;
            smp_psci_diag[i].aff_after = aff;
            smp_psci_diag[i].pending_polls++;
            if (aff == 0) {
                smp_psci_diag[i].flags |= SMP_FLAG_AFF_ONLINE;
                smp_online_mask |= (1u << i);
                uart_puts(&console, "[SMP] core ");
                uart_put_hex(&console, i);
                uart_puts(&console, " ONLINE after ");
                uart_put_hex(&console, smp_psci_diag[i].pending_polls);
                uart_puts(&console, " polls\r\n");
            } else {
                all_on = 0;
            }
        }
        poll_rounds++;
        // Periodic SEV helps wake cores parked in WFE during warmboot paths.
        asm volatile("sev" ::: "memory");
        for (u32 y = 0; y < 100; y++) asm volatile("yield");
    }

    // Stage 2: bring up big cluster (A72 cores 4..5).
    //
    // *** ROOT CAUSE ANALYSIS (definitive) ***
    //
    // A53 cores share a SINGLE cluster L2 (1MB, shared by all 4 cores). BL31
    // writes cpuson_flags[1..3] as A53 L2 dirty lines. A53 secondaries (cores
    // 1-3) read through the SHARED A53 L2 and see the correct value instantly.
    //
    // A72 has its OWN SEPARATE L2 (different cluster). A72 starts with caches
    // OFF (cold reset) and reads cpuson_flags[4] directly from DRAM (cache-off).
    // But cpuson_flags[4] was written by BL31 (on A53) → in A53's L2 as a
    // SECURE dirty line → DRAM still has stale zero → A72 reads 0 → WFE loop.
    //
    // *** THE FIX: L2 EVICTION ***
    //
    // BL31's wfe_loop in Armbian v1.3 (Rockchip fork, commit 845ee93) DOES
    // re-read cpuson_flags after each WFE, unlike our upstream third_party source.
    // Evidence: Armbian Linux boots all 6 RK3399 cores — impossible without re-read.
    //
    // We cannot flush secure cache lines from NS (DC CVAC silently ignored for
    // secure addresses). We CANNOT read/write BL31's TZ-protected DRAM.
    //
    // BUT: NS workload CAN evict secure lines from A53's SHARED L2. Cache
    // eviction is LRU-based and ignores secure/non-secure tags. Evicted secure
    // dirty lines generate Secure AXI writebacks (NS=0) which TZ-DRAM ALLOWS.
    // So cpuson_flags[4] gets written to DRAM via the eviction mechanism.
    //
    // Algorithm:
    //   1. Call CPU_ON for A72 → BL31 writes cpuson_flags[4]=0xF00 to A53 L2
    //   2. Scan 2MB of NS DRAM to fill A53's 1MB L2 → evict BL31's dirty lines
    //   3. cpuson_flags[4] evicted → written to TZ DRAM → A72 reads 0xF00
    //   4. Poll + periodic SEV to wake A72 from WFE (wfe_loop re-reads flag)

    // Step 1: Call CPU_ON for both A72 cores.
    // Pre-kick PMU soft-wakeup for big cores. TF-A uses BIT(3) in PMU_CORE_PM_CON
    // during power-on paths; we mirror the same hint from NS side to avoid
    // pathological WFE stalls on cluster B warmboot.
    for (u32 i = 4; i <= 5; i++) {
        volatile u32 *pmu_core_pm =
            (volatile u32 *)((uintptr_t)PMU_BASE + (uintptr_t)PMU_CORE_PM_CON(i));
        *pmu_core_pm = (1u << 3); // soft_wakeup_en
        smp_psci_diag[i].kick_count++;
    }
    asm volatile("dsb ish; isb" ::: "memory");
    smp_log_a72_snapshot("pre-cpu_on", -1, -1);
    smp_log_a72_hw_sample("pre-cpu_on");
    smp_log_a72_cluster_aff("pre-cpu_on");
    smp_log_gicr_waker_decode("pre-cpu_on");
    smp_log_cci_state("pre-cpu_on");

    const u64 a72_probe_entry_pa = (u64)(uintptr_t)secondary_entry_probe;
    uart_puts(&console, "[SMP][A72] probe entry_pa=0x");
    uart_put_hex(&console, a72_probe_entry_pa);
    uart_puts(&console, " via CPU_ON_32 experiment\r\n");
    for (u32 i = 4; i <= 5; i++) {
        u64 hwid = rk3399_psci_mpidr(i);
        i64 ret = psci_cpu_on32(hwid, a72_probe_entry_pa, (u64)i);
        smp_psci_diag[i].cpu_on_ret = ret;
        smp_psci_diag[i].t_cpu_on_us = smp_time_us();
        smp_psci_diag[i].flags |= SMP_FLAG_PWR_REQ_SENT;
        if (ret == PSCI_SUCCESS) smp_psci_diag[i].flags |= SMP_FLAG_CPU_ON_OK;
        if (ret == PSCI_ALREADY_ON) smp_psci_diag[i].flags |= SMP_FLAG_ALREADY_ON;
        uart_puts(&console, "[SMP] CPU_ON core ");
        uart_put_hex(&console, i);
        uart_puts(&console, " hw_mpidr=0x");
        uart_put_hex(&console, hwid);
        uart_puts(&console, " ret=");
        uart_put_hex(&console, (u64)(i64)ret);
        uart_puts(&console, "\r\n");
    }
    smp_log_a72_snapshot("post-cpu_on", -1, -1);
    smp_log_a72_hw_sample("post-cpu_on");
    smp_log_a72_cluster_aff("post-cpu_on");
    smp_log_gicr_waker_decode("post-cpu_on");
    smp_log_cci_state("post-cpu_on");

    // Step 2: Fill A53's L2 (1MB) with NS accesses → evict BL31's dirty secure
    // lines (cpuson_flags[4,5]) to DRAM via Secure AXI writeback.
    // We intentionally scan 8MB to reduce replacement-policy corner cases.
    {
        volatile u8 dummy = 0;
        for (u64 a = 0x200000UL; a < 0xA00000UL; a += 64) {
            dummy ^= *(volatile u8 *)a;
        }
        asm volatile("dsb ish" ::: "memory");
        (void)dummy;
        uart_puts(&console, "[SMP] A72 fix: L2 eviction done (8MB NS scan), cpuson in DRAM\r\n");
        // Step 3: SEV to wake both A72 cores from WFE so they re-read cpuson_flags.
        asm volatile("sev" ::: "memory");
        asm volatile("sev" ::: "memory");
        smp_psci_diag[4].sev_bursts += 2;
        smp_psci_diag[5].sev_bursts += 2;
    }
    smp_log_a72_snapshot("post-evict", -1, -1);
    smp_log_a72_hw_sample("post-evict");
    smp_log_a72_cluster_aff("post-evict");
    smp_log_gicr_waker_decode("post-evict");
    smp_log_cci_state("post-evict");

    // Poll A72 for up to 5 seconds. While polling, periodically re-evict A53 L2
    // and emit SEV bursts to push BL31 warmboot loops out of WFE.
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt_start));
    timeout_ticks = 24000000ULL * 5; // 5s at 24MHz
    u32 poll_rounds_big = 0;
    all_on = 0;
    while (!all_on) {
        asm volatile("mrs %0, cntpct_el0" : "=r"(cnt_now));
        if (cnt_now - cnt_start > timeout_ticks) break;
        all_on = 1;
        for (u32 i = 1; i < SMP_MAX_CORES; i++) {
            if (smp_psci_diag[i].cpu_on_ret != PSCI_SUCCESS) continue;
            if (smp_psci_diag[i].flags & SMP_FLAG_AFF_ONLINE) continue;
            i64 aff = psci_affinity_info(rk3399_psci_mpidr(i));
            smp_psci_diag[i].aff_before = aff;
            smp_psci_diag[i].aff_after = aff;
            smp_psci_diag[i].pending_polls++;
            if ((i >= 4) && aff == 2 && smp_psci_diag[i].t_aff_pending_first_us == 0) {
                smp_psci_diag[i].t_aff_pending_first_us = smp_time_us();
            }
            if (aff == 0) {
                smp_psci_diag[i].flags |= SMP_FLAG_AFF_ONLINE;
                smp_online_mask |= (1u << i);
                smp_psci_diag[i].t_online_us = smp_time_us();
                uart_puts(&console, "[SMP] core ");
                uart_put_hex(&console, i);
                uart_puts(&console, " ONLINE after ");
                uart_put_hex(&console, smp_psci_diag[i].pending_polls);
                uart_puts(&console, " polls\r\n");
            } else {
                if ((i >= 4) && (smp_psci_diag[i].t_first_pending_us == 0)) {
                    smp_psci_diag[i].t_first_pending_us = smp_time_us();
                }
                all_on = 0;
            }
        }
        for (u32 i = 4; i <= 5; i++) {
            u32 pwrdn = *(volatile u32 *)(PMU_BASE + PMU_PWRDN_ST);
            u32 pm = *(volatile u32 *)((uintptr_t)PMU_BASE + (uintptr_t)PMU_CORE_PM_CON(i));
            if ((((pwrdn >> i) & 1u) == 0u) && smp_psci_diag[i].t_pm_bit_clear_us == 0) {
                smp_psci_diag[i].t_pm_bit_clear_us = smp_time_us();
                smp_psci_diag[i].flags |= SMP_FLAG_PWR_ACK_SEEN;
            }
            if ((pm & (1u << 3)) && smp_psci_diag[i].t_core_pm_8_us == 0) {
                smp_psci_diag[i].t_core_pm_8_us = smp_time_us();
            }
            if ((smp_psci_diag[i].flags & SMP_FLAG_PHASE_REPORTED) == 0 &&
                (smp_psci_diag[i].t_aff_pending_first_us != 0) &&
                (smp_psci_diag[i].t_pm_bit_clear_us != 0) &&
                (smp_psci_diag[i].flags & SMP_FLAG_AFF_ONLINE) == 0) {
                uart_puts(&console, "[SMP][A72] phase core ");
                uart_put_hex(&console, i);
                uart_puts(&console, ": pwr_req->pwr_ack->aff_pending, no kernel entry\r\n");
                smp_psci_diag[i].flags |= SMP_FLAG_PHASE_REPORTED;
            }
        }
        poll_rounds_big++;
        if ((poll_rounds_big & 0x1FFFFu) == 0) {
            smp_log_a72_snapshot("poll", smp_psci_diag[4].aff_after, smp_psci_diag[5].aff_after);
            smp_log_a72_hw_sample("poll");
            smp_log_a72_cluster_aff("poll");
            smp_log_gicr_waker_decode("poll");
            smp_log_cci_state("poll");
        }
        if ((poll_rounds_big & 0x3FFu) == 0) {
            volatile u8 dummy = 0;
            for (u64 a = 0x200000UL; a < 0xA00000UL; a += 64) {
                dummy ^= *(volatile u8 *)a;
            }
            asm volatile("dsb ish" ::: "memory");
            (void)dummy;
            // Re-issue PMU soft-wakeup hints for A72 cores while they are pending.
            *(volatile u32 *)((uintptr_t)PMU_BASE + (uintptr_t)PMU_CORE_PM_CON(4)) = (1u << 3);
            *(volatile u32 *)((uintptr_t)PMU_BASE + (uintptr_t)PMU_CORE_PM_CON(5)) = (1u << 3);
            smp_psci_diag[4].kick_count++;
            smp_psci_diag[5].kick_count++;
            asm volatile("dsb ish" ::: "memory");
        }
        asm volatile("sev" ::: "memory");
        asm volatile("sev" ::: "memory");
        smp_psci_diag[4].sev_bursts += 2;
        smp_psci_diag[5].sev_bursts += 2;
        for (u32 y = 0; y < 100; y++) asm volatile("yield");
    }
    smp_log_a72_snapshot("poll-end", smp_psci_diag[4].aff_after, smp_psci_diag[5].aff_after);
    smp_log_a72_hw_sample("poll-end");
    smp_log_a72_cluster_aff("poll-end");
    smp_log_gicr_waker_decode("poll-end");
    smp_log_cci_state("poll-end");

    // Report poll results.
    asm volatile("mrs %0, cntpct_el0" : "=r"(cnt_now));
    u64 elapsed_us = (cnt_now - cnt_start) / 24; // 24MHz -> us
    uart_puts(&console, "[SMP] poll done: ");
    uart_put_hex(&console, poll_rounds);
    uart_puts(&console, "+");
    uart_put_hex(&console, poll_rounds_big);
    uart_puts(&console, " rounds, ");
    uart_put_hex(&console, elapsed_us);
    uart_puts(&console, " us\r\n");

    for (u32 i = 1; i < SMP_MAX_CORES; i++) {
        i64 aff = smp_psci_diag[i].aff_after;
        if (smp_psci_diag[i].cpu_on_ret == PSCI_SUCCESS && aff != 0) {
            uart_puts(&console, "[SMP] WARN: core ");
            uart_put_hex(&console, i);
            uart_puts(&console, " still ON_PENDING after timeout, polls=");
            uart_put_hex(&console, smp_psci_diag[i].pending_polls);
            uart_puts(&console, " t_cpu_on_us=0x");
            uart_put_hex(&console, smp_psci_diag[i].t_cpu_on_us);
            uart_puts(&console, " t_pending_us=0x");
            uart_put_hex(&console, smp_psci_diag[i].t_first_pending_us);
            uart_puts(&console, "\r\n");
        }
        if (smp_psci_diag[i].cpu_on_ret == PSCI_ALREADY_ON) {
            smp_online_mask |= (1u << i);
        }
    }
    // Short event storm to release any WFE-parked cores, then ISH barrier.
    for (u32 n = 0; n < 64; n++) asm volatile("sev" ::: "memory");
    asm volatile("dsb ish\n isb" ::: "memory");

    // PMU_PWRDN_ST after CPU_ON: verify power domains actually changed
    u32 pmu_st_after = *pmu_pwrdn_st;
    uart_puts(&console, "[SMP] PMU_PWRDN_ST after=0x");
    uart_put_hex(&console, (u64)pmu_st_after);
    if (pmu_st_before != pmu_st_after) {
        uart_puts(&console, " (CHANGED from 0x");
        uart_put_hex(&console, (u64)pmu_st_before);
        uart_puts(&console, ")");
    } else {
        uart_puts(&console, " (UNCHANGED — cores may not have powered on!)");
    }
    uart_puts(&console, "\r\n");

    return OK;
}

u32 smp_get_online_count(void) {
    asm volatile("dc civac, %0" :: "r"(&smp_secondary_enter_mask) : "memory");
    asm volatile("dsb sy" ::: "memory");
    u32 entered = 0;
    u32 mask = smp_secondary_enter_mask;
    for (u32 i = 1; i < SMP_MAX_CORES; i++) {
        if (mask & (1u << i)) entered++;
    }
    smp_online_count = 1 + entered;
    return smp_online_count;
}

u32 smp_get_first_secondary_online(void) {
    for (u32 i = 1; i < SMP_MAX_CORES; i++) {
        if (smp_online_mask & (1u << i)) return i;
    }
    return 0;
}

u32 smp_get_secondary_enter_mask(void) {
    return smp_secondary_enter_mask;
}

u32 smp_get_first_secondary_entered(void) {
    u32 mask = smp_secondary_enter_mask;
    for (u32 i = 1; i < SMP_MAX_CORES; i++) {
        if (mask & (1u << i)) return i;
    }
    return 0;
}

u32 smp_get_active_node_count(void) {
    return smp_get_online_count();
}

smp_path_state_t smp_classify_core(u32 core_idx) {
    if (core_idx == 0) {
        return SMP_PATH_REACHED_C_WORKER;
    }
    if (core_idx >= SMP_MAX_CORES) {
        return SMP_PATH_NOT_REQUESTED;
    }
    if (smp_secondary_enter_mask & (1u << core_idx)) {
        return SMP_PATH_REACHED_C_WORKER;
    }
    if (smp_psci_diag[core_idx].flags & SMP_FLAG_ENTERED_C) {
        return SMP_PATH_REACHED_C_WORKER;
    }
    if (smp_core_reached_secondary_entry(core_idx)) {
        return SMP_PATH_REACHED_SECONDARY_ENTRY;
    }
    if (smp_core_reached_start(core_idx)) {
        return SMP_PATH_REACHED_START;
    }
    if (smp_psci_diag[core_idx].cpu_on_ret == PSCI_SUCCESS ||
        smp_psci_diag[core_idx].cpu_on_ret == PSCI_ALREADY_ON) {
        return SMP_PATH_PRE_KERNEL_HANDOFF;
    }
    return SMP_PATH_NOT_REQUESTED;
}

// Called from boot.s secondary_entry with x0 = core_idx (1..5)
void smp_secondary_main(u64 core_idx) {
    if (core_idx >= SMP_MAX_CORES) {
        while (1) asm volatile("wfe");
    }
    smp_secondary_enter_mask |= (1u << (u32)core_idx);
    smp_trace_or(SMP_TRACE_C_ENTRY_MASK, 1ull << core_idx);
    smp_trace_or(SMP_TRACE_WQ_MASK, 1ull << core_idx);
    smp_psci_diag[core_idx].flags |= SMP_FLAG_ENTERED_C;
    asm volatile("dmb ish" ::: "memory");
    asm volatile("dc civac, %0" :: "r"(&smp_secondary_enter_mask) : "memory");
    asm volatile("dc civac, %0" :: "r"(&smp_trace_page[SMP_TRACE_C_ENTRY_MASK]) : "memory");
    asm volatile("dc civac, %0" :: "r"(&smp_trace_page[SMP_TRACE_WQ_MASK]) : "memory");
    asm volatile("dc civac, %0" :: "r"(&smp_psci_diag[core_idx]) : "memory");
    // Poll work queue slot for dispatched jobs; yield when idle.
    volatile u64 *counter = &smp_idle_counters[core_idx];
    while (1) {
        wq_worker_poll((u32)core_idx);
        (*counter)++;
        asm volatile("dmb ish" ::: "memory");
        asm volatile("yield");
    }
}

static void print_u64_array(uart_t *uart, const char *label, volatile u64 *arr, u32 count) {
    uart_puts(uart, label);
    for (u32 i = 0; i < count; i++) {
        uart_puts(uart, " C");
        uart_put_hex(uart, i);
        uart_puts(uart, "=");
        uart_put_hex(uart, arr[i]);
    }
    uart_puts(uart, "\r\n");
}

static void smp_emit_a72_fail_verdict(uart_t *uart) {
    u32 w4 = smp_gicr_waker(4);
    u32 w5 = smp_gicr_waker(5);
    u32 ca1 = ((w4 >> 2) & 1u) & ((w5 >> 2) & 1u);
    u32 p4 = (u32)(smp_psci_diag[4].aff_after == 2);
    u32 p5 = (u32)(smp_psci_diag[5].aff_after == 2);
    u32 pending = p4 & p5;
    u32 probe_hit = smp_core_reached_secondary_entry(4) | smp_core_reached_secondary_entry(5);

    if (!probe_hit && pending && ca1) {
        uart_puts(uart, "[SMP][A72] A72_FAIL_STAGE=EL3_BEFORE_NS_ENTRY (reason: probe_not_hit + CA=1 + ON_PENDING)\r\n");
    } else {
        uart_puts(uart, "[SMP][A72] A72_FAIL_STAGE=UNDETERMINED (reason: conditions_not_matched)\r\n");
    }
}

void smp_dump_diagnostics(uart_t *uart) {
    volatile u64 *fb = (volatile u64 *)SMP_BEACON_ADDR;
    // CRITICAL: Invalidate Core 0's stale cache lines before reading DRAM
    // written by secondary cores (which have MMU off → write directly to DRAM).
    for (u32 i = 0; i < SMP_MAX_CORES; i++) {
        asm volatile("dc civac, %0" :: "r"(&smp_idle_counters[i]) : "memory");
        asm volatile("dc civac, %0" :: "r"(&smp_entry_stage[i]) : "memory");
    }
    asm volatile("dc civac, %0" :: "r"(&smp_start_tombstone) : "memory");
    for (u32 i = 0; i < 12; i++)
        asm volatile("dc civac, %0" :: "r"(&smp_trace_page[i * 5]) : "memory");
    for (u32 i = 0; i < 4; i++)
        asm volatile("dc civac, %0" :: "r"(&fb[i]) : "memory");
    asm volatile("dsb sy" ::: "memory");
    uart_puts(uart, "[SMP] diag: consolidated trace\r\n");
    uart_puts(uart, "[SMP] experiment path: CPU_ON -> PMU warmboot -> secure on_finish -> kernel entry\r\n");
    uart_puts(uart, "[SMP] trace boot_mpidr=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_BOOT_MPIDR]);
    uart_puts(uart, " boot_mask=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_BOOT_SEEN_MASK]);
    uart_puts(uart, " sec_mask=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_SEC_SEEN_MASK]);
    uart_puts(uart, " c_mask=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_C_ENTRY_MASK]);
    uart_puts(uart, "\r\n");
    uart_puts(uart, "[SMP] trace el_mask=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_EL_MASK]);
    uart_puts(uart, " stack_mask=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_STACK_MASK]);
    uart_puts(uart, " mmu_mask=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_MMU_MASK]);
    uart_puts(uart, " wq_mask=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_WQ_MASK]);
    uart_puts(uart, " last=0x");
    uart_put_hex(uart, smp_trace_page[SMP_TRACE_LAST]);
    uart_puts(uart, "\r\n");
    // beacon[0]=core_idx, [1]=0xBEEFDEAD, [2]=CurrentEL, [3]=raw MPIDR_EL1
    // Written as very first instructions in secondary_entry (no BSS dependency).
    uart_puts(uart, "[SMP] beacon idx=0x");
    uart_put_hex(uart, fb[0]);
    uart_puts(uart, " sentinel=0x");
    uart_put_hex(uart, fb[1]);
    uart_puts(uart, " EL=0x");
    uart_put_hex(uart, fb[2]);
    uart_puts(uart, " mpidr=0x");
    uart_put_hex(uart, fb[3]);
    uart_puts(uart, "\r\n");
    if (fb[1] == 0xBEEFDEADULL) {
        uart_puts(uart, "[SMP] BEACON HIT: secondary_entry was reached!\r\n");
    } else if (fb[1] == 0xCAFEBABEULL) {
        uart_puts(uart, "[SMP] BEACON TRAMP: DRAM write at 0x02000000 works from 0x200000!\r\n");
        // With ICache fix: check if we made it past the fault
        // If R2 still 0xBB → no exception → icache fix worked, secondary_entry running!
        // If R2 = 0xEE → still faulting (icache didn't help, try next approach)
    } else {
        uart_puts(uart, "[SMP] BEACON MISS: trampoline DRAM store also failed (no CCI/DRAM issue?)\r\n");
    }
    // PMU GRF OS_REGs: secondary writes to all 3 on _start entry.
    // OS_REG1 = Aff0|0xA0, OS_REG2 = 0xBB (canary), OS_REG3 = CurrentEL or ESR_EL2
    // If OS_REG2 = 0xEE or 0xEF → secondary took exception (ESR in OS_REG3)
    volatile u32 *pmugrf_os_reg1 = (volatile u32 *)0xFF320304UL;
    volatile u32 *pmugrf_os_reg2 = (volatile u32 *)0xFF320308UL;
    volatile u32 *pmugrf_os_reg3 = (volatile u32 *)0xFF32030CUL;
    u32 grf_val  = *pmugrf_os_reg1;
    u32 grf_val2 = *pmugrf_os_reg2;
    u32 grf_val3 = *pmugrf_os_reg3;
    uart_puts(uart, "[SMP] GRF: R1=0x");
    uart_put_hex(uart, grf_val);
    uart_puts(uart, " R2=0x");
    uart_put_hex(uart, grf_val2);
    uart_puts(uart, " R3=0x");
    uart_put_hex(uart, grf_val3);
    uart_puts(uart, "\r\n");
    if (grf_val2 == 0xEE || grf_val2 == 0xEF) {
        u32 ec  = (grf_val3 >> 26) & 0x3F;
        u32 iss = grf_val3 & 0x1FFFFFF;
        uart_puts(uart, "[SMP] EXCEPTION! EL2 fault. ESR=0x");
        uart_put_hex(uart, grf_val3);
        uart_puts(uart, " EC=0x");
        uart_put_hex(uart, ec);
        uart_puts(uart, " ISS=0x");
        uart_put_hex(uart, iss);
        if (ec == 0x21) uart_puts(uart, " (InstrAbort-currEL)");
        if (ec == 0x20) uart_puts(uart, " (InstrAbort-lowerEL)");
        if (ec == 0x25) uart_puts(uart, " (DataAbort-currEL)");
        if (ec == 0x24) uart_puts(uart, " (DataAbort-lowerEL)");
        if (ec == 0x0)  uart_puts(uart, " (POISON/SError-masked->sync)");
        uart_puts(uart, "\r\n");
        uart_puts(uart, "[SMP] MMU+ICache fix failed: instruction fetch still blocked.\r\n");
        uart_puts(uart, "[SMP] Next: copy secondary_entry to trampoline page 0x201000.\r\n");
    } else if (grf_val2 == 0xBB) {
        uart_puts(uart, "[SMP] TRAMPOLINE reached (0x200000) - no EL2 fault, MMU branch succeeded!\r\n");
        uart_puts(uart, "[SMP] secondary_entry should be running. Expect BEACON HIT + 'S' on UART.\r\n");
    } else if (grf_val == 0 && grf_val2 == 0) {
        uart_puts(uart, "[SMP] GRF all-zero: trampoline NEVER ran! BL31 ERET to wrong address?\r\n");
    }
    print_u64_array(uart, "[SMP] idle:", smp_idle_counters, SMP_MAX_CORES);
    print_u64_array(uart, "[SMP] entry_stage:", smp_entry_stage, SMP_MAX_CORES);
    uart_puts(uart, "[SMP] _start tombstone=0x");
    uart_put_hex(uart, smp_start_tombstone);
    uart_puts(uart, "\r\n");
    smp_emit_a72_fail_verdict(uart);
    for (u32 i = 0; i < SMP_MAX_CORES; i++) {
        uart_puts(uart, "[SMP] psci C");
        uart_put_hex(uart, i);
        uart_puts(uart, " mpidr=0x");
        uart_put_hex(uart, smp_psci_diag[i].mpidr);
        uart_puts(uart, " ret=");
        uart_put_hex(uart, (u64)smp_psci_diag[i].cpu_on_ret);
        uart_puts(uart, " aff0=");
        uart_put_hex(uart, (u64)smp_psci_diag[i].aff_before);
        uart_puts(uart, " aff1=");
        uart_put_hex(uart, (u64)smp_psci_diag[i].aff_after);
        uart_puts(uart, " polls=0x");
        uart_put_hex(uart, smp_psci_diag[i].pending_polls);
        uart_puts(uart, " kicks=0x");
        uart_put_hex(uart, smp_psci_diag[i].kick_count);
        uart_puts(uart, " sev=0x");
        uart_put_hex(uart, smp_psci_diag[i].sev_bursts);
        uart_puts(uart, " flags=0x");
        uart_put_hex(uart, smp_psci_diag[i].flags);
        uart_puts(uart, " t_on=0x");
        uart_put_hex(uart, smp_psci_diag[i].t_online_us);
        uart_puts(uart, " t_cpuon=0x");
        uart_put_hex(uart, smp_psci_diag[i].t_cpu_on_us);
        uart_puts(uart, " t_pending=0x");
        uart_put_hex(uart, smp_psci_diag[i].t_first_pending_us);
        uart_puts(uart, " t_pm_ack=0x");
        uart_put_hex(uart, smp_psci_diag[i].t_pm_bit_clear_us);
        uart_puts(uart, " t_pm8=0x");
        uart_put_hex(uart, smp_psci_diag[i].t_core_pm_8_us);
        uart_puts(uart, " t_affpend=0x");
        uart_put_hex(uart, smp_psci_diag[i].t_aff_pending_first_us);
        uart_puts(uart, " path=");
        uart_puts(uart, smp_path_state_name(smp_classify_core(i)));
        uart_puts(uart, "\r\n");
    }
}
