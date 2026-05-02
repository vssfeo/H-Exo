// H-Exo HAL: RK3399 CRU implementation. Ports just enough of
// drivers/clk/rockchip/clk_rk3399.c (rkclk_set_pll, rk3399_configure_cpu_b)
// to retune ABPLL from NS EL2 bare-metal.
// Added: bare-metal I2C0 + SYR827 PMIC driver for VDD_CPU_B voltage scaling.

#include "cru.h"
#include "../core/types.h"

static inline u32 mmio_read32(uintptr_t addr) {
    return *(volatile u32*)addr;
}
static inline void mmio_write32(uintptr_t addr, u32 val) {
    *(volatile u32*)addr = val;
}

static inline void delay_cycles(volatile u32 n) {
    while (n--) {
        asm volatile("nop");
    }
}

/* ==========================================================================
 * Bare-metal Rockchip RK3x native I2C0 driver (RK3399 PMU I2C)
 * Base address: 0xFF3C0000  (assigned clock = 200 MHz from PMUCRU)
 * 
 * RK3399 uses the Rockchip native I2C controller (rk3x), NOT DesignWare.
 * Register map derived from Linux drivers/i2c/busses/i2c-rk3x.c
 * ========================================================================== */
#define I2C0_BASE       0xFF3C0000UL

/* Register Map - Rockchip RK3x native I2C */
#define REG_CON         0x00    /* control register */
#define REG_CLKDIV      0x04    /* clock divisor register */
#define REG_MRXADDR     0x08    /* slave address for REGISTER_TX */
#define REG_MRXRADDR    0x0c    /* slave register address for REGISTER_TX */
#define REG_MTXCNT      0x10    /* number of bytes to be transmitted */
#define REG_MRXCNT      0x14    /* number of bytes to be received */
#define REG_IEN         0x18    /* interrupt enable */
#define REG_IPD         0x1c    /* interrupt pending */
#define REG_FCNT        0x20    /* finished count */

/* Data buffer offsets */
#define TXBUFFER_BASE   0x100
#define RXBUFFER_BASE   0x200

/* REG_CON bits */
#define REG_CON_EN          (1U << 0)
#define REG_CON_MOD_TX      0       /* transmit data */
#define REG_CON_MOD_REGISTER_TX 1   /* select register and restart */
#define REG_CON_MOD_RX      2       /* receive data */
#define REG_CON_MOD(mod)    ((mod) << 1)
#define REG_CON_MOD_MASK    ((1U << 1) | (1U << 2))
#define REG_CON_START       (1U << 3)
#define REG_CON_STOP        (1U << 4)
#define REG_CON_LASTACK     (1U << 5)   /* 1: send NACK after last received byte */
#define REG_CON_ACTACK      (1U << 6)   /* 1: stop if NACK is received */

/* REG_MRXADDR bits */
#define REG_MRXADDR_VALID(x) (1U << (24 + (x)))   /* [x*8+7:x*8] of MRX[R]ADDR valid */

/* REG_IEN/REG_IPD bits */
#define REG_INT_BTF         (1U << 0)   /* a byte was transmitted */
#define REG_INT_BRF         (1U << 1)   /* a byte was received */
#define REG_INT_MBTF        (1U << 2)   /* master data transmit finished */
#define REG_INT_MBRF        (1U << 3)   /* master data receive finished */
#define REG_INT_START       (1U << 4)   /* START condition generated */
#define REG_INT_STOP        (1U << 5)   /* STOP condition generated */
#define REG_INT_NAKRCV      (1U << 6)   /* NACK received */
#define REG_INT_ALL         0x7f

// PMUCRU for I2C0 clock gate diagnostics
#define PMUCRU_BASE         0xFF750000UL
#define PMUCRU_CLKGATE_CON0 (PMUCRU_BASE + 0x100)

// SYR827 (Silergy FAN53555-family) on I2C0, addr 0x40
#define SYR827_ADDR     0x40
#define SYR827_VSEL0    0x00
#define SYR827_VSEL1    0x01
#define SYR827_CONTROL  0x02
#define SYR827_ID1      0x03
#define SYR827_ID2      0x04

// SYR827 voltage formula: Vout = 712500 uV + (vsel & 0x3F) * 12500 uV
#define SYR827_VSEL_MIN_UV  712500UL
#define SYR827_VSEL_STEP_UV 12500UL
#define SYR827_VSEL_MASK    0x3Fu

static inline void i2c0_writel(u32 val, u32 off) {
    mmio_write32(I2C0_BASE + off, val);
}
static inline u32 i2c0_readl(u32 off) {
    return mmio_read32(I2C0_BASE + off);
}

/* Reset all interrupt pending bits */
static inline void i2c0_clear_ipd(void) {
    i2c0_writel(REG_INT_ALL, REG_IPD);
}

