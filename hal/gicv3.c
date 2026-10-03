// H-Exo Omni-Core: GICv3 Implementation
// Interrupt controller management for RK3399

#include "gicv3.h"

// Spinlock for GICD register protection (RMW race prevention)
static volatile u32 gicd_lock = 0;

static inline void lock_gicd(void) {
    u32 loaded;
    u32 status;
    do {
        do {
            asm volatile("ldaxr %w0, [%1]"
                         : "=&r"(loaded)
                         : "r"(&gicd_lock)
                         : "memory");
            if (loaded != 0u) {
                asm volatile("yield");
            }
        } while (loaded != 0u);

        asm volatile("stxr %w0, %w2, [%1]"
                     : "=&r"(status)
                     : "r"(&gicd_lock), "r"(1u)
                     : "memory");
    } while (status != 0u);

    asm volatile("dmb ish" ::: "memory");
}

static inline void unlock_gicd(void) {
    asm volatile("stlr %w1, [%0]" :: "r"(&gicd_lock), "r"(0u) : "memory");
}

// Register access helpers - proper 64-bit address handling
static inline void gicd_write(u32 reg, u32 val) {
    *(volatile u32*)((uintptr_t)GICD_BASE + reg) = val;
    asm volatile("dsb sy" ::: "memory");
}

// Per-core WAKER handshake telemetry (captured in gicv3_force_wake_core):
// [0]=pre, [1]=after_ps1_write_readback, [2]=after_ps0_write_readback,
// [3]=final, [4]=retries_used, [5]=flags
// flags: bit0=sleep_phase_timeout, bit1=wake_phase_timeout,
//        bit2=ps1_write_ignored, bit3=ps0_write_ignored
volatile u64 __attribute__((aligned(64))) gicv3_waker_trace[6][6];

static inline u32 gicd_read(u32 reg) {
    u32 val = *(volatile u32*)((uintptr_t)GICD_BASE + reg);
    asm volatile("dsb sy" ::: "memory");
    return val;
}

// Wait until the Distributor has drained the writes it is tracking.
//
// Arm IHI 0069G s12.9.4, GICD_CTLR.RWP bit [31], verbatim:
//   "This field tracks writes to: GICD_CTLR[2:0], the Group Enables, for
//    transitions from 1 to 0 only. GICD_CTLR[7:4], the ARE bits, E1NWF bit and
//    DS bit. GICR_ICENABLER<n>."
//
// So RWP covers exactly the ARE bits and the group-enable clears - both of
// which gicv3_init() writes. The old code only ever polled GICR_CTLR.RWP, which
// is a different register in a different frame and does not cover any of this.
//
// dsb sy alone is not sufficient: it orders the store, it does not tell you the
// Distributor has consumed it. Bounded so a wedged Distributor cannot hang boot.
static u32 gicd_wait_for_rwp(void) {
    u32 spins = 2000000u;
    while ((gicd_read(GICD_CTLR) & (1u << 31)) && spins--) {
        asm volatile("yield");
    }
    return spins;
}

static inline void gicr_write(u32 reg, u32 val) {
    *(volatile u32*)((uintptr_t)GICR_BASE + reg) = val;
    asm volatile("dsb sy" ::: "memory");
}

static inline u32 gicr_read(u32 reg) {
    u32 val = *(volatile u32*)((uintptr_t)GICR_BASE + reg);
    asm volatile("dsb sy" ::: "memory");
    return val;
}

u32 gicv3_read_waker(u32 core) {
    if (core >= 6) return 0xFFFFFFFFu;
    volatile u32 *waker = (volatile u32*)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000 + GICR_WAKER);
    return *waker;
}

u32 gicv3_force_wake_core(u32 core, u32 retries) {
    if (core >= 6) return 0xFFFFFFFFu;
    if (retries == 0) retries = 1;

    volatile u32 *waker = (volatile u32*)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000 + GICR_WAKER);

    // GICv3 wake handshake:
    //   1) Ensure ProcessorSleep=1 and wait until ChildrenAsleep=1
    //   2) Clear ProcessorSleep and wait until ChildrenAsleep=0
    // Some RK3399 A72 bring-up paths can get stuck in CA=1/PS=0 after CPU_ON;
    // toggling PS forces a clean redistributor state transition.
    u32 flags = 0;
    u32 retries_used = 0;
    u32 pre = *waker;
    u32 after_ps1 = pre;
    u32 after_ps0 = pre;

    for (u32 retry = 0; retry < retries; retry++) {
        retries_used = retry + 1;
        u32 v = *waker;

        // Phase A: request sleep (PS=1) and wait until CA becomes 1.
        // IMPORTANT: always drive PS=1 first (even if CA already reads 1),
        // then drive PS=0. On some GIC-500 paths, wake only takes effect on
        // a real PS transition edge (1 -> 0), not on repeated PS=0 writes.
        *waker = (v | (1u << 1));
        asm volatile("dsb sy" ::: "memory");
        after_ps1 = *waker;
        if ((after_ps1 & (1u << 1)) == 0u) {
            flags |= (1u << 2);
        }
        int t_sleep = 200000;
        while (((*waker & (1u << 2)) == 0u) && t_sleep--) {
            asm volatile("yield");
        }
        if (((*waker & (1u << 2)) == 0u)) {
            flags |= (1u << 0);
        }

        // Phase B: request wake (PS=0) and wait until CA clears.
        v = *waker;
        *waker = (v & ~(1u << 1));
        asm volatile("dsb sy" ::: "memory");
        after_ps0 = *waker;
        if ((after_ps0 & (1u << 1)) != 0u) {
            flags |= (1u << 3);
        }

        int t = 400000;
        while (((*waker & (1u << 2)) != 0u) && t--) {
            asm volatile("yield");
        }
        if (((*waker & (1u << 2)) != 0u)) {
            flags |= (1u << 1);
        }
        if (((*waker & (1u << 2)) == 0u)) {
            break;
        }
    }

    asm volatile("dsb sy\n isb" ::: "memory");
    u32 final = *waker;
    gicv3_waker_trace[core][0] = pre;
    gicv3_waker_trace[core][1] = after_ps1;
    gicv3_waker_trace[core][2] = after_ps0;
    gicv3_waker_trace[core][3] = final;
    gicv3_waker_trace[core][4] = retries_used;
    gicv3_waker_trace[core][5] = flags;
    asm volatile("dc civac, %0" :: "r"(&gicv3_waker_trace[core][0]) : "memory");
    asm volatile("dsb sy" ::: "memory");
    return final;
}

// Wake all redistributors (clear ProcessorSleep) BEFORE PSCI CPU_ON.
// BL31 v1.3 parks secondary cores in WFI and sets ProcessorSleep=1 for them.
// If ProcessorSleep=1 when BL31 sends the wake SGI during CPU_ON, the SGI is
// silently dropped by the redistributor and the core stays in WFI forever.
// Call this ONCE before smp_init() so the redistributors are ready.
void gicv3_prewake_redistributors(void) {
    // GICR stride = 0x20000 (LPI frame 64KB + SGI frame 64KB per core)
    // RK3399 has 6 CPU interfaces total; prewake covers the A53 frames only.
    /* Cores 4/5 deliberately excluded - this prewake WAS the A72 interrupt bug.
     * Writing PS=0 on a frame whose PE is powered off starts a redistributor
     * wake transition that cannot complete; the frame is then stuck at PS=0/CA=1,
     * further PS writes are ignored from NS and EL3 alike, and BL31's
     * mark_core_awake() no-ops because PS is already 0. CA=1 means the RD never
     * presents anything to the CPU interface, which was the entire "A72 takes no
     * interrupts" failure. Left untouched, the frames stay at PS=1/CA=1 and
     * BL31's own handshake during CPU_ON completes normally (measured 2026-10-03:
     * frames come up 0x0, SGI_TEST c4 irq_cnt 0->1). smp_a72_wake_guard() in
     * core/smp.c verifies the outcome, read-only. See A72_INVESTIGATION_LOG.md
     * Resolution. */
    for (u32 cpu = 0; cpu < 4; cpu++) {
        (void)gicv3_force_wake_core(cpu, 4);
    }
    asm volatile("dsb sy\n isb" ::: "memory");
}

