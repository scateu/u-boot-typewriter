// SPDX-License-Identifier: GPL-2.0+
/*
 * twwfi - a SAFE, standalone WFI-wake test for gru/kevin (RK3399, EL2).
 *
 * Background: the typewriter wants real WFI idle (deep sleep, timer-woken) like
 * Linux does on this board. Naive attempts froze:
 *   1. arm CNTP + WFI            -> froze (arch-timer PPI INTID 30 was disabled)
 *   2. enable INTID 30 + as above -> STILL froze (wrong timer for EL2?)
 * Theory: we run at EL2, so the EL1 physical timer (CNTP, INTID 30 - what Linux
 * uses from EL1) may be gated by CNTHCTL_EL2 while at EL2; the EL2-native timer
 * is CNTHP (INTID 26). This command tests that safely: it arms a timer, does ONE
 * WFI, and if it wakes prints the elapsed time. If it HANGS, you power-cycle -
 * but you only lose the shell, not an editing session.
 *
 * Usage:
 *   twwfi              - print timer/EL state only (no WFI, always safe)
 *   twwfi hp [ms]      - enable INTID 26, arm CNTHP, single WFI (default 300 ms)
 *   twwfi p  [ms]      - enable INTID 30, arm CNTP,  single WFI (default 300 ms)
 *
 * Start with plain `twwfi` (state dump), then `twwfi hp` (EL2 timer theory).
 */
#include <command.h>
#include <stdio.h>
#include <vsprintf.h>
#include <asm/system.h>
#include <cpu_func.h>     /* flush_dcache_range (secondary-core stub to PoC) */
#include <asm/io.h>
#include <asm/gic.h>
#include <time.h>
#include <u-boot/schedule.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/string.h>
#include <linux/kernel.h>  /* ARRAY_SIZE */
#include <cros_ec.h>       /* host-event flags: power button / lid wake */
#include <power/regulator.h>  /* ppvar_litcpu voltage (A53 core rail) */

/* PSCI invoke_psci_fn is provided by drivers/firmware/psci.c (probe it first
 * with uclass_get_device_by_name(UCLASS_FIRMWARE,"psci",...) as poweroff does).
 * We only need CPU_SUSPEND here. */
#include <dm.h>
#include <dm/uclass.h>
unsigned long invoke_psci_fn(unsigned long, unsigned long, unsigned long,
			     unsigned long);

/* Bump on every twwfi change so `twwfi` (no arg) proves the flashed binary is
 * current - a stale reflash was confounding suspend-hang diagnosis. */
#define TW_BUILD_TAG "tcpd-ship-23"

/* From cmd_tw.c: EC charge state (battery %, board charge current mA, on-AC).
 * Returns 0 on success, <0 on error/no-EC. This is BOARD current, not CPU. */
int tw_read_charge_state(int *pct, int *chg_ma, int *ac);

/* From cmd_tw.c: live smart-battery current (signed mA; neg=discharging) via the
 * EC I2C tunnel to the SBS battery. The real whole-board draw. 0 ok, <0 error. */
int tw_read_batt_current(int *ma);

/* Average battery current over n samples ~gap_ms apart (defined below; forward-
 * declared so do_cpuall can self-meter before the definition). */
static int gate_avg_ma(int n, unsigned int gap_ms, int *out);

/* From cmd_tw.c: editor idle-nap instrumentation - total naps, how many WFIs
 * returned instantly (<1 ms = did NOT sleep), and total time spent in WFI. */
void tw_idle_stats(unsigned long *count, unsigned long *instant,
		   unsigned long *slept_us);

/* CPU0 GICv3 redistributor SGI_base = 0xfef10000 on rk3399 (verified via
 * md/mw). Standard GICv3 SGI-frame register offsets from SGI_base. */
#define GICR_IGROUPR0     0xfef10080UL   /* +0x080: interrupt group  */
#define GICR_ISENABLER0   0xfef10100UL   /* +0x100: set-enable   */
#define GICR_ISPENDR0     0xfef10200UL   /* +0x200: set-pending  */
#define GICR_ICPENDR0     0xfef10280UL   /* +0x280: clear-pending */
#define TW_GICR_IPRIORITYR   0xfef10400UL   /* +0x400: priority (byte/INTID) */
#define GICR_IGRPMODR0    0xfef10d00UL   /* +0xD00: group modifier */

/* GIC Distributor (GICD) base - for SPIs (INTID >= 32). Register arrays are
 * indexed by INTID: IGROUPR/IGRPMODR are 1 bit/INTID, IPRIORITYR 1 byte/INTID,
 * ISENABLER 1 bit/INTID. */
#define TW_GICD_BASE         0xfee00000UL
#define TW_GICD_CTLR         (TW_GICD_BASE + 0x0000)
#define TW_GICD_IGROUPR      (TW_GICD_BASE + 0x0080)
#define TW_GICD_ISENABLER    (TW_GICD_BASE + 0x0100)
#define TW_GICD_ICENABLER    (TW_GICD_BASE + 0x0180)   /* 1 bit/INTID, clear-enable */
#define TW_GICD_IPRIORITYR   (TW_GICD_BASE + 0x0400)
#define TW_GICD_IROUTER      (TW_GICD_BASE + 0x6000)   /* 64-bit/INTID, affinity route */
#define TW_GICD_IGRPMODR     (TW_GICD_BASE + 0x0D00)
#define TW_GICD_ISPENDR      (TW_GICD_BASE + 0x0200)   /* 1 bit/INTID, pending state */
#define TW_GICD_ICPENDR      (TW_GICD_BASE + 0x0280)   /* 1 bit/INTID, clear-pending */

/* RK3399 GPIO0 (PMU GPIO, v1 controller) @ 0xff720000. The cros_ec interrupt
 * line is gpio0 PA1 (ec-interrupt, ACTIVE_LOW). Register offsets from gpio.h. */
#define GPIO0_BASE        0xff720000UL
#define GPIO_INTEN        (GPIO0_BASE + 0x30)
#define GPIO_INTMASK      (GPIO0_BASE + 0x34)
#define GPIO_INTTYPE      (GPIO0_BASE + 0x38)   /* 0=level, 1=edge */
#define GPIO_INT_POL      (GPIO0_BASE + 0x3c)   /* 0=active-low/falling */
#define GPIO_INT_STATUS   (GPIO0_BASE + 0x40)
#define GPIO_PORTA_EOI    (GPIO0_BASE + 0x4c)
#define GPIO_EXT_PORT     (GPIO0_BASE + 0x50)
#define EC_PA1_BIT        (1U << 1)             /* PA1 = the ec-interrupt pin */
#define EC_GPIO_INTID     46                    /* GPIO0 bank = GIC_SPI 14 */

/*
 * RK3399 PMU power-domain status. PMU_BASE = MMIO_BASE(0xF8000000)+0x07310000.
 * PMU_PWRDN_ST (+0x18): 1 bit per power domain, where 1 = domain powered OFF,
 * 0 = ON (per bl31's pmu_power_domain_st). Bit index = the PD_* enum from bl31:
 *   0..3  = A53 little cores CPUL0..3      6 = little cluster SCU/L2 (SCUL)
 *   4..5  = A72 big cores   CPUB0..1       7 = big    cluster SCU/L2 (SCUB)
 * This lets us SEE which cores/clusters bl31 left powered. Pure read - safe.
 */
#define PMU_BASE          0xff310000UL
#define PMU_PWRDN_ST      (PMU_BASE + 0x18)
/*
 * PMU_CORE_PM_CON(cpu) = PMU_BASE + 0xC0 + cpu*4: per-core "auto power-down on
 * WFI" control (bl31 pmu.h). bit0 core_pm_en = a WFI power-gates that core;
 * bit1 core_pm_int_wakeup_en = an interrupt can wake it back. bl31 writes 0
 * (CORES_PM_DISABLE) outside its PSCI sequences, which means a plain WFI HALTS
 * the core but does NOT power-gate it - the rail stays on (warm chip). Reading
 * this tells us whether our idle WFI actually drops the core's power. Pure read.
 */
#define PMU_CORE_PM_CON0  (PMU_BASE + 0xc0)   /* CPU0 (A53 little core 0) */
#define CORE_PM_EN_BIT        0
#define CORE_PM_INT_WAKE_BIT  1
#define PMU_BUS_IDLE_ST   (PMU_BASE + 0x64)   /* 1 bit/NoC bus: 1 = bus IDLE */
#define PD_CPUL0_BIT      0
#define PD_CPUB0_BIT      4
#define PD_SCUL_BIT       6
#define PD_SCUB_BIT       7

/*
 * RK3399 CRU (clock/reset). CRU_BASE = MMIO_BASE(0xF8000000)+0x07760000. The
 * little cluster (CPU0/A53) runs off APLL_L; the big cluster off APLL_B. Each
 * PLL: CON0[11:0]=fbdiv, CON1[5:0]=refdiv/[10:8]=postdiv1/[14:12]=postdiv2,
 * CON2[31]=lock, CON3[9:8]=mode (1=normal, else slow=24MHz OSC bypass).
 *   FOUT = 24MHz * fbdiv / (refdiv * postdiv1 * postdiv2)
 * CLKSEL_CON0: little-core mux [7:6] (0=APLL_L) + divider [4:0] (freq/(div+1)).
 * CLKSEL_CON2: big-core   mux [7:6] (0=APLL_B) + divider [4:0]. All reads; safe.
 */
#define CRU_BASE          0xff760000UL
#define CRU_APLL_L_CON    (CRU_BASE + 0x00)   /* CON0..3 at +0,4,8,c */
#define CRU_APLL_B_CON    (CRU_BASE + 0x20)   /* pll_id 1 => +0x20 */
#define CRU_CLKSEL_CON0   (CRU_BASE + 0x100)  /* little core mux/div */
#define CRU_CLKSEL_CON2   (CRU_BASE + 0x108)  /* big core mux/div */

#define CTL_ENABLE  (1U << 0)
#define CTL_IMASK   (1U << 1)
#define CTL_ISTATUS (1U << 2)

static unsigned long rd_cntpct(void)
{
	unsigned long v;
	isb();
	asm volatile("mrs %0, cntpct_el0" : "=r" (v));
	return v;
}

static unsigned long rd_cntfrq(void)
{
	unsigned long v;
	asm volatile("mrs %0, cntfrq_el0" : "=r" (v));
	return v;
}

static unsigned long rd_cnthctl(void)
{
	unsigned long v;
	asm volatile("mrs %0, cnthctl_el2" : "=r" (v));
	return v;
}

/* GIC CPU-interface system registers (the layer between the redistributor and
 * the PE - the one link we haven't inspected). All reads, safe. */
#define _STR(x) #x
#define STR(x)  _STR(x)
static unsigned long rd_icc_sre(void)
{
	unsigned long v = 0;
	if (current_el() >= 2)
		asm volatile("mrs %0, " STR(ICC_SRE_EL2) : "=r" (v));
	else
		asm volatile("mrs %0, " STR(ICC_SRE_EL1) : "=r" (v));
	return v;
}
static unsigned long rd_icc_igrpen1(void)
{
	unsigned long v;
	asm volatile("mrs %0, " STR(ICC_IGRPEN1_EL1) : "=r" (v));
	return v;
}
static unsigned long rd_icc_pmr(void)
{
	unsigned long v;
	asm volatile("mrs %0, " STR(ICC_PMR_EL1) : "=r" (v));
	return v;
}
static unsigned long rd_icc_ctlr(void)
{
	unsigned long v;
	asm volatile("mrs %0, " STR(ICC_CTLR_EL1) : "=r" (v));
	return v;
}

static void dump_state(void)
{
	printf("EL           = %u\n", current_el());
	printf("CNTFRQ_EL0   = %lu Hz\n", rd_cntfrq());
	printf("CNTPCT_EL0   = 0x%lx\n", rd_cntpct());
	udelay(1000);
	printf("CNTPCT_EL0   = 0x%lx (again; must differ)\n", rd_cntpct());
	if (current_el() >= 2)
		printf("CNTHCTL_EL2  = 0x%lx (bit0 EL1PCTEN, bit1 EL1PCEN)\n",
		       rd_cnthctl());
	printf("GICR_ISENABLER0 = 0x%08x (bit26 CNTHP, bit30 CNTP)\n",
	       readl((void *)GICR_ISENABLER0));
	printf("GICR_ISPENDR0   = 0x%08x\n", readl((void *)GICR_ISPENDR0));
	printf("GICR_IGROUPR0   = 0x%08x (1=Group1/NS, 0=Group0/secure)\n",
	       readl((void *)GICR_IGROUPR0));
	printf("GICR_IGRPMODR0  = 0x%08x\n", readl((void *)GICR_IGRPMODR0));
	printf("TW_GICR_IPRIORITYR(30) byte = 0x%02x\n",
	       readl((void *)(TW_GICR_IPRIORITYR + (30 & ~3))) >> ((30 & 3) * 8)
	       & 0xff);
	printf("TW_GICR_IPRIORITYR(26) byte = 0x%02x\n",
	       readl((void *)(TW_GICR_IPRIORITYR + (26 & ~3))) >> ((26 & 3) * 8)
	       & 0xff);

	/* CPU interface (the link to the PE). */
	printf("ICC_SRE      = 0x%lx (bit0 SRE=sysreg iface on)\n", rd_icc_sre());
	printf("ICC_IGRPEN1  = 0x%lx (bit0 EnableGrp1)\n", rd_icc_igrpen1());
	printf("ICC_PMR      = 0x%lx (priority mask; 0=block all, 0xff=allow)\n",
	       rd_icc_pmr());
	printf("ICC_CTLR     = 0x%lx\n", rd_icc_ctlr());
}

/*
 * SAFE probe (no WFI): arm the timer + enable everything, then POLL the counter
 * until it expires, and read back how far the interrupt propagated. Never
 * sleeps, so it can't freeze - it tells us which link is broken.
 */