/* Bus recovery: toggle SCL 9 times with SDA high to unlock stuck slaves */
static void i2c0_bus_recovery(void) {
    // Force GPIO mode on I2C0 pins temporarily (RK3399 I2C0 = GPIO1 A[6:7])
    // GRF_GPIO1A_IOMUX = 0xFF770004
    volatile u32 *grf_gpio1a_iomux = (volatile u32*)0xFF770004UL;
    u32 iomux_orig = *grf_gpio1a_iomux;

    // Set GPIO1A6 (SDA) and GPIO1A7 (SCL) to GPIO mode (bits 12:15 and 14:15)
    // Write-mask format: bits [31:16] = mask, [15:0] = data
    // GPIO mode = 0 for both pins
    *grf_gpio1a_iomux = (0xF << 16) | (0x0);  // Mask bits 12-15, set to 0 (GPIO)

    // GRF_GPIO1A_P = 0xFF770000 (GPIO direction/output)
    // GRF_GPIO1A_D = 0xFF770008 (GPIO data)
    volatile u32 *grf_gpio1a_p = (volatile u32*)0xFF770000UL;
    volatile u32 *grf_gpio1a_d = (volatile u32*)0xFF770008UL;

    // Set both pins as output
    u32 dir_orig = *grf_gpio1a_p;
    *grf_gpio1a_p = (0x3 << 16) | 0x3;  // Bits 6,7 as output (mask + value)

    // Drive SDA high, toggle SCL 9 times
    for (int i = 0; i < 9; i++) {
        *grf_gpio1a_d = (0x3 << 16) | 0x2;  // SDA=1, SCL=1 (bit 6=SDA, bit 7=SCL)
        for (volatile int d = 0; d < 100; d++) asm volatile("nop");
        *grf_gpio1a_d = (0x3 << 16) | 0x0;  // SDA=1, SCL=0
        for (volatile int d = 0; d < 100; d++) asm volatile("nop");
    }

    // Send STOP condition (SDA low->high while SCL high)
    *grf_gpio1a_d = (0x3 << 16) | 0x0;  // SDA=1, SCL=0
    for (volatile int d = 0; d < 100; d++) asm volatile("nop");
    *grf_gpio1a_d = (0x3 << 16) | 0x2;  // SDA=1, SCL=1
    for (volatile int d = 0; d < 100; d++) asm volatile("nop");

    // Restore original direction and iomux
    *grf_gpio1a_p = (0x3 << 16) | (dir_orig & 0x3);
    *grf_gpio1a_iomux = (0xF << 16) | (iomux_orig & 0xF);

    // Ensure I2C controller is disabled after recovery
    i2c0_writel(0, REG_CON);
    i2c0_clear_ipd();
}

// Pre-init diagnostic snapshot (captured once per pmic_set_vdd_cpu_b call)
static u32 g_pmic_diag_pre_en_stat = 0;
static u32 g_pmic_diag_pre_status  = 0;
static u32 g_pmic_diag_pre_con     = 0;
static u32 g_pmic_diag_pre_txflr   = 0;
static u32 g_pmic_diag_pre_rxflr   = 0;
static u32 g_pmic_diag_pmu_gate0   = 0;
static u32 g_pmic_diag_pmu_gate1   = 0;
static u32 g_pmic_diag_pmu_gate2   = 0;

/* Calculate CLKDIV for target SCL rate.
 * SCL = PCLK / (16 * (DIV + 1))
 * For 200 MHz PCLK -> 400 kHz: DIV = (200M / (16 * 400k)) - 1 = 30.25 -> 30
 */
static void i2c0_set_clk_rate(u32 scl_rate_hz) {
    u32 pclk = 200000000;   /* 200 MHz PMU I2C clock */
    u32 div = (pclk / (16 * scl_rate_hz)) - 1;
    if (div > 0xFFFF) div = 0xFFFF;
    i2c0_writel((div << 16) | (div & 0xFFFF), REG_CLKDIV);
}

static void i2c0_init_fast_mode(void) {
    // Snapshot PMUCRU clock gates before touching anything
    g_pmic_diag_pmu_gate0 = mmio_read32(PMUCRU_BASE + 0x100);
    g_pmic_diag_pmu_gate1 = mmio_read32(PMUCRU_BASE + 0x104);
    g_pmic_diag_pmu_gate2 = mmio_read32(PMUCRU_BASE + 0x108);

    // UNGATE all PMUCRU clocks: Rockchip convention is bit=1=gated, bit=0=ungated.
    mmio_write32(PMUCRU_BASE + 0x100, 0xFFFF0000);
    mmio_write32(PMUCRU_BASE + 0x104, 0xFFFF0000);
    mmio_write32(PMUCRU_BASE + 0x108, 0xFFFF0000);

    // Snapshot controller state BEFORE touching anything
    g_pmic_diag_pre_en_stat = i2c0_readl(REG_CON);
    g_pmic_diag_pre_status  = i2c0_readl(REG_IPD);
    g_pmic_diag_pre_con     = i2c0_readl(REG_CON);
    g_pmic_diag_pre_txflr   = i2c0_readl(REG_FCNT);
    g_pmic_diag_pre_rxflr   = 0;  // Not applicable for RK3x

    // Disable controller if enabled
    if (g_pmic_diag_pre_con & REG_CON_EN) {
        // Try graceful shutdown first
        u32 timeout = 500;
        while (timeout--) {
            i2c0_writel(0, REG_CON);
            if ((i2c0_readl(REG_CON) & REG_CON_EN) == 0)
                break;
            for (volatile u32 d = 0; d < 5000; d++) { }
        }
    }

    // Clear all pending interrupts
    i2c0_clear_ipd();

    // CRITICAL: Bus recovery for stuck slaves (0x6F readback fix)
    // If bus is stuck (SDA/SCL low), the following init will fail silently
    i2c0_bus_recovery();

    // Set clock rate for 400 kHz fast mode (slower = more reliable for PMIC)
    i2c0_set_clk_rate(100000);  // 100 kHz standard mode for stability

    // Disable all interrupts (polling mode)
    i2c0_writel(0, REG_IEN);

    // Small delay after init to let bus settle
    for (volatile int d = 0; d < 1000; d++) asm volatile("nop");
}

/* Wait for interrupt pending bit with timeout */
static int i2c0_wait_ipd(u32 bit, u32 timeout_us) {
    u32 timeout = timeout_us * 10;  /* rough cycle count @ ~100 MHz */
    while (timeout--) {
        if (i2c0_readl(REG_IPD) & bit)
            return 0;
    }
    return -1;
}