result_t gicv3_init(void) {
    // 1. Stop interrupt delivery, and do it the way the specification defines.
    //
    //    Arm IHI 0069G s2.3.3, verbatim:
    //      "Changing GICD_CTLR.ARE_NS from 1 to 0 is UNPREDICTABLE."
    //      "Changing GICD_CTLR.ARE_NS from 0 to 1 is unpredictable except when
    //       GICD_CTLR.EnableGrp1NS == 0."
    //      "The effect of clearing GICD_CTLR.EnableGrp0, GICD_CTLR.EnableGrp1S,
    //       or GICD_CTLR.EnableGrp1NS, as appropriate, must be visible when
    //       changing GICD_CTLR.ARE_S or GICD_CTLR.ARE_NS from 0 to 1.
    //       Software can poll GICD_CTLR.RWP to check that writes that clear
    //       GICD_CTLR.EnableGrp0, GICD_CTLR.EnableGrp1S, or GICD_CTLR.EnableGrp1NS
    //       bits have completed."
    //
    //    So: clear the Group Enables and WAIT for that to be visible, never touch
    //    ARE on the way down (1->0 is itself UNPREDICTABLE), then raise ARE, then
    //    re-enable the groups. The previous code wrote 0 and moved straight on,
    //    so the group-enable clear had not necessarily landed when ARE was raised.
    gicd_write(GICD_CTLR, 0);
    gicd_wait_for_rwp();

    // 2. Wake up Redistributor
    u32 waker = gicr_read(GICR_WAKER);
    waker &= ~(1 << 1); // Clear ProcessorSleep
    gicr_write(GICR_WAKER, waker);
    
    // Wait for ChildrenAsleep to be cleared
    int timeout = 1000000;
    while ((gicr_read(GICR_WAKER) & (1 << 2)) && timeout--) {
        asm volatile("yield");
    }
    
    if (timeout <= 0) return ERR_TIMEOUT;

    // 3. Configure Group 1 (Normal World) interrupts.
    // Raise ARE only with the Group Enables observably clear - see s2.3.3 quoted
    // above. Belt and braces: the Distributor was already zeroed and drained
    // above, but assert the precondition explicitly rather than rely on it.
    u32 ctrl = gicd_read(GICD_CTLR);
    ctrl &= ~((1u << 0) | (1u << 1) | (1u << 2)); // EnableGrp0/1NS/1S clear
    gicd_write(GICD_CTLR, ctrl);
    gicd_wait_for_rwp();

    /*
     * Disable Security (GICD_CTLR.DS, bit 6) - MANDATORY on RK3399.
     *
     * Linux carries a quirk entry for this exact SoC:
     *   gic_enable_quirk_rk3399() -> FLAGS_WORKAROUND_INSECURE -> "rockchip,rk3399"
     * and gic_prio_init() then does:
     *   val = readl_relaxed(dist_base + GICD_CTLR);
     *   val |= GICD_CTLR_DS;
     *   writel_relaxed(val, dist_base + GICD_CTLR);
     *   pr_warn("Broken GIC integration, security disabled\n");
     *
     * The rationale (Marc Zyngier, "[PATCH] irqchip/gic-v3: Work around insecure
     * GIC integrations"): RK3399 exposes the GIC's *secure* programming interface
     * to non-secure, so priorities programmed through the NS view behave wrongly
     * and the machine can die. DS puts the GIC in a single-security-state mode
     * where every interrupt is Group 1 NS and the register views match.
     *
     * Measured on this board: Armbian prints exactly
     *   "GICv3: Broken GIC integration, security disabled"
     *   "GICv3: GICD_CTLR.DS=1, SCR_EL3.FIQ=1"
     * and all six cores then take interrupts. H-Exo never set DS: its GICD_CTLR
     * read 0x33/0x35, bit 6 always clear, and cores 4/5 never took an interrupt.
     */
    ctrl = gicd_read(GICD_CTLR);
    ctrl |= (1u << 6);          // DS - see the RK3399 insecure-integration quirk
    gicd_write(GICD_CTLR, ctrl);
    gicd_wait_for_rwp();

    ctrl = gicd_read(GICD_CTLR);
    ctrl |= (1 << 4) | (1 << 5); // ARE_S and ARE_NS
    gicd_write(GICD_CTLR, ctrl);
    gicd_wait_for_rwp();

    // Enable Group 0 (BL31 EL3 SGIs) AND Group 1 NS (our IRQs)
    // EnableGrp0 MUST be restored: a pending BL31 Group-0 wake-SGI is blocked
    // until this bit is set again.  Leaving it at 0 silently drops the SGI.
    ctrl |= (1 << 0) | (1 << 1); // EnableGrp0 + EnableGrp1NS
    gicd_write(GICD_CTLR, ctrl);
    gicd_wait_for_rwp();

    // 4. Configure CPU Interface (System Registers)
    u32 sre;
    asm volatile("mrs %0, ICC_SRE_EL2" : "=r"(sre));
    sre |= (1 << 0) | (1 << 1) | (1 << 2); // SRE, DFB, DFE
    asm volatile("msr ICC_SRE_EL2, %0" :: "r"(sre));
    asm volatile("isb");

    // Set priority mask to allow all interrupts
    u32 pmr = 0xFF;
    asm volatile("msr ICC_PMR_EL1, %0" :: "r"((u64)pmr));
    
    // Enable Group 0 + Group 1 interrupts at CPU interface.
    // Some firmware/security routes SGI via Group0/FIQ path.
    u32 igrp = 1;
    asm volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"((u64)igrp));
    asm volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"((u64)igrp));
    asm volatile("isb");

    return OK;
}

// Per-core diagnostic snapshot of GIC state captured AFTER init completes.
// Layout per core (8 u64 slots): 0=init_called_count, 1=ICC_SRE_EL2,
// 2=ICC_PMR_EL1, 3=ICC_IGRPEN1_EL1, 4=GICR_WAKER, 5=GICR_ISENABLER0,
// 6=GICR_IGROUPR0, 7=GICR_TYPER_aff_hi.
volatile u64 __attribute__((aligned(64))) gicv3_core_diag[6][8];
// Extended diag: IGRPMODR0 (bits per intid) + IPRIORITYR0..3 (4 SGIs)
// + ICC_BPR1_EL1 + ICC_CTLR_EL1.
volatile u64 __attribute__((aligned(64))) gicv3_core_diag2[6][17];

// Which numbered step of gicv3_init_cpu_iface() this PE reached. Written BEFORE
// each step, so a PE that never completes leaves behind the number of the step it
// stopped on. 99 means it returned normally.
//
// Added 2026-10-02 because gicv3_init_cpu_iface() is known not to return when
// called on a Cortex-A72 after smp_init(), while the same call does return on the
// same core during smp_secondary_main(). Without this there is no way to tell
// which statement wedges it.
volatile u64 __attribute__((aligned(64))) gicv3_cpu_iface_stage[6];

/*
 * H-Exo: two-moment GIC snapshot, taken by the PE itself.
 *
 * Motivation. Everything reachable from software compares identical between the
 * working A53s and the dead A72s, the A72 completes gicv3_init_cpu_iface()
 * (cpuiface_stage reaches 99), and every clock on the A72 interrupt path reads
 * enabled. Yet an SGI or a locally injected PPI latches and is never forwarded.
 * Register-by-register elimination has run out of candidates, so compare the A72
 * against itself at two moments instead: A, immediately after its CPU interface
 * init returns, and B, on its first pass through the idle loop.
 *
 * Both moments are captured by the PE itself, because six of the slots are system
 * registers (ICC_*) that only that PE can read. Taking both from core 0 would
 * measure core 0 twice.
 *
 * Slot map is in gicv3.c near this comment; the names are printed by main_neuro.c.
 */
#define GICV3_SNAP_SLOTS 30
volatile u64 __attribute__((aligned(64))) g_gic_snap[6][2][GICV3_SNAP_SLOTS];
volatile u8  g_gic_snap_b_done[6];

/*
 * Memory-mapped half of the snapshot only, and safe to call from core 0.
 *
 * Deliberately NOT called from the secondary idle loops. The A72 cores are the
 * fragile ones - an MMIO or system-register read that faults there wedges them
 * before the runtime console exists - and there is no remote recovery from that.
 * The moment-A snapshot stays inside gicv3_init_cpu_iface(), which is already
 * proven to run to completion on the A72.
 */