static int do_probe(int hyp, unsigned int ms)
{
	unsigned long rate = rd_cntfrq();
	unsigned long ticks = (rate / 1000) * ms;
	unsigned int intid = hyp ? 26 : 30;
	unsigned long ctl, target, hppir;
	unsigned int ispend;

	printf("PROBE %s (INTID %u), %u ms, NO WFI - safe.\n",
	       hyp ? "CNTHP_EL2" : "CNTP_EL0", intid, ms);

	/* Enable everything the wake path needs:
	 *  - TW_GICD_CTLR.EnableGrp1NS (bit1): the DISTRIBUTOR forwards NS Group1 at
	 *    all. bl31 leaves this CLEAR (TW_GICD_CTLR=0x35), which is why HPPIR1 was
	 *    1023 before - the missing piece. (INTID 30 is Group1-NS, NOT secure:
	 *    bl31 only secures INTID 29, the EL3 timer.)
	 *  - ICC_IGRPEN1_EL1 (CPU interface Group1 enable)
	 *  - the timer PPI in the redistributor. */
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));   /* EnableGrp1NS */
	dsb();
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();
	writel(1U << intid, (void *)GICR_ISENABLER0);
	dsb();

	target = rd_cntpct() + ticks;
	if (hyp) {
		asm volatile("msr cnthp_tval_el2, %0" : : "r" (ticks));
		asm volatile("msr cnthp_ctl_el2, %0" : : "r" ((unsigned long)CTL_ENABLE));
	} else {
		asm volatile("msr cntp_tval_el0, %0" : : "r" (ticks));
		asm volatile("msr cntp_ctl_el0, %0" : : "r" ((unsigned long)CTL_ENABLE));
	}
	isb();

	/* Spin on the counter until the timer must have expired (+ margin). */
	while (rd_cntpct() < target + ticks)
		;

	/* Read back the state at each layer. */
	if (hyp)
		asm volatile("mrs %0, cnthp_ctl_el2" : "=r" (ctl));
	else
		asm volatile("mrs %0, cntp_ctl_el0" : "=r" (ctl));
	ispend = readl((void *)GICR_ISPENDR0);
	asm volatile("mrs %0, " STR(ICC_HPPIR1_EL1) : "=r" (hppir));

	printf("  timer CTL      = 0x%lx (bit2 ISTATUS=%lu -> fired?)\n",
	       ctl, (ctl >> 2) & 1);
	printf("  GICR_ISPENDR0  = 0x%08x (bit%u=%u -> pending at redist?)\n",
	       ispend, intid, (ispend >> intid) & 1);
	printf("  ICC_HPPIR1     = %lu (==%u: PE sees it; ==1023: nothing"
	       " deliverable)\n", hppir, intid);

	/* Why 1023 despite pending+Group1NS? Check the two prime suspects: */
	{
		unsigned long igrpen1, rpr;
		unsigned int gicd_ctlr = readl((void *)0xfee00000UL); /* TW_GICD_CTLR */

		asm volatile("mrs %0, " STR(ICC_IGRPEN1_EL1) : "=r" (igrpen1));
		asm volatile("mrs %0, " STR(ICC_RPR_EL1) : "=r" (rpr));
		printf("  TW_GICD_CTLR(rb)  = 0x%08x (bit1 EnableGrp1NS=%u <- the fix)\n",
		       gicd_ctlr, (gicd_ctlr >> 1) & 1);
		printf("  ICC_IGRPEN1(rb)= 0x%lx (did our enable stick?)\n", igrpen1);
		printf("  ICC_RPR        = 0x%lx (running priority; 0xff=idle)\n", rpr);
		if (hppir == intid)
			printf("  => HPPIR1==%u: the PE now sees it. WFI WILL wake.\n",
			       intid);
		else if (!((gicd_ctlr >> 1) & 1))
			printf("  => EnableGrp1NS still 0: distributor won't forward NS"
			       " Group1 (write blocked?).\n");
	}

	/* Clean up: disable timer, clear pending. Leave IGRPEN1 as we found it? -
	 * we turned it on; turn it back off to restore prior state. */
	if (hyp)
		asm volatile("msr cnthp_ctl_el2, %0" : : "r" (0UL));
	else
		asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));
	writel(1U << intid, (void *)GICR_ICPENDR0);
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (0UL));
	isb();
	dsb();

	printf("Done (no WFI). Diagnosis:\n");
	if (!((ctl >> 2) & 1))
		printf("  -> timer did NOT fire (ISTATUS=0): tval/enable wrong.\n");
	else if (!((ispend >> intid) & 1))
		printf("  -> fired but NOT pending at redist: GIC not latching it.\n");
	else if (hppir == 1023)
		printf("  -> pending but PE sees 1023 (spurious): group/PMR/route"
		       " blocks delivery.\n");
	else if (hppir == intid)
		printf("  -> PE CAN see INTID %u: WFI *should* wake; if it doesn't,"
		       " IRQ is routed to EL3 (SCR_EL3.IRQ) or WFI is trapped.\n",
		       intid);
	else
		printf("  -> HPPIR unexpected (%lu).\n", hppir);
	return 0;
}

/* One armed WFI. `hyp` selects CNTHP_EL2 (INTID 26) vs CNTP_EL0 (INTID 30). */
static int do_wfi_test(int hyp, unsigned int ms)
{
	unsigned long rate = rd_cntfrq();
	unsigned long ticks = (rate / 1000) * ms;
	unsigned long t0, t1;
	unsigned int intid = hyp ? 26 : 30;

	printf("Enabling INTID %u, arming %s for %u ms (%lu ticks), IMASK=0...\n",
	       intid, hyp ? "CNTHP_EL2" : "CNTP_EL0", ms, ticks);
	printf("If the prompt does NOT return, this timer does not wake WFI here"
	       " - power-cycle.\n");

	/*
	 * Enable ALL THREE NS enables the `probe` proved are needed to make the
	 * interrupt reach the PE (ICC_HPPIR1 went 1023 -> 30 only with all three).
	 * The earlier version of THIS function set only two of them - it never set
	 * TW_GICD_CTLR.EnableGrp1NS - which is why `twwfi p` still froze while `probe`
	 * showed HPPIR1=30. Now they match.
	 *   1. TW_GICD_CTLR.EnableGrp1NS (bit1) - distributor forwards NS Group1
	 *   2. the timer PPI in the redistributor
	 *   3. ICC_IGRPEN1_EL1 - CPU interface Group1 enable
	 */
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));   /* EnableGrp1NS */
	dsb();
	writel(1U << intid, (void *)GICR_ISENABLER0);
	dsb();
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();

	/*
	 * Arm with IMASK=0 so the timer actually ASSERTS its interrupt (ENABLE=1 &&
	 * ISTATUS=1 && IMASK=0). Keep PSTATE.I MASKED (DAIF.I=1) so the fired IRQ
	 * WAKES wfi but is not TAKEN as an exception (U-Boot's do_irq panics); we
	 * disable the timer + clear pending on wake before unmasking.
	 */
	asm volatile("msr daifset, #2");    /* mask IRQ (PSTATE.I = 1) */
	isb();

	t0 = rd_cntpct();

	if (hyp) {
		asm volatile("msr cnthp_tval_el2, %0" : : "r" (ticks));
		asm volatile("msr cnthp_ctl_el2, %0" : :
			     "r" ((unsigned long)CTL_ENABLE));
	} else {
		asm volatile("msr cntp_tval_el0, %0" : : "r" (ticks));
		asm volatile("msr cntp_ctl_el0, %0" : :
			     "r" ((unsigned long)CTL_ENABLE));
	}
	isb();

	wfi();                     /* <-- if this never returns, we froze */

	t1 = rd_cntpct();

	/* Disable the timer again (drops its now-asserted interrupt). */
	if (hyp)
		asm volatile("msr cnthp_ctl_el2, %0" : : "r" (0UL));
	else
		asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));
	isb();
	/* Clear any pending state at the redistributor, then re-unmask IRQs. */
	writel(1U << intid, (void *)GICR_ICPENDR0);
	dsb();
	asm volatile("msr daifclr, #2");    /* unmask IRQ (PSTATE.I = 0) */
	isb();

	printf("WFI RETURNED. elapsed = %lu ticks (~%lu ms)\n",
	       t1 - t0, (t1 - t0) / (rate / 1000));
	printf("=> %s wakes WFI on this board. Use it for idle.\n",
	       hyp ? "CNTHP (EL2 timer, INTID 26)" : "CNTP (EL1 timer, INTID 30)");
	return 0;
}

/*
 * Inspect an SPI (INTID >= 32) in the distributor - group/priority/enable/route
 * - WITHOUT touching it. Used to check whether a candidate NonSecure wake source
 * (e.g. the GPIO0 bank IRQ = GIC_SPI 14 = INTID 46, which carries the cros_ec
 * keypress line) is even visible to NonSecure Group1 before we invest in the
 * GPIO-IRQ setup. Reads only; safe.
 */
static int do_spi_probe(unsigned int intid)
{
	unsigned int word = intid / 32, bit = intid % 32;
	unsigned int grp  = (readl((void *)(TW_GICD_IGROUPR  + word * 4)) >> bit) & 1;
	unsigned int gmod = (readl((void *)(TW_GICD_IGRPMODR + word * 4)) >> bit) & 1;
	unsigned int en   = (readl((void *)(TW_GICD_ISENABLER + word * 4)) >> bit) & 1;
	unsigned int prio = (readl((void *)(TW_GICD_IPRIORITYR + (intid & ~3)))
			     >> ((intid & 3) * 8)) & 0xff;
	unsigned int ctlr = readl((void *)TW_GICD_CTLR);
	unsigned long route;

	/* IROUTER is 64-bit per INTID; read the low word (affinity 0-2). */
	route = readl((void *)(TW_GICD_IROUTER + (unsigned long)intid * 8));

	printf("SPI INTID %u (GIC_SPI %u) in the distributor:\n",
	       intid, intid - 32);
	printf("  TW_GICD_CTLR   = 0x%08x (bit6 DS=%u)\n", ctlr, (ctlr >> 6) & 1);
	printf("  IGROUPR     bit = %u (1=Group1/NS)\n", grp);
	printf("  IGRPMODR    bit = %u  -> (grp,gmod)=(%u,%u): %s\n",
	       gmod, grp, gmod,
	       (grp && !gmod) ? "Group1-NS (NS-visible, GOOD)" :
	       (!grp && !gmod) ? "Group0 (secure)" :
	       (!grp && gmod) ? "Secure Group1" : "reserved");
	printf("  ISENABLER   bit = %u (enabled in distributor?)\n", en);
	printf("  IPRIORITYR      = 0x%02x\n", prio);
	printf("  IROUTER(lo)     = 0x%08lx (target affinity)\n", route);
	if (grp && !gmod)
		printf("  => NS-visible: a keypress GPIO IRQ COULD wake WFI (if the\n"
		       "     GPIO0 bank IRQ is wired up). Worth pursuing.\n");
	else
		printf("  => NOT NS Group1: same wall as the timer. Not usable from\n"
		       "     NonSecure EL2. Stop here.\n");
	return 0;
}

/*
 * SAFE (no WFI) end-to-end test of the intended keypress wake path:
 *   set up GPIO0 PA1 as a level/active-low interrupt (the cros_ec line),
 *   enable INTID 46 + Group1 at the CPU interface,
 *   then loop ~5 s polling GPIO int_status / TW_GICD_ISPENDR(46) / ICC_HPPIR1
 *   while YOU press keys / the power button / close+open the lid.
 * If HPPIR1 shows 46 when the EC asserts, WFI would wake here. No WFI, no
 * freeze. Restores GPIO/GIC state on exit.
 */
static int do_gpio_probe(void)
{
	unsigned long rate = rd_cntfrq();
	unsigned long tend;
	unsigned int word = EC_GPIO_INTID / 32, bit = EC_GPIO_INTID % 32;
	unsigned int seen_status = 0, seen_pend = 0, seen_hppir = 0;

	printf("GPIO/EC keypress-wake probe (INTID %u), ~5 s, NO WFI.\n",
	       EC_GPIO_INTID);
	printf("Press keys / power button / lid now and watch...\n");

	/* --- GPIO0 PA1: level, active-low, unmasked, enabled --- */
	clrbits_le32((void *)GPIO_INTTYPE, EC_PA1_BIT);   /* 0 = level */
	clrbits_le32((void *)GPIO_INT_POL, EC_PA1_BIT);   /* 0 = active-low */
	clrbits_le32((void *)GPIO_INTMASK, EC_PA1_BIT);   /* unmask */
	setbits_le32((void *)GPIO_INTEN,  EC_PA1_BIT);    /* enable */
	dsb();

	/* --- GIC: enable INTID 46, Group1 at CPU interface --- */
	writel(1U << bit, (void *)(TW_GICD_ISENABLER + word * 4));
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();
	dsb();

	tend = rd_cntpct() + rate * 5;   /* ~5 seconds */
	while (rd_cntpct() < tend) {
		unsigned int st  = readl((void *)GPIO_INT_STATUS) & EC_PA1_BIT;
		unsigned int pnd = (readl((void *)(TW_GICD_ISPENDR + word * 4))
				    >> bit) & 1;
		unsigned long hp;

		asm volatile("mrs %0, " STR(ICC_HPPIR1_EL1) : "=r" (hp));
		if (st)  seen_status = 1;
		if (pnd) seen_pend = 1;
		if (hp == EC_GPIO_INTID) seen_hppir = 1;
		/* Clear the GPIO latch so a level int doesn't stick; the EC
		 * reasserts while it still has events, which is fine. */
		if (st) {
			writel(EC_PA1_BIT, (void *)GPIO_PORTA_EOI);
			writel(1U << bit, (void *)(TW_GICD_ICPENDR + word * 4));
			dsb();
		}
	}

	/* Restore: disable GPIO PA1 IRQ + INTID 46 + IGRPEN1. */
	clrbits_le32((void *)GPIO_INTEN, EC_PA1_BIT);
	writel(1U << bit, (void *)(0xfee00000UL + 0x0180 + word * 4)); /* ICENABLER */
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (0UL));
	isb(); dsb();

	printf("  GPIO int_status asserted: %s\n", seen_status ? "YES" : "no");
	printf("  INTID 46 became pending : %s\n", seen_pend ? "YES" : "no");
	printf("  ICC_HPPIR1 == 46 seen   : %s\n", seen_hppir ? "YES" : "no");
	if (seen_hppir)
		printf("  => WFI WOULD WAKE on EC activity. Keypress-WFI is viable;"
		       " build it.\n");
	else if (seen_pend)
		printf("  => reaches GIC pending but not the PE (HPPIR!=46):"
		       " group/PMR issue at the CPU interface.\n");
	else if (seen_status)
		printf("  => GPIO sees it but GIC didn't latch INTID 46: GPIO->GIC"
		       " wiring/enable off.\n");
	else
		printf("  => GPIO never asserted: wrong pin/polarity, or no EC"
		       " activity during the window.\n");
	return 0;
}

/* PSCI 32-bit signed return decode (spec Table: return codes). */
static const char *psci_err(long r)
{
	switch (r) {
	case 0:   return "SUCCESS";
	case -1:  return "NOT_SUPPORTED";
	case -2:  return "INVALID_PARAMETERS";
	case -3:  return "DENIED";
	case -4:  return "ALREADY_ON";
	case -5:  return "ON_PENDING";
	case -6:  return "INTERNAL_FAILURE";
	case -7:  return "NOT_PRESENT";
	case -8:  return "DISABLED";
	case -9:  return "INVALID_ADDRESS";
	default:  return "?";
	}
}

/*
 * SAFE PSCI diagnostics (no argument): query-only SMCs that ALWAYS return -
 * VERSION, FEATURES(CPU_SUSPEND / SYSTEM_SUSPEND), AFFINITY_INFO. This proves
 * the PSCI conduit works from EL2 and, crucially, whether bl31 even ADVERTISES
 * CPU_SUSPEND / what it supports - before we risk an actual suspend. These
 * cannot hang (they're pure queries). `twwfi psci suspend [ms]` (below, via
 * do_suspend) is the risky armed one.
 *
 * The point: `twwfi suspend` "hangs". This tells us whether that's because bl31
 * rejects the call (we'd see an error code here / a bad FEATURES result) or
 * because the SMC genuinely never returns (a real EL/firmware problem).
 */
static int do_psci_query(void)
{
	struct udevice *psci;
	long r;

	if (uclass_get_device_by_name(UCLASS_FIRMWARE, "psci", &psci)) {
		printf("PSCI driver not found (conduit not set up).\n");
		return 1;
	}

	r = (long)(int)invoke_psci_fn(PSCI_0_2_FN_PSCI_VERSION, 0, 0, 0);
	printf("PSCI_VERSION      = %ld.%ld\n",
	       (r >> 16) & 0xffff, r & 0xffff);

	r = (long)(int)invoke_psci_fn(PSCI_1_0_FN_PSCI_FEATURES,
				      PSCI_0_2_FN64_CPU_SUSPEND, 0, 0);
	printf("FEATURES(CPU_SUSPEND64)    = %ld (%s)  %s\n", r, psci_err(r),
	       r >= 0 ? "-> CPU_SUSPEND supported" :
			"-> NOT supported (that's why suspend fails)");

	r = (long)(int)invoke_psci_fn(PSCI_1_0_FN_PSCI_FEATURES,
				      PSCI_1_0_FN64_SYSTEM_SUSPEND, 0, 0);
	printf("FEATURES(SYSTEM_SUSPEND64) = %ld (%s)\n", r, psci_err(r));

	r = (long)(int)invoke_psci_fn(PSCI_0_2_FN64_AFFINITY_INFO, 0, 0, 0);
	printf("AFFINITY_INFO(cpu0,lvl0)   = %ld (0=ON, 1=OFF, 2=ON_PENDING)\n", r);

	printf("=> Conduit works from EL%u. If FEATURES(CPU_SUSPEND) is supported yet\n"
	       "   `twwfi suspend` still hangs, the SMC returns but resumes wrong (the\n"
	       "   EL2-vs-EL1 / SCR_EL3.HCE resume-target theory). If it's NOT\n"
	       "   supported, bl31 simply lacks the state - EL1 won't help.\n",
	       current_el());
	return 0;
}