/* Poll for NACK reception */
static int i2c0_check_nack(void) {
    if (i2c0_readl(REG_IPD) & REG_INT_NAKRCV) {
        i2c0_writel(REG_INT_NAKRCV, REG_IPD);  /* ack */
        return -1;
    }
    return 0;
}

/* 
 * RK3x I2C write: single byte to register.
 * Uses TXBUFFER_BASE for data and polling for completion.
 */
static int i2c0_write_byte(u8 dev_addr, u8 reg, u8 data) {
    int ret;
    
    // Clear pending interrupts
    i2c0_clear_ipd();
    
    // Setup slave address with valid bit
    u32 addr_val = ((dev_addr & 0x7f) << 1) | REG_MRXADDR_VALID(0);
    i2c0_writel(addr_val, REG_MRXADDR);
    i2c0_writel(0, REG_MRXRADDR);
    
    // Fill TX buffer: RK3x uses one byte per TXDATA slot (stride 4 bytes).
    // Writing both bytes packed into one word can silently drop byte #2,
    // causing PMIC write-readback mismatches.
    i2c0_writel((u32)reg,  TXBUFFER_BASE + 0x0);
    i2c0_writel((u32)data, TXBUFFER_BASE + 0x4);
    i2c0_writel(2, REG_MTXCNT);  /* 2 bytes to transmit */
    
    // Enable controller in TX mode, send START
    u32 con = REG_CON_EN | REG_CON_MOD(REG_CON_MOD_TX) | REG_CON_START | REG_CON_ACTACK;
    i2c0_writel(con, REG_CON);
    
    // Wait for START interrupt
    ret = i2c0_wait_ipd(REG_INT_START, 1000);
    if (ret) goto err;
    i2c0_writel(REG_INT_START, REG_IPD);  /* ack */
    
    // Wait for master transmit finish or NACK
    u32 timeout = 50000;
    while (timeout--) {
        u32 ipd = i2c0_readl(REG_IPD);
        if (ipd & REG_INT_NAKRCV) {
            i2c0_writel(REG_INT_NAKRCV, REG_IPD);
            ret = -2;  /* NACK */
            goto err;
        }
        if (ipd & REG_INT_MBTF) {
            i2c0_writel(REG_INT_MBTF, REG_IPD);
            break;
        }
    }
    if (timeout == 0) { ret = -3; goto err; }
    
    // Send STOP
    con = i2c0_readl(REG_CON);
    con |= REG_CON_STOP;
    i2c0_writel(con, REG_CON);
    
    // Wait for STOP
    ret = i2c0_wait_ipd(REG_INT_STOP, 1000);
    if (ret) goto err;
    i2c0_writel(REG_INT_STOP, REG_IPD);
    
    // Disable controller
    i2c0_writel(0, REG_CON);
    return 0;
    
err:
    // Disable and clear
    i2c0_writel(0, REG_CON);
    i2c0_clear_ipd();
    return ret - 100;
}

/* 
 * RK3x I2C read: single byte from register.
 * Uses REGISTER_TX mode: sends write+reg, then restart+read.
 * FIXED: Proper bus handling, NACK for last byte, buffer validation.
 */
static int i2c0_read_byte(u8 dev_addr, u8 reg, u8 *out) {
    int ret;
    
    if (!out) return -10;
    *out = 0;  // Clear output early
    
    // Clear pending interrupts and ensure clean state
    i2c0_clear_ipd();
    
    // Ensure controller is disabled before setup
    i2c0_writel(0, REG_CON);
    for (volatile int d = 0; d < 100; d++) asm volatile("nop");
    
    // Setup slave address (7-bit addr << 1, write mode)
    // MRXADDR: bits [7:1] = slave addr, bit 0 = 0 (write for register addr phase)
    u32 addr_val = ((dev_addr & 0x7f) << 1) | REG_MRXADDR_VALID(0);
    i2c0_writel(addr_val, REG_MRXADDR);
    
    // Put register address in MRXRADDR with valid bit
    u32 raddr_val = reg | REG_MRXADDR_VALID(0);
    i2c0_writel(raddr_val, REG_MRXRADDR);
    
    // Set RX count = 1
    i2c0_writel(1, REG_MRXCNT);
    
    // Clear any stale data from RX buffer (read and discard)
    volatile u32 dummy = i2c0_readl(RXBUFFER_BASE);
    (void)dummy;
    
    // Enable controller in REGISTER_TX mode
    // CRITICAL: LASTACK must be SET for single-byte read (send NACK after byte)
    // ACTACK = stop transaction if NACK received
    u32 con = REG_CON_EN | REG_CON_MOD(REG_CON_MOD_REGISTER_TX) | 
              REG_CON_START | REG_CON_LASTACK | REG_CON_ACTACK;
    i2c0_writel(con, REG_CON);
    
    // Wait for START interrupt
    ret = i2c0_wait_ipd(REG_INT_START, 5000);
    if (ret) goto err;
    i2c0_writel(REG_INT_START, REG_IPD);
    
    // Wait for master receive finish or NACK
    // Extended timeout for PMIC response
    u32 timeout = 200000;
    u32 ipd_status = 0;
    while (timeout--) {
        ipd_status = i2c0_readl(REG_IPD);
        if (ipd_status & REG_INT_NAKRCV) {
            i2c0_writel(REG_INT_NAKRCV, REG_IPD);
            ret = -2;  /* NACK from slave */
            goto err;
        }
        if (ipd_status & REG_INT_MBRF) {
            i2c0_writel(REG_INT_MBRF, REG_IPD);
            break;
        }
        if (ipd_status & REG_INT_BRF) {
            // Byte received flag - but we wait for MBRF (all done)
            i2c0_writel(REG_INT_BRF, REG_IPD);
        }
    }
    if (timeout == 0) { 
        ret = -3;  /* Timeout waiting for MBRF */
        goto err; 
    }
    
    // CRITICAL: Verify data was actually received by checking FCNT
    // FCNT should show bytes finished (bits [4:0])
    u32 fcnt = i2c0_readl(REG_FCNT);
    if ((fcnt & 0x1F) == 0) {
        // No bytes in buffer - bus may be stuck
        ret = -4;
        goto err;
    }
    
    // Read data from RX buffer (first byte at offset 0)
    u32 rx_data = i2c0_readl(RXBUFFER_BASE);
    *out = (u8)(rx_data & 0xFF);
    
    // Send STOP
    con = i2c0_readl(REG_CON);
    con |= REG_CON_STOP;
    i2c0_writel(con, REG_CON);
    
    // Wait for STOP with longer timeout
    ret = i2c0_wait_ipd(REG_INT_STOP, 5000);
    if (ret) goto err;
    i2c0_writel(REG_INT_STOP, REG_IPD);
    
    // Disable controller
    i2c0_writel(0, REG_CON);
    
    // Post-transaction delay to let bus settle
    for (volatile int d = 0; d < 500; d++) asm volatile("nop");
    
    return 0;
    
err:
    // Force STOP and disable
    i2c0_writel(REG_CON_STOP, REG_CON);
    for (volatile int d = 0; d < 1000; d++) asm volatile("nop");
    i2c0_writel(0, REG_CON);
    i2c0_clear_ipd();
    return ret - 100;
}