void gicv3_take_snapshot_mmio(u32 core, u32 moment) {
    if (core >= 6u || moment >= 2u) return;
    volatile u64 *snap = g_gic_snap[core][moment];
    uintptr_t rd  = GICR_BASE + (uintptr_t)core * 0x20000u;
    uintptr_t sgi = rd + GICR_SGI_OFFSET;
    snap[0]  = *(volatile u32 *)(rd + 0x00);
    snap[1]  = *(volatile u32 *)(rd + 0x04);
    snap[2]  = *(volatile u32 *)(rd + 0x08);
    snap[3]  = *(volatile u32 *)(rd + 0x0C);
    snap[4]  = *(volatile u32 *)(rd + 0x10);
    snap[5]  = *(volatile u32 *)(rd + 0x14);
    snap[6]  = *(volatile u32 *)(sgi + 0x080);
    snap[7]  = *(volatile u32 *)(sgi + 0x100);
    snap[8]  = *(volatile u32 *)(sgi + 0x180);
    snap[9]  = *(volatile u32 *)(sgi + 0x200);
    snap[10] = *(volatile u32 *)(sgi + 0x280);
    snap[11] = *(volatile u32 *)(sgi + 0x400);
    snap[12] = *(volatile u32 *)(sgi + 0xC00);
    snap[13] = *(volatile u32 *)(sgi + 0xD00);
    snap[14] = *(volatile u32 *)(sgi + 0xE00);
    snap[15] = gicd_read(0x000);
    snap[16] = gicd_read(0x008);
    snap[17] = gicd_read(0x080);
    for (u32 k = 18u; k < GICV3_SNAP_SLOTS; k++) snap[k] = 0;  /* not readable off-PE */
    snap[29] = 0xA72A72A72A72A72ULL;
    for (u32 off = 0; off < (GICV3_SNAP_SLOTS * 8u); off += 64u) {
        asm volatile("dc civac, %0" :: "r"((volatile u64 *)((uintptr_t)snap + off)) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
    if (moment == 1u) g_gic_snap_b_done[core] = 1;
}

void gicv3_take_snapshot(u32 core, u32 moment) {
    if (core >= 6u || moment >= 2u) return;
    volatile u64 *snap = g_gic_snap[core][moment];
    uintptr_t rd  = GICR_BASE + (uintptr_t)core * 0x20000u;
    uintptr_t sgi = rd + GICR_SGI_OFFSET;
    u64 v;

    /* RD frame */
    snap[0]  = *(volatile u32 *)(rd + 0x00);   /* GICR_CTLR   */
    snap[1]  = *(volatile u32 *)(rd + 0x04);   /* GICR_IIDR   */
    snap[2]  = *(volatile u32 *)(rd + 0x08);   /* GICR_TYPER  */
    snap[3]  = *(volatile u32 *)(rd + 0x0C);
    snap[4]  = *(volatile u32 *)(rd + 0x10);
    snap[5]  = *(volatile u32 *)(rd + 0x14);   /* GICR_WAKER */
    /* SGI frame */
    snap[6]  = *(volatile u32 *)(sgi + 0x080); /* IGROUPR0    */
    snap[7]  = *(volatile u32 *)(sgi + 0x100); /* ISENABLER0  */
    snap[8]  = *(volatile u32 *)(sgi + 0x180); /* ICENABLER0  */
    snap[9]  = *(volatile u32 *)(sgi + 0x200); /* ISPENDR0   */
    snap[10] = *(volatile u32 *)(sgi + 0x280); /* ICPENDR0   */
    snap[11] = *(volatile u32 *)(sgi + 0x400); /* IPRIORITYR0 */
    snap[12] = *(volatile u32 *)(sgi + 0xC00); /* ICFGR0     */
    snap[13] = *(volatile u32 *)(sgi + 0xD00); /* IGRPMODR0  */
    snap[14] = *(volatile u32 *)(sgi + 0xE00); /* NSACR      */
    /* Distributor, shared */
    snap[15] = gicd_read(0x000);              /* GICD_CTLR   */
    snap[16] = gicd_read(GICD_TYPER);        /* was 0x008, which is GICD_IIDR */
    snap[17] = gicd_read(0x080);              /* GICD_IGROUPR */
    /* CPU interface system registers - only valid on the capturing PE */
    asm volatile("mrs %0, ICC_SRE_EL1"     : "=r"(v)); snap[18] = v;
    asm volatile("mrs %0, ICC_PMR_EL1"     : "=r"(v)); snap[19] = v;
    asm volatile("mrs %0, ICC_IGRPEN0_EL1" : "=r"(v)); snap[20] = v;
    asm volatile("mrs %0, ICC_IGRPEN1_EL1" : "=r"(v)); snap[21] = v;
    asm volatile("mrs %0, ICC_HPPIR1_EL1"  : "=r"(v)); snap[22] = v;
    asm volatile("mrs %0, ICC_RPR_EL1"    : "=r"(v)); snap[23] = v;
    asm volatile("mrs %0, ICC_AP1R0_EL1"  : "=r"(v)); snap[24] = v;
    asm volatile("mrs %0, daif"           : "=r"(v)); snap[25] = v;
    asm volatile("mrs %0, sctlr_el2"      : "=r"(v)); snap[26] = v;
    asm volatile("mrs %0, hcr_el2"        : "=r"(v)); snap[27] = v;
    asm volatile("mrs %0, cntfrq_el0"     : "=r"(v)); snap[28] = v;
    snap[29] = 0xA72A72A72A72A72ULL;          /* end marker */

    /* Clean every cache line: the A72 cluster does not snoop A53 stores, and
     * core 0 has to be able to read what this PE just published. */
    for (u32 off = 0; off < (GICV3_SNAP_SLOTS * 8u); off += 64u) {
        asm volatile("dc civac, %0" :: "r"((volatile u64 *)((uintptr_t)snap + off)) : "memory");
    }
    asm volatile("dsb sy" ::: "memory");
    if (moment == 1u) g_gic_snap_b_done[core] = 1;
}

static inline void gicv3_stage(u32 core, u32 v) {
    if (core >= 6u) return;
    gicv3_cpu_iface_stage[core] = v;
    // Clean to DRAM: the A72 cluster does not snoop A53 stores, so a dirty line
    // here would never be visible to the reporting core.
    asm volatile("dc cvac, %0" :: "r"(&gicv3_cpu_iface_stage[core]) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

// Phase 2: Per-core CPU interface init (must be called on each secondary core)
// Each core has its own ICC_SRE/PMR/IGRPEN1 system registers
// ===== EXPERIMENT-3: discriminate the remaining A72 interrupt-delivery
// hypotheses using only this PE and its own redistributor. Nothing here
// changes functional state beyond a self-test PPI that is cleared again.
//
// Observed 2026-10-02: on cores 4/5 (A72) GICR_WAKER reads 0x4
// (ProcessorSleep=0, ChildrenAsleep=1) and never changes, SGI sent by core 0
// latches in ISPENDR0 but is never acknowledged, while every software-
// visible CPU-interface register is byte-identical to the A53 cores that do
// take interrupts. Two probes separate the remaining causes:
//
// PROBE A - local-PE wake cycle. Runs ON the affected core, so the write is
//   from the PE that owns the redistributor (not a cross-PE write). It drives
//   ProcessorSleep 1 then 0 and records whether ChildrenAsleep ever moves.
//   If PS reads back as written but CA never clears, the wake state machine
//   itself is wedged (TF-A side). If PS reads back unchanged, this SoC ignores
//   WAKER writes entirely on the big cluster.
//
// PROBE B - private PPI delivery. PPI 29 is per-PE and already enabled in
//   ISENABLER0, so it needs no IROUTER routing. Setting ISPENDR0 bit 29 on
//   this core's own redistributor and then reading ICC_HPPIR1_EL1 on this
//   PE answers the decisive question:
//     HPPIR returns a real INTID  => the redistributor->CPU-interface forward
//                                 path is alive; only the SGI path is broken.
//     HPPIR returns 0x3FF (spurious) => the forward path itself is dead for
//                                 this PE, which is what CA=1 would imply.
static void gicv3_probe_local_wake(u32 core) {
    volatile u32 *waker = (volatile u32 *)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000u + GICR_WAKER);
    u32 pre  = *waker;
    u32 ps1  = pre;
    u32 ca1_polls = 0;
    u32 ps0  = pre;
    u32 ca0_polls = 0;
    u32 fin  = pre;

    // Drive ProcessorSleep=1 (bit 1) and see whether it takes effect.
    *waker = pre | (1u << 1);
    asm volatile("dsb sy" ::: "memory");
    ps1 = *waker;
    for (u32 i = 0; i < 20000u; i++) {
        if ((*waker & (1u << 2)) != 0u) { ca1_polls = i + 1u; break; }
        asm volatile("yield");
    }

    // Drive ProcessorSleep=0 and watch ChildrenAsleep (bit 2).
    *waker = *waker & ~(1u << 1);
    asm volatile("dsb sy" ::: "memory");
    ps0 = *waker;
    for (u32 i = 0; i < 20000u; i++) {
        if ((*waker & (1u << 2)) == 0u) { ca0_polls = i + 1u; break; }
        asm volatile("yield");
    }
    fin = *waker;
    asm volatile("dsb sy" ::: "memory");
    asm volatile("isb" ::: "memory");

    gicv3_core_diag2[core][7]  = pre;
    gicv3_core_diag2[core][8]  = ps1;
    gicv3_core_diag2[core][9]  = ca1_polls;
    gicv3_core_diag2[core][10] = ps0;
    gicv3_core_diag2[core][11] = ca0_polls;
    gicv3_core_diag2[core][12] = fin;
}

/*
 * H-Exo: staged boundary probe - WHERE does an interrupt die?
 *
 * Everything up to the pending bit is verified correct for the A72: it reaches
 * GICR_ISPENDR0 and stays there. The chain from there is
 *
 *     GICR_ISPENDR0  ->  ICC_HPPIR1_EL1  ->  ICC_IAR1_EL1  ->  exception vector
 *
 * and every stage is recorded separately, so the result is binary rather than
 * another theory: whichever stage first fails to advance names the boundary.
 *
 * Run ON the A72 itself, so the ICC_* system registers read are the ones that
 * belong to this PE. PPI 29 is per-PE and pre-enabled.
 *
 * Slots: 0 hppir before | 1 ispendr after write | 2 hppir after  <-- boundary 1
 *        3 iar value   | 4 handler magic       | 5 handler count
 *        6 last intid  | 7 ENDMARK
 */
/*
 * H-Exo: two-phase boundary probe - WHERE does the interrupt die on this PE?
 *
 * Split in two because of ordering, not convenience. Phase A runs inside
 * gicv3_init_cpu_iface(), which is BEFORE "msr daifclr, #3" in
 * smp_secondary_main(); at that point PSTATE.I is still 1, so no exception can
 * possibly be taken and the "did the vector run" stage would be unanswerable.
 * Phase B therefore runs immediately after daifclr.
 *
 * Slot 0 is a step marker cleaned at every stage, so a probe that wedges still
 * reports where it stopped. Slot 1 holds the pre-daif phase so a B-side stop is
 * distinguishable from "phase A never ran".
 *
 *  0 step  1 GICR_CTLR inherited  2 ICENABLER0 inherited  3 GICR_WAKER
 *  4 HPPIR1 idle   5 GICR_CTLR after enable  6 ICENABLER0 after SGI-15 enable
 *  7 HPPIR1 pre-inject          8 ISPENDR0 readback
 *  9 HPPIR1 post-inject  <-- BOUNDARY 1     10 IAR1 / DEAD  <-- BOUNDARY 2
 * 11 handler entry delta    <-- BOUNDARY 3   12 handler magic
 * 13 ICC_CTLR_EL1  14 ICC_IGRPEN1 before  15 ICC_IGRPEN1 after  16 DAIF_EL1
 * 17 ICC_PMR_EL1 at p2 (after the SGI fired)  18 ICC_RPR_EL1
 * 19 ICC_PMR_EL1 at p1 (before this probe's IGRPEN1 write)
 * 20 GICR_IGROUPR0 readback   21 GICR_IGRPMODR0 readback
 * 22 GICR_ISENABLER0 readback  23 GICR_IPRIORITYR0 readback
 *  5 GICR_ISPENDR0 BEFORE the SGI (residue test)   6 SCTLR_EL1   7 ICC_SRE_EL2
 *
 * Slots 17/19 answered a question that measurement closed: p1 = p2 = 0xF8 on all six
 * cores, so the PMR = 0 seen on core 4 in one run was a one-off, not a property of
 * the hardware, and the "interrupts are masked" explanation for HPPIR = 0x3FF was
 * wrong. With PMR = 0xF8, RPR = 0xFF and IGRPEN1 = 1 on every core, the CPU interface
 * is fully configured and idle; SGI priority is 0xA0 = 160 < 248, so the delivery
 * condition is satisfied. The break is therefore INSIDE the redistributor, between
 * GICR_ISPENDR0 (which does latch) and ICC_HPPIR1_EL1 (which never shows it).
 *
 * Slots 20..23 read back the redistributor's own view of the interrupt: which group
 * it is in, which security modifier, whether it is enabled, and at what priority.
 * Every one of those is written by gicv3_init_cpu_iface and none was ever read back,
 * so a write that silently failed on the A72 frames would have been invisible.
 */
volatile u64 __attribute__((aligned(64))) g_icr_boundary[6][56];

/*
 * CPU-interface snapshot, taken twice: once in phase A strictly before the probe
 * SGI is sent, once in phase B after it has latched. This is the one measurement
 * that can distinguish "the redistributor holds a pending interrupt that the CPU
 * interface refuses to present" from "something is already running at a priority
 * that outranks it".
 *
 * ICC_RPR_EL1 is the running priority. If it is not 0xFF, an interrupt is already
 * active on this PE, and per IHI 0069 section 4.2.2 a new interrupt is only
 * signalled when its priority is higher (numerically lower) than RPR. Our SGI is
 * programmed at 0xA0 = 160, so an RPR of, say, 0x80 would block it forever while
 * leaving GICR_ISPENDR0 set - which is exactly the measured signature.
 *
 * The APnRn registers hold one priority per implemented priority bit and show
 * which priorities are currently active. Non-zero AP entries on the A72 would be
 * direct evidence of leftover active state.
 *
 * HPPIR0 is read alongside HPPIR1 as a control. Our SGIs are Group 1, so a normal
 * run shows HPPIR0 = 1023 and HPPIR1 = the INTID. If the A72 showed the INTID in
 * HPPIR0 instead, the interrupt was being routed as Group 0.
 *
 * Every encoding below is taken from Linux arch/arm64/include/asm/sysreg.h:
 *   ICC_PMR_EL1       (3,0, 4, 6,0)  ICC_RPR_EL1     (3,0,12,11,3)
 *   ICC_BPR0_EL1      (3,0,12, 8,3)  ICC_BPR1_EL1    (3,0,12,12,3)
 *   ICC_HPPIR0_EL1    (3,0,12, 8,2)  ICC_HPPIR1_EL1  (3,0,12,12,2)
 *   ICC_IAR0_EL1      (3,0,12, 8,0)  ICC_IAR1_EL1    (3,0,12,12,0)
 *   ICC_AP0R0_EL1     (3,0,12, 8,4)   ICC_AP1R0_EL1     (3,0,12, 9,0)
 *   ICC_IGRPEN0_EL1   (3,0,12,12,6)   ICC_IGRPEN1_EL1   (3,0,12,12,7)
 * sys_reg(op0,op1,CRn,CRm,op2) maps to S3_<op1>_C<CRn>_C<CRm>_<op2>, and for the
 * APnR family the varying field is op2, NOT CRm.
 *
 * AP0R1/AP0R2/AP0R3/AP1R1 are declared in sysreg.h but NOT implemented on this
 * GIC: reading them traps. Only the 12 registers below are read, so the snapshot
 * has 12 slots, not 16.
 */
#define HExO_SNAP_PRE   24   /* 24..35 */
#define HExO_SNAP_POST  36   /* 36..47 */
#define HExO_SNAP_N        12

static inline void gicv3_ciface_snapshot(volatile u64 *d) {
    u64 v;
    asm volatile("mrs %0, S3_0_C4_C6_0"   : "=r"(v)); d[0]  = v;  /* PMR       */
    asm volatile("mrs %0, S3_0_C12_C11_3" : "=r"(v)); d[1]  = v;  /* RPR       */
    asm volatile("mrs %0, S3_0_C12_C8_3"  : "=r"(v)); d[2]  = v;  /* BPR0      */
    asm volatile("mrs %0, S3_0_C12_C12_3" : "=r"(v)); d[3]  = v;  /* BPR1      */
    asm volatile("mrs %0, S3_0_C12_C8_2"  : "=r"(v)); d[4]  = v;  /* HPPIR0    */
    asm volatile("mrs %0, S3_0_C12_C12_2" : "=r"(v)); d[5]  = v;  /* HPPIR1    */
    asm volatile("mrs %0, S3_0_C12_C8_0"  : "=r"(v)); d[6]  = v;  /* IAR0      */
    asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v)); d[7]  = v;  /* IAR1      */
    asm volatile("mrs %0, S3_0_C12_C12_6" : "=r"(v)); d[8]  = v;  /* IGRPEN0   */
    asm volatile("mrs %0, S3_0_C12_C12_7" : "=r"(v)); d[9]  = v;  /* IGRPEN1   */
    /* AP0R0 and AP1R0 only, and deliberately not the rest.
     *
     * Linux sysreg.h declares AP0R0..R3 and AP1R0..R3, but this GIC does not
     * implement AP0R1, AP0R2, AP0R3 or AP1R1. Reading them raises a synchronous
     * exception from a lower EL:
     *
     *   [EL2] ELR -> gicv3_probe_local_irq_path   ESR 0x20000000 (EC=0)   FAR 0
     *
     * Confirmed against the assembler rather than the header: S3_0_C12_C8_5
     * assembles to d538c8a1 (icc_ap0r1_el1), while the faulting instruction was
     * d538c8a0 - one encoding off, because the register name encodes
     * CRn=12 CRm=8 with op2=4+n, and the extra bit belongs in op2, not CRm.
     * So the specification lists registers this part does not have, and only
     * executing them distinguishes "declared" from "implemented".
     *
     * AP0R0 and AP1R0 are the only two that are real, and they are what would
     * show leftover active priority state. */
    asm volatile("mrs %0, S3_0_C12_C8_4"  : "=r"(v)); d[10] = v;  /* AP0R0     */
    asm volatile("mrs %0, S3_0_C12_C9_0"  : "=r"(v)); d[11] = v;  /* AP1R0     */
}

/* Core 0 sets this after every target reports ARMED, then sends the SGI. */
volatile u64 g_icr_go[6];

/* g_irq_entry_count snapshotted on each target while its IRQs are still masked,
 * i.e. before any probe SGI can possibly arrive. Whatever shows up as a later
 * delta was therefore caused by the probe, and a delta on a core the SGI was
 * NOT addressed to means the distributor mis-routed it instead of dropping it. */
volatile u64 g_icr_entry_pre[6];

static inline void gicv3_bmark(volatile u64 *b, u64 v) {
    b[0] = v;
    asm volatile("dc civac, %0" :: "r"(&b[0]) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

static inline void gicv3_bclean(volatile u64 *b, u32 lo, u32 hi) {
    for (u32 k = lo; k <= hi; k++)
        asm volatile("dc civac, %0" :: "r"(&b[k]) : "memory");
    asm volatile("dsb sy" ::: "memory");
}

/*
 * Why SGI 0 and ICC_SGI1R_EL1, and not a local redistributor write.
 *
 * The previous version injected into this PE's own GICR_ISPENDR0 and then read
 * ICC_HPPIR1_EL1. Measured: the pending bit latches (readback = 1) but HPPIR
 * stays 0x3FF - on cores 1, 2 and 3 as well as on 4 and 5. Cores 1-3 are known to
 * receive SGIs (irq_cnt 0x0 -> 0x1). So that instrument is invalid, and its
 * "FAIL" said nothing about the A72. That is why the control run exists.
 *
 * The one path measured to deliver is the system-register interface:
 * ICC_SGI1R_EL1, which gicv3_sgi_send() already uses and which reached core 1.
 * Two encodings matter:
 *   - ICC_SGI1R_EL1.SGI_ID is bits [25:24] and gicv3_sgi_send() does
 *     "(sgi_id & 0xF) << 24", so only SGI 0..3 survive that encoding.
 *   - SGI 0 is SGI_STAGE_INPUT, whose handler case is empty. It bumps the
 *     entry counter at the top of handle_irq_exception and EOIs. Nothing else.
 */
#define HExO_PROBE_SGI_ID    0

/* ---- PHASE B: BEFORE daifclr, so PSTATE.I=1 and nothing can be consumed.
 * Reads HPPIR1 the instant core 0 has fired the SGI. ---- */
void gicv3_probe_local_irq_path(u32 core) {
    if (core < 1u || core > 5u) return;   /* core 0 is the sender; leave it alone */
    uintptr_t rd = (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000u;
    volatile u64 *b = g_icr_boundary[core];
    extern volatile u64 g_irq_entry_count[6];
    u64 v = 0;

    b[1] = *(volatile u32 *)(rd + GICR_CTLR);          /* GICR_CTLR as inherited */
    b[2] = *(volatile u32 *)(rd + GICR_ISENABLER0);    /* reads as ICENABLER0 */
    b[3] = *(volatile u32 *)(rd + GICR_WAKER);

    /* The redistributor's own view of SGI 0. All four are written by
     * gicv3_init_cpu_iface and none was ever read back, so a write that did not
     * stick on the A72 frames would look identical to a working one. ISENABLER0
     * reads as ICENABLER0, so 0 in the nibbles means "enabled". */
    b[20] = *(volatile u32 *)(rd + GICR_IGROUPR0);
    b[21] = *(volatile u32 *)(rd + GICR_IGRPMODR0);
    b[22] = *(volatile u32 *)(rd + GICR_ISENABLER0);
    b[23] = *(volatile u32 *)(rd + GICR_IPRIORITYR0);

    /* RESIDUE TEST. b[8] is GICR_ISPENDR0 read in phase B, AFTER core 0 has fired
     * the probe SGI. It reads 1 on cores 4 and 5 and 0 on cores 1-3, and that has
     * been read as "the SGI reached the A72 redistributor and latched". The
     * alternative has never been excluded: the bit is ALREADY set from earlier
     * boot activity (the SMPEN/GICR_WAKER work touches these frames), and our SGI
     * never arrived at all. The two are indistinguishable from b[8] alone.
     *
     * This read happens at the top of phase A, strictly before b[0] is set to 5
     * (ARMED), which is the signal core 0 waits for before sending anything. So
     *   b[5] == 0  -> the SGI really did arrive; latching is proven.
     *   b[5] == 1  -> the bit is residue; our SGI never reached the redistributor,
     *                 and "it latches but is not presented" is the wrong model.
     * That inverts the conclusion, which is why it is worth a slot. */
    b[5] = *(volatile u32 *)(rd + GICR_ISPENDR0);

    /* PRE snapshot. Taken before b[0] is set to ARMED, so before core 0 can have
     * sent anything: this is the CPU interface's baseline state. */
    gicv3_ciface_snapshot(&b[HExO_SNAP_PRE]);
    gicv3_bclean(b, HExO_SNAP_PRE, HExO_SNAP_PRE + HExO_SNAP_N - 1);

    /* Read-only frame registers nobody has ever sampled:
     *   GICR_STATUSR (+0x10) - the redistributor's own log of invalid accesses.
     *     If the ignored PS writes were rejected as writes-to-read-only, the
     *     WROD/RWOD bits here say so, distinguishing "mid-transition lockout"
     *     from "the bit is RO on this implementation".
     *   GICR_PWRR (+0x24) - redistributor power state (GIC-600 family; U-Boot
     *     gates its USE on CONFIG_GICV3_SUPPORT_GIC600, the read itself is
     *     harmless and a nonzero value here would be news). */
    b[48] = *(volatile u32 *)(rd + 0x10);
    b[49] = *(volatile u32 *)(rd + 0x24);
    gicv3_bclean(b, 48, 49);

    /* L2ACTLR_EL1 CONTROL. The per-PE probe at core/smp.c:1016 is gated to
     * core >= 4 because S3_1_C15_C0_0 is the A72-specific encoding and would
     * fault in EL3 on an A53. That leaves the A53 cluster with no control at all,
     * so a zero L2ACTLR reading on cores 4 and 5 cannot be attributed: it might
     * mean the clock really is off, or that the probe never ran. Core 0 is alive
     * and the SMC is serviced in EL3 regardless of the calling PE, so the same
     * request issued from core 0 measures the same A72 L2 block and gives the
     * control the per-PE probe could not.
     *
     * Stored in b[22], which was free, and printed on its own line so it is never
     * confused with the per-PE probe. It deliberately does NOT reuse b[5]: that
     * slot holds the pre-injection GICR_ISPENDR0 residue test a few lines above,
     * and overwriting it silently destroyed the residue reading.
     *
     * This reads only. It does not attempt to enable any clock: L2ACTLR_EL1 is
     * EL3-only and this runs at EL2, so a write here would fault rather than help.
     * Its only job is to make the existing reading interpretable. */
    {
        register u64 x0 asm("x0") = 0xC200009BUL;   /* RK_SIP_L2ACTLR_GET_64 */
        register u64 x1 asm("x1") = 0;
        register u64 x2 asm("x2") = 0;
        register u64 x3 asm("x3") = 0;
        asm volatile("smc #0"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x3)
                     : "memory",
                       "x4", "x5", "x6", "x7", "x8", "x9",
                       "x10", "x11", "x12", "x13", "x14", "x15",
                       "x16", "x17");
        b[22] = x0;                                 /* raw L2ACTLR_EL1   */
    }

    asm volatile("mrs %0, sctlr_el1"      : "=r"(v));  b[6] = v;   /* SCTLR_EL1 */
    asm volatile("mrs %0, S3_4_C12_C9_5"      : "=r"(v));  b[7] = v;   /* ICC_SRE_EL2 */
    asm volatile("mrs %0, S3_0_C12_C12_2" : "=r"(v));  b[4] = v;   /* HPPIR idle */
    asm volatile("mrs %0, S3_0_C12_C12_4" : "=r"(b[13]));          /* ICC_CTLR_EL1 */
    asm volatile("mrs %0, S3_0_C12_C12_7" : "=r"(b[14]));          /* ICC_IGRPEN1_EL1 */
    asm volatile("mrs %0, daif"           : "=r"(b[16]));
    gicv3_bclean(b, 1, 4);
    gicv3_bclean(b, 13, 14);
    gicv3_bclean(b, 16, 16);
    gicv3_bclean(b, 20, 23);
    gicv3_bclean(b, 5, 7);

    /* p1: PMR BEFORE the IGRPEN1 write below. The point of this probe is to find
     * out who zeroes ICC_PMR_EL1 on core 4, and the only difference between the
     * two measurement points is the write at L598. gicv3_core_diag[core][2],
     * sampled at stage 9 of gicv3_init_cpu_iface, already reads 0xF8 on all six
     * cores, so the register is NOT left at 0 by init. If p1 reads 0xF8 and the
     * later read in phase B reads 0, this probe caused it. If p1 already reads
     * 0, something between stage 9 and here did it and this probe is innocent. */
    asm volatile("mrs %0, S3_0_C4_C6_0" : "=r"(v));      /* ICC_PMR_EL1 */
    b[19] = v;
    gicv3_bclean(b, 19, 19);

    /* ICC_IGRPEN1_EL1 was 0 on core 5 and 1 on cores 1-3 in an earlier run. Bit 0
     * is EnableGrp1S, bit 1 is EnableGrp1NS. Align with the working cores and read
     * back. Note this write is the suspected cause of the PMR change above, which
     * is precisely what p1/p2 is there to test. */
    asm volatile("msr S3_0_C12_C12_7, %0" :: "r"(1ULL));
    asm volatile("isb" ::: "memory");
    asm volatile("mrs %0, S3_0_C12_C12_7" : "=r"(v));
    b[15] = v;                                          /* ICC_IGRPEN1_EL1 after */
    gicv3_bclean(b, 15, 15);

    /* Clean zero point for the routing question: IRQs are still masked here, so
     * no probe SGI can have arrived yet on this core. */
    g_icr_entry_pre[core] = g_irq_entry_count[core];
    asm volatile("dc civac, %0" :: "r"(&g_icr_entry_pre[core]) : "memory");
    asm volatile("dsb sy" ::: "memory");

    gicv3_bmark(b, 5ULL);                                /* ARMED */

    /* Wait for core 0 to arm us and fire. Bounded: on timeout the step marker
     * says so instead of spinning forever into an unrecoverable early hang. */
    /* The PSCI CPU_OFF self-power-cycle hook that lived here was removed the
     * same day it was added: rkbin BL31 v1.36 returns NOT_SUPPORTED (-1) for
     * CPU_OFF (measured), and it turned out to be unnecessary - the A72 frames
     * are healthy once H-Exo stops writing their GICR_WAKER before CPU_ON. */
    u32 t = 0;
    for (; t < 100000000u; t++) {
        asm volatile("dc ivac, %0" :: "r"(&g_icr_go[core]) : "memory");
        asm volatile("dsb sy" ::: "memory");
        if (g_icr_go[core] != 0ULL) break;
        asm volatile("yield");
    }
    if (t >= 100000000u) { gicv3_bmark(b, 8ULL); return; }   /* go timeout */

    /* Core 0 writes the flag first and the SGI after, so a plain dsb here does
     * not order the other core's system-register write. Bounded settle delay. */
    for (u32 w = 0; w < 200000u; w++) asm volatile("yield");

    asm volatile("mrs %0, S3_0_C12_C12_2" : "=r"(v));
    b[9] = v;                                             /* BOUNDARY 1 */

    /* ICC_IAR1_EL1 is read UNCONDITIONALLY, which is the point of this revision.
     * The previous version gated the read on HPPIR != 1023, so when the interface
     * reported spurious it never asked the only question that separates the two
     * remaining hypotheses:
     *
     *   IAR1 == 1023  -> the CPU interface is not offering an INTID at all, so the
     *                     break is inside the interface / priority-presentation path.
     *   IAR1 == SGI   -> the redistributor did its job and the break is further
     *                     out: exception routing, VBAR, vector, handler.
     *
     * Reading it with nothing pending is harmless - it returns 1023. Only the EOI
     * stays conditional, because EOIR with a spurious value means nothing.
     * Encodings verified against Linux arch/arm64/include/asm/sysreg.h:
     *   ICC_IAR1_EL1  = sys_reg(3,0,12,12,0) -> S3_0_C12_C12_0
     *   ICC_EOIR1_EL1 = sys_reg(3,0,12,12,1) -> S3_0_C12_C12_1
     *
     * ICC_RPR_EL1 is deliberately NOT read: it is a GICv2 register, and guessing
     * an encoding is what produced two wrong conclusions earlier in this project.
     * ICC_PMR_EL1 = sys_reg(3,0,4,6,0) is used instead, verified the same way.
     */
    asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(v));       /* ICC_IAR1_EL1 */
    b[10] = v;
    if ((v & 0x3FFu) != 0x3FFu) {
        asm volatile("msr S3_0_C12_C12_1, %0" :: "r"(v));   /* ICC_EOIR1_EL1 */
    }
    asm volatile("mrs %0, S3_0_C4_C6_0" : "=r"(v));         /* ICC_PMR_EL1 = p2 */
    b[17] = v;
    /* ICC_RPR_EL1, the running priority. An earlier note in this file claimed
     * this register does not exist in GICv3. That was wrong: it is defined both
     * by U-Boot (ICC_RPR_EL1 = S3_0_C12_C11_3, gic.h) and by Linux
     * (SYS_ICC_RPR_EL1 = sys_reg(3,0,12,11,3)). It is worth reading here: RPR
     * returns 0xFF when nothing is active and 0x00 when a priority-0 interrupt is
     * running, so it distinguishes "the interface is idle" from "the interface has
     * something and is not offering it". */
    asm volatile("mrs %0, S3_0_C12_C11_3" : "=r"(v));      /* ICC_RPR_EL1 */
    b[18] = v;

    /* POST snapshot: the same CPU interface after the SGI has latched in
     * GICR_ISPENDR0. Comparing it against HExO_SNAP_PRE is what separates
     * "pending and starved by a running priority" from "pending and simply not
     * presented". Read-only: IAR1 appears in both snapshots but reading it with
     * nothing acceptable pending just returns 1023 and changes no state. */
    gicv3_ciface_snapshot(&b[HExO_SNAP_POST]);
    gicv3_bclean(b, HExO_SNAP_POST, HExO_SNAP_POST + HExO_SNAP_N - 1);
    b[8] = *(volatile u32 *)(rd + GICR_ISPENDR0);         /* did it latch locally too */
    gicv3_bclean(b, 8, 10);
    gicv3_bclean(b, 17, 18);
    gicv3_bmark(b, 6ULL);                                 /* HPPIR captured */
}

/* ---- PHASE C: immediately AFTER daifclr, IRQs unmasked. Answers BOUNDARY 3. ---- */
void gicv3_probe_local_irq_path_b(u32 core) {
    if (core < 1u || core > 5u) return;
    volatile u64 *b = g_icr_boundary[core];
    extern volatile u64 g_irq_entry_count[6];
    extern volatile u64 g_irq_entry_magic[6];
    u64 entry0;

    if (b[0] != 6ULL) { gicv3_bmark(b, 10ULL); return; }  /* phase B never got there */
    entry0 = g_irq_entry_count[core];
    for (u32 w = 0; w < 2000000u; w++) {
        asm volatile("dsb sy" ::: "memory");
        if (g_irq_entry_count[core] != entry0) break;
        asm volatile("yield");
    }
    b[11] = g_irq_entry_count[core] - entry0;              /* BOUNDARY 3 */
    b[12] = g_irq_entry_magic[core];
    gicv3_bclean(b, 11, 12);
    gicv3_bmark(b, 4ULL);                                 /* COMPLETE */
}

/* Core 0 side of the boundary probe. Declared in gicv3.h so the print site in
 * main_neuro.c can call it without duplicating the affinity encoding. */
void gicv3_boundary_probe_send(void) {
    for (u32 core = 1; core < 6u; core++) {
        u64 aff = (core >= 4u) ? (0x100ULL | (u64)(core - 4u)) : (u64)(core & 3u);
        g_icr_go[core] = 1ULL;
        asm volatile("dc civac, %0" :: "r"(&g_icr_go[core]) : "memory");
        asm volatile("dsb sy" ::: "memory");
        gicv3_sgi_send(HExO_PROBE_SGI_ID, aff);
    }
}

void gicv3_init_cpu_iface(void) {
    // 1. Wake this core's redistributor (ProcessorSleep clear) - A53 ONLY.
    //    Measured: on the A72 frames every ProcessorSleep write is ignored, from
    //    NS (ps1_ignored in gicv3_force_wake_core) and from EL3 (SiP wake_try
    //    flags=0x2 timeout). The frames arrive from the bootloader already at
    //    PS=0/CA=1 - a wake transition started while the PE was off and never
    //    completed - and GIC-500 ignores PS writes while a transition is in
    //    progress. Writing more cannot restart it, and could re-stick a frame
    //    that the PSCI power-cycle test just freed. A72 path is READ-ONLY.
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    // RK3399 redistributor layout: 6 frames at GICR_BASE + N*0x20000.
    //   N=0..3 -> A53 cluster (Aff1=0, Aff0=0..3)
    //   N=4..5 -> A72 cluster (Aff1=1, Aff0=0..1)
    // Using (mpidr & 0xFF) alone => for core 4 (mpidr=0x100) -> N=0 collides
    // with core 0! That meant cores 4/5 enabled SGI on the WRONG redistributor
    // and never received SGIs themselves => Pipe-it hidden/output stages
    // wedged forever. Fix: linearise full Aff1:Aff0 to redistributor index.
    u32 aff0 = (u32)(mpidr & 0xFF);
    u32 aff1 = (u32)((mpidr >> 8) & 0xFF);
    u32 core = (aff1 ? (aff0 + 4) : aff0);
    if (core >= 6) return;
    gicv3_stage(core, 1u);
    u32 waker_after;
    if (core < 4u) {
        waker_after = gicv3_force_wake_core(core, 16);
    } else {
        /* READ-ONLY on A72 - see the step-1 comment. Record the frame state so
         * A72_PROBE_A still reports something meaningful; flags 0x80 marks
         * "skipped by design", distinct from any real handshake outcome. */
        waker_after = gicv3_read_waker(core);
        gicv3_waker_trace[core][0] = waker_after;
        gicv3_waker_trace[core][3] = waker_after;
        gicv3_waker_trace[core][5] = 0x80u;
        asm volatile("dc civac, %0" :: "r"(&gicv3_waker_trace[core][0]) : "memory");
        asm volatile("dsb sy" ::: "memory");
    }

    gicv3_stage(core, 2u);
    // 2. Enable system register access (ICC_SRE_EL2)
    u32 sre;
    asm volatile("mrs %0, ICC_SRE_EL2" : "=r"(sre));
    sre |= (1 << 0) | (1 << 1) | (1 << 2);
    asm volatile("msr ICC_SRE_EL2, %0" :: "r"(sre));
    asm volatile("isb");
    
    gicv3_stage(core, 3u);
    // 3. priority mask + EOImode
    // 3. Set priority mask
    u64 pmr = 0xFF;
    asm volatile("msr ICC_PMR_EL1, %0" :: "r"(pmr));
    
    gicv3_stage(core, 4u);
    // 4. Enable Group 0 + Group 1 interrupts at CPU interface.
    u64 igrp = 1;
    asm volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"(igrp));
    asm volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(igrp));
    asm volatile("isb");
    
    gicv3_stage(core, 5u);
    // 5. CRITICAL: Set per-core SGI priority. GICR_IPRIORITYR0..3 (one byte per
    //    SGI 0-15). PMR is 0xFF and the rule is `prio < PMR`, so a reset value
    //    of 0xFF would silently block every SGI on this core. Use 0xA0 (lower
    //    priority numerically => higher priority semantically than 0xFF).
    //    NOTE: GICD_IPRIORITYR is IGNORED for SGI/PPI in GICv3 with ARE=1 —
    //    only the GICR copy is honoured.
    uintptr_t rd_sgi = (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000;
    for (u32 i = 0; i < 16; i += 4) {
        volatile u32 *p = (volatile u32*)(rd_sgi + GICR_IPRIORITYR0 + i);
        *p = 0xA0A0A0A0u;
    }
    
    gicv3_stage(core, 6u);
    // 6. Configure SGI as Group 1 NS (must be set BEFORE enable).
    //    Per GICv3 spec, group is encoded by TWO bits per intid:
    //      IGROUPR  = 0, IGRPMODR = 0  -> Group 0 (Secure)
    //      IGROUPR  = 1, IGRPMODR = 0  -> Group 1 NS
    //      IGROUPR  = 0, IGRPMODR = 1  -> Group 1 Secure
    //    On RK3399 reset, IGRPMODR0 may default to 1 -> SGIs land in Group 1
    //    Secure, invisible to ICC_IAR1_EL1/HPPIR1 from NS EL2 (returns 0x3FF).
    //    Telemetry caught this: ispendr0=0x2 but hppir1=0x3FF.
    volatile u32 *igroupr  = (volatile u32*)(rd_sgi + GICR_IGROUPR0);
    volatile u32 *igrpmodr = (volatile u32*)(rd_sgi + GICR_IGRPMODR0);
    *igroupr  = 0xFFFFFFFFu;   // SGIs and PPIs into Group 1
    *igrpmodr = 0u;            // Clear Secure modifier -> Group 1 NS
    asm volatile("dsb sy" ::: "memory");
    
    gicv3_stage(core, 7u);
    // 7. Enable SGI 0-15 in this core's redistributor
    volatile u32 *isenabler = (volatile u32*)(rd_sgi + GICR_ISENABLER0);
    *isenabler = 0xFFFF;  // Enable SGI 0-15
    asm volatile("dsb sy" ::: "memory");
    
    gicv3_stage(core, 8u);
    // 8. Wait for redistributor RWP (Register Write Pending) to clear so the
    //    enable/group/priority writes are committed before we hit WFI.
    volatile u32 *gicr_ctlr = (volatile u32*)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000 + GICR_CTLR);
    int rwp_t = 1000000;
    while ((*gicr_ctlr & (1u << 3)) && rwp_t--) asm volatile("yield");
    
    asm volatile("dsb sy\n isb" ::: "memory");
    
    gicv3_stage(core, 9u);
    // 9. Diagnostic snapshot — record GIC state visible from THIS core so we
    //    can prove init actually executed and registers stuck.
    u64 v_sre, v_pmr, v_igrpen;
    asm volatile("mrs %0, ICC_SRE_EL2"     : "=r"(v_sre));
    asm volatile("mrs %0, ICC_PMR_EL1"     : "=r"(v_pmr));
    asm volatile("mrs %0, ICC_IGRPEN1_EL1" : "=r"(v_igrpen));
    gicv3_core_diag[core][0] += 1;            // init_called_count
    gicv3_core_diag[core][1]  = v_sre;
    gicv3_core_diag[core][2]  = v_pmr;
    gicv3_core_diag[core][3]  = v_igrpen;
    gicv3_core_diag[core][4]  = (u64)waker_after;
    gicv3_core_diag[core][5]  = (u64)*isenabler;
    gicv3_core_diag[core][6]  = (u64)*igroupr;
    // Slot 7 = GICR_TYPER affinity (bits [63:32]) — proves we're talking to
    // the redistributor that maps to THIS PE's MPIDR. Mismatch = wrong addr.
    volatile u64 *typer = (volatile u64*)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000 + GICR_TYPER);
    gicv3_core_diag[core][7]  = (*typer) >> 32;
    // Extended diagnostic: per-intid group modifier (IGRPMODR0) and the
    // actual priority bytes that landed in the redistributor SGI frame.
    // Plus EL2-side BPR1 and CTLR — to rule out priority preemption issues.
    volatile u32 *igrpmodr_dbg = (volatile u32*)(rd_sgi + GICR_IGRPMODR0);
    volatile u32 *ipri0_dbg    = (volatile u32*)(rd_sgi + GICR_IPRIORITYR0 + 0);
    volatile u32 *ipri1_dbg    = (volatile u32*)(rd_sgi + GICR_IPRIORITYR0 + 4);
    u64 v_bpr1, v_ctlr, v_ap1r0;
    asm volatile("mrs %0, S3_0_C12_C12_3" : "=r"(v_bpr1));   // ICC_BPR1_EL1
    asm volatile("mrs %0, S3_0_C12_C12_4" : "=r"(v_ctlr));   // ICC_CTLR_EL1
    asm volatile("mrs %0, S3_0_C12_C9_0"  : "=r"(v_ap1r0));  // ICC_AP1R0_EL1
    // GICR_CTLR is at offset 0 in the RD_BASE frame (NOT in SGI_BASE).
    // Bits of interest: [3]=RWP, [24]=DPG0, [25]=DPG1NS, [26]=DPG1S.
    // DPG1NS=1 -> RD refuses to deliver Group 1 NS interrupts to its PE.
    volatile u32 *gicr_ctlr_dbg = (volatile u32*)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000 + 0x0000);
    gicv3_core_diag2[core][0] = (u64)*igrpmodr_dbg;
    gicv3_core_diag2[core][1] = (u64)*ipri0_dbg;
    gicv3_core_diag2[core][2] = (u64)*ipri1_dbg;
    gicv3_core_diag2[core][3] = v_bpr1;
    gicv3_core_diag2[core][4] = v_ctlr;
    gicv3_core_diag2[core][5] = ((u64)*gicr_ctlr_dbg) | (v_ap1r0 << 32);
    gicv3_core_diag2[core][6] = (u64)gicv3_waker_trace[core][5];  // local wake flags from gicv3_force_wake_core()
    asm volatile("dc civac, %0" :: "r"(&gicv3_core_diag[core][0]) : "memory");
    asm volatile("dc civac, %0" :: "r"(&gicv3_core_diag2[core][0]) : "memory");
    asm volatile("dsb sy" ::: "memory");

    // EXPERIMENT-3 self-tests DISABLED (2026-10-03).
    //
    // These inject a PPI into the calling core's own redistributor
    // (gicv3_probe_local_irq_path). With GICD_CTLR.DS now set, interrupt
    // delivery on the A72 apparently WORKS, the injected PPI is actually
    // delivered, and it evicts core 4 from baseline_runner_a72 - the boot
    // wedges right at "[BASELINE_A72] Dispatching baseline". The probes were
    // written when delivery was dead and the injection was harmless; with DS set
    // they are actively destructive. Disabled rather than deleted so the
    // telemetry stays available under a future no-DS build.
    //
    // THE DECISIVE TEST IS NOW THE SGI_TEST BELOW: if c4 irq_cnt goes 0x0->0x1
    // we can finally conclude the DS quirk was the missing piece.
    if (core >= 4u && core < 6u) {
        gicv3_probe_local_wake(core);
    }
    // Control: the identical probe also runs on cores 1..3, which are
    // known to receive interrupts. A FAIL on the A72 only means something
    // if the same probe PASSES on an A53.
    gicv3_probe_local_irq_path(core);
}