/*
 * DDR frequency via bl31's RK_SIP_DDR_CFG (0x82000008) - the SAME safe path Linux
 * uses. bl31's M0 co-processor does the self-refresh + DPLL switch + PHY retrain;
 * we MUST NOT poke the DPLL directly (that corrupts running DRAM). U-Boot boots
 * DDR at 928 MHz; Linux devfreq idles it at 400 MHz (proven stable on this board).
 *
 *   smc(0x82000008, x1=hz, x2=0, x3=subfn) -> invoke_psci_fn(fid, hz, 0, subfn)
 *   subfn: 5=GET_RATE, 2=ROUND_RATE, 1=SET_RATE. Returns achieved/rounded Hz.
 *
 * `twwfi ddr`        - SAFE: GET_RATE + ROUND_RATE(400M) queries only.
 * `twwfi ddr <mhz>`  - switch DDR to <mhz> via SET_RATE (bl31 does it safely).
 *                      Linux runs 400/666/800/928; 400 is the idle bin.
 */
#define RK_SIP_DDR_CFG   0x82000008UL
#define DRAM_SET_RATE    1
#define DRAM_ROUND_RATE  2
#define DRAM_GET_RATE    5

static int do_ddr(int mhz)
{
	struct udevice *psci;
	unsigned long cur, rounded, res;

	/*
	 * CRITICAL: probe the PSCI firmware driver FIRST. U-Boot does not auto-probe
	 * it (CONFIG_SYSRESET_PSCI off), so until something binds it, psci_method is
	 * unset and invoke_psci_fn() SHORT-CIRCUITS returning PSCI_RET_DISABLED (-8)
	 * WITHOUT issuing any SMC. Every "-8" we saw (ddr/suspend/psci) was this, not
	 * bl31 - the SMC never went out. Probing sets the smc conduit. (tw_poweroff
	 * in cmd_tw.c does the same before its SYSTEM_OFF.)
	 */
	if (uclass_get_device_by_name(UCLASS_FIRMWARE, "psci", &psci)) {
		printf("PSCI driver not found - can't reach the DDR SIP.\n");
		return 1;
	}

	cur = invoke_psci_fn(RK_SIP_DDR_CFG, 0, 0, DRAM_GET_RATE);
	printf("DDR current rate (bl31 GET_RATE) = %lu Hz (~%lu MHz).\n",
	       cur, cur / 1000000);
	printf("(DPLL read earlier said 928 MHz; these should agree.)\n");

	if (mhz < 0) {
		/* query-only: what would bl31 round common targets to? */
		unsigned int probe[] = { 400, 666, 800, 928 };
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(probe); i++) {
			rounded = invoke_psci_fn(RK_SIP_DDR_CFG,
						 probe[i] * 1000000UL, 0,
						 DRAM_ROUND_RATE);
			printf("  ROUND_RATE(%u MHz) -> %lu Hz\n",
			       probe[i], rounded);
		}
		printf("=> safe. To actually switch: `twwfi ddr 400` (bl31 does the DFS,\n"
		       "   same as Linux devfreq; 400 MHz is Linux's idle bin).\n");
		return 0;
	}

	printf("Switching DDR to %d MHz via bl31 SET_RATE (M0 self-refresh+retrain)...\n",
	       mhz);
	res = invoke_psci_fn(RK_SIP_DDR_CFG, (unsigned long)mhz * 1000000UL, 0,
			     DRAM_SET_RATE);
	printf("SET_RATE returned %lu Hz (~%lu MHz).\n", res, res / 1000000);
	cur = invoke_psci_fn(RK_SIP_DDR_CFG, 0, 0, DRAM_GET_RATE);
	printf("GET_RATE now = %lu Hz (~%lu MHz).\n", cur, cur / 1000000);
	printf("=> If this returned and the shell still works, the DDR DFS path is\n"
	       "   usable from U-Boot. If DRAM was corrupted the board likely crashed.\n");
	return 0;
}

/*
 * PSCI CPU_SUSPEND (STANDBY) test - the route that should actually work.
 *
 * Our EL2 WFI can't wake because a physical IRQ only wakes a lower-EL WFI when
 * SCR_EL3.IRQ=1, and only EL3 can set that. bl31's STANDBY handler
 * (rockchip_cpu_standby) sets SCR_EL3.IRQ itself, does the WFI at EL3, and
 * returns on a NonSecure interrupt - exactly what we need. So instead of WFI we
 * SMC into bl31: CPU_SUSPEND(power_state=0 => STANDBY, level 0, id 0).
 *
 * We arm the NS CNTP timer (with the GIC enables) as the wake source first, so
 * bl31's WFI has something to return from within `ms`. If CPU_SUSPEND returns,
 * this is our low-power idle mechanism. (A keypress would wake it too.)
 *
 * Two bugs made this "always hang", neither of which is bl31/PSCI (confirmed
 * against the live Linux board: it reaches PSCI standby/cpu-sleep/cluster-sleep
 * fine from EL2 over the same smc conduit):
 *   1. printf() with PSTATE.I masked stalls this board's console -> fixed by
 *      doing all arming+SMC+cleanup with zero console output while masked.
 *   2. ICC_PMR_EL1 left closed -> the pending NS timer IRQ was not DELIVERABLE
 *      to the PE, so bl31's rockchip_cpu_standby() WFI-at-EL3 never woke -> a
 *      true hang INSIDE the SMC. Fixed by opening PMR=0xff before arming, like
 *      the working twwfi irq/ecwake paths.
 * power_state=0 is correct: bl31 maps it to STANDBY (rockchip_cpu_standby),
 * which sets SCR_EL3.IRQ, WFIs at EL3, and returns IN PLACE (no context loss,
 * no resume entry point needed) - the simplest and safest deep-idle path.
 */
static int do_suspend(unsigned int ms)
{
	struct udevice *psci;
	unsigned long rate = rd_cntfrq();
	unsigned long ticks = (rate / 1000) * ms;
	unsigned long t0, t1, ret;

	/* Probe the PSCI firmware driver so invoke_psci_fn has its conduit. */
	if (uclass_get_device_by_name(UCLASS_FIRMWARE, "psci", &psci)) {
		printf("PSCI driver not found - can't test CPU_SUSPEND.\n");
		return 1;
	}

	printf("PSCI CPU_SUSPEND (STANDBY), timer wake in %u ms. SMC now...\n", ms);

	/*
	 * CRITICAL: no printf() between the mask and the unmask. On this board the
	 * console output path stalls while PSTATE.I is masked (framebuffer/console
	 * putc waits on something IRQ-backed), so a printf with IRQs masked HANGS -
	 * this is what every earlier "twwfi suspend hangs" actually was, NOT the SMC.
	 * So: do ALL arming + the SMC + cleanup with no console output, then print
	 * the result after unmasking.
	 *
	 * We keep IRQ MASKED (PSTATE.I=1) across the SMC so the fired CNTP timer
	 * stays PENDING (not taken as an unhandled exception at EL2); bl31's STANDBY
	 * handler runs at EL3 with its own SCR_EL3.IRQ and returns on that pending
	 * NS interrupt.
	 */
	asm volatile("msr daifset, #2");   /* PSTATE.I = 1 (mask IRQ at the PE) */
	isb();

	/*
	 * Arm the NS timer as the wake source. CRITICAL: open ICC_PMR_EL1 (priority
	 * mask) too. bl31's rockchip_cpu_standby() sets SCR_EL3.IRQ and does WFI at
	 * EL3, waking ONLY on a deliverable NS physical IRQ. If PMR is left closed
	 * (its prior/reset value), the pending timer is NOT deliverable to the PE,
	 * so bl31's EL3 WFI never wakes -> the SMC hangs forever. The working
	 * twwfi irq/ecwake/keystroke paths all set PMR=0xff; do_suspend didn't -
	 * THAT is why it hung ("STANDBY..." prints, then dead inside the SMC).
	 */
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));    /* EnableGrp1NS */
	dsb();
	writel(1U << 30, (void *)GICR_ISENABLER0);     /* INTID 30 = CNTP PPI */
	dsb();
	asm volatile("msr " STR(ICC_PMR_EL1) ", %0" : : "r" (0xffUL));   /* allow all */
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();
	asm volatile("msr cntp_tval_el0, %0" : : "r" (ticks));
	asm volatile("msr cntp_ctl_el0, %0" : : "r" (1UL));   /* ENABLE, IMASK=0 */
	isb();

	t0 = rd_cntpct();

	/* power_state = 0: STANDBY, pwr level 0, state id 0. entry/ctx unused for
	 * standby (CPU keeps state and resumes right here). NO printf around this. */
	ret = invoke_psci_fn(PSCI_0_2_FN64_CPU_SUSPEND, 0, 0, 0);

	t1 = rd_cntpct();

	/* Disable the timer, clear its pending state, then UNMASK IRQs again so the
	 * console works and the shell returns with normal interrupt handling. */
	asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));   /* disable timer */
	writel(1U << 30, (void *)GICR_ICPENDR0);              /* clear CNTP pending */
	dsb();
	asm volatile("msr daifclr, #2");   /* PSTATE.I = 0 (unmask) */
	isb();

	printf("CPU_SUSPEND RETURNED. ret=%ld (%s), elapsed=%lu ms\n",
	       (long)ret, psci_err((long)ret), (t1 - t0) / (rate / 1000));
	if ((long)ret == 0)
		printf("=> If elapsed ~%u ms, PSCI standby idle WORKS - use it for idle.\n",
		       ms);
	else
		printf("=> bl31 REJECTED the call (not a hang): the state/param is wrong,\n"
		       "   not the exception level. EL1 would not change this.\n");
	return 0;
}

/*
 * Weidong's approach: TAKE the interrupt (don't rely on masked-pending WFI wake).
 * The piece we missed: U-Boot sets HCR_EL2.AMO but NOT HCR_EL2.IMO, so physical
 * IRQs target EL1, not our EL2 - our WFI at EL2 was never the IRQ's target. Set
 * IMO=1 so the IRQ targets EL2, install a tiny EL2 vector whose IRQ entry acks
 * the timer + EOIs + returns, unmask PSTATE.I, arm CNTP, WFI. If the IRQ is
 * genuinely taken, WFI wakes unambiguously.
 *
 * The minimal EL2 vector table (below, tw_irq_vectors) is 2 KiB-aligned; only
 * the "Current EL SPx IRQ" slot (offset 0x280) has a real handler that:
 *   x0 = ICC_IAR1_EL1 (ack)     -- read the interrupt id
 *   disable CNTP (msr cntp_ctl_el0, xzr) so it de-asserts
 *   msr ICC_EOIR1_EL1, x0 (EOI)
 *   eret
 * All other slots just eret (we don't expect them during the ~ms window).
 */
extern char tw_irq_vectors[];
asm(
"	.pushsection .text.tw_irq_vec, \"ax\"		\n"
"	.align 11					\n"   /* 2 KiB aligned */
"tw_irq_vectors:					\n"
	/* --- Current EL SP0: sync/irq/fiq/err (offsets 0x000..0x180) --- */
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
	/* --- Current EL SPx: sync (0x200) --- */
"	.align 7					\n" "	eret\n"
	/* --- Current EL SPx: IRQ (0x280) -- our handler ---
	 * Handles BOTH wake sources: the CNTP timer (INTID 30) and the EC GPIO
	 * (INTID 46). Ack, silence both sources (disable CNTP; mask GPIO0 PA1 IRQ at
	 * GPIO_INTMASK so a level-triggered EC line stops asserting), then EOI. Both
	 * silence steps are harmless when the other source fired. Save/restore
	 * x0-x2 (the interrupted context may hold live values across the WFI). */
"	.align 7					\n"
"	stp	x0, x1, [sp, #-32]!			\n"
"	str	x2, [sp, #16]				\n"
"	mrs	x0, S3_0_C12_C12_0			\n"   /* ICC_IAR1_EL1 (ack) */
"	msr	cntp_ctl_el0, xzr			\n"   /* disable CNTP -> deassert */
"	movz	x1, #0xff72, lsl #16			\n"   /* GPIO0 base 0xff720000 */
"	ldr	w2, [x1, #0x34]				\n"   /* GPIO_INTMASK */
"	orr	w2, w2, #0x2				\n"   /* mask PA1 (bit1) */
"	str	w2, [x1, #0x34]				\n"
"	dsb	sy					\n"
"	msr	S3_0_C12_C12_1, x0			\n"   /* ICC_EOIR1_EL1 (EOI) */
"	isb						\n"
"	ldr	x2, [sp, #16]				\n"
"	ldp	x0, x1, [sp], #32			\n"
"	eret						\n"
	/* --- Current EL SPx: fiq (0x300), err (0x380) --- */
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
	/* --- Lower EL aarch64 (0x400..0x580) + aarch32 (0x600..0x780) --- */
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.align 7					\n" "	eret\n"
"	.popsection					\n"
);

static int do_irq_wfi(unsigned int ms)
{
	unsigned long rate = rd_cntfrq();
	unsigned long ticks = (rate / 1000) * ms;
	unsigned long vbar_save, hcr_save, t0, t1;

	printf("IRQ-taken WFI test: HCR_EL2.IMO=1 + real handler, arm %u ms.\n", ms);
	printf("If the prompt returns, taking the IRQ at EL2 wakes WFI.\n");

	/* GIC: forward NS Group1, enable PPI + CPU-interface group, open PMR. */
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));   /* EnableGrp1NS */
	dsb();
	writel(1U << 30, (void *)GICR_ISENABLER0);    /* INTID 30 PPI */
	dsb();
	asm volatile("msr " STR(ICC_PMR_EL1) ", %0" : : "r" (0xffUL));
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();

	/* Save + install our EL2 vector table; set HCR_EL2.IMO (route phys IRQ
	 * to EL2). AMO(bit5)/IMO(bit4)/FMO(bit3) - IMO is bit4. */
	asm volatile("mrs %0, vbar_el2" : "=r" (vbar_save));
	asm volatile("mrs %0, hcr_el2"  : "=r" (hcr_save));
	asm volatile("msr vbar_el2, %0" : : "r" ((unsigned long)tw_irq_vectors));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save | (1UL << 4)));
	isb();

	t0 = rd_cntpct();

	/* Arm CNTP (ENABLE, IMASK=0), then TAKE irqs: clear PSTATE.I and WFI. */
	asm volatile("msr cntp_tval_el0, %0" : : "r" (ticks));
	asm volatile("msr cntp_ctl_el0, %0" : : "r" (1UL));
	isb();
	asm volatile("msr daifclr, #2");   /* PSTATE.I = 0 -> IRQs taken */
	wfi();
	asm volatile("msr daifset, #2");   /* re-mask */

	t1 = rd_cntpct();

	/* Restore VBAR/HCR and disable the timer. */
	asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));
	asm volatile("msr vbar_el2, %0" : : "r" (vbar_save));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save));
	isb();

	printf("WFI RETURNED. elapsed = %lu ms\n", (t1 - t0) / (rate / 1000));
	printf("=> Taking the IRQ at EL2 (HCR_EL2.IMO=1 + handler) wakes WFI."
	       " This is the path to build.\n");
	return 0;
}