/* ==========================================================================
 * SYR827 PMIC: set VDD_CPU_B voltage (A72 cluster supply)
 * ========================================================================== */
// RK808 at I2C addr 0x1b; register 0x17 = ID1, expected 0x00 for RK808
#define RK808_ADDR      0x1b
#define RK808_REG_ID1   0x17
#define RK808_REG_BUCK2_ON_VSEL 0x21
#define RK808_REG_DCDC_EN2 0x23
#define RK808_REG_LDO_EN1  0x24
#define RK808_LDO6_ON_VSEL 0x4D
#define RK808_LDO9_ON_VSEL 0x54
#define RK808_LDO_EN2      0x24

// RK3399 eMMC host + PHY low-level register windows
#define RK3399_EMMC_BASE      0xFE330000UL
#define RK3399_GRF_SOC_CON22  0xFF77E590UL
#define RK3399_EMMC_PHY_BASE  0xFF77F780UL

#define EMMC_CTRL             0x000
#define EMMC_PWREN            0x004
#define EMMC_CLKENA           0x010
#define EMMC_CDETECT          0x050
#define EMMC_RINTSTS          0x044
#define EMMC_STATUS           0x048
#define EMMC_PHY_STATUS       0x020
#define EMMC_PHY_ST_READY     (1u << 0)
#define EMMC_CTRL_RESET_ALL   0x00000007u

#define GRF_EMMC_PHY_PD_BIT       0u
#define GRF_EMMC_PHY_RST_BIT      12u
#define GRF_MASK_BIT(b)           (1u << ((b) + 16))
#define GRF_DATA_BIT(b)           (1u << (b))

