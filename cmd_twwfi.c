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
#include <asm/io.h>
#include <asm/gic.h>
#include <time.h>
#include <u-boot/schedule.h>
#include <linux/delay.h>
#include <linux/psci.h>
#include <linux/string.h>
#include <linux/kernel.h>  /* ARRAY_SIZE */
#include <cros_ec.h>       /* host-event flags: power button / lid wake */

/* PSCI invoke_psci_fn is provided by drivers/firmware/psci.c (probe it first
 * with uclass_get_device_by_name(UCLASS_FIRMWARE,"psci",...) as poweroff does).
 * We only need CPU_SUSPEND here. */
#include <dm.h>
#include <dm/uclass.h>
unsigned long invoke_psci_fn(unsigned long, unsigned long, unsigned long,
			     unsigned long);

/* CPU0 GICv3 redistributor SGI_base = 0xfef10000 on rk3399 (verified via
 * md/mw). Standard GICv3 SGI-frame register offsets from SGI_base. */
#define GICR_IGROUPR0     0xfef10080UL   /* +0x080: interrupt group  */
#define GICR_ISENABLER0   0xfef10100UL   /* +0x100: set-enable   */
#define GICR_ISPENDR0     0xfef10200UL   /* +0x200: set-pending  */
#define GICR_ICPENDR0     0xfef10280UL   /* +0x280: clear-pending */
#define GICR_IPRIORITYR   0xfef10400UL   /* +0x400: priority (byte/INTID) */
#define GICR_IGRPMODR0    0xfef10d00UL   /* +0xD00: group modifier */

/* GIC Distributor (GICD) base - for SPIs (INTID >= 32). Register arrays are
 * indexed by INTID: IGROUPR/IGRPMODR are 1 bit/INTID, IPRIORITYR 1 byte/INTID,
 * ISENABLER 1 bit/INTID. */