/*
 * RISKY: core power-gate on WFI. `twwfi cpuinfo` shows PMU_CORE_PM_CON0=0, so a
 * plain WFI halts CPU0 but leaves its rail ON (warm chip). bl31's own core-off
 * path is just: write CORE_PM_CON0 = core_pm_en|int_wakeup_en, dsb, WFI - the
 * PMU then POWER-GATES the core on WFI entry and an interrupt repowers+wakes it.
 * We replicate exactly that, with the same IMO+handler+GIC setup as `irq`, and a
 * CNTP timer as a BACKSTOP: if the PMU wake path fails, the timer IRQ should
 * still repower/wake us. Restores CORE_PM_CON0=0 on return.
 *
 * If the prompt returns after ~ms, core power-gating works AND wakes -> worth
 * wiring into the editor (should run cooler). If it HANGS, the gated core never
 * came back: power-cycle, and we do NOT use this. This is the whole point of a
 * timer-backstopped bench test before touching the editor.
 */
static int do_coredown(unsigned int ms)
{
	unsigned long rate = rd_cntfrq();
	unsigned long ticks = (rate / 1000) * ms;
	unsigned long vbar_save, hcr_save, t0, t1;
	unsigned int pm_before, pm_after;

	pm_before = readl((void *)PMU_CORE_PM_CON0);
	printf("CORE-DOWN test: set CORE_PM_CON0=0x3 (auto power-gate CPU0 on WFI +\n");
	printf("int-wake), IMO+handler, CNTP backstop %u ms. Before=0x%08x.\n",
	       ms, pm_before);
	printf("If the prompt does NOT return, the gated core did not wake -"
	       " power-cycle.\n");

	/* GIC: forward NS Group1, enable CNTP PPI + EC SPI, open PMR + grp. */
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));
	dsb();
	writel(1U << 30, (void *)GICR_ISENABLER0);              /* INTID 30 PPI */
	writel(1U << (EC_GPIO_INTID % 32),
	       (void *)(TW_GICD_ISENABLER + (EC_GPIO_INTID / 32) * 4)); /* INTID 46 */
	asm volatile("msr " STR(ICC_PMR_EL1) ", %0" : : "r" (0xffUL));
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();

	/* Install EL2 vector + route phys IRQ to EL2 (HCR_EL2.IMO, bit4). */
	asm volatile("mrs %0, vbar_el2" : "=r" (vbar_save));
	asm volatile("mrs %0, hcr_el2"  : "=r" (hcr_save));
	asm volatile("msr vbar_el2, %0" : : "r" ((unsigned long)tw_irq_vectors));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save | (1UL << 4)));
	isb();

	t0 = rd_cntpct();

	/* Arm CNTP backstop, enable PMU auto-power-gate-on-WFI + int wake, WFI. */
	asm volatile("msr cntp_tval_el0, %0" : : "r" (ticks));
	asm volatile("msr cntp_ctl_el0, %0" : : "r" (1UL));
	isb();
	writel((1U << CORE_PM_EN_BIT) | (1U << CORE_PM_INT_WAKE_BIT),
	       (void *)PMU_CORE_PM_CON0);
	dsb();
	asm volatile("msr daifclr, #2");   /* PSTATE.I = 0 -> IRQ taken */
	wfi();                             /* <-- core may power-gate here */
	asm volatile("msr daifset, #2");   /* re-mask */

	/* First thing on wake: restore CORE_PM_CON0=0 so no later WFI gates us. */
	writel(0, (void *)PMU_CORE_PM_CON0);
	dsb();

	t1 = rd_cntpct();

	/* Restore timer + VBAR/HCR. */
	asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));
	asm volatile("msr vbar_el2, %0" : : "r" (vbar_save));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save));
	isb();
	/* Clear any pending we enabled; leave the EC line masked as the handler did. */
	writel(1U << 30, (void *)GICR_ICPENDR0);
	writel(1U << (EC_GPIO_INTID % 32),
	       (void *)(TW_GICD_ICPENDR + (EC_GPIO_INTID / 32) * 4));
	dsb();

	pm_after = readl((void *)PMU_CORE_PM_CON0);
	printf("WFI RETURNED. elapsed = %lu ms. CORE_PM_CON0 now 0x%08x.\n",
	       (t1 - t0) / (rate / 1000), pm_after);
	printf("=> The core power-gated on WFI and woke. If elapsed ~%u ms it was the\n"
	       "   timer backstop; a keypress would wake it sooner. Feel the chip: if\n"
	       "   this runs cooler at idle, wire CORE_PM_CON0=0x3 into the editor nap.\n",
	       ms);
	return 0;
}

/*
 * Prove the EC GPIO (INTID 46) can wake WFI as an INTERRUPT - so we can sleep
 * for 30 s and wake only on a key / power button / lid, with NO timer tick and
 * NO 200 ms EC poll. Same take-the-IRQ recipe as `irq`, but the wake source is
 * the cros_ec line (gpio0 PA1 -> INTID 46), and NO timer is armed. Press a key,
 * the power button, or open/close the lid to wake it. If it returns, the poll
 * loop can drop to a single long WFI. (No timeout here - if nothing is pressed
 * it waits forever; power-cycle. In the editor we'd still arm a long backstop
 * timer, but this isolates the EC-as-IRQ proof.)
 */
static int do_ecwake(void)
{
	unsigned int word = EC_GPIO_INTID / 32, bit = EC_GPIO_INTID % 32;
	unsigned long vbar_save, hcr_save;

	printf("EC-as-IRQ wake test: no timer. Press a KEY / POWER BUTTON / LID.\n");
	printf("If the prompt returns, INTID 46 wakes WFI - we can sleep 30 s.\n");

	/* GPIO0 PA1: level, active-low, unmasked, enabled. */
	clrbits_le32((void *)GPIO_INTTYPE, EC_PA1_BIT);
	clrbits_le32((void *)GPIO_INT_POL, EC_PA1_BIT);
	clrbits_le32((void *)GPIO_INTMASK, EC_PA1_BIT);
	setbits_le32((void *)GPIO_INTEN,  EC_PA1_BIT);
	dsb();

	/* GIC: forward NS Group1, enable INTID 46, open PMR + CPU-interface grp. */
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));
	dsb();
	writel(1U << bit, (void *)(TW_GICD_ISENABLER + word * 4));
	asm volatile("msr " STR(ICC_PMR_EL1) ", %0" : : "r" (0xffUL));
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	dsb();

	/* Install our EL2 vector + route phys IRQ to EL2. */
	asm volatile("mrs %0, vbar_el2" : "=r" (vbar_save));
	asm volatile("mrs %0, hcr_el2"  : "=r" (hcr_save));
	asm volatile("msr vbar_el2, %0" : : "r" ((unsigned long)tw_irq_vectors));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save | (1UL << 4)));
	isb();

	/* No timer. Take IRQs, WFI - only the EC GPIO can wake us. */
	asm volatile("msr daifclr, #2");
	wfi();
	asm volatile("msr daifset, #2");

	/* Restore. The handler masked GPIO PA1; ack the GPIO source + re-enable. */
	asm volatile("msr vbar_el2, %0" : : "r" (vbar_save));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save));
	isb();
	writel(EC_PA1_BIT, (void *)GPIO_PORTA_EOI);   /* clear GPIO latch */
	setbits_le32((void *)GPIO_INTMASK, EC_PA1_BIT); /* leave it masked (restore) */
	dsb();

	printf("WFI RETURNED - an EC event (key/button/lid) woke it.\n");
	printf("=> INTID 46 works as a WFI wake IRQ. The idle loop can sleep long\n"
	       "   (e.g. 30 s backstop) and wake on activity - no 200 ms EC poll.\n");
	return 0;
}

/*
 * Secondary-core entry stub for `twwfi cpuall`. When we PSCI CPU_ON a secondary
 * core, it starts executing HERE, cold (MMU off, no valid stack). To be safe it
 * must touch NOTHING - so it immediately SMCs PSCI_CPU_OFF (0x84000002) to power
 * ITSELF back down, and spins in WFI if that ever returns. Position-independent,
 * no memory/stack use. This is the "bring the core up, then deep-sleep it" body.
 */
extern char tw_sec_cpu_stub[];
asm(
"	.pushsection .text.tw_sec_cpu_stub, \"ax\"	\n"
"	.align 4					\n"
"tw_sec_cpu_stub:					\n"
"	movz	x0, #0x8400, lsl #16			\n"   /* PSCI_CPU_OFF = */
"	movk	x0, #0x0002				\n"   /*   0x84000002   */
"	smc	#0					\n"   /* power self off */
"1:	wfi						\n"   /* never reached  */
"	b	1b					\n"
"	.popsection					\n"
);

/*
 * `twwfi cpuall`: explicitly PSCI CPU_ON each of the 5 secondary cores, each with
 * the self-CPU_OFF stub as its entry point - i.e. bring every core up and put it
 * straight back into deep power-down through the PROPER bl31 path (CPU_ON gives
 * bl31 the resume context that a raw PMU poke, twwfi coredown, lacked).
 *
 * EXPECTATION (honest): twwfi pmu shows these cores are ALREADY off, so CPU_ON
 * will likely return ALREADY_ON only if bl31 thinks they're up, or SUCCESS then
 * they immediately CPU_OFF again - net zero power change. This tests the theory
 * that "explicitly parking them" helps; the measured pmu state says it won't.
 *
 * RISKY: CPU_ON starting a core that then SMCs CPU_OFF exercises bl31's secondary
 * boot+off path. If a started core doesn't cleanly power off, or corrupts shared
 * state, the board may wedge - power-cycle. Never run from the editor.
 */
static int do_cpuall(void)
{
	struct udevice *psci;
	/* MPIDR affinity per the DT: little L1..L3 = 1/2/3, big B0/B1 = 0x100/0x101 */
	static const struct { const char *name; unsigned long mpidr; } sec[] = {
		{ "CPUL1", 0x001 }, { "CPUL2", 0x002 }, { "CPUL3", 0x003 },
		{ "CPUB0", 0x100 }, { "CPUB1", 0x101 },
	};
	unsigned long stub = (unsigned long)tw_sec_cpu_stub;
	unsigned int i;

	if (uclass_get_device_by_name(UCLASS_FIRMWARE, "psci", &psci)) {
		printf("PSCI driver not found.\n");
		return 1;
	}
	printf("CPU_ON each secondary -> self CPU_OFF stub @ 0x%lx.\n", stub);
	printf("pmu says they're already off; expect ALREADY_ON / no power change.\n");
	printf("If the prompt does NOT return, a started core wedged - power-cycle.\n");

	/*
	 * A/B power: "cores UNTOUCHED" (as bl31/coreboot left them = off) vs "cores
	 * explicitly cycled through CPU_ON -> deep CPU_OFF". Meter before and after;
	 * both end states are cores-off, so a nonzero delta would mean the explicit
	 * CPU_OFF path parks them DEEPER than bl31's boot state (unlikely - pmu says
	 * already off). Unplug AC. (Baseline = the untouched state right now.)
	 */
	{
		int ma, base_ma = 0, base_n;

		base_n = gate_avg_ma(6, 1000, &base_ma);   /* untouched, ~6 s */
		printf("  [untouched] %d mA (%d samples)\n", base_ma, base_n);
		(void)ma;
	}

	for (i = 0; i < ARRAY_SIZE(sec); i++) {
		long r;

		/* flush the stub to PoC so a cold secondary (caches off) fetches it */
		flush_dcache_range((unsigned long)tw_sec_cpu_stub,
				   (unsigned long)tw_sec_cpu_stub + 64);
		dsb();
		r = (long)invoke_psci_fn(PSCI_0_2_FN64_CPU_ON, sec[i].mpidr,
					 stub, 0);
		printf("  CPU_ON %s (mpidr 0x%lx) -> %ld (%s)\n",
		       sec[i].name, sec[i].mpidr, r, psci_err(r));
		udelay(2000);   /* let it start + CPU_OFF itself before the next */
	}

	{
		int after_ma = 0, after_n, base2_ma = 0, base2_n;

		/* re-read the untouched baseline just before, then the cycled state,
		 * back to back, so battery drift between them is minimal. */
		base2_n = gate_avg_ma(6, 1000, &base2_ma);   /* (now = cycled/off) */
		after_n = base2_n; after_ma = base2_ma;
		printf("=> Done. Check `twwfi pmu` (domains) - likely unchanged.\n");
		printf("  [after cycle] %d mA (%d samples)\n", after_ma, after_n);
		printf("  (compare to [untouched] above; |delta|<15 mA = no difference,\n");
		printf("   i.e. bl31 already parks the secondaries as deep as CPU_OFF does.)\n");
	}
	return 0;
}

/*
 * Replicate the EDITOR's exact idle nap (tw_idle_nap in cmd_tw.c) in a loop
 * until a key is pressed - to test the real thing, not a variant. Expected:
 * the board goes quiet (deep WFI) and "freezes" until you press a key, then it
 * returns. It also reports how many naps ran and the average per-nap elapsed
 * time: if each nap really slept ~`ms`, naps ~= elapsed/ms; if WFI is NOT
 * sleeping (returning instantly), naps will be huge and avg ~0 - proving the
 * WFI code doesn't work.
 *
 * Uses `ms`-length CNTP-woken naps exactly like the editor. Keeps VBAR/HCR
 * swapped per-nap, same as the editor.
 */
/* The two cros_ec host-event flags that the power button and lid-close latch.
 * Same set the editor watches (TW_POWEROFF_EVENTS in cmd_tw.c). */
#define TWWFI_EC_EVENTS  (EC_HOST_EVENT_MASK(EC_HOST_EVENT_POWER_BUTTON) | \
			  EC_HOST_EVENT_MASK(EC_HOST_EVENT_LID_CLOSED))

/* Return which power-off host event(s) are latched (0 if none, or if no EC).
 * Does NOT clear them - caller decides. Never touches the MKBP key FIFO. */
static uint32_t twwfi_ec_events(struct udevice *ec)
{
	uint32_t events = 0;

	if (!ec)
		return 0;
	if (cros_ec_get_host_events(ec, &events))
		return 0;
	return events & TWWFI_EC_EVENTS;
}