static int pmic_set_vdd_cpu_b(u32 target_uv, u32 *diag_rk808_id1, u32 *diag_i2c_status) {
    // Calculate vsel for SYR827: V = 712500 + vsel*12500
    if (target_uv < SYR827_VSEL_MIN_UV) target_uv = SYR827_VSEL_MIN_UV;
    u32 vsel = (target_uv - SYR827_VSEL_MIN_UV) / SYR827_VSEL_STEP_UV;
    if (vsel > SYR827_VSEL_MASK) vsel = SYR827_VSEL_MASK;
    u8 val = (u8)(vsel & SYR827_VSEL_MASK);

    // Initialize I2C controller
    i2c0_init_fast_mode();

    // First probe RK808 to verify I2C bus integrity
    u8 rk808_id1 = 0xFF;
    int ret = i2c0_read_byte(RK808_ADDR, RK808_REG_ID1, &rk808_id1);
    if (diag_rk808_id1) *diag_rk808_id1 = (u32)rk808_id1;
    // Don't fail here; RK808 probe is diagnostic only.
    (void)ret;

    // Read current selector bytes first, then update only VSEL[5:0] (RMW).
    // This preserves control bits in [7:6] and avoids clobbering mode flags.
    u8 cur_vsel0 = 0xFF;
    u8 cur_vsel1 = 0xFF;
    ret = i2c0_read_byte(SYR827_ADDR, SYR827_VSEL0, &cur_vsel0);
    if (ret) {
        if (diag_i2c_status) {
            *diag_i2c_status = (i2c0_readl(REG_CON) & 0xFFFF0000) |
                               (i2c0_readl(REG_IPD) & 0xFFFF);
        }
        return ret;
    }
    ret = i2c0_read_byte(SYR827_ADDR, SYR827_VSEL1, &cur_vsel1);
    if (ret) {
        if (diag_i2c_status) {
            *diag_i2c_status = (i2c0_readl(REG_CON) & 0xFFFF0000) |
                               (i2c0_readl(REG_IPD) & 0xFFFF);
        }
        return ret;
    }

    const u8 wr_vsel0 = (u8)((cur_vsel0 & (u8)~SYR827_VSEL_MASK) | val);
    const u8 wr_vsel1 = (u8)((cur_vsel1 & (u8)~SYR827_VSEL_MASK) | val);

    // Program both SYR827 selector registers to avoid board-specific VSEL mux ambiguity.
    // Some boards can boot with VSEL1 selected; writing both makes voltage raise deterministic.
    ret = i2c0_write_byte(SYR827_ADDR, SYR827_VSEL0, wr_vsel0);
    if (ret) {
        if (diag_i2c_status) {
            *diag_i2c_status = (i2c0_readl(REG_CON) & 0xFFFF0000) |
                               (i2c0_readl(REG_IPD) & 0xFFFF);
        }
        return ret;
    }

    ret = i2c0_write_byte(SYR827_ADDR, SYR827_VSEL1, wr_vsel1);
    if (ret) {
        if (diag_i2c_status) {
            *diag_i2c_status = (i2c0_readl(REG_CON) & 0xFFFF0000) |
                               (i2c0_readl(REG_IPD) & 0xFFFF);
        }
        return ret;
    }

    // Read back both VSEL registers; treat mismatch as PMIC voltage-program failure.
    u8 rb_vsel0 = 0xFF;
    u8 rb_vsel1 = 0xFF;
    ret = i2c0_read_byte(SYR827_ADDR, SYR827_VSEL0, &rb_vsel0);
    if (ret) {
        if (diag_i2c_status) {
            *diag_i2c_status = (i2c0_readl(REG_CON) & 0xFFFF0000) |
                               (i2c0_readl(REG_IPD) & 0xFFFF);
        }
        return ret;
    }

    ret = i2c0_read_byte(SYR827_ADDR, SYR827_VSEL1, &rb_vsel1);
    if (ret) {
        if (diag_i2c_status) {
            *diag_i2c_status = (i2c0_readl(REG_CON) & 0xFFFF0000) |
                               (i2c0_readl(REG_IPD) & 0xFFFF);
        }
        return ret;
    }

    const u8 rb0_vsel = (u8)(rb_vsel0 & SYR827_VSEL_MASK);
    const u8 rb1_vsel = (u8)(rb_vsel1 & SYR827_VSEL_MASK);
    const bool rb0_ok = (rb0_vsel >= val);
    const bool rb1_ok = (rb1_vsel >= val);

    if (diag_i2c_status) {
        // Pack diagnostic info: VSEL0[31:24] | VSEL1[23:16] | IPD[15:0]
        *diag_i2c_status = ((u32)rb_vsel0 << 24) |
                           ((u32)rb_vsel1 << 16) |
                           (i2c0_readl(REG_IPD) & 0xFFFF);
    }

    if (!(rb0_ok || rb1_ok)) {
        return -106;
    }

    // Small delay for voltage ramp (~1 mV/us typical -> 300us for 300mV step)
    for (volatile u32 d = 0; d < 30000; d++) { /* ~300 us at ~100 MHz */ }

    return 0;
}

int pmic_get_vdd_cpu_b(u32 *out_uv) {
    u8 val;
    i2c0_init_fast_mode();
    int ret = i2c0_read_byte(SYR827_ADDR, SYR827_VSEL0, &val);
    if (ret) return ret;
    u32 vsel = val & SYR827_VSEL_MASK;
    *out_uv = SYR827_VSEL_MIN_UV + vsel * SYR827_VSEL_STEP_UV;
    return 0;
}

i32 cru_emmc_low_level_probe(emmc_low_level_diag_t *out) {
    if (!out) return -1;

    out->i2c_ret = -1;
    out->rk808_chip_id = 0xFF;
    out->rk808_buck2_on = 0xFF;
    out->rk808_reg23 = 0;
    out->rk808_reg24 = 0;
    out->sw1_en = 0;
    out->sw2_en = 0;
    out->ldo6_vsel = 0;
    out->ldo9_vsel = 0;
    out->ldo_en2 = 0;

    out->mmc_ctrl    = mmio_read32(RK3399_EMMC_BASE + EMMC_CTRL);
    out->mmc_pwren   = mmio_read32(RK3399_EMMC_BASE + EMMC_PWREN);
    out->mmc_clkena  = mmio_read32(RK3399_EMMC_BASE + EMMC_CLKENA);
    out->mmc_cdetect = mmio_read32(RK3399_EMMC_BASE + EMMC_CDETECT);
    out->mmc_status  = mmio_read32(RK3399_EMMC_BASE + EMMC_STATUS);
    out->mmc_rintsts = mmio_read32(RK3399_EMMC_BASE + EMMC_RINTSTS);

    out->grf_soc_con22 = mmio_read32(RK3399_GRF_SOC_CON22);
    out->emmc_phy_con0 = mmio_read32(RK3399_EMMC_PHY_BASE + 0x00);
    out->emmc_phy_status = mmio_read32(RK3399_EMMC_PHY_BASE + 0x20);

    i2c0_init_fast_mode();
    u8 chip_id = 0xFF;
    i2c0_read_byte(RK808_ADDR, RK808_REG_ID1, &chip_id);
    out->rk808_chip_id = chip_id;

    u8 buck2 = 0xFF;
    i2c0_read_byte(RK808_ADDR, RK808_REG_BUCK2_ON_VSEL, &buck2);
    out->rk808_buck2_on = buck2;

    u8 reg23 = 0;
    i32 ret = i2c0_read_byte(RK808_ADDR, RK808_REG_DCDC_EN2, &reg23);
    if (ret) {
        out->i2c_ret = ret;
        return ret;
    }

    u8 reg24 = 0;
    i32 ret24 = i2c0_read_byte(RK808_ADDR, RK808_REG_LDO_EN1, &reg24);

    u8 ldo6 = 0, ldo9 = 0, ldoen = 0;
    i2c0_read_byte(RK808_ADDR, RK808_LDO6_ON_VSEL, &ldo6);
    i2c0_read_byte(RK808_ADDR, RK808_LDO9_ON_VSEL, &ldo9);
    i2c0_read_byte(RK808_ADDR, RK808_LDO_EN2, &ldoen);

    out->i2c_ret = ret24;
    out->rk808_reg23 = reg23;
    out->rk808_reg24 = reg24;
    out->sw1_en = (reg23 >> 6) & 1u;
    out->sw2_en = (reg23 >> 7) & 1u;
    out->ldo6_vsel = ldo6;
    out->ldo9_vsel = ldo9;
    out->ldo_en2   = ldoen;

    return ret24;
}

