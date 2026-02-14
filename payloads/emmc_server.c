#include "types.h"
#include "t124.h"
#include "emmc.h"
#include "emmc_server.h"

typedef void (*ep1_x_imm_t)(void *buffer, u32 size, u32 *num_xfer);

static inline u32 read32(uintptr_t addr) {
    return *(vu32 *)addr;
}

static inline void write32(uintptr_t addr, u32 val) {
    *(vu32 *)addr = val;
}

static inline void or32(uintptr_t addr, u32 val) {
    write32(addr, read32(addr) | val);
}

static inline void and32(uintptr_t addr, u32 val) {
    write32(addr, read32(addr) & val);
}

void enter_rcm() {
    or32(PMC_BASE + PMC_SCRATCH0, PMC_SCRATCH0_MODE_RCM);
    or32(PMC_BASE + PMC_CNTRL, PMC_CNTRL_MAIN_RST);
}

static void delay(u32 count) {
    for (volatile u32 d = 0; d < count; d++) ;
}

/*
 * SDHCI register access: use native 16-bit/8-bit widths where the SDHCI
 * spec defines sub-word registers. The Linux kernel uses writew/readw for
 * Clock Control (0x2C) and writeb/readb for Software Reset (0x2F).
 * ARM7TDMI supports LDRH/STRH and LDRB/STRB natively.
 *
 * Register map at SDHCI offset 0x2C (32-bit word):
 *   [15:0]  = Clock Control  (16-bit at 0x2C)
 *   [23:16] = Timeout Control (8-bit at 0x2E)
 *   [31:24] = Software Reset  (8-bit at 0x2F)
 */

static inline u16 read16(uintptr_t addr) {
    return *(vu16 *)addr;
}

static inline void write16(uintptr_t addr, u16 val) {
    *(vu16 *)addr = val;
}

static inline u8 read8(uintptr_t addr) {
    return *(vu8 *)addr;
}

static inline void write8(uintptr_t addr, u8 val) {
    *(vu8 *)addr = val;
}

/* Clock Control: 16-bit register at offset 0x2C */
static u16 read_clkctl(void) {
    return read16(SDMMC4_BASE + 0x2C);
}

static void write_clkctl(u16 val) {
    write16(SDMMC4_BASE + 0x2C, val);
}

/* Software Reset: 8-bit register at offset 0x2F */
static void write_swrst(u8 bits) {
    write8(SDMMC4_BASE + 0x2F, bits);
}

static u8 read_swrst(void) {
    return read8(SDMMC4_BASE + 0x2F);
}

/* Host Control: 8-bit at offset 0x28 */
static void write_hostctl(u8 val) {
    write8(SDMMC4_BASE + 0x28, val);
}

/* Power Control: 8-bit at offset 0x29 */
static void write_pwrctl(u8 val) {
    write8(SDMMC4_BASE + 0x29, val);
}

/* Timeout Control: 8-bit at offset 0x2E */
static void write_timeout(u8 val) {
    write8(SDMMC4_BASE + 0x2E, val);
}

static u32 last_cmd_int_status = 0;  /* INT_STATUS captured on last cmd error */

/*
 * Send a command to the eMMC card via SDHCI.
 * cmd_val: 16-bit command register value
 * argument: 32-bit command argument
 * Returns 0 on success, negative on error:
 *   -1 = CMD_INHIBIT timeout (command line busy)
 *   -2 = SDHCI_INT_ERROR (card/controller error)
 *   -3 = CMD_COMPLETE timeout (command sent but no response)
 *   -4 = DAT_INHIBIT timeout (R1b busy signal)
 */