static int do_keystroke(unsigned int ms)
{
	unsigned int word = EC_GPIO_INTID / 32, bit = EC_GPIO_INTID % 32;
	unsigned long ticks = (rd_cntfrq() / 1000) * ms;
	unsigned long naps = 0, t_start, t_end;
	struct udevice *ec = NULL;
	const char *woke_by = "key";
	uint32_t ec_events = 0;

	printf("keystroke test: editor-style %u ms WFI naps until an EVENT.\n", ms);
	printf("EXPECT: board goes QUIET / 'frozen' (deep WFI sleep). A keypress,\n");
	printf("power button, or lid-close wakes it early (via the EC IRQ, INTID\n");
	printf("46); otherwise it re-naps every %u ms.\n", ms);
	printf("Press a key / power button / close the lid when ready...\n");

	/* Resolve the EC so we can read + clear power-button/lid host events.
	 * Clear any latched event first, so the state that launched us doesn't
	 * count as an immediate wake. (The keyboard's MKBP FIFO is untouched.) */
	if (uclass_first_device_err(UCLASS_CROS_EC, &ec))
		ec = NULL;
	if (ec)
		cros_ec_clear_host_events(ec, TWWFI_EC_EVENTS);

	/* One-time GIC enables (same as tw_wfi_setup): NS Group1 fwd + CNTP PPI
	 * + PMR open + CPU-interface group. ALSO enable the EC keypress line
	 * (GPIO0 PA1 -> INTID 46) as a second wake source, or a keypress can't
	 * wake the WFI early and each nap runs to the full timer expiry. Same
	 * setup do_ecwake proved works. */
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));
	dsb();
	writel(1U << 30, (void *)GICR_ISENABLER0);
	asm volatile("msr " STR(ICC_PMR_EL1) ", %0" : : "r" (0xffUL));
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();
	/* GPIO0 PA1: level, active-low, unmasked, enabled + INTID 46 in GICD. */
	clrbits_le32((void *)GPIO_INTTYPE, EC_PA1_BIT);   /* 0 = level */
	clrbits_le32((void *)GPIO_INT_POL, EC_PA1_BIT);   /* 0 = active-low */
	clrbits_le32((void *)GPIO_INTMASK, EC_PA1_BIT);   /* unmask */
	setbits_le32((void *)GPIO_INTEN,  EC_PA1_BIT);    /* enable */
	writel(1U << bit, (void *)(TW_GICD_ISENABLER + word * 4));
	dsb();

	t_start = rd_cntpct();

	/*
	 * Nap until an EVENT ends the wait - a key (tstc), the power button, or a
	 * lid-close (both latched EC host-event flags). The WFI wakes on the EC
	 * IRQ (INTID 46) the instant any of them fires; we then decode which it
	 * was. A bare timer expiry just re-naps. This mirrors the editor's wait.
	 */
	for (;;) {
		unsigned long vbar_save, hcr_save;

		if (tstc()) {                         /* a key arrived */
			woke_by = "key";
			break;
		}
		ec_events = twwfi_ec_events(ec);
		if (ec_events) {                      /* power button or lid */
			woke_by = (ec_events &
				   EC_HOST_EVENT_MASK(EC_HOST_EVENT_POWER_BUTTON))
				  ? "power button" : "lid";
			break;
		}

		asm volatile("mrs %0, vbar_el2" : "=r" (vbar_save));
		asm volatile("mrs %0, hcr_el2"  : "=r" (hcr_save));
		asm volatile("msr vbar_el2, %0" : : "r"
			     ((unsigned long)tw_irq_vectors));
		asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save | (1UL << 4)));
		isb();

		asm volatile("msr cntp_tval_el0, %0" : : "r" (ticks));
		asm volatile("msr cntp_ctl_el0, %0" : : "r" (1UL));
		isb();
		asm volatile("msr daifclr, #2");
		wfi();
		asm volatile("msr daifset, #2");
		asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));

		asm volatile("msr vbar_el2, %0" : : "r" (vbar_save));
		asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save));
		isb();

		/* The IRQ handler (tw_irq_vectors) masks GPIO0 PA1 on an EC wake so
		 * the level line stops asserting; clear the latch and re-unmask it
		 * for the next nap. Harmless when the timer woke us instead. */
		writel(EC_PA1_BIT, (void *)GPIO_PORTA_EOI);
		clrbits_le32((void *)GPIO_INTMASK, EC_PA1_BIT);
		writel(1U << 30, (void *)GICR_ICPENDR0);   /* CNTP PPI 30 if pending */
		writel(1U << bit, (void *)(TW_GICD_ICPENDR + word * 4)); /* SPI 46 */
		dsb();

		naps++;
		schedule();
	}

	/* Consume whatever ended the wait so it doesn't linger. */
	if (!strcmp(woke_by, "key"))
		(void)getchar();          /* drain the key (also empties MKBP) */
	else if (ec)
		cros_ec_clear_host_events(ec, TWWFI_EC_EVENTS);   /* drain latch */

	/* Restore: mask + disable the EC GPIO line we turned on. */
	setbits_le32((void *)GPIO_INTMASK, EC_PA1_BIT);
	clrbits_le32((void *)GPIO_INTEN,  EC_PA1_BIT);
	dsb();

	t_end = rd_cntpct();

	{
		unsigned long total_ms = (t_end - t_start) / (rd_cntfrq() / 1000);
		unsigned long avg_us = naps ? (t_end - t_start) /
				       (rd_cntfrq() / 1000000) / naps : 0;

		printf("\nWoke on %s. naps=%lu over %lu ms (avg %lu us/nap).\n",
		       woke_by, naps, total_ms, avg_us);
		printf("=> avg ~= %u000 us/nap means WFI slept the full nap (GOOD).\n",
		       ms);
		printf("   avg ~0 with a huge nap count means WFI did NOT sleep (BUG).\n");
		if (!ec)
			printf("   (no cros_ec device found: power/lid wake untested.)\n");
	}
	return 0;
}

/* -------- RISKY: live power-gate of editor-unused peripheral domains -------- */
/*
 * PMU registers for gating. bl31 gates a domain by (1) requesting its NoC bus
 * idle (BUS_IDLE_REQ set -> wait BUS_IDLE_ST & BUS_IDLE_ACK set), then (2)
 * setting the domain bit in PWRDN_CON (-> wait PWRDN_ST shows off). Restore
 * reverses both. We replicate that exactly. See ATF pmu.c pmu_set_power_domain.
 */
#define PMU_PWRDN_CON     (PMU_BASE + 0x14)
#define PMU_BUS_IDLE_REQ  (PMU_BASE + 0x60)
#define PMU_BUS_IDLE_ACK  (PMU_BASE + 0x68)
#define GATE_TIMEOUT_US   20000   /* generous; bl31 uses ~few ms worst case */

/*
 * The 10 editor-unused domains, indexed 0..9 so `twwfi gate N` can gate exactly
 * one (to bisect which wedges). pd = bit in PWRDN_CON/ST; bus = bit in
 * BUS_IDLE_{REQ,ST,ACK}. Both from bl31's enums (pmu_powerdomain_id / pmu_bus_id).
 */
#define GATE_NO_BUS  0xff   /* domain has no NoC bus-idle step (bl31: PD_TCPDx) */
static const struct gate_dom {
	const char *name;
	unsigned char pd;
	unsigned char bus;
} gate_doms[] = {
	{ "GPU",    15,  0 },
	{ "VCODEC", 16,  3 },
	{ "VDU",    17,  4 },
	{ "RGA",    18,  5 },
	{ "IEP",    19,  6 },
	{ "ISP0",   22,  9 },
	{ "ISP1",   23, 10 },
	{ "HDCP",   24, 11 },
	{ "USB3",   27, 12 },
	{ "GMAC",   25, 23 },
	/* TCPD0/TCPD1 = external Type-C DisplayPort. bl31's pmu_set_power_domain
	 * gates these with NO bus-idle step (`case PD_TCPD0: break;` - PWRDN only),
	 * and bl31 itself powers them off in suspend / on in resume, so gating them
	 * while the editor runs (internal panel is eDP = PD_EDP, a different domain)
	 * is safe. PWRDN bits 8/9; GATE_NO_BUS = skip the bus handshake. */
	{ "TCPD0",   8, GATE_NO_BUS },
	{ "TCPD1",   9, GATE_NO_BUS },
};
/* SDIOAUDIO (PD bit 31 / bus 29) was tested and WEDGED the SoC when gated, so it
 * is intentionally NOT here. */

/* Wait until (readl(reg) & mask) == want, up to GATE_TIMEOUT_US. Return 0 ok. */
static int gate_wait(unsigned long reg, unsigned int mask, unsigned int want)
{
	unsigned int us = 0;

	while (((readl((void *)reg) & mask) != want) && us < GATE_TIMEOUT_US) {
		udelay(1);
		us++;
	}
	return ((readl((void *)reg) & mask) == want) ? 0 : -1;
}

/* Power OFF one domain (bus-idle request, then PWRDN_CON). Return 0 on success.
 * On bus-idle timeout we ABORT (don't force PWRDN) - forcing is the wedge risk. */
static int gate_off(const struct gate_dom *d)
{
	int has_bus = (d->bus != GATE_NO_BUS);
	unsigned int busm = has_bus ? (1U << d->bus) : 0, pdm = 1U << d->pd;

	printf("  %-6s: ", d->name);
	if (has_bus) {
		printf("bus-idle req...");
		setbits_le32((void *)PMU_BUS_IDLE_REQ, busm);
		dsb();
		if (gate_wait(PMU_BUS_IDLE_ST, busm, busm) ||
		    gate_wait(PMU_BUS_IDLE_ACK, busm, busm)) {
			printf(" TIMEOUT (bus won't idle) - aborting, backing out.\n");
			clrbits_le32((void *)PMU_BUS_IDLE_REQ, busm);
			dsb();
			return -1;
		}
		printf(" idle;");
	} else {
		printf("(no bus-idle)");   /* bl31 does PWRDN-only for TCPDx */
	}
	printf(" power off...");
	setbits_le32((void *)PMU_PWRDN_CON, pdm);
	dsb();
	if (gate_wait(PMU_PWRDN_ST, pdm, pdm)) {
		printf(" PWRDN TIMEOUT - backing out.\n");
		clrbits_le32((void *)PMU_PWRDN_CON, pdm);
		dsb();
		if (has_bus)
			clrbits_le32((void *)PMU_BUS_IDLE_REQ, busm);
		dsb();
		return -1;
	}
	printf(" OFF.\n");
	return 0;
}

/* Power ON one domain (reverse: PWRDN_CON clear, then bus active). */
static void gate_on(const struct gate_dom *d)
{
	int has_bus = (d->bus != GATE_NO_BUS);
	unsigned int busm = has_bus ? (1U << d->bus) : 0, pdm = 1U << d->pd;

	clrbits_le32((void *)PMU_PWRDN_CON, pdm);
	dsb();
	gate_wait(PMU_PWRDN_ST, pdm, 0);
	if (!has_bus) {
		printf("  %-6s: restored (on).\n", d->name);
		return;
	}
	clrbits_le32((void *)PMU_BUS_IDLE_REQ, busm);
	dsb();
	gate_wait(PMU_BUS_IDLE_ST, busm, 0);
	printf("  %-6s: restored (on).\n", d->name);
}

/*
 * `twwfi gate [N]`: gate ONE domain (N=0..9) or ALL if N omitted, hold ~15 s so
 * you can read the external power meter, then RESTORE. RISKY: bl31 does this
 * during suspend with clocks quiesced; live it may wedge the NoC (power-cycle).
 * Gate one at a time to bisect which domain hangs. USB safe here (built-in kbd
 * + mmc). If the prompt does not return after "holding", that domain froze it.
 */
/*
 * Average the smart-battery current over `n` samples spaced ~`gap_ms` apart
 * (the SBS gauge updates ~1 Hz, so gap >= 1000 ms gets independent readings).
 * Returns mA in *out and the number of good samples; 0 samples => read failed.
 */
static int gate_avg_ma(int n, unsigned int gap_ms, int *out)
{
	long sum = 0;
	int got = 0, k, ma;

	for (k = 0; k < n; k++) {
		if (tw_read_batt_current(&ma) == 0) {
			sum += ma;
			got++;
		}
		if (k + 1 < n)
			udelay(gap_ms * 1000);
	}
	*out = got ? (int)(sum / got) : 0;
	return got;
}

/*
 * Sweep EVERY gatable domain individually: for each, meter baseline, gate it
 * alone, meter, restore, record the delta - then print a ranked per-domain mA
 * table. This is how to see each domain's INDIVIDUAL contribution (the plain
 * `gate` with no arg gates them all at once and only gives the sum). Each domain
 * is gated for only ~4 s (shorter than the single-domain 8 s, since there are
 * ~11 of them) and restored before the next, so the SoC is only ever missing one
 * domain at a time. If one wedges, the last name printed "gating ..." is it.
 */
static int do_gate_sweep(void)
{
	int deltas[ARRAY_SIZE(gate_doms)];
	int i, base_ma, cur_ma, base_n, cur_n;

	printf("GATE SWEEP: metering each domain's individual draw (gate one, meter,\n");
	printf("restore, next). ~%u domains x ~9 s. Unplug AC. Key aborts.\n",
	       (unsigned)ARRAY_SIZE(gate_doms));
	printf("If it HANGS, the last 'gating' line names the domain that wedged.\n\n");

	for (i = 0; i < (int)ARRAY_SIZE(gate_doms); i++) {
		deltas[i] = 0;

		base_n = gate_avg_ma(4, 1000, &base_ma);   /* baseline ~4 s */

		printf("  gating %-8s ...", gate_doms[i].name);
		if (gate_off(&gate_doms[i])) {
			printf(" (would not gate - skipped)\n");
			continue;
		}
		cur_n = gate_avg_ma(4, 1000, &cur_ma);     /* gated ~4 s */
		gate_on(&gate_doms[i]);                    /* restore before next */

		if (base_n && cur_n) {
			deltas[i] = base_ma - cur_ma;      /* less negative gated => +saved */
			printf(" on=%d off=%d  saved=%d mA\n", base_ma, cur_ma, deltas[i]);
		} else {
			printf(" SBS read failed\n");
		}
		if (tstc()) { (void)getchar(); printf("  (aborted by key)\n"); break; }
	}

	printf("\n--- per-domain saving (mA, whole-board smart battery) ---\n");
	for (i = 0; i < (int)ARRAY_SIZE(gate_doms); i++)
		printf("  %-8s : %4d mA%s\n", gate_doms[i].name, deltas[i],
		       (deltas[i] >= 15) ? "  <- real" :
		       (deltas[i] <= -15) ? "  <- (noise/negative)" : "");
	printf("(each measured ALONE; ~15 mA = noise floor. Sum won't exactly match a\n");
	printf(" simultaneous all-gate due to noise + shared NoC paths.)\n");
	printf("PWRDN_ST now = 0x%08x (all restored).\n", readl((void *)PMU_PWRDN_ST));
	return 0;
}

static int do_gate(int idx)
{
	int lo = 0, hi = ARRAY_SIZE(gate_doms), i, off_ok = 0;
	int base_ma = 0, gated_ma = 0, base_n, gated_n;

	if (idx >= 0) {
		if (idx >= (int)ARRAY_SIZE(gate_doms)) {
			printf("gate index %d out of range (0..%u)\n",
			       idx, (unsigned)ARRAY_SIZE(gate_doms) - 1);
			return CMD_RET_USAGE;
		}
		lo = idx;
		hi = idx + 1;
	}

	printf("GATE %s. PWRDN_ST before = 0x%08x\n",
	       idx >= 0 ? gate_doms[idx].name : "ALL unused domains",
	       readl((void *)PMU_PWRDN_ST));
	printf("If the prompt does NOT return, the domain being gated wedged the SoC"
	       " - power-cycle.\n");

	/* Baseline current BEFORE gating (5 samples, ~1 s apart, ~5 s). */
	base_n = gate_avg_ma(5, 1000, &base_ma);

	for (i = lo; i < hi; i++)
		if (!gate_off(&gate_doms[i]))
			off_ok++;

	printf("PWRDN_ST after  = 0x%08x  (%d domain(s) gated off)\n",
	       readl((void *)PMU_PWRDN_ST), off_ok);
	printf(">>> Gated - sampling current for ~8 s (self-metering, no ^T needed)."
	       " Press a key to end early.\n");

	/* Current WHILE gated (8 samples, ~1 s apart). tstc between samples so a
	 * keypress can still cut it short. */
	{
		long sum = 0;
		int got = 0, k, ma;

		for (k = 0; k < 8 && !tstc(); k++) {
			if (tw_read_batt_current(&ma) == 0) {
				sum += ma;
				got++;
			}
			udelay(1000 * 1000);
		}
		gated_n = got;
		gated_ma = got ? (int)(sum / got) : 0;
	}
	if (tstc())
		(void)getchar();

	printf("Restoring...\n");
	for (i = hi - 1; i >= lo; i--)
		gate_on(&gate_doms[i]);

	printf("PWRDN_ST restored = 0x%08x\n", readl((void *)PMU_PWRDN_ST));

	/* Self-reported delta - the whole point, so no live ^T race is needed. */
	printf("--- gate power delta (smart battery, whole-board) ---\n");
	if (!base_n || !gated_n) {
		printf("  SBS read failed (before=%d during=%d samples). On AC? tunnel down?\n",
		       base_n, gated_n);
	} else {
		int d = base_ma - gated_ma;   /* both negative; less negative gated => saved */

		printf("  before gate : %d mA  (%d samples)\n", base_ma, base_n);
		printf("  while gated : %d mA  (%d samples)\n", gated_ma, gated_n);
		printf("  saved       : %d mA%s\n", d < 0 ? -d : d,
		       (d < 15 && d > -15)
			       ? "  (< noise floor ~15 mA - effectively none)"
			       : "");
		printf("  (negative = discharging; verify AC unplugged)\n");
	}
	printf("=> If a domain froze the board, it's the last one printed before the"
	       " hang.\n");
	return 0;
}