// Small busy-wait delay helper (~1us per iteration at 100MHz, scale as needed)
static inline void cru_udelay(volatile u32 us) {
    for (volatile u32 d = 0; d < us * 100; d++) { /* ~1us */ }
}

i32 cru_emmc_recover_power_and_phy(void) {
    i2c0_init_fast_mode();

    // 1. Set eMMC supply voltages via RK808 LDO6 (VCC 3.3V) and LDO9 (VCCQ 1.8V).
    i32 ret = i2c0_write_byte(RK808_ADDR, RK808_LDO6_ON_VSEL, 0x0C); // 3.3V
    if (ret) return ret;
    ret = i2c0_write_byte(RK808_ADDR, RK808_LDO9_ON_VSEL, 0x06); // 1.8V
    if (ret) return ret;

    // 2. Enable LDO6 (bit 1) and LDO9 (bit 4) in LDO_EN2.
    u8 ldo_en2 = 0;
    ret = i2c0_read_byte(RK808_ADDR, RK808_LDO_EN2, &ldo_en2);
    if (ret) return ret;
    const u8 enable_ldo = (u8)(ldo_en2 | ((1u << 1) | (1u << 4)));
    ret = i2c0_write_byte(RK808_ADDR, RK808_LDO_EN2, enable_ldo);
    if (ret) return ret;

    // 3. Wait for voltage to settle (~5ms).
    cru_udelay(5000);

    // 4. Clear any stale host interrupts before PHY reset.
    mmio_write32(RK3399_EMMC_BASE + EMMC_RINTSTS, 0xFFFFFFFFu);

    // 5. PHY reset + power-cycle via GRF_SOC_CON22 mask-write bits:
    //    - bit0:  PHY power-down
    //    - bit12: PHY reset
    const u32 phy_status_before = mmio_read32(RK3399_EMMC_PHY_BASE + EMMC_PHY_STATUS);

    // Assert reset + power-down.
    mmio_write32(RK3399_GRF_SOC_CON22,
                 GRF_MASK_BIT(GRF_EMMC_PHY_PD_BIT) |
                 GRF_MASK_BIT(GRF_EMMC_PHY_RST_BIT) |
                 GRF_DATA_BIT(GRF_EMMC_PHY_PD_BIT) |
                 GRF_DATA_BIT(GRF_EMMC_PHY_RST_BIT));
    delay_cycles(2400000);

    // Keep reset asserted, release power-down.
    mmio_write32(RK3399_GRF_SOC_CON22,
                 GRF_MASK_BIT(GRF_EMMC_PHY_PD_BIT) |
                 GRF_MASK_BIT(GRF_EMMC_PHY_RST_BIT) |
                 GRF_DATA_BIT(GRF_EMMC_PHY_RST_BIT));
    delay_cycles(2400000);

    // Release reset.
    mmio_write32(RK3399_GRF_SOC_CON22,
                 GRF_MASK_BIT(GRF_EMMC_PHY_RST_BIT));
    delay_cycles(2400000);

    // 6. Reinitialize DW-MMC front-end after PHY reset.
    mmio_write32(RK3399_EMMC_BASE + EMMC_PWREN, 0x1u);
    mmio_write32(RK3399_EMMC_BASE + EMMC_CLKENA, 0x1u);
    mmio_write32(RK3399_EMMC_BASE + EMMC_CTRL, EMMC_CTRL_RESET_ALL);

    // Wait until reset bits self-clear.
    for (u32 i = 0; i < 200; i++) {
        if ((mmio_read32(RK3399_EMMC_BASE + EMMC_CTRL) & EMMC_CTRL_RESET_ALL) == 0) {
            break;
        }
        delay_cycles(24000);
    }

    // 7. Wait up to ~500ms for PHY ready/calibration complete after power-up.
    bool phy_progress = false;
    for (u32 i = 0; i < 500; i++) {
        u32 phy_status = mmio_read32(RK3399_EMMC_PHY_BASE + EMMC_PHY_STATUS);
        if (phy_status != phy_status_before) {
            phy_progress = true;
        }
        if ((phy_status & EMMC_PHY_ST_READY) != 0) {
            return 0;
        }
        delay_cycles(24000); // ~1ms at 24MHz
    }

    return phy_progress ? -111 : -110;
}

// Last PMIC diagnostic values (valid after a PMIC-related error in cru_set_a72_freq_mhz)
static u32 g_last_pmic_rk808_id1 = 0xFF;
static u32 g_last_pmic_i2c_status  = 0;
// Note: With RK3x native I2C driver, DEAD0065 marker should no longer appear.

void cru_get_last_pmic_diag(u32 *out_rk808_id1, u32 *out_i2c_status,
                              u32 *out_pre_en, u32 *out_pre_st, u32 *out_pre_con,
                              u32 *out_pre_tx, u32 *out_pre_rx,
                              u32 *out_gate0, u32 *out_gate1, u32 *out_gate2) {
    if (out_rk808_id1) *out_rk808_id1 = g_last_pmic_rk808_id1;
    if (out_i2c_status) *out_i2c_status = g_last_pmic_i2c_status;
    if (out_pre_en) *out_pre_en = g_pmic_diag_pre_en_stat;
    if (out_pre_st) *out_pre_st = g_pmic_diag_pre_status;
    if (out_pre_con) *out_pre_con = g_pmic_diag_pre_con;
    if (out_pre_tx) *out_pre_tx = g_pmic_diag_pre_txflr;
    if (out_pre_rx) *out_pre_rx = g_pmic_diag_pre_rxflr;
    if (out_gate0) *out_gate0 = g_pmic_diag_pmu_gate0;
    if (out_gate1) *out_gate1 = g_pmic_diag_pmu_gate1;
    if (out_gate2) *out_gate2 = g_pmic_diag_pmu_gate2;
}

