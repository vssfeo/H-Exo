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
    // RK3399 has 6 CPU interfaces total, so prewake all redistributors.
    for (u32 cpu = 0; cpu < 6; cpu++) {
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

static void gicv3_probe_local_irq_path(u32 core) {
    volatile u32 *ispendr = (volatile u32 *)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000u + GICR_ISPENDR0);
    volatile u32 *icpendr = (volatile u32 *)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000u + GICR_ICPENDR0);
    const u32 PPI_BIT = 1u << 29;   // PPI 29 = ARM_IRQ_SEC_PHY_TIMER, per-PE, pre-enabled
    u64 hppir_before, hppir_after, iar;

    asm volatile("mrs %0, S3_0_C12_C12_2" : "=r"(hppir_before));  // ICC_HPPIR1_EL1
    *ispendr = PPI_BIT;
    asm volatile("dsb sy" ::: "memory");
    asm volatile("isb");
    asm volatile("mrs %0, S3_0_C12_C12_2" : "=r"(hppir_after));

    if ((hppir_after & 0x3FFu) != 0x3FFu) {
        // A real INTID reached the CPU interface: consume it so we do not
        // leave a stray interrupt pending.
        asm volatile("mrs %0, S3_0_C12_C12_0" : "=r"(iar));   // ICC_IAR1_EL1
        asm volatile("msr S3_0_C12_C12_1, %0" :: "r"(iar));   // ICC_EOIR1_EL1
    }
    *icpendr = PPI_BIT;
    asm volatile("dsb sy" ::: "memory");
    asm volatile("isb" ::: "memory");

    /*
     * The decisive signal is hppir_after. hppir_before is read before anything
     * is injected, so it is trivially 0x3FF ("nothing pending") and cannot
     * distinguish a live forward path from a dead one. Stash the AFTER value so
     * the NS-side verdict reflects what actually reached the CPU interface.
     */
    gicv3_core_diag2[core][6]  |= ((u64)(hppir_after & 0x3FFu) << 32);
    gicv3_core_diag2[core][13] = (u64)(hppir_before & 0x3FFu);
    gicv3_core_diag2[core][14] = (u64)(hppir_after  & 0x3FFu);
    gicv3_core_diag2[core][15] = (u64)((hppir_after & 0x3FFu) != 0x3FFu);
    {
        u64 hppir_final;
        asm volatile("mrs %0, S3_0_C12_C12_2" : "=r"(hppir_final));  /* ICC_HPPIR1_EL1 */
        gicv3_core_diag2[core][16] = (u64)(hppir_final & 0x3FFu);
    }
    asm volatile("dc civac, %0" :: "r"(&gicv3_core_diag2[core][0]) : "memory");
    asm volatile("dsb sy" ::: "memory");
}
void gicv3_init_cpu_iface(void) {
    // 1. Wake this core's redistributor (ProcessorSleep clear)
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
    u32 waker_after = gicv3_force_wake_core(core, 16);

    // 2. Enable system register access (ICC_SRE_EL2)
    u32 sre;
    asm volatile("mrs %0, ICC_SRE_EL2" : "=r"(sre));
    sre |= (1 << 0) | (1 << 1) | (1 << 2);
    asm volatile("msr ICC_SRE_EL2, %0" :: "r"(sre));
    asm volatile("isb");
    
    // 3. Set priority mask
    u64 pmr = 0xFF;
    asm volatile("msr ICC_PMR_EL1, %0" :: "r"(pmr));
    
    // 4. Enable Group 0 + Group 1 interrupts at CPU interface.
    u64 igrp = 1;
    asm volatile("msr ICC_IGRPEN0_EL1, %0" :: "r"(igrp));
    asm volatile("msr ICC_IGRPEN1_EL1, %0" :: "r"(igrp));
    asm volatile("isb");
    
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
    
    // 7. Enable SGI 0-15 in this core's redistributor
    volatile u32 *isenabler = (volatile u32*)(rd_sgi + GICR_ISENABLER0);
    *isenabler = 0xFFFF;  // Enable SGI 0-15
    asm volatile("dsb sy" ::: "memory");
    
    // 8. Wait for redistributor RWP (Register Write Pending) to clear so the
    //    enable/group/priority writes are committed before we hit WFI.
    volatile u32 *gicr_ctlr = (volatile u32*)(
        (uintptr_t)GICR_BASE + (uintptr_t)core * 0x20000 + GICR_CTLR);
    int rwp_t = 1000000;
    while ((*gicr_ctlr & (1u << 3)) && rwp_t--) asm volatile("yield");
    
    asm volatile("dsb sy\n isb" ::: "memory");
    
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

    // EXPERIMENT-3: self-tests, A72 only (cores 0-3 already work).
    if (core >= 4u && core < 6u) {
        gicv3_probe_local_wake(core);
        gicv3_probe_local_irq_path(core);
    }
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
    return *(volatile u64*)((uintptr_t)GICD_BASE + 0x0008);
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