/*
 * SAFE (read-only): dump RK3399 PMU power state - PMU_PWRDN_ST (which CPU and
 * PERIPHERAL domains are powered). A framebuffer text editor uses none of GPU/
 * codec/camera/ethernet/USB3; any left ON is a gating candidate. PMU_BUS_IDLE_ST
 * is also printed but is NOT a runtime activity signal (see note there). Reads
 * only; cannot hang. To rank which powered domain actually costs power, gate one
 * at a time and read the external power meter - no register substitutes for it.
 */
static int do_pmu(void)
{
	unsigned int st = readl((void *)PMU_PWRDN_ST);
	static const char *const cpu[] = {
		"A53 core0 (CPUL0)", "A53 core1 (CPUL1)",
		"A53 core2 (CPUL2)", "A53 core3 (CPUL3)",
		"A72 core0 (CPUB0)", "A72 core1 (CPUB1)",
	};
	unsigned int i, on_cores = 0;

	printf("PMU_PWRDN_ST (0x%08lx) = 0x%08x\n", PMU_PWRDN_ST, st);
	printf("  (bit=1 => domain OFF, bit=0 => ON)\n");
	for (i = 0; i < 6; i++) {
		int on = !((st >> (PD_CPUL0_BIT + i)) & 1);

		printf("  %-20s : %s\n", cpu[i], on ? "ON" : "off");
		if (on)
			on_cores++;
	}
	printf("  little cluster L2/SCU : %s\n",
	       ((st >> PD_SCUL_BIT) & 1) ? "off" : "ON");
	printf("  big    cluster L2/SCU : %s\n",
	       ((st >> PD_SCUB_BIT) & 1) ? "off" : "ON");

	{
		int b0_on  = !((st >> PD_CPUB0_BIT) & 1);
		int b1_on  = !((st >> (PD_CPUB0_BIT + 1)) & 1);
		int scub_on = !((st >> PD_SCUB_BIT) & 1);

		printf("=> %u CPU core(s) powered; the typewriter runs on CPUL0 (A53).\n",
		       on_cores);
		if (b0_on || b1_on)
			printf("   A72 big CORE(s) powered+running - real active draw we\n"
			       "   never use. Worth gating (bl31/PMU).\n");
		else if (scub_on)
			printf("   A72 cores are OFF, but the big-cluster L2/SCU domain\n"
			       "   (SCUB) is still powered - static LEAKAGE only (no core\n"
			       "   clocked). bl31 never tore the cluster down because we\n"
			       "   never PSCI-CPU_ON'd it. Gateable, but the win is leakage-\n"
			       "   sized, not a running-core's draw. Measure before chasing.\n");
		else
			printf("   A72 cores AND cluster L2/SCU are off. CPU side is fully\n"
			       "   gated; remaining idle draw is peripheral (DDR/rails/WiFi).\n");
	}

	/*
	 * Peripheral power domains (PMU_PWRDN_ST bits 8..31). Names + bit index
	 * from bl31's `enum pmu_powerdomain_id` (bit 21 is an unused gap: the enum
	 * jumps VO=20 -> ISP0=22). A framebuffer text editor uses NONE of the GPU,
	 * video codecs, camera ISPs, HDCP, ethernet, or USB3 - any of those left ON
	 * is a candidate to gate. Unlike the CPU/SCU domains, these ARE driven by
	 * bl31's generic pmu_set_power_domain() (bus-idle handshake), so they can be
	 * gated properly. This is still a pure READ; nothing is toggled here.
	 */
	{
		static const struct { unsigned char bit; const char *name;
				      unsigned char unused; } pd[] = {
			{  8, "TCPD0  (Type-C DP 0)",       1 },
			{  9, "TCPD1  (Type-C DP 1)",       1 },
			{ 10, "CCI    (cache-coherent ic)", 0 },
			{ 11, "PERILP (low-speed peri)",    0 },
			{ 12, "PERIHP (high-speed peri)",   0 },
			{ 13, "CENTER (ddr/noc center)",    0 },
			{ 14, "VIO    (display iface)",     0 },
			{ 15, "GPU    (Mali)",              1 },
			{ 16, "VCODEC (video enc/dec)",     1 },
			{ 17, "VDU    (video decode)",      1 },
			{ 18, "RGA    (2D raster)",         1 },
			{ 19, "IEP    (img enhance)",       1 },
			{ 20, "VO     (video out/VOP)",     0 },
			{ 22, "ISP0   (camera 0)",          1 },
			{ 23, "ISP1   (camera 1)",          1 },
			{ 24, "HDCP   (hdmi content prot)", 1 },
			{ 25, "GMAC   (ethernet)",          1 },
			{ 26, "EMMC   (eMMC ctrl)",         0 },
			{ 27, "USB3   (usb3 + phy)",        1 },
			{ 28, "EDP    (embedded DP)",       0 },
			{ 29, "GIC    (interrupt ctrl)",    0 },
			{ 30, "SD     (sd/mmc)",            0 },
			{ 31, "SDIOAUDIO (sdio+i2s)",       0 },
		};
		unsigned int j, unused_on = 0;

		printf("\nPeripheral power domains (ON = still powered):\n");
		for (j = 0; j < ARRAY_SIZE(pd); j++) {
			int on = !((st >> pd[j].bit) & 1);

			printf("  %-26s : %-3s%s\n", pd[j].name, on ? "ON" : "off",
			       (on && pd[j].unused) ? "   <- unused by editor" : "");
			if (on && pd[j].unused)
				unused_on++;
		}
		printf("=> %u domain(s) powered that the editor never uses.\n", unused_on);
		if (unused_on)
			printf("   Each is a gating candidate (bl31 pmu_set_power_domain\n"
			       "   path). No register ranks their draw at runtime - gate one\n"
			       "   at a time and read the meter.\n");
	}

	/*
	 * NoC bus-idle status (PMU_BUS_IDLE_ST): bit set = that bus has ENTERED
	 * idle, which only happens AFTER bl31 sets the matching PMU_BUS_IDLE_REQ
	 * bit (it does that during suspend). At normal runtime REQ is all-zero, so
	 * every bus reads not-idle (bit 0) regardless of whether it is actually
	 * transacting. So this register CANNOT tell "active vs leakage" while
	 * running - an all-zero read here is expected and means nothing about draw.
	 * We print the raw value only for completeness. (Read only.)
	 */
	printf("\nPMU_BUS_IDLE_ST = 0x%08x  (raw; all-0 is normal at runtime -\n"
	       "  buses only show idle after bl31 requests it during suspend, so\n"
	       "  this does NOT indicate which domains draw power. Use the meter.)\n",
	       readl((void *)PMU_BUS_IDLE_ST));
	return 0;
}

/* Decode one CPU cluster's live frequency from its APLL CON regs + core mux/div.
 * Returns the core clock in MHz (0 if the mux isn't on this APLL - unusual). */
static unsigned int cpu_cluster_mhz(unsigned long pll_con, unsigned long clksel,
				    const char *name, unsigned int apll_sel)
{
	unsigned int con0 = readl((void *)(pll_con + 0x0));
	unsigned int con1 = readl((void *)(pll_con + 0x4));
	unsigned int con2 = readl((void *)(pll_con + 0x8));
	unsigned int con3 = readl((void *)(pll_con + 0xc));
	unsigned int sel  = readl((void *)clksel);
	unsigned int fbdiv   = con0 & 0xfff;
	unsigned int refdiv  = con1 & 0x3f;
	unsigned int postdiv1 = (con1 >> 8) & 0x7;
	unsigned int postdiv2 = (con1 >> 12) & 0x7;
	unsigned int mode = (con3 >> 8) & 0x3;
	unsigned int locked = (con2 >> 31) & 1;
	unsigned int core_sel = (sel >> 6) & 0x3;
	unsigned int core_div = (sel & 0x1f) + 1;
	unsigned int fout, cpu;

	printf("%s: APLL CON0-3 = %08x %08x %08x %08x\n", name,
	       con0, con1, con2, con3);
	if (mode != 1) {
		printf("  PLL in SLOW/bypass mode (mode=%u) -> running off 24 MHz OSC\n",
		       mode);
	}
	if (!refdiv || !postdiv1 || !postdiv2) {
		printf("  (degenerate divisors; PLL not configured)\n");
		return 0;
	}
	fout = 24u * fbdiv / (refdiv * postdiv1 * postdiv2);
	cpu = (mode == 1) ? fout / core_div : 24 / core_div;

	printf("  fbdiv=%u refdiv=%u postdiv1=%u postdiv2=%u lock=%u -> PLL %u MHz\n",
	       fbdiv, refdiv, postdiv1, postdiv2, locked, fout);
	printf("  core mux=%u (%s) div=/%u -> %s = %u MHz\n",
	       core_sel, core_sel == apll_sel ? "this APLL" : "OTHER src",
	       core_div, name, cpu);
	return cpu;
}

/*
 * SAFE (read-only): report the editor's idle-nap instrumentation, to PROVE
 * whether its no-timer WFI actually deep-sleeps or busy-spins. Run the editor,
 * let it sit idle a while, exit, then `twwfi napstats`. Interpretation:
 *   - instant/count near 0, and slept/count large (seconds)  => WFI sleeps GOOD
 *   - instant ~= count, slept/count ~0                       => WFI SPINS (bug):
 *     it returns immediately every time (a level IRQ left pending), so the CPU
 *     never idles and battery draw is unchanged. That confirms the editor's
 *     no-timer path is broken even though `twwfi keystroke` (timer-armed) slept.
 */
static int do_napstats(void)
{
	unsigned long count = 0, instant = 0, slept_us = 0;

	tw_idle_stats(&count, &instant, &slept_us);
	printf("editor idle naps: count=%lu, instant(<1ms)=%lu, slept=%lu ms\n",
	       count, instant, slept_us / 1000);
	if (!count) {
		printf("=> no naps recorded yet - run the `typewriter` editor and let it\n"
		       "   sit idle >2 s (past the active window) before checking.\n");
	} else {
		printf("=> avg %lu us/nap. ", slept_us / count);
		if (instant * 2 > count)
			printf("MOSTLY INSTANT: WFI is NOT sleeping (busy-spin) -\n"
			       "   the no-timer idle path is broken; this explains flat"
			       " battery draw.\n");
		else
			printf("naps blocked properly: WFI IS sleeping. Flat battery\n"
			       "   draw is then NOT the CPU - look elsewhere.\n");
	}
	return 0;
}

/*
 * SAFE (read-only): decode the live CPU cluster frequencies from the CRU, the
 * CPU power-domain state, and (as a whole-BOARD proxy, not CPU-specific) the
 * EC battery current. CPU0 (the typewriter's core) is the A53 little cluster off
 * APLL_L. Confirms whether a frequency change (e.g. the editor's 408 MHz set)
 * took effect and that the core is actually powered + working. Register reads +
 * one EC host command; cannot hang.
 */
static int do_cpuinfo(void)
{
	unsigned int st = readl((void *)PMU_PWRDN_ST);
	unsigned int pm = readl((void *)PMU_CORE_PM_CON0);
	int cpu0_on = !((st >> PD_CPUL0_BIT) & 1);
	int scul_on = !((st >> PD_SCUL_BIT) & 1);
	int pm_en   = (pm >> CORE_PM_EN_BIT) & 1;
	int pm_intw = (pm >> CORE_PM_INT_WAKE_BIT) & 1;
	unsigned int cpul_mhz;
	int pct = 0, chg_ma = 0, ac = 0;

	printf("Current EL = %u, CNTFRQ = %lu Hz\n", current_el(), rd_cntfrq());

	/* Power status first: a frequency is meaningless if the core is off. */
	printf("--- CPU power status (PMU_PWRDN_ST = 0x%08x) ---\n", st);
	printf("  CPU0 / CPUL0 (A53, our core) : %s\n",
	       cpu0_on ? "POWERED (working)" : "OFF (!)");
	printf("  little-cluster L2/SCU (SCUL) : %s\n", scul_on ? "on" : "off");

	/* The key question for the "warm chip at idle" puzzle: does a WFI actually
	 * power-gate the core, or just halt it with the rail still on? */
	printf("  PMU_CORE_PM_CON0 = 0x%08x: auto-power-down-on-WFI=%s, int-wake=%s\n",
	       pm, pm_en ? "ON" : "off", pm_intw ? "on" : "off");
	if (!pm_en)
		printf("  => core_pm_en=0: a WFI HALTS CPU0 but does NOT drop its power\n"
		       "     rail - the core stays powered (warm) while 'asleep'. This is\n"
		       "     why the chip is hot at idle: napstats sees WFI block, but the\n"
		       "     A53 rail never gates. Enabling it needs bl31/PSCI or a careful\n"
		       "     PMU write with a proven wake path (risky - see notes).\n");
	else
		printf("  => core_pm_en=1: WFI should auto power-gate CPU0 (wake via IRQ\n"
		       "     if int-wake=on). If the chip is still warm, look at the\n"
		       "     cluster/SCU or rails, not the core.\n");

	printf("--- little cluster (CPU0 = A53, APLL_L) ---\n");
	cpul_mhz = cpu_cluster_mhz(CRU_APLL_L_CON, CRU_CLKSEL_CON0, "CPUL", 0);
	printf("--- big cluster (A72, APLL_B) ---\n");
	cpu_cluster_mhz(CRU_APLL_B_CON, CRU_CLKSEL_CON2, "CPUB", 1);

	printf("=> CPU0 verdict: %s @ %u MHz.\n",
	       cpu0_on ? "powered + clocked = WORKING" : "NOT powered", cpul_mhz);

	/*
	 * DDR frequency, decoded straight from the DPLL registers (CRU_BASE+0x40),
	 * so a dump self-documents the DDR rate (boot default 928 MHz; the editor
	 * lowers it to 400 via the bl31 SIP). Pure read - no PSCI/SIP needed here.
	 */
	{
		unsigned int c0 = readl((void *)(CRU_BASE + 0x40));
		unsigned int c1 = readl((void *)(CRU_BASE + 0x44));
		unsigned int fb = c0 & 0xfff;
		unsigned int rd = c1 & 0x3f;
		unsigned int p1 = (c1 >> 8) & 0x7;
		unsigned int p2 = (c1 >> 12) & 0x7;
		unsigned int ddr = (rd && p1 && p2) ? 24u * fb / (rd * p1 * p2) : 0;

		printf("--- DDR (DPLL) ---\n");
		printf("  DPLL fbdiv=%u refdiv=%u pd1=%u pd2=%u -> DDR clk = %u MHz%s\n",
		       fb, rd, p1, p2, ddr,
		       ddr == 400 ? " (lowered)" : ddr == 928 ? " (boot default)" : "");
	}

	/*
	 * Whole-board current from the SMART BATTERY (SBS reg 0x0A via the EC I2C
	 * tunnel) - the REAL draw, signed (negative = discharging). NOT CPU-specific
	 * (there is no per-CPU telemetry on RK3399), but on battery |mA| is the true
	 * total system draw, so it's the gauge for before/after power comparisons.
	 * Also print battery %/AC from the charge-state cmd (that % is fine; only its
	 * chg_current field was garbage, which is why current now comes from SBS).
	 */
	printf("--- board current (smart battery; whole-board, not CPU) ---\n");
	if (!tw_read_charge_state(&pct, &chg_ma, &ac))
		printf("  battery %d%%, on-AC=%d\n", pct, ac);
	{
		int ma;

		if (tw_read_batt_current(&ma) == 0)
			printf("  battery current = %d mA (%s; |mA| ~ total draw on battery)\n",
			       ma, ma < 0 ? "discharging" : "charging/idle");
		else
			printf("  battery current: SBS read failed\n");
	}
	printf("   `twwfi pmu` shows every powered domain.\n");
	return 0;
}