// CRU base = MMIO_BASE (0xF8000000) + 0x07760000.
#define CRU_BASE        0xFF760000UL

// PLL register layout (per RK3399 TRM, all 8 PLLs identical):
//   CON0[11:0]   FBDIV
//   CON1[5:0]    REFDIV
//   CON1[10:8]   POSTDIV1
//   CON1[14:12]  POSTDIV2
//   CON2[31]     LOCK status
//   CON3[3]      DSMPD (1 = integer / DSM disabled)
//   CON3[9:8]    MODE  (0 slow, 1 normal, 2 deep slow)
// Cluster B (A72) ABPLL_CON sits at offset 0x20 in CRU.
#define ABPLL_CON(n)    (CRU_BASE + 0x20 + (n) * 4)

// CRU clksel_con[] starts at offset 0x100 inside CRU (per struct rockchip_cru
// layout: 6 PLLs * (6 con + 2 reserved) * 4 = 192 + reserved6[0x0a]*4 = 232... 
// matches TRM 0x100).
#define CLKSEL_CON(n)   (CRU_BASE + 0x100 + (n) * 4)

// PLL bitfield helpers
#define PLL_FBDIV_SHIFT     0u
#define PLL_FBDIV_MASK      0xFFFu
#define PLL_REFDIV_SHIFT    0u
#define PLL_REFDIV_MASK     0x3Fu
#define PLL_POSTDIV1_SHIFT  8u
#define PLL_POSTDIV1_MASK   0x7u
#define PLL_POSTDIV2_SHIFT  12u
#define PLL_POSTDIV2_MASK   0x7u
#define PLL_LOCK_BIT        (1u << 31)
#define PLL_DSMPD_BIT       (1u << 3)
#define PLL_MODE_SHIFT      8u
#define PLL_MODE_MASK       0x3u
#define PLL_MODE_SLOW       0u
#define PLL_MODE_NORM       1u

// CLKSEL_CON[2] = cluster B core clock:
//   [12:8] ACLKM_CORE_B_DIV
//   [7:6]  CLK_CORE_B_PLL_SEL (1 = ABPLL)
//   [4:0]  CLK_CORE_B_DIV     (CPU-clock divider; 0 = no extra divide)
#define CON2_ACLKM_SHIFT    8u
#define CON2_ACLKM_MASK     (0x1Fu << 8)
#define CON2_PLLSEL_SHIFT   6u
#define CON2_PLLSEL_MASK    (0x3u  << 6)
#define CON2_PLLSEL_ABPLL   1u
#define CON2_CORE_SHIFT     0u
#define CON2_CORE_MASK      0x1Fu

// CLKSEL_CON[3] = cluster B debug:
//   [12:8] PCLK_DBG_B_DIV
//   [4:0]  ATCLK_CORE_B_DIV
#define CON3_PCLKDBG_SHIFT  8u
#define CON3_PCLKDBG_MASK   (0x1Fu << 8)
#define CON3_ATCLK_SHIFT    0u
#define CON3_ATCLK_MASK     0x1Fu

// RK3399 CRU registers use write-mask format: high 16 bits enable, low 16
// bits set. Equivalent to U-Boot rk_clrsetreg().
static inline void rk_clrsetreg(uintptr_t addr, u32 mask, u32 val) {
    mmio_write32(addr, (mask << 16) | (val & mask));
}

typedef struct {
    u32 freq_mhz;
    u32 fbdiv;
    u32 refdiv;
    u32 postdiv1;
    u32 postdiv2;
    u32 aclkm_div;     // (freq / aclkm_div+1) <= 300 MHz
    u32 atclk_div;     // (freq / atclk_div+1) <= 300 MHz
    u32 pclk_dbg_div;  // (freq / pclk_dbg_div+1) <= 100 MHz
} a72_opp_t;

// OPP table for 24 MHz XTAL (confirmed by CNTFRQ_EL0=0x16E3600=24000000).
// PLL freq = 24 MHz × fbdiv / (refdiv × postdiv1 × postdiv2).
// VCO must be in [800, 3200] MHz (RK3399 PLL spec).
static const a72_opp_t a72_opps[] = {
    /* MHz   fb ref p1 p2  aclkm atclk pclk_dbg */
    {  600,  25, 1, 1, 1,    1,    1,    5 },  // 24×25=600   300/300/100
    {  816,  34, 1, 1, 1,    2,    2,    7 },  // 24×34=816   272/272/102
    { 1008,  42, 1, 1, 1,    3,    3,    9 },  // 24×42=1008  252/252/100.8
    { 1200,  50, 1, 1, 1,    3,    3,   11 },  // 24×50=1200  300/300/100
    { 1416,  59, 1, 1, 1,    4,    4,   13 },  // 24×59=1416  283/283/101
    { 1608,  67, 1, 1, 1,    5,    5,   16 },  // 24×67=1608  268/268/94.6
    { 1800,  75, 1, 1, 1,    5,    5,   17 },  // 24×75=1800  300/300/100
};