#define GICD_BASE         0xfee00000UL
#define GICD_CTLR         (GICD_BASE + 0x0000)
#define GICD_IGROUPR      (GICD_BASE + 0x0080)
#define GICD_ISENABLER    (GICD_BASE + 0x0100)
#define GICD_IPRIORITYR   (GICD_BASE + 0x0400)
#define GICD_IROUTER      (GICD_BASE + 0x6000)   /* 64-bit/INTID, affinity route */
#define GICD_IGRPMODR     (GICD_BASE + 0x0D00)
#define GICD_ISPENDR      (GICD_BASE + 0x0200)   /* 1 bit/INTID, pending state */
#define GICD_ICPENDR      (GICD_BASE + 0x0280)   /* 1 bit/INTID, clear-pending */

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
	printf("GICR_IPRIORITYR(30) byte = 0x%02x\n",
	       readl((void *)(GICR_IPRIORITYR + (30 & ~3))) >> ((30 & 3) * 8)
	       & 0xff);
	printf("GICR_IPRIORITYR(26) byte = 0x%02x\n",
	       readl((void *)(GICR_IPRIORITYR + (26 & ~3))) >> ((26 & 3) * 8)
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
	 *  - GICD_CTLR.EnableGrp1NS (bit1): the DISTRIBUTOR forwards NS Group1 at
	 *    all. bl31 leaves this CLEAR (GICD_CTLR=0x35), which is why HPPIR1 was
	 *    1023 before - the missing piece. (INTID 30 is Group1-NS, NOT secure:
	 *    bl31 only secures INTID 29, the EL3 timer.)
	 *  - ICC_IGRPEN1_EL1 (CPU interface Group1 enable)
	 *  - the timer PPI in the redistributor. */
	setbits_le32((void *)GICD_CTLR, (1U << 1));   /* EnableGrp1NS */
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
		unsigned int gicd_ctlr = readl((void *)0xfee00000UL); /* GICD_CTLR */

		asm volatile("mrs %0, " STR(ICC_IGRPEN1_EL1) : "=r" (igrpen1));
		asm volatile("mrs %0, " STR(ICC_RPR_EL1) : "=r" (rpr));
		printf("  GICD_CTLR(rb)  = 0x%08x (bit1 EnableGrp1NS=%u <- the fix)\n",
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
	 * GICD_CTLR.EnableGrp1NS - which is why `twwfi p` still froze while `probe`
	 * showed HPPIR1=30. Now they match.
	 *   1. GICD_CTLR.EnableGrp1NS (bit1) - distributor forwards NS Group1
	 *   2. the timer PPI in the redistributor
	 *   3. ICC_IGRPEN1_EL1 - CPU interface Group1 enable
	 */
	setbits_le32((void *)GICD_CTLR, (1U << 1));   /* EnableGrp1NS */
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
	unsigned int grp  = (readl((void *)(GICD_IGROUPR  + word * 4)) >> bit) & 1;
	unsigned int gmod = (readl((void *)(GICD_IGRPMODR + word * 4)) >> bit) & 1;
	unsigned int en   = (readl((void *)(GICD_ISENABLER + word * 4)) >> bit) & 1;
	unsigned int prio = (readl((void *)(GICD_IPRIORITYR + (intid & ~3)))
			     >> ((intid & 3) * 8)) & 0xff;
	unsigned int ctlr = readl((void *)GICD_CTLR);
	unsigned long route;

	/* IROUTER is 64-bit per INTID; read the low word (affinity 0-2). */
	route = readl((void *)(GICD_IROUTER + (unsigned long)intid * 8));

	printf("SPI INTID %u (GIC_SPI %u) in the distributor:\n",
	       intid, intid - 32);
	printf("  GICD_CTLR   = 0x%08x (bit6 DS=%u)\n", ctlr, (ctlr >> 6) & 1);
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
 *   then loop ~5 s polling GPIO int_status / GICD_ISPENDR(46) / ICC_HPPIR1
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
	writel(1U << bit, (void *)(GICD_ISENABLER + word * 4));
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();
	dsb();

	tend = rd_cntpct() + rate * 5;   /* ~5 seconds */
	while (rd_cntpct() < tend) {
		unsigned int st  = readl((void *)GPIO_INT_STATUS) & EC_PA1_BIT;
		unsigned int pnd = (readl((void *)(GICD_ISPENDR + word * 4))
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
			writel(1U << bit, (void *)(GICD_ICPENDR + word * 4));
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

	printf("PSCI CPU_SUSPEND (STANDBY), timer wake in %u ms...\n", ms);

	/* Arm the NS timer as the wake source (same GIC enables as the probe). */
	setbits_le32((void *)GICD_CTLR, (1U << 1));    /* EnableGrp1NS */
	dsb();
	writel(1U << 30, (void *)GICR_ISENABLER0);     /* INTID 30 = CNTP PPI */
	dsb();
	asm volatile("msr " STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();
	asm volatile("msr cntp_tval_el0, %0" : : "r" (ticks));
	asm volatile("msr cntp_ctl_el0, %0" : : "r" (1UL));   /* ENABLE, IMASK=0 */
	isb();

	t0 = rd_cntpct();

	/* power_state = 0: STANDBY, pwr level 0, state id 0. entry/ctx unused for
	 * standby (CPU keeps state and resumes right here). */
	ret = invoke_psci_fn(PSCI_0_2_FN64_CPU_SUSPEND, 0, 0, 0);

	t1 = rd_cntpct();

	asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));   /* disable timer */
	isb();

	printf("CPU_SUSPEND RETURNED. ret=%ld, elapsed=%lu ms\n",
	       (long)ret, (t1 - t0) / (rate / 1000));
	printf("=> If elapsed ~%u ms, PSCI standby idle WORKS - use it for idle.\n",
	       ms);
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
	setbits_le32((void *)GICD_CTLR, (1U << 1));   /* EnableGrp1NS */
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
	setbits_le32((void *)GICD_CTLR, (1U << 1));
	dsb();
	writel(1U << bit, (void *)(GICD_ISENABLER + word * 4));
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
	setbits_le32((void *)GICD_CTLR, (1U << 1));
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
	writel(1U << bit, (void *)(GICD_ISENABLER + word * 4));
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
		writel(1U << bit, (void *)(GICD_ICPENDR + word * 4)); /* SPI 46 */
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
};

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
	unsigned int busm = 1U << d->bus, pdm = 1U << d->pd;

	printf("  %-6s: bus-idle req...", d->name);
	setbits_le32((void *)PMU_BUS_IDLE_REQ, busm);
	dsb();
	if (gate_wait(PMU_BUS_IDLE_ST, busm, busm) ||
	    gate_wait(PMU_BUS_IDLE_ACK, busm, busm)) {
		printf(" TIMEOUT (bus won't idle) - aborting this domain, backing out.\n");
		clrbits_le32((void *)PMU_BUS_IDLE_REQ, busm);
		dsb();
		return -1;
	}
	printf(" idle; power off...");
	setbits_le32((void *)PMU_PWRDN_CON, pdm);
	dsb();
	if (gate_wait(PMU_PWRDN_ST, pdm, pdm)) {
		printf(" PWRDN TIMEOUT - backing out.\n");
		clrbits_le32((void *)PMU_PWRDN_CON, pdm);
		dsb();
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
	unsigned int busm = 1U << d->bus, pdm = 1U << d->pd;

	clrbits_le32((void *)PMU_PWRDN_CON, pdm);
	dsb();
	gate_wait(PMU_PWRDN_ST, pdm, 0);
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
static int do_gate(int idx)
{
	int lo = 0, hi = ARRAY_SIZE(gate_doms), i, off_ok = 0;
	unsigned long t;

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
	       idx >= 0 ? gate_doms[idx].name : "ALL 10 unused domains",
	       readl((void *)PMU_PWRDN_ST));
	printf("If the prompt does NOT return, the domain being gated wedged the SoC"
	       " - power-cycle.\n");

	for (i = lo; i < hi; i++)
		if (!gate_off(&gate_doms[i]))
			off_ok++;

	printf("PWRDN_ST after  = 0x%08x  (%d domain(s) gated off)\n",
	       readl((void *)PMU_PWRDN_ST), off_ok);
	printf(">>> Holding ~15 s - READ THE POWER METER NOW. Press a key to end"
	       " early.\n");

	t = rd_cntpct() + rd_cntfrq() * 15;
	while (rd_cntpct() < t && !tstc())
		udelay(1000);
	if (tstc())
		(void)getchar();

	printf("Restoring...\n");
	for (i = hi - 1; i >= lo; i--)
		gate_on(&gate_doms[i]);

	printf("PWRDN_ST restored = 0x%08x\n", readl((void *)PMU_PWRDN_ST));
	printf("=> Compare the meter delta while gated. If a domain froze the board,"
	       " it's the last one printed before the hang.\n");
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
 * SAFE (read-only): decode the live CPU cluster frequencies from the CRU. CPU0
 * (the typewriter's core) is the A53 little cluster off APLL_L. Confirms what a
 * frequency change (e.g. the editor's 408 MHz set) actually took effect. Only
 * reads CRU registers; cannot hang.
 */
static int do_cpuinfo(void)
{
	printf("Current EL = %u, CNTFRQ = %lu Hz\n", current_el(), rd_cntfrq());
	printf("--- little cluster (CPU0 = A53, APLL_L) ---\n");
	cpu_cluster_mhz(CRU_APLL_L_CON, CRU_CLKSEL_CON0, "CPUL", 0);
	printf("--- big cluster (A72, APLL_B) ---\n");
	cpu_cluster_mhz(CRU_APLL_B_CON, CRU_CLKSEL_CON2, "CPUB", 1);
	printf("=> CPU0 (the core the typewriter runs on) is the CPUL figure above.\n");
	printf("   `twwfi pmu` shows which cores are actually powered.\n");
	return 0;
}

static int do_twwfi(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	unsigned int ms = 300;

	if (argc < 2) {
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
	if (!strcmp(argv[1], "gate"))
		return do_gate(argc >= 3 ? (int)simple_strtoul(argv[2], NULL, 10)
					 : -1);  /* -1 = all 10; else one domain 0..9 */
	if (!strcmp(argv[1], "keystroke"))
		return do_keystroke(argc >= 3 ? simple_strtoul(argv[2], NULL, 10)
					      : 5000);  /* 5 s naps: human-observable */
	if (!strcmp(argv[1], "suspend"))
		return do_suspend(ms);
	if (!strcmp(argv[1], "irq"))
		return do_irq_wfi(ms);
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

	printf("usage: twwfi [pmu|cpuinfo|gate|keystroke|irq|ecwake|suspend|probe"
	       "|gpio|spi|p|hp] [ms|N]\n");
	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	twwfi, 4, 0, do_twwfi,
	"WFI/idle test (power idle debug)",
	"                 - dump EL/timer/GIC state (safe, no WFI)\n"
	"twwfi pmu          - SAFE: which CPU + peripheral power domains are on (PMU_PWRDN_ST)\n"
	"twwfi cpuinfo      - SAFE: live CPU cluster frequencies from the CRU (APLL_L/B)\n"
	"twwfi gate [N]     - RISKY: power-gate editor-unused domain N (0=GPU 1=VCODEC\n"
	"                     2=VDU 3=RGA 4=IEP 5=ISP0 6=ISP1 7=HDCP 8=USB3 9=GMAC), or\n"
	"                     ALL if N omitted; hold 15s to meter, then restore. Gate one\n"
	"                     at a time to find which wedges (power-cycle if it hangs).\n"
	"twwfi keystroke [ms] - the EDITOR's exact idle nap in a loop until a key\n"
	"                     (default 5000 ms so it's observable). Should go quiet +\n"
	"                     freeze till a key; reports naps/avg to prove WFI sleeps.\n"
	"twwfi irq [ms]     - TAKE timer IRQ (HCR_EL2.IMO + handler) + WFI. Works;\n"
	"                     this is what the editor's idle nap does.\n"
	"twwfi ecwake       - EC GPIO (INTID 46) as WFI wake IRQ, NO timer. Press a\n"
	"                     key/power button/lid; returns => can sleep 30 s.\n"
	"twwfi suspend [ms] - PSCI CPU_SUSPEND (STANDBY) via bl31 (HANGS - evidence)\n"
	"twwfi probe [p|hp] - SAFE: arm timer, poll (no WFI), report propagation\n"
	"twwfi spi [n]      - SAFE: read an SPI's group/prio in GICD (default 46)\n"
	"twwfi gpio         - SAFE: EC GPIO IRQ (INTID 46) poll ~5 s, press keys\n"
	"twwfi p|hp [ms]    - arm CNTP/CNTHP + one raw WFI (hangs - no IMO/handler)\n"
	"  pmu/cpuinfo/probe/spi/gpio/dump never hang. gate/suspend/p/hp may - power-cycle if so."
);