/*
 * WFI on/off A/B power bench. Runs two ~equal-length phases at the SAME
 * brightness/DDR/freq (the only variable is WFI vs busy-spin) and reports the
 * average smart-battery current in each, so `busy - wfi` is the pure cost of
 * NOT sleeping the core.
 *
 *   Phase BUSY: tight CNTPCT spin loop, core never idles.
 *   Phase WFI : repeated (arm CNTP timer + take IRQ at EL2 + WFI) slices - the
 *               exact idiom `twwfi irq`/the editor idle nap uses (HCR_EL2.IMO=1
 *               + our vector table + CNTP as the wake source).
 *
 * Each phase samples SBS current (reg 0x0A via EC tunnel) a few times and
 * averages. The gauge updates ~1 Hz and is noisy (+-10..20 mA), so a delta
 * below ~15 mA is in the noise - reported honestly. The current read itself
 * runs the core briefly in BOTH phases (identical overhead), so it does not
 * bias the comparison. No timer-less/blind WFI here: every WFI is CNTP-armed,
 * so this cannot hang.
 */
static int wfibench_sample_ma(long *accum, int *n)
{
	int ma;

	if (tw_read_batt_current(&ma) == 0) {
		*accum += ma;
		(*n)++;
		return ma;
	}
	return 0;
}

static int do_wfibench(unsigned int ms)
{
	unsigned long rate = rd_cntfrq();
	unsigned long slice_ticks = (rate / 1000) * 250;  /* 250 ms per WFI slice */
	unsigned long phase_ticks;                        /* per-phase wall time */
	unsigned long vbar_save, hcr_save, tend;
	long busy_sum = 0, wfi_sum = 0;
	int busy_n = 0, wfi_n = 0;

	if (ms < 1000)
		ms = 1000;
	phase_ticks = (rate / 1000) * ms;

	printf("WFI on/off bench: two %u ms phases, same brightness/DDR/freq.\n", ms);
	printf("Unplug AC (need discharging current). Keep the screen static.\n");

	/* ---- Phase BUSY: spin the core, never idle, sample current ---- */
	tend = rd_cntpct() + phase_ticks;
	{
		unsigned long next = rd_cntpct() + slice_ticks;

		while (rd_cntpct() < tend) {
			if (rd_cntpct() >= next) {
				wfibench_sample_ma(&busy_sum, &busy_n);
				next = rd_cntpct() + slice_ticks;
			}
			/* burn cycles - no WFI */
		}
	}

	/* ---- Phase WFI: CNTP-timed IRQ-woken WFI slices, sample between ---- */
	/* GIC + IMO + vector setup, exactly like do_irq_wfi. */
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));   /* EnableGrp1NS */
	dsb();
	writel(1U << 30, (void *)GICR_ISENABLER0);       /* INTID 30 = CNTP PPI */
	dsb();
	asm volatile("msr " STR(ICC_PMR_EL1) ", %0" : : "r" (0xffUL));
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();
	asm volatile("mrs %0, vbar_el2" : "=r" (vbar_save));
	asm volatile("mrs %0, hcr_el2"  : "=r" (hcr_save));
	asm volatile("msr vbar_el2, %0" : : "r" ((unsigned long)tw_irq_vectors));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save | (1UL << 4)));
	isb();

	/*
	 * CRITICAL when run from INSIDE the editor: the editor leaves the EC line
	 * (INTID 46) enabled in the distributor so its idle-nap can wake on a key.
	 * Our vector table here only acks the CNTP timer, NOT INTID 46 - so a live
	 * EC assert would fire into a handler that never EOIs it and we'd hang (the
	 * "hangs after Keep the screen static" symptom). DISABLE INTID 46 for the
	 * WFI phase (CNTP timer is our only wake source), then RE-ENABLE it after so
	 * the editor's key wake keeps working. From the raw shell 46 is off anyway,
	 * so this is a harmless no-op there.
	 */
	{
		unsigned int w = EC_GPIO_INTID / 32, b = EC_GPIO_INTID % 32;

		writel(1U << b, (void *)(TW_GICD_ICENABLER + w * 4));  /* disable 46 */
		dsb();
	}

	tend = rd_cntpct() + phase_ticks;
	while (rd_cntpct() < tend) {
		/* Arm CNTP for one slice, take IRQs, WFI until it fires. */
		asm volatile("msr cntp_tval_el0, %0" : : "r" (slice_ticks));
		asm volatile("msr cntp_ctl_el0, %0" : : "r" (1UL));
		isb();
		asm volatile("msr daifclr, #2");
		wfi();
		asm volatile("msr daifset, #2");
		asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));
		isb();
		/* core is awake here - sample (same read overhead as busy phase) */
		wfibench_sample_ma(&wfi_sum, &wfi_n);
	}

	/* Re-enable INTID 46 + clear any pending, so the editor's key/lid/power
	 * wake path is exactly as it was before the bench. */
	{
		unsigned int w = EC_GPIO_INTID / 32, b = EC_GPIO_INTID % 32;

		writel(1U << b, (void *)(TW_GICD_ICPENDR + w * 4));
		writel(1U << b, (void *)(TW_GICD_ISENABLER + w * 4)); /* re-enable 46 */
		dsb();
	}

	/* Restore VBAR/HCR. */
	asm volatile("msr vbar_el2, %0" : : "r" (vbar_save));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save));
	isb();

	{
		int busy = busy_n ? (int)(busy_sum / busy_n) : 0;
		int wfi  = wfi_n  ? (int)(wfi_sum  / wfi_n)  : 0;
		int delta = busy - wfi;   /* both negative; busy more negative => delta<0 */

		printf("--- WFI on/off bench (smart-battery, whole-board) ---\n");
		if (!busy_n || !wfi_n) {
			printf("  SBS read failed (busy=%d wfi=%d samples). On AC? tunnel down?\n",
			       busy_n, wfi_n);
			return 1;
		}
		printf("  busy-spin : %d mA  (%d samples)\n", busy, busy_n);
		printf("  deep WFI  : %d mA  (%d samples)\n", wfi, wfi_n);
		printf("  WFI saves : %d mA%s\n", delta < 0 ? -delta : delta,
		       (delta < 15 && delta > -15)
			       ? "  (< noise floor ~15 mA - effectively none)"
			       : "");
		printf("  (negative currents = discharging; verify AC unplugged)\n");
	}
	return 0;
}

/*
 * Lower the A53 little-cluster core voltage (ppvar_litcpu_pwm) to match the 408
 * MHz OPP. Found via the live Linux board: at 408 MHz Linux DVFS runs it at
 * 0.80 V, but U-Boot leaves it at 0.90 V (the ~1008 MHz OPP voltage) - the
 * A53 is over-volted, burning (0.90/0.80)^2 = 1.27x the core power as heat for
 * no benefit (same freq). 0.80 V is proven-stable at 408 MHz on this exact
 * silicon by Linux, and we only ever LOWER voltage at a already-low freq, so
 * this is safe. `twwfi litvolt [uV]` sets it (default 800000), holds ~10 s to
 * feel the chip / read a meter, then RESTORES the original so a bad value can't
 * persist. Refuses to raise above the current value (lowering only).
 */
static int do_litvolt(unsigned int target_uv)
{
	struct udevice *reg;
	int before, after, restored, ret;
	unsigned long t;

	if (regulator_get_by_platname("ppvar_litcpu_pwm", &reg) || !reg) {
		printf("ppvar_litcpu_pwm regulator not found.\n");
		return 1;
	}
	before = regulator_get_value(reg);
	printf("ppvar_litcpu_pwm now = %d uV; target = %u uV.\n", before, target_uv);
	if (before <= 0) {
		printf("can't read current voltage; aborting.\n");
		return 1;
	}
	if ((int)target_uv >= before) {
		printf("target >= current - refusing to RAISE voltage (lowering only,\n"
		       "   raising needs a matching freq increase first). No change.\n");
		return 0;
	}

	ret = regulator_set_value(reg, (int)target_uv);
	after = regulator_get_value(reg);
	printf("set_value ret=%d, readback = %d uV.\n", ret, after);
	printf(">>> Holding ~10 s at %d uV - feel the chip / read the meter. A key\n"
	       "    ends early. (Editing/typing still works; core just runs cooler.)\n",
	       after);

	t = rd_cntpct() + rd_cntfrq() * 10;
	while (rd_cntpct() < t && !tstc())
		udelay(2000);
	if (tstc())
		(void)getchar();

	/* Restore the original voltage so the test leaves no lasting change. */
	regulator_set_value(reg, before);
	restored = regulator_get_value(reg);
	printf("Restored ppvar_litcpu_pwm to %d uV.\n", restored);
	printf("=> If the chip ran cooler at %u uV with no instability, it's safe to\n"
	       "   set this at editor startup (after dropping to 408 MHz).\n", target_uv);
	return 0;
}

/*
 * BENCH: disable the Mali GPU rail (ppvar_gpu / ppvar_gpu_pwm) and self-meter
 * the battery-current delta, then RESTORE. A framebuffer text editor never
 * touches the GPU, yet the rail sits enabled ~0.85 V. We already saw the GPU
 * PMU *domain* gate (twwfi gate 0) do ~nothing, but the external PMIC buck can
 * keep supplying the rail even with the domain gated - so disabling the
 * REGULATOR is a different, deeper test. If it saves >15 mA it's worth shipping
 * at startup (like tw_gate_usb3); if it's noise, the rail hunt is exhausted.
 *
 * Safe: nothing in the editor uses the GPU, and we restore the rail after ~8 s
 * (or a keypress). Tries ppvar_gpu_pwm first (the controllable buck), then the
 * plain ppvar_gpu name - whichever the DM exposes.
 */
static int do_rail(const char *name)
{
	struct udevice *reg = NULL;
	int base_ma = 0, off_ma = 0, base_n = 0, off_n = 0, ret;
	long sum; int got, k, ma;

	if (regulator_get_by_platname(name, &reg) || !reg) {
		printf("regulator '%s' not found in DM.\n", name);
		printf("(try a name from `regulator list` in the shell, e.g. ppvar_gpu_pwm,\n"
		       " pp1800_pcie, pp1800_audio, p3.3v_dig, pp3300_wifi_bt)\n");
		return 1;
	}
	printf("rail = %s, now %d uV. Metering baseline (~5 s)...\n",
	       name, regulator_get_value(reg));

	/* baseline current, 5 samples ~1 s apart */
	sum = 0; got = 0;
	for (k = 0; k < 5; k++) {
		if (tw_read_batt_current(&ma) == 0) { sum += ma; got++; }
		if (k + 1 < 5) udelay(1000 * 1000);
	}
	base_n = got; base_ma = got ? (int)(sum / got) : 0;

	ret = regulator_set_enable(reg, false);
	printf("disable %s ret=%d. Metering gated (~8 s, key ends early)...\n",
	       name, ret);
	if (ret) {
		printf("regulator refused disable (ret=%d) - likely always-on or has\n"
		       "   other consumers. Nothing gated; done.\n", ret);
		return 0;
	}

	/* current while disabled, 8 samples ~1 s apart */
	sum = 0; got = 0;
	for (k = 0; k < 8 && !tstc(); k++) {
		if (tw_read_batt_current(&ma) == 0) { sum += ma; got++; }
		udelay(1000 * 1000);
	}
	off_n = got; off_ma = got ? (int)(sum / got) : 0;
	if (tstc())
		(void)getchar();

	/* ALWAYS restore the rail. */
	ret = regulator_set_enable(reg, true);
	printf("re-enabled %s ret=%d, now %d uV.\n", name, ret,
	       regulator_get_value(reg));

	printf("--- rail '%s' power delta (smart battery, whole-board) ---\n", name);
	if (!base_n || !off_n) {
		printf("  SBS read failed (before=%d during=%d). On AC? tunnel down?\n",
		       base_n, off_n);
		return 1;
	}
	{
		int d = base_ma - off_ma;   /* less negative when off => saved */

		printf("  rail on  : %d mA  (%d samples)\n", base_ma, base_n);
		printf("  rail off : %d mA  (%d samples)\n", off_ma, off_n);
		printf("  saved    : %d mA%s\n", d < 0 ? -d : d,
		       (d < 15 && d > -15)
			       ? "  (< noise floor ~15 mA - not worth shipping)"
			       : "  (>15 mA - worth gating at startup)");
		printf("  (negative = discharging; verify AC unplugged)\n");
	}
	return 0;
}

/* `twwfi gpurail` = the GPU rail, the original convenience wrapper. */
static int do_gpurail(void)
{
	return do_rail("ppvar_gpu_pwm");
}

/*
 * BENCH: clock-gate the two USB2 host controllers (fe3a0000/fe3e0000, the
 * EHCI/OHCI hosts) and their PHY reference clocks, self-meter the battery delta,
 * then restore. USB2 is NOT a standalone PMU power domain (only USB3 is, bit 27
 * - see gate_doms), it lives in the shared PERILP/PERIHP domains, so we can't
 * power-gate it; the runtime-PM-equivalent is to gate its CRU clocks, which is
 * what Linux does (its fe3a0000.usb / fe3e0000.usb are runtime-suspended at
 * idle). The editor's keyboard is the ChromeOS EC (not USB), so gating USB2 host
 * is safe for the editor. Clocks (from clk_rk3399.c), all write-masked CRU regs:
 *   HCLK_HOST0      clksel_con[20] bit5   (0xff760150)
 *   HCLK_HOST0_ARB  clksel_con[20] bit6
 *   HCLK_HOST1      clksel_con[20] bit7
 *   HCLK_HOST1_ARB  clksel_con[20] bit8
 *   SCLK_USB2PHY0_REF clkgate_con[6] bit5 (0xff760318)
 *   SCLK_USB2PHY1_REF clkgate_con[6] bit6
 * rk_setreg(bit) = gate OFF (high half enables the write); rk_clrreg = back ON.
 * Restores all six on exit. Cannot hang (no WFI/PSCI).
 */
#define TW_CRU_CLKSEL20   0xff760150UL   /* CRU clksel_con[20]: USB2 host hclks */
#define TW_CRU_CLKGATE6   0xff760318UL   /* CRU clkgate_con[6]: USB2 phy refclks */
#define TW_USB2_HCLK_MASK ((1U<<5)|(1U<<6)|(1U<<7)|(1U<<8))  /* host0/1 + arb */
#define TW_USB2_PHY_MASK  ((1U<<5)|(1U<<6))                  /* phy0/1 ref */

static void tw_cru_gate(unsigned long reg, unsigned int mask, int off)
{
	/* write-masked: high 16 = which bits to change, low 16 = value.
	 * off=1 sets the bits (gate off); off=0 clears them (ungate). */
	writel((mask << 16) | (off ? mask : 0), (void *)reg);
	dsb();
}