void gicv3_enable_irq(u32 irq) {
    if (irq < 32) {
        // SGI/PPI: enable in local redistributor (no lock needed, per-core)
        u64 mpidr;
        asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        u32 aff0 = (u32)(mpidr & 0xFF);
        u32 aff1 = (u32)((mpidr >> 8) & 0xFF);
        u32 core = (aff1 ? (aff0 + 4) : aff0);

        uintptr_t rd_sgi = (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000;
        *(volatile u32*)(rd_sgi + GICR_ISENABLER0) = (1u << irq);
    } else {
        // SPI: enable in distributor with spinlock protection
        u32 reg = GICD_ISENABLER + (irq / 32) * 4;
        u32 bit = 1 << (irq % 32);
        lock_gicd();
        gicd_write(reg, bit);
        unlock_gicd();
    }
}

// Route SPI irq to the CPU described by affinity (0 = core 0, matches MPIDR).
// GICD_IROUTER[n] is a 64-bit register at offset 0x6000 + n*8.
void gicv3_route_irq(u32 irq, u64 affinity) {
    if (irq < 32) return;
    volatile u64* r = (volatile u64*)((uintptr_t)GICD_BASE + GICD_IROUTER + irq * 8);
    *r = affinity;
    asm volatile("dmb sy" ::: "memory");
}

// Set interrupt priority (0 = highest, 0xFF = lowest).
// For SPI (irq >= 32): GICD_IPRIORITYR with spinlock protection.
// For SGI/PPI (irq < 32): GICR_IPRIORITYR (local to core, no lock needed).
void gicv3_set_priority(u32 irq, u8 prio) {
    if (irq < 32) {
        // SGI/PPI priority in redistributor (ARE=1: GICR only, GICD ignored)
        u64 mpidr;
        asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        u32 aff0 = (u32)(mpidr & 0xFF);
        u32 aff1 = (u32)((mpidr >> 8) & 0xFF);
        u32 core = (aff1 ? (aff0 + 4) : aff0);

        uintptr_t rd_sgi = (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000;
        u32 reg_offset = GICR_IPRIORITYR0 + (irq & ~3u);
        u32 shift = (irq & 3) * 8;

        volatile u32 *p = (volatile u32*)(rd_sgi + reg_offset);
        u32 val = *p;
        val = (val & ~(0xFFu << shift)) | ((u32)prio << shift);
        *p = val;
        asm volatile("dsb sy" ::: "memory");
    } else {
        // SPI priority in distributor with spinlock
        u32 reg = GICD_IPRIORITYR + (irq & ~3u);
        u32 shift = (irq & 3) * 8;
        lock_gicd();
        u32 val = gicd_read(reg);
        val = (val & ~(0xFFu << shift)) | ((u32)prio << shift);
        gicd_write(reg, val);
        unlock_gicd();
    }
}

// Acknowledge interrupt: read INTID from ICC_IAR1_EL1 (also deactivates spurious).
u32 gicv3_ack_irq(void) {
    u64 intid;
    asm volatile("mrs %0, ICC_IAR1_EL1" : "=r"(intid));
    asm volatile("isb");
    return (u32)intid;
}

// End-of-interrupt: signal completion to GIC CPU interface.
void gicv3_eoi_irq(u32 intid) {
    asm volatile("msr ICC_EOIR1_EL1, %0" :: "r"((u64)intid));
    asm volatile("isb");
}

// ============================================================================
// Phase 2: GICv3 SGI (Software Generated Interrupts) for Pipe-it pipeline
// Targeted IPI: efficient core-to-core wakeup vs SEV/WFE
// ============================================================================

// Initialize SGI for non-secure group 1 on calling core's redistributor.
// CRITICAL: With ARE=1, SGI/PPI priorities are ONLY in GICR (not GICD).
void gicv3_sgi_init(void) {
    // Get this core's redistributor index
    u64 mpidr;
    asm volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    u32 aff0 = (u32)(mpidr & 0xFF);
    u32 aff1 = (u32)((mpidr >> 8) & 0xFF);
    u32 core = (aff1 ? (aff0 + 4) : aff0);
    if (core >= 6) return;

    uintptr_t rd_sgi = (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000;

    // Set priority for all SGIs 0-15 to 0x80 (mid priority) in REDISTRIBUTOR.
    // GICD_IPRIORITYR is IGNORED for SGI/PPI when ARE=1.
    for (u32 i = 0; i < 16; i += 4) {
        volatile u32 *p = (volatile u32*)(rd_sgi + GICR_IPRIORITYR0 + i);
        *p = 0x80808080u;
    }

    // Configure SGIs as Group 1 Non-Secure (IGROUPR=1, IGRPMODR=0)
    *(volatile u32*)(rd_sgi + GICR_IGROUPR0) = 0xFFFFFFFFu;
    *(volatile u32*)(rd_sgi + GICR_IGRPMODR0) = 0x0u;

    // Enable SGI 0-15
    *(volatile u32*)(rd_sgi + GICR_ISENABLER0) = 0x0000FFFFu;

    asm volatile("dsb sy\n isb" ::: "memory");
}

// Diagnostic: tracks every gicv3_sgi_send invocation and the exact ICC_SGI1R_EL1
// value written. Lets us prove the MSR actually executed (vs a silent trap).
volatile u64 g_gicv3_sgi_send_count = 0;
volatile u64 g_gicv3_sgi_last_val   = 0;
volatile u64 g_gicv3_sgi_last_id    = 0;
volatile u64 g_gicv3_sgi_last_aff   = 0;

void gicv3_sgi_send(u32 sgi_id, u64 target_aff) {
    u32 aff1 = (u32)((target_aff >> 8) & 0xFF);
    u32 aff0 = (u32)(target_aff & 0xFF);
    u64 val = ((u64)1 << aff0);
    val |= ((u64)aff1 & 0xFF) << 16;
    val |= ((u64)(sgi_id & 0xF)) << 24;
    g_gicv3_sgi_last_val = val;
    g_gicv3_sgi_last_id  = sgi_id;
    g_gicv3_sgi_last_aff = target_aff;
    asm volatile("dsb sy" ::: "memory");
    asm volatile("msr ICC_SGI1R_EL1, %0" :: "r"(val));
    asm volatile("isb");
    g_gicv3_sgi_send_count += 1;
}

// Read GICR_ISPENDR0 on a specific redistributor (any core can call).
// SGI N pending bit = (val >> N) & 1.
u32 gicv3_read_ispendr0(u32 core) {
    if (core >= 6) return 0xDEADBEEF;
    volatile u32 *isp = (volatile u32*)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000 + GICR_ISPENDR0);
    return *isp;
}

u32 gicv3_read_gicd_ctlr(void) { return gicd_read(GICD_CTLR); }
u64 gicv3_read_gicd_typer(void) {
    // Two bugs stacked here, and fixing only the visible one broke the boot.
    //
    // 1. The old code read offset 0x0008. That is GICD_IIDR, not GICD_TYPER -
    //    gicv3.h has defined GICD_TYPER as 0x0004 all along. What it produced,
    //    0x0041143B, is the textbook ARM GICv3 IIDR, which is why it looked
    //    plausible and every "gicd_typer=" line in SGI_TEST had been reporting
    //    IIDR.
    //
    // 2. Changing the literal to GICD_TYPER without changing the width made it
    //    WORSE: a 64-bit load at GICD_BASE+0x0004 is not naturally aligned
    //    (0xFEE00004), and this GIC takes a synchronous data abort at EL2 on it:
    //        [EL2] ESR: 0x96000021   FAR: 0xFEE00004
    //    The 0x0008 it replaced WAS 8-byte aligned, which is why the original
    //    worked at all. GICD_TYPER is a 32-bit register and must be read as one;
    //    reading it as u32 at the same address is fine.
    //
    // Kept returning u64 because the caller stores it in a u64 and prints it in
    // hex; widening a u32 read is not the same as an unaligned u64 load.
    return (u64)gicd_read(GICD_TYPER);
}

// Optimized SGI send to list of cores using cluster-based TargetList.
// GICv3 ICC_SGI1R_EL1 format: [63:56]=RSV, [55:48]=Aff3, [47:40]=RSV, [39:32]=Aff2,
//                               [31:24]=INTID, [23:16]=Aff1, [15:0]=TargetList (Aff0 bits)
void gicv3_sgi_send_optimized(u32 sgi_id, u32 cluster, u16 target_list) {
    u64 val = ((u64)(sgi_id & 0xF) << 24);
    val |= ((u64)(cluster & 0xFF) << 16);
    val |= (target_list & 0xFFFF);

    asm volatile("dsb ishst" ::: "memory");
    asm volatile("msr ICC_SGI1R_EL1, %0" :: "r"(val));
    asm volatile("isb");
}

// Send SGI to list of cores (bits 0-5 for cores 0-5)
// RK3399 affinity: A53 cluster (cores 0-3) Aff1=0; A72 cluster (cores 4-5) Aff1=1
void gicv3_sgi_send_to_list(u32 sgi_id, u16 core_list) {
    // Cluster 0 (A53): cores 0-3 -> Aff0 bits 0-3
    u16 cluster0_targets = core_list & 0xF;
    if (cluster0_targets) {
        gicv3_sgi_send_optimized(sgi_id, 0, cluster0_targets);
    }

    // Cluster 1 (A72): cores 4-5 -> Aff0 bits 0-1 (mapped from core_list bits 4-5)
    u16 cluster1_targets = (core_list >> 4) & 0x3;
    if (cluster1_targets) {
        gicv3_sgi_send_optimized(sgi_id, 1, cluster1_targets);
    }
}

// Acknowledge SGI and return source core
u32 gicv3_sgi_ack(void) {
    // Read ICC_IAR1_EL1 - returns INTID and source core for SGIs
    u64 iar;
    asm volatile("mrs %0, ICC_IAR1_EL1" : "=r"(iar));
    asm volatile("isb");
    
    u32 intid = (u32)(iar & 0xFFFFFF);
    // Source core in bits [32:39] for SGI
    u32 source = (u32)((iar >> 32) & 0xFF);
    
    return (source << 16) | intid;  // Pack source and intid
}