static int send_cmd(u32 cmd_val, u32 argument) {
    u32 status;
    u32 timeout;

    /* Wait for CMD line free */
    timeout = 500000;
    while (read32(SDMMC4_BASE + SDHCI_PRESENT_STATE) & SDHCI_CMD_INHIBIT) {
        if (--timeout == 0) {
            last_cmd_int_status = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
            return -1;
        }
    }

    /* Clear all pending interrupts */
    write32(SDMMC4_BASE + SDHCI_INT_STATUS, 0xFFFFFFFF);

    /* Set argument */
    write32(SDMMC4_BASE + SDHCI_ARGUMENT, argument);

    /* Issue command (32-bit write: command in upper 16, xfer mode=0 in lower 16) */
    write32(SDMMC4_BASE + SDHCI_TRANSFER_MODE, (cmd_val << 16));

    /* Wait for Command Complete (even for no-response commands like CMD0,
     * the SDHCI controller sets CMD_COMPLETE after sending the command) */
    timeout = 500000;
    do {
        status = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
        if (status & SDHCI_INT_ERROR) {
            last_cmd_int_status = status;
            write32(SDMMC4_BASE + SDHCI_INT_STATUS, 0xFFFFFFFF);
            write_swrst(SDHCI_RESET_CMD);
            delay(1000);
            return -2;
        }
        if (status & SDHCI_INT_CMD_COMPLETE) break;
        if (--timeout == 0) {
            last_cmd_int_status = status;
            return -3;
        }
    } while (1);

    /* Clear Command Complete */
    write32(SDMMC4_BASE + SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

    /* For R1b responses, wait for DAT line to become free (busy signal) */
    if ((cmd_val & 0x03) == 0x03) {
        timeout = 1000000;
        while (read32(SDMMC4_BASE + SDHCI_PRESENT_STATE) & SDHCI_DAT_INHIBIT) {
            if (--timeout == 0) return -4;
        }
    }

    return 0;
}

/*
 * MMC command register values (16-bit, for upper half of 0x0C write).
 * Format: (cmd_index << 8) | flags
 * Response: 00=none, 01=136-bit(R2), 02=48-bit(R1/R3), 03=48-bit+busy(R1b)
 */
#define MMC_CMD0    0x0000  /* GO_IDLE_STATE: no response */
#define MMC_CMD1    0x0102  /* SEND_OP_COND: R3, no CRC/index check */
#define MMC_CMD2    0x0209  /* ALL_SEND_CID: R2 (136-bit), CRC check */
#define MMC_CMD3    0x031A  /* SET_RELATIVE_ADDR: R1, CRC+index check */
#define MMC_CMD7    0x071B  /* SELECT_CARD: R1b, CRC+index check */
#define MMC_CMD16   0x101A  /* SET_BLOCKLEN: R1, CRC+index check */

static u32 sdmmc4_initialized = 0;
static u32 init_error = 0;

/* Diagnostic trace: stores CAR/SDHCI state at key init steps */
static u32 diag[28];

/*
 * Perform pad auto-calibration (from IROM reverse engineering at 0x10a788).
 * Calibrates I/O pad impedance for SDMMC4 interface.
 * Must be called after clock is enabled and reset deasserted.
 */
static void sdmmc4_auto_cal(void) {
    u32 timeout;

    /* Set COMP_PADCTRL_E_INPUT (bit 31) - force pads powered for calibration */
    or32(SDMMC4_BASE + SDMMC_SDMEMCOMP_PADCTRL, SDMMC_COMP_PADCTRL_E_INPUT);

    /* Start auto-calibration: set AUTO_CAL_START (bit 31) + AUTO_CAL_ENABLE (bit 29) */
    or32(SDMMC4_BASE + SDMMC_AUTO_CAL_CONFIG,
         SDMMC_AUTO_CAL_START | SDMMC_AUTO_CAL_ENABLE);

    /* Readback for memory fence */
    (void)read32(SDMMC4_BASE + SDMMC_AUTO_CAL_CONFIG);

    /* Poll AUTO_CAL_STATUS bit 31 until clear (calibration complete) */
    timeout = 100000;
    while ((read32(SDMMC4_BASE + SDMMC_AUTO_CAL_STATUS) & SDMMC_AUTO_CAL_ACTIVE)
           && --timeout) {
        delay(1);
    }

    if (!timeout) {
        /* Timeout - disable auto-cal enable */
        and32(SDMMC4_BASE + SDMMC_AUTO_CAL_CONFIG, ~SDMMC_AUTO_CAL_ENABLE);
    }

    /* Clear COMP_PADCTRL_E_INPUT (bit 31) - release forced pad power */
    and32(SDMMC4_BASE + SDMMC_SDMEMCOMP_PADCTRL, ~SDMMC_COMP_PADCTRL_E_INPUT);
}

/*
 * Initialize SDMMC4 controller and eMMC card.
 * Sequence follows Linux kernel sdhci-tegra.c patterns with additions
 * from IROM reverse engineering. All register access is 32-bit only.
 */
static void init_sdmmc4(void) {
    u32 timeout;
    int cmd_ret;

    if (sdmmc4_initialized) return;

    init_error = 0;

    /* Diagnostic: state BEFORE our init */
    diag[0] = read32(CAR_BASE + 0x10);    /* CLK_OUT_ENB_L (bit15=SDMMC4 clk gate) */
    diag[1] = read32(CAR_BASE + 0x04);    /* RST_DEVICES_L (bit15=SDMMC4 reset) */
    diag[2] = read32(CAR_BASE + 0x164);   /* CLK_SOURCE_SDMMC4 */
    diag[3] = read32(PMC_BASE + 0x1C4);   /* IO_DPD2_STATUS before */

    /* === Step 1: Release I/O pads from Deep Power Down === */
    write32(PMC_BASE + 0x1B8, 0x7FFFFFFF);  /* DPD_OFF all DPD1 signals */
    delay(2000);
    write32(PMC_BASE + 0x1C0, 0x7FFFFFFF);  /* DPD_OFF all DPD2 signals */
    delay(5000);
    diag[4] = read32(PMC_BASE + 0x1C4);   /* IO_DPD2_STATUS after release */

    /* === Step 2: Configure SDMMC4 pinmux === */
    /* SDMMC4 pins on T124 at APB_MISC pinmux registers.
     * IROM left these as func=2(RSVD2), tristate=1. Must reconfigure.
     * Pinmux bits: [1:0]=FUNC, [3:2]=PUPD, [4]=TRISTATE, [5]=E_INPUT, [8]=IO_HV
     * CLK: func=0(SDMMC4), pupd=00(none), tri=0, e_input=1, IO_HV=1 → 0x120
     * CMD/DAT: func=0(SDMMC4), pupd=10(pull-up), tri=0, e_input=1, IO_HV=1 → 0x128
     */
    diag[20] = read32(0x70003260);  /* SDMMC4_CLK pinmux BEFORE */
    diag[21] = read32(0x70003270);  /* SDMMC4_DAT2 pinmux BEFORE */

    write32(0x70003260, 0x00000120);  /* SDMMC4_CLK: SDMMC4 func, no pull, e_in, IO_HV */
    write32(0x70003264, 0x00000128);  /* SDMMC4_CMD: SDMMC4 func, pull-up, e_in, IO_HV */
    write32(0x70003268, 0x00000128);  /* SDMMC4_DAT0 */
    write32(0x7000326C, 0x00000128);  /* SDMMC4_DAT1 */
    write32(0x70003270, 0x00000128);  /* SDMMC4_DAT2 */
    write32(0x70003274, 0x00000128);  /* SDMMC4_DAT3 */
    write32(0x70003278, 0x00000128);  /* SDMMC4_DAT4 */
    write32(0x7000327C, 0x00000128);  /* SDMMC4_DAT5 */
    write32(0x70003280, 0x00000128);  /* SDMMC4_DAT6 */
    write32(0x70003284, 0x00000128);  /* SDMMC4_DAT7 */
    (void)read32(0x70003284);         /* readback commit */
    delay(2000);

    diag[22] = read32(0x70003260);  /* SDMMC4_CLK pinmux AFTER */
    diag[23] = read32(0x70003270);  /* SDMMC4_DAT2 pinmux AFTER */

    /* === Step 3: CAR Clock and Reset === */
    write32(CAR_BASE + 0x324, CAR_SDMMC4_BIT);   /* CLK_OUT_ENB_L_CLR */
    (void)read32(CAR_BASE + 0x10);
    write32(CAR_BASE + CAR_RST_DEV_L_SET, CAR_SDMMC4_BIT);
    (void)read32(CAR_BASE + 0x04);

    /* CLK_SOURCE_SDMMC4: Use PLLP_OUT0 (mux index 0, confirmed locked).
     * Previous tests with CLK_M (mux 6) failed. Now pinmux is fixed, retry PLLP.
     * PLLP=408MHz, CAR div N=30 → rate = 408*2/(30+2) = 25.5 MHz module clock.
     * SDHCI div=0x20(32) → card_clk = 25.5/64 = 398 KHz ✓ */
    write32(CAR_BASE + 0x164, 0x0000001E);  /* PLLP, N=30 (25.5 MHz) */
    (void)read32(CAR_BASE + 0x164);          /* readback commit */
    delay(2000);

    write32(CAR_BASE + CAR_CLK_ENB_L_SET, CAR_SDMMC4_BIT);
    (void)read32(CAR_BASE + 0x10);
    delay(10000);
    write32(CAR_BASE + CAR_RST_DEV_L_CLR, CAR_SDMMC4_BIT);
    (void)read32(CAR_BASE + 0x04);
    delay(10000);
    diag[5] = read32(CAR_BASE + 0x10);    /* CLK_OUT_ENB_L after */

    /* === Step 3: SDHCI Full Reset (Linux: sdhci_reset + tegra_sdhci_reset) === */
    write_swrst(SDHCI_RESET_ALL);
    timeout = 100000;
    while ((read_swrst() & SDHCI_RESET_ALL) && --timeout) {
        delay(1);
    }
    diag[6] = read32(SDMMC4_BASE + 0x2C);  /* 0x2C after reset (should be 0) */
    diag[7] = timeout;  /* >0 = reset completed, 0 = timed out */

    /* === Step 4: Configure Vendor Registers (from Linux sdhci-tegra.c) === */
    /* VENDOR_MISC_CTRL: Enable SDHCI Spec 3.0 mode (REQUIRED for T124) */
    or32(SDMMC4_BASE + SDMMC_VENDOR_MISC_CTRL, SDMMC_MISC_CTRL_SPEC_300);
    (void)read32(SDMMC4_BASE + SDMMC_VENDOR_MISC_CTRL);

    /* VENDOR_CLK_CTRL: Set PADPIPE_CLKEN_OVERRIDE, clear SPI_MODE_CLKEN_OVERRIDE */
    {
        u32 clk_ctrl = read32(SDMMC4_BASE + SDMMC_VENDOR_CLK_CTRL);
        clk_ctrl |= SDMMC_CLK_CTRL_PADPIPE;
        clk_ctrl &= ~SDMMC_CLK_CTRL_SPI_MODE;
        write32(SDMMC4_BASE + SDMMC_VENDOR_CLK_CTRL, clk_ctrl);
        (void)read32(SDMMC4_BASE + SDMMC_VENDOR_CLK_CTRL);
    }

    diag[8] = read32(SDMMC4_BASE + SDMMC_VENDOR_MISC_CTRL);
    diag[9] = read32(SDMMC4_BASE + SDMMC_VENDOR_CLK_CTRL);

    /* === Step 4b: Additional vendor config (from Hekate research) === */

    /* iospare: Set bit 19 ("1 cycle delayed cmd_oen") */
    or32(SDMMC4_BASE + 0x1F0, (1u << 19));
    (void)read32(SDMMC4_BASE + 0x1F0);

    /* veniotrimctl: Clear bit 2 ("Band Gap VREG to supply DLL") */
    and32(SDMMC4_BASE + 0x1AC, ~(1u << 2));
    (void)read32(SDMMC4_BASE + 0x1AC);

    /* venclkgatehystcnt: Set max hysteresis to prevent auto clock gating */
    write32(SDMMC4_BASE + 0x1D0, 0x0000FFFF);
    (void)read32(SDMMC4_BASE + 0x1D0);

    /* vendor_sys_sw_ctrl: Try clock gate override bits (speculative, T210-style) */
    or32(SDMMC4_BASE + 0x104, 0x03);
    (void)read32(SDMMC4_BASE + 0x104);

    diag[24] = read32(SDMMC4_BASE + 0x1AC);  /* veniotrimctl after */
    diag[25] = read32(SDMMC4_BASE + 0x1F0);  /* iospare after */
    diag[26] = read32(SDMMC4_BASE + 0x1D0);  /* venclkgatehystcnt after */
    diag[27] = read32(SDMMC4_BASE + 0x104);  /* vendor_sys_sw_ctrl after */

    /* === Step 5: Pad auto-calibration === */
    sdmmc4_auto_cal();
    diag[10] = read32(SDMMC4_BASE + SDMMC_AUTO_CAL_STATUS);

    /* === Step 6: Bus Power FIRST (SDHCI spec 3.2.1: power before clock) === */
    /* T124 quirk: SINGLE_POWER_WRITE - set voltage + enable in one write */
    write_pwrctl(0x0D);  /* SD Bus Voltage: 3.0V (bits[3:1]=110) + Bus Power ON (bit 0) */
    (void)read8(SDMMC4_BASE + 0x29);  /* readback commit */
    delay(5000);

    /* === Step 7: Host Control (1-bit bus width for init) === */
    write_hostctl(0x00);
    (void)read8(SDMMC4_BASE + 0x28);

    /* Timeout control */
    write_timeout(0x0E);
    (void)read8(SDMMC4_BASE + 0x2E);

    /* === Step 8: Clock Setup (using 16-bit writes like Linux kernel) === */
    /* First disable all clocks (clean state) */
    write_clkctl(0x0000);
    (void)read16(SDMMC4_BASE + 0x2C);  /* readback commit */
    delay(1000);

    /* Set SDHCI divider + Internal Clock Enable (16-bit write) */
    /* div=0x20 (Spec3.0: base_clk / 64 = ~187 KHz with 12 MHz CLK_M) */
    write_clkctl(0x2001);
    (void)read16(SDMMC4_BASE + 0x2C);  /* readback commit */

    /* Poll Internal Clock Stable (bit 1) with 100ms hardware timer timeout */
    {
        u32 start = read32(0x60005010);  /* TIMERUS_CNTR_1US */
        u32 stable = 0;
        while ((read32(0x60005010) - start) < 100000) {  /* 100ms */
            if (read_clkctl() & 0x0002) {
                stable = 1;
                break;
            }
        }
        diag[11] = (u32)read_clkctl();     /* Clock Control after poll (16-bit) */
        diag[12] = stable;                  /* 1=stable achieved, 0=timeout */
    }

    /* If stable bit didn't set, continue with extra settling delay */
    if (!(diag[12])) {
        u32 start = read32(0x60005010);
        while ((read32(0x60005010) - start) < 10000) ;  /* extra 10ms */
    }

    /* === Step 9: Enable SD Clock to card (bit 2, 16-bit write) === */
    write_clkctl(read_clkctl() | 0x0004);
    (void)read16(SDMMC4_BASE + 0x2C);  /* readback commit */
    delay(5000);

    diag[13] = read32(SDMMC4_BASE + 0x2C);   /* final 0x2C (clk+timeout+reset) */
    diag[14] = read32(SDMMC4_BASE + 0x28);   /* final host ctrl + power */
    diag[18] = read32(CAR_BASE + 0xA0);       /* PLLP_BASE (PLL config) */
    diag[19] = read32(CAR_BASE + 0x164);      /* CLK_SOURCE_SDMMC4 after our write */

    /* Enable interrupt status bits for polling */
    write32(SDMMC4_BASE + SDHCI_INT_ENABLE, 0x03FF00FF);

    /* === eMMC card initialization === */

    /* CMD0: GO_IDLE_STATE (now waits for CMD_COMPLETE to verify clock works) */
    cmd_ret = send_cmd(MMC_CMD0, 0);
    if (cmd_ret < 0) {
        init_error = 0xE0000001;
        diag[15] = (u32)(-cmd_ret);          /* 1=INHIBIT, 2=ERROR, 3=TIMEOUT */
        diag[16] = last_cmd_int_status;
        diag[17] = read32(SDMMC4_BASE + SDHCI_PRESENT_STATE);
        return;
    }

    /* CMD1: SEND_OP_COND - poll until card ready */
    timeout = 200;
    while (1) {
        cmd_ret = send_cmd(MMC_CMD1, 0x40FF8080);
        if (cmd_ret < 0) {
            init_error = 0xE0000002;
            diag[15] = (u32)(-cmd_ret);
            diag[16] = last_cmd_int_status;
            diag[17] = read32(SDMMC4_BASE + SDHCI_PRESENT_STATE);
            return;
        }
        u32 ocr = read32(SDMMC4_BASE + SDHCI_RESPONSE);
        if (ocr & (1u << 31)) break;
        if (--timeout == 0) { init_error = 0xE0000003; return; }
        delay(1000);
    }

    /* CMD2: ALL_SEND_CID */
    cmd_ret = send_cmd(MMC_CMD2, 0);
    if (cmd_ret < 0) { init_error = 0xE0000004; diag[15] = (u32)(-cmd_ret); diag[16] = last_cmd_int_status; return; }

    /* CMD3: SET_RELATIVE_ADDR (RCA = 1 for eMMC) */
    cmd_ret = send_cmd(MMC_CMD3, 0x00010000);
    if (cmd_ret < 0) { init_error = 0xE0000005; diag[15] = (u32)(-cmd_ret); diag[16] = last_cmd_int_status; return; }

    /* CMD7: SELECT_CARD (RCA = 1) */
    cmd_ret = send_cmd(MMC_CMD7, 0x00010000);
    if (cmd_ret < 0) { init_error = 0xE0000006; diag[15] = (u32)(-cmd_ret); diag[16] = last_cmd_int_status; return; }

    /* CMD16: SET_BLOCKLEN (512 bytes) */
    cmd_ret = send_cmd(MMC_CMD16, 512);
    if (cmd_ret < 0) { init_error = 0xE0000007; diag[15] = (u32)(-cmd_ret); diag[16] = last_cmd_int_status; return; }

    sdmmc4_initialized = 1;
}

/* Wait for CMD and DAT lines to be free */
static int wait_ready(void) {
    u32 timeout = 500000;
    while (read32(SDMMC4_BASE + SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DAT_INHIBIT)) {
        if (--timeout == 0) return -1;
    }
    return 0;
}

/* Reset CMD and DAT lines after error */
static void reset_cmd_dat(void) {
    write_swrst(SDHCI_RESET_CMD | SDHCI_RESET_DAT);
    u32 timeout = 10000;
    while ((read_swrst() & (SDHCI_RESET_CMD | SDHCI_RESET_DAT)) && --timeout) ;
}

/* Read a single 512-byte sector from eMMC */
static int read_emmc_sector(u32 sector, u32 *buffer) {
    u32 status;
    u32 timeout;

    if (wait_ready() < 0) return -1;

    write32(SDMMC4_BASE + SDHCI_INT_STATUS, 0xFFFFFFFF);
    write32(SDMMC4_BASE + SDHCI_BLOCK_SIZE, (1 << 16) | 0x200);
    write32(SDMMC4_BASE + SDHCI_ARGUMENT, sector);
    write32(SDMMC4_BASE + SDHCI_TRANSFER_MODE, ((u32)MMC_CMD17_READ << 16) | XFER_MODE_READ);

    timeout = 500000;
    do {
        status = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
        if (status & SDHCI_INT_ERROR) { reset_cmd_dat(); return -2; }
        if (--timeout == 0) return -3;
    } while (!(status & SDHCI_INT_CMD_COMPLETE));

    write32(SDMMC4_BASE + SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

    timeout = 500000;
    do {
        status = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
        if (status & SDHCI_INT_ERROR) { reset_cmd_dat(); return -4; }
        if (--timeout == 0) return -5;
    } while (!(status & SDHCI_INT_BUF_RD_READY));

    for (u32 i = 0; i < 128; i++) {
        buffer[i] = read32(SDMMC4_BASE + SDHCI_BUFFER);
    }

    timeout = 500000;
    do {
        status = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
        if (status & SDHCI_INT_ERROR) { reset_cmd_dat(); return -6; }
        if (--timeout == 0) return -7;
    } while (!(status & SDHCI_INT_XFER_COMPLETE));

    write32(SDMMC4_BASE + SDHCI_INT_STATUS, 0xFFFFFFFF);
    return 0;
}

/* Write a single 512-byte sector to eMMC */
static int write_emmc_sector(u32 sector, u32 *buffer) {
    u32 status;
    u32 timeout;

    if (wait_ready() < 0) return -1;

    write32(SDMMC4_BASE + SDHCI_INT_STATUS, 0xFFFFFFFF);
    write32(SDMMC4_BASE + SDHCI_BLOCK_SIZE, (1 << 16) | 0x200);
    write32(SDMMC4_BASE + SDHCI_ARGUMENT, sector);
    write32(SDMMC4_BASE + SDHCI_TRANSFER_MODE, ((u32)MMC_CMD24_WRITE << 16) | XFER_MODE_WRITE);

    timeout = 500000;
    do {
        status = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
        if (status & SDHCI_INT_ERROR) { reset_cmd_dat(); return -2; }
        if (--timeout == 0) return -3;
    } while (!(status & SDHCI_INT_CMD_COMPLETE));

    write32(SDMMC4_BASE + SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

    timeout = 500000;
    do {
        status = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
        if (status & SDHCI_INT_ERROR) { reset_cmd_dat(); return -4; }
        if (--timeout == 0) return -5;
    } while (!(status & SDHCI_INT_BUF_WR_READY));

    for (u32 i = 0; i < 128; i++) {
        write32(SDMMC4_BASE + SDHCI_BUFFER, buffer[i]);
    }

    timeout = 500000;
    do {
        status = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
        if (status & SDHCI_INT_ERROR) { reset_cmd_dat(); return -6; }
        if (--timeout == 0) return -7;
    } while (!(status & SDHCI_INT_XFER_COMPLETE));

    write32(SDMMC4_BASE + SDHCI_INT_STATUS, 0xFFFFFFFF);
    return 0;
}

__attribute__((section(".init")))
void entry() {

    u32 num_xfer;
    struct emmc_cmd_s cmd;
    u8 *buffer = (u8*)0x40020000;

    ep1_x_imm_t ep1_out_read_imm = (ep1_x_imm_t)(BOOTROM_EP1_OUT_READ_IMM | 1);
    ep1_x_imm_t ep1_in_write_imm = (ep1_x_imm_t)(BOOTROM_EP1_IN_WRITE_IMM | 1);

    while (1) {
        ep1_out_read_imm(&cmd, sizeof(cmd), &num_xfer);

        if (cmd.op == EMMC_CMD_EXIT) {
            break;
        }

        if (cmd.op == EMMC_CMD_STATUS) {
            u32 regs[128];

            init_sdmmc4();

            regs[0] = 0xCAFE0000;
            regs[1] = init_error;
            regs[2] = sdmmc4_initialized;
            regs[3] = read32(SDMMC4_BASE + SDHCI_PRESENT_STATE);
            regs[4] = read_clkctl();
            regs[5] = read32(SDMMC4_BASE + SDHCI_INT_STATUS);
            regs[6] = read32(SDMMC4_BASE + SDHCI_INT_ENABLE);
            regs[7] = read32(SDMMC4_BASE + SDHCI_CAPABILITIES);
            regs[8] = read32(SDMMC4_BASE + SDHCI_HOST_CONTROL);
            regs[9] = read32(SDMMC4_BASE + SDHCI_RESPONSE);
            regs[10] = read32(SDMMC4_BASE + SDHCI_RESPONSE + 4);
            regs[11] = read32(SDMMC4_BASE + SDHCI_RESPONSE + 8);
            regs[12] = read32(SDMMC4_BASE + SDHCI_RESPONSE + 12);

            /* Init diagnostic trace (diag[0..27]) at regs[16..43] */
            for (u32 d = 0; d < 28; d++) regs[16 + d] = diag[d];

            /* Try reading sector 0 if init succeeded */
            regs[13] = 0xCAFE0001;
            if (sdmmc4_initialized) {
                int r = read_emmc_sector(0, &regs[14]);
                regs[13] = (u32)r;
            }

            ep1_in_write_imm(regs, SDMMC4_REG_SIZE, &num_xfer);
            continue;
        }

        if (cmd.op == EMMC_CMD_READ) {
            init_sdmmc4();
            u32 sector = cmd.start_sector;
            u32 remaining = cmd.num_sectors;

            while (remaining > 0) {
                u32 batch = remaining > EMMC_CHUNK_SECTORS ? EMMC_CHUNK_SECTORS : remaining;
                u32 batch_bytes = batch * EMMC_SECTOR_SIZE;

                for (u32 i = 0; i < batch; i++) {
                    int result = read_emmc_sector(sector + i, (u32*)(buffer + i * EMMC_SECTOR_SIZE));
                    if (result < 0) {
                        u32 *err = (u32*)(buffer + i * EMMC_SECTOR_SIZE);
                        err[0] = 0xDEAD0000 | (u32)((-result) & 0xFFFF);
                        for (u32 j = 1; j < 128; j++) err[j] = 0xDEADDEAD;
                    }
                }

                ep1_in_write_imm(buffer, batch_bytes, &num_xfer);

                sector += batch;
                remaining -= batch;
            }
            continue;
        }

        if (cmd.op == EMMC_CMD_WRITE) {
            init_sdmmc4();
            u32 sector = cmd.start_sector;
            u32 remaining = cmd.num_sectors;
            u32 write_result = 0;

            while (remaining > 0) {
                u32 batch = remaining > EMMC_CHUNK_SECTORS ? EMMC_CHUNK_SECTORS : remaining;
                u32 batch_bytes = batch * EMMC_SECTOR_SIZE;

                ep1_out_read_imm(buffer, batch_bytes, &num_xfer);

                if (write_result == 0) {
                    for (u32 i = 0; i < batch; i++) {
                        int result = write_emmc_sector(sector + i, (u32*)(buffer + i * EMMC_SECTOR_SIZE));
                        if (result < 0) {
                            write_result = 0xDEAD0000 | (u32)((-result) & 0xFFFF);
                            break;
                        }
                    }
                }

                sector += batch;
                remaining -= batch;
            }

            ep1_in_write_imm(&write_result, 4, &num_xfer);
            continue;
        }
    }

    enter_rcm();
}