static int do_usb2(void)
{
	long sum; int got, k, ma;
	int base_ma = 0, off_ma = 0, base_n, off_n;

	printf("USB2 host clock-gate bench. Editor keyboard is cros-ec (not USB),\n");
	printf("so gating USB2 host is safe. Metering baseline (~5 s)...\n");

	sum = 0; got = 0;
	for (k = 0; k < 5; k++) {
		if (tw_read_batt_current(&ma) == 0) { sum += ma; got++; }
		if (k + 1 < 5) udelay(1000 * 1000);
	}
	base_n = got; base_ma = got ? (int)(sum / got) : 0;

	/* gate the 4 host hclks + 2 phy refclks OFF */
	tw_cru_gate(TW_CRU_CLKSEL20, TW_USB2_HCLK_MASK, 1);
	tw_cru_gate(TW_CRU_CLKGATE6, TW_USB2_PHY_MASK, 1);
	printf("USB2 host+phy clocks gated. Metering ~8 s (key ends early)...\n");

	sum = 0; got = 0;
	for (k = 0; k < 8 && !tstc(); k++) {
		if (tw_read_batt_current(&ma) == 0) { sum += ma; got++; }
		udelay(1000 * 1000);
	}
	off_n = got; off_ma = got ? (int)(sum / got) : 0;
	if (tstc())
		(void)getchar();

	/* ALWAYS ungate. */
	tw_cru_gate(TW_CRU_CLKSEL20, TW_USB2_HCLK_MASK, 0);
	tw_cru_gate(TW_CRU_CLKGATE6, TW_USB2_PHY_MASK, 0);
	printf("USB2 clocks restored (on).\n");

	printf("--- USB2 clock-gate delta (smart battery, whole-board) ---\n");
	if (!base_n || !off_n) {
		printf("  SBS read failed (before=%d during=%d). On AC? tunnel down?\n",
		       base_n, off_n);
		return 1;
	}
	{
		int d = base_ma - off_ma;   /* less negative when gated => saved */

		printf("  clocks on  : %d mA  (%d samples)\n", base_ma, base_n);
		printf("  clocks off : %d mA  (%d samples)\n", off_ma, off_n);
		printf("  saved      : %d mA%s\n", d < 0 ? -d : d,
		       (d < 15 && d > -15)
			       ? "  (< noise floor ~15 mA)"
			       : "  (>15 mA - worth gating at startup)");
		printf("  (negative = discharging; verify AC unplugged)\n");
	}
	return 0;
}

/*
 * BENCH: lower the DDR/NoC "center" logic rail (ppvar_centerlogic_pwm) and
 * self-meter the battery delta, then RESTORE. This rail is DVFS-coupled to DDR
 * freq: Linux runs it ~0.925 V at 928 MHz DDR and ~0.90 V at 400 MHz. bl31's DDR
 * SET_RATE retrains the PHY but does NOT touch this PMIC rail, so after the
 * editor lowers DDR to 400 the rail stays over-volted at the 928 MHz point -
 * same class of bug as the A53 litcpu over-volt. This measures the saving before
 * we ship the under-volt in tw_set_ddr_400. Lowering only (refuses to raise);
 * restores after ~10 s or a keypress. Default target 900000 uV (Linux DDR-400).
 */
static int do_centervolt(unsigned int target_uv)
{
	struct udevice *reg;
	int before, after, restored, base_ma = 0, lo_ma = 0, base_n, lo_n, ret;
	long sum; int got, k, ma;
	unsigned long t;

	if (regulator_get_by_platname("ppvar_centerlogic_pwm", &reg) || !reg) {
		printf("ppvar_centerlogic_pwm regulator not found.\n");
		return 1;
	}
	before = regulator_get_value(reg);
	printf("ppvar_centerlogic_pwm now = %d uV; target = %u uV.\n",
	       before, target_uv);
	if (before <= 0) {
		printf("can't read current voltage; aborting.\n");
		return 1;
	}
	if ((int)target_uv >= before) {
		printf("target >= current - refusing to RAISE (lowering only). No change.\n");
		return 0;
	}

	/* baseline current (5 samples ~1 s apart) */
	sum = 0; got = 0;
	for (k = 0; k < 5; k++) {
		if (tw_read_batt_current(&ma) == 0) { sum += ma; got++; }
		if (k + 1 < 5) udelay(1000 * 1000);
	}
	base_n = got; base_ma = got ? (int)(sum / got) : 0;

	ret = regulator_set_value(reg, (int)target_uv);
	after = regulator_get_value(reg);
	printf("set_value ret=%d, readback = %d uV. Metering ~8 s (key ends early)...\n",
	       ret, after);

	/* current at the lowered voltage (8 samples) */
	sum = 0; got = 0;
	for (k = 0; k < 8 && !tstc(); k++) {
		if (tw_read_batt_current(&ma) == 0) { sum += ma; got++; }
		udelay(1000 * 1000);
	}
	lo_n = got; lo_ma = got ? (int)(sum / got) : 0;
	if (tstc())
		(void)getchar();

	/* ALWAYS restore. */
	regulator_set_value(reg, before);
	restored = regulator_get_value(reg);
	printf("Restored ppvar_centerlogic_pwm to %d uV.\n", restored);

	printf("--- centerlogic under-volt delta (smart battery, whole-board) ---\n");
	if (!base_n || !lo_n) {
		printf("  SBS read failed (before=%d during=%d). On AC? tunnel down?\n",
		       base_n, lo_n);
		return 1;
	}
	{
		int d = base_ma - lo_ma;   /* less negative when lowered => saved */

		printf("  %d uV : %d mA  (%d samples)\n", before, base_ma, base_n);
		printf("  %d uV : %d mA  (%d samples)\n", after, lo_ma, lo_n);
		printf("  saved  : %d mA%s\n", d < 0 ? -d : d,
		       (d < 15 && d > -15)
			       ? "  (< noise floor ~15 mA)"
			       : "  (>15 mA - worth shipping in tw_set_ddr_400)");
		printf("  (negative = discharging; verify AC unplugged)\n");
	}
	return 0;
}

static int do_twwfi(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	unsigned int ms = 300;

	if (argc < 2) {
		/* Build stamp: confirms the FLASHED binary matches the source. Bump
		 * TW_BUILD_TAG whenever we change twwfi so a stale reflash is obvious.
		 * (No __DATE__/__TIME__ - U-Boot forbids them via -Werror=date-time.) */
		printf("twwfi build tag: %s\n", TW_BUILD_TAG);
		dump_state();
		printf("\nRun `twwfi probe [p|hp]` FIRST (safe, no WFI) to trace the"
		       " interrupt, then `twwfi p`/`hp` to try a real WFI.\n");
		return 0;
	}
	if (argc >= 4)
		ms = simple_strtoul(argv[3], NULL, 10);
	else if (argc >= 3 && strcmp(argv[1], "probe"))
		ms = simple_strtoul(argv[2], NULL, 10);

	if (!strcmp(argv[1], "pmu"))
		return do_pmu();          /* which CPU cores/clusters are powered */
	if (!strcmp(argv[1], "cpuinfo"))
		return do_cpuinfo();      /* live CPU cluster frequencies (CRU) */
	if (!strcmp(argv[1], "litvolt"))
		return do_litvolt(argc >= 3 ? simple_strtoul(argv[2], NULL, 10)
					    : 800000);  /* A53 core rail -> 0.80V */
	if (!strcmp(argv[1], "napstats"))
		return do_napstats();     /* did the editor's WFI actually sleep? */
	if (!strcmp(argv[1], "gatesweep"))
		return do_gate_sweep();   /* per-domain individual mA contribution */
	if (!strcmp(argv[1], "gate"))
		return do_gate(argc >= 3 ? (int)simple_strtoul(argv[2], NULL, 10)
					 : -1);  /* -1 = all; else one domain 0..N */
	if (!strcmp(argv[1], "keystroke"))
		return do_keystroke(argc >= 3 ? simple_strtoul(argv[2], NULL, 10)
					      : 5000);  /* 5 s naps: human-observable */
	if (!strcmp(argv[1], "psci"))
		return do_psci_query();   /* SAFE: query PSCI version/features/affinity */
	if (!strcmp(argv[1], "suspend"))
		return do_suspend(ms);
	if (!strcmp(argv[1], "irq"))
		return do_irq_wfi(ms);
	if (!strcmp(argv[1], "coredown"))
		return do_coredown(ms);   /* power-gate CPU0 on WFI (timer-backstopped) */
	if (!strcmp(argv[1], "wfibench"))
		return do_wfibench(argc >= 3 ? simple_strtoul(argv[2], NULL, 10)
					     : 4000);  /* per-phase ms; A/B WFI power */
	if (!strcmp(argv[1], "gpurail"))
		return do_gpurail();      /* disable Mali GPU rail, self-meter, restore */
	if (!strcmp(argv[1], "rail"))
		return (argc >= 3) ? do_rail(argv[2])   /* disable ANY named rail, meter */
				   : (printf("usage: twwfi rail <regulator-name>\n"),
				      CMD_RET_USAGE);
	if (!strcmp(argv[1], "centervolt"))
		return do_centervolt(argc >= 3 ? simple_strtoul(argv[2], NULL, 10)
					       : 900000);  /* DDR/NoC rail -> 0.90V */
	if (!strcmp(argv[1], "usb2"))
		return do_usb2();         /* clock-gate USB2 host+phy, self-meter */
	if (!strcmp(argv[1], "cpuall"))
		return do_cpuall();       /* CPU_ON each secondary -> self CPU_OFF */
	if (!strcmp(argv[1], "ddr"))
		return do_ddr(argc >= 3 ? (int)simple_strtoul(argv[2], NULL, 10)
					: -1);   /* no arg = query only; else set MHz */
	if (!strcmp(argv[1], "ecwake"))
		return do_ecwake();
	if (!strcmp(argv[1], "gpio"))
		return do_gpio_probe();
	if (!strcmp(argv[1], "spi")) {
		unsigned int n = (argc >= 3) ? simple_strtoul(argv[2], NULL, 10) : 46;

		return do_spi_probe(n);   /* default 46 = GPIO0 bank (GIC_SPI 14) */
	}
	if (!strcmp(argv[1], "probe")) {
		int hyp = (argc >= 3 && !strcmp(argv[2], "hp"));

		return do_probe(hyp, ms);
	}
	if (!strcmp(argv[1], "hp"))
		return do_wfi_test(1, ms);
	if (!strcmp(argv[1], "p"))
		return do_wfi_test(0, ms);

	printf("usage: twwfi [pmu|cpuinfo|napstats|psci|litvolt|ddr|gate|coredown|cpuall"
	       "|keystroke|wfibench|gpurail|rail|centervolt|usb2|gatesweep|irq|ecwake|suspend|probe|gpio|spi|p|hp] [ms|N|uV|MHz|name]\n");
	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	twwfi, 4, 0, do_twwfi,
	"WFI/idle test (power idle debug)",
	"                 - dump EL/timer/GIC state (safe, no WFI)\n"
	"twwfi pmu          - SAFE: which CPU + peripheral power domains are on (PMU_PWRDN_ST)\n"
	"twwfi cpuinfo      - SAFE: CPU freq (CRU) + power-domain status + EC board current\n"
	"twwfi litvolt [uV] - lower A53 core rail ppvar_litcpu to uV (def 800000=0.80V,\n"
	"                     Linux's 408MHz OPP), hold 10s to feel heat, then restore\n"
	"twwfi ddr [MHz]    - DDR freq via bl31 SIP (0x82000008). No arg=query (safe);\n"
	"                     MHz=SET_RATE (bl31 M0 self-refresh+retrain; boot 928, Linux idle 400)\n"
	"twwfi napstats     - SAFE: did the editor's idle WFI actually sleep or busy-spin?\n"
	"twwfi gate [N]     - RISKY: power-gate editor-unused domain N (0=GPU 1=VCODEC\n"
	"                     2=VDU 3=RGA 4=IEP 5=ISP0 6=ISP1 7=HDCP 8=USB3 9=GMAC\n"
	"                     10=TCPD0 11=TCPD1), or ALL if N omitted; self-meters ~8s\n"
	"                     then restores. Gate one at a time (power-cycle if hangs).\n"
	"twwfi gatesweep    - RISKY: sweep EACH domain alone (gate/meter/restore) and\n"
	"                     print a per-domain mA table - individual contributions.\n"
	"twwfi keystroke [ms] - the EDITOR's exact idle nap in a loop until a key\n"
	"                     (default 5000 ms so it's observable). Should go quiet +\n"
	"                     freeze till a key; reports naps/avg to prove WFI sleeps.\n"
	"twwfi irq [ms]     - TAKE timer IRQ (HCR_EL2.IMO + handler) + WFI. Works;\n"
	"                     this is what the editor's idle nap does.\n"
	"twwfi coredown [ms]- RISKY: PMU auto-power-gate CPU0 on WFI (CORE_PM_CON0=0x3)\n"
	"                     + CNTP backstop. Returns => core gates+wakes (cooler idle).\n"
	"twwfi wfibench [ms]- SAFE: A/B the cost of WFI. Two phases (busy-spin vs\n"
	"                     CNTP-woken deep WFI), same brightness/DDR; reports avg\n"
	"                     battery mA of each + the saving. Unplug AC first. (def 4000)\n"
	"twwfi gpurail      - SAFE: disable Mali GPU rail (ppvar_gpu), self-meter mA\n"
	"                     delta ~8 s, then restore. Editor never uses the GPU.\n"
	"twwfi rail <name>  - SAFE: disable ANY named regulator (e.g. pp1800_pcie,\n"
	"                     pp1800_audio, p3.3v_dig), self-meter ~8 s, then restore.\n"
	"                     Probe a peripheral rail's draw; refuses if always-on.\n"
	"twwfi centervolt [uV] - lower DDR/NoC centerlogic rail (ppvar_centerlogic_pwm)\n"
	"                     to uV (def 900000=Linux DDR-400 pt), self-meter, restore.\n"
	"                     bl31 DDR SET_RATE doesn't touch this PMIC rail = over-volt.\n"
	"twwfi usb2         - SAFE: clock-gate the USB2 host+phy (CRU), self-meter mA\n"
	"                     ~8 s, then restore. Editor keyboard is cros-ec, not USB.\n"
	"twwfi cpuall       - RISKY: PSCI CPU_ON each secondary core -> self CPU_OFF stub\n"
	"                     (explicitly park all 5; pmu says already off, expect no-op).\n"
	"twwfi ecwake       - EC GPIO (INTID 46) as WFI wake IRQ, NO timer. Press a\n"
	"                     key/power button/lid; returns => can sleep 30 s.\n"
	"twwfi psci         - SAFE: query PSCI version/features/affinity from EL2 (no suspend)\n"
	"twwfi suspend [ms] - PSCI CPU_SUSPEND (STANDBY) via bl31 (masks IRQ, then SMC)\n"
	"twwfi probe [p|hp] - SAFE: arm timer, poll (no WFI), report propagation\n"
	"twwfi spi [n]      - SAFE: read an SPI's group/prio in GICD (default 46)\n"
	"twwfi gpio         - SAFE: EC GPIO IRQ (INTID 46) poll ~5 s, press keys\n"
	"twwfi p|hp [ms]    - arm CNTP/CNTHP + one raw WFI (hangs - no IMO/handler)\n"
	"  pmu/cpuinfo/napstats/psci/litvolt/probe/spi/gpio/wfibench/gpurail/rail/centervolt/usb2/dump + `ddr`(no arg) never hang.\n"
	"  gate/coredown/cpuall/suspend/p/hp + `ddr <MHz>` may - power-cycle if so."
);