int cru_set_a72_freq_mhz(u32 freq_mhz) {
    g_last_pmic_rk808_id1 = 0xFF;
    g_last_pmic_i2c_status = 0;

    const a72_opp_t* opp = 0;
    for (u32 i = 0; i < sizeof(a72_opps) / sizeof(a72_opps[0]); i++) {
        if (a72_opps[i].freq_mhz == freq_mhz) {
            opp = &a72_opps[i];
            break;
        }
    }
    if (!opp) return -2;

    // If target frequency is >= 1608 MHz, raise VDD_CPU_B voltage to 1.2V
    // before switching PLL. Without this, the A72 core crashes on undershoot.
    if (freq_mhz >= 1608) {
        u32 pmic_id1 = 0xFF;
        u32 pmic_i2c_stat = 0;
        int pmic_ret = pmic_set_vdd_cpu_b(1200000, &pmic_id1, &pmic_i2c_stat);
        g_last_pmic_rk808_id1 = pmic_id1;
        g_last_pmic_i2c_status = pmic_i2c_stat;
        if (pmic_ret) return pmic_ret;
    }
    
    // 1. ABPLL -> slow mode. Cluster B CPU runs from OSC (24 MHz) for the
    //    duration of this function. Code keeps executing, just slower.
    rk_clrsetreg(ABPLL_CON(3),
                 PLL_MODE_MASK << PLL_MODE_SHIFT,
                 PLL_MODE_SLOW << PLL_MODE_SHIFT);
    asm volatile("dmb ish" ::: "memory");

    // 2. Force integer mode (DSMPD=1). Required for non-fractional freqs.
    rk_clrsetreg(ABPLL_CON(3), PLL_DSMPD_BIT, PLL_DSMPD_BIT);
    asm volatile("dmb ish" ::: "memory");
    
    // 3. Update bus dividers BEFORE PLL goes back to normal so that downstream
    //    ACLKM/ATCLK/PCLK_DBG don't briefly exceed their max ratings.
    rk_clrsetreg(CLKSEL_CON(2),
                 CON2_ACLKM_MASK | CON2_PLLSEL_MASK | CON2_CORE_MASK,
                 (opp->aclkm_div   << CON2_ACLKM_SHIFT) |
                 (CON2_PLLSEL_ABPLL << CON2_PLLSEL_SHIFT) |
                 (0u                << CON2_CORE_SHIFT));
    asm volatile("dmb ish" ::: "memory");
    rk_clrsetreg(CLKSEL_CON(3),
                 CON3_PCLKDBG_MASK | CON3_ATCLK_MASK,
                 (opp->pclk_dbg_div << CON3_PCLKDBG_SHIFT) |
                 (opp->atclk_div    << CON3_ATCLK_SHIFT));
    asm volatile("dmb ish" ::: "memory");
    
    // 4. Program ABPLL FBDIV / REFDIV / POSTDIV.
    rk_clrsetreg(ABPLL_CON(0),
                 PLL_FBDIV_MASK << PLL_FBDIV_SHIFT,
                 (opp->fbdiv & PLL_FBDIV_MASK) << PLL_FBDIV_SHIFT);
    asm volatile("dmb ish" ::: "memory");
    rk_clrsetreg(ABPLL_CON(1),
                 (PLL_POSTDIV2_MASK << PLL_POSTDIV2_SHIFT) |
                 (PLL_POSTDIV1_MASK << PLL_POSTDIV1_SHIFT) |
                 (PLL_REFDIV_MASK   << PLL_REFDIV_SHIFT),
                 (opp->postdiv2 << PLL_POSTDIV2_SHIFT) |
                 (opp->postdiv1 << PLL_POSTDIV1_SHIFT) |
                 (opp->refdiv   << PLL_REFDIV_SHIFT));
    asm volatile("dmb ish" ::: "memory");
    
    // 5. Wait for ABPLL_CON2.LOCK = 1. PLL lock typically completes < 200 us.
    u32 timeout = 200000u;
    while (timeout > 0u) {
        if (mmio_read32(ABPLL_CON(2)) & PLL_LOCK_BIT) break;
        for (volatile u32 d = 0u; d < 100u; d++) { /* spin */ }
        timeout--;
    }
    if (!(mmio_read32(ABPLL_CON(2)) & PLL_LOCK_BIT)) {
        // Lock failed - leave PLL in slow mode (24 MHz) as failsafe.
        rk_clrsetreg(ABPLL_CON(3),
                     PLL_MODE_MASK << PLL_MODE_SHIFT,
                     PLL_MODE_SLOW << PLL_MODE_SHIFT);
        return -1;
    }
    asm volatile("dmb ish" ::: "memory");
    
    // 6. Switch ABPLL back to normal -> A72 immediately runs at target freq.
    rk_clrsetreg(ABPLL_CON(3),
                 (PLL_MODE_MASK << PLL_MODE_SHIFT) | PLL_DSMPD_BIT,
                 (PLL_MODE_NORM << PLL_MODE_SHIFT) | PLL_DSMPD_BIT);
    asm volatile("isb" ::: "memory");

    // After a large frequency jump, invalidate I-cache and drain the pipeline
    // so the CPU fetches fresh instructions at the new frequency.
    asm volatile(
        "ic iallu\n"
        "dsb ish\n"
        "isb\n"
        ::: "memory"
    );
    // Brief stall for cache RAM stabilization at new frequency
    for (volatile u32 _stall = 0; _stall < 50; _stall++) {
        asm volatile("nop" ::: "memory");
    }
    asm volatile("dsb ish; isb" ::: "memory");
    
    // 7. Verify mode switch took effect
    u32 con3_final = mmio_read32(ABPLL_CON(3));
    if (((con3_final >> 8) & 0x3) != PLL_MODE_NORM) {
        return -3;  // Locked but still in slow mode
    }

    return 0;
}
