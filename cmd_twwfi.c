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
#include <linux/delay.h>
#include <linux/string.h>

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

	/* Enable the chosen PPI in the redistributor (write-1-to-set). */
	writel(1U << intid, (void *)GICR_ISENABLER0);
	dsb();

	/*
	 * THE FIX (found via the state dump): ICC_IGRPEN1 was 0, i.e. Group 1
	 * interrupts were DISABLED at the GIC CPU interface, so a pending INTID 30
	 * was never signalled to the PE and WFI never woke. Enable Group 1 here.
	 * (ICC_SRE=0xf, ICC_PMR=0xf8 > priority 0x80, INTID 30 is Group1-NS - all
	 * already fine per the dump, so this one bit is the whole fix.)
	 */
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

	printf("usage: twwfi [probe [p|hp]] [gpio] [spi [intid]] [p|hp] [ms]\n");
	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	twwfi, 4, 0, do_twwfi,
	"WFI-wake test (timer idle debug)",
	"                 - dump EL/timer/GIC state (safe, no WFI)\n"
	"twwfi probe [p|hp] - SAFE: arm timer, poll (no WFI), report propagation\n"
	"twwfi spi [intid]  - SAFE: read an SPI's group/prio in GICD (default 46)\n"
	"twwfi gpio         - SAFE: set up the EC GPIO IRQ (INTID 46), poll ~5 s\n"
	"                     while you press keys/button/lid; no WFI. Does it reach\n"
	"                     the PE (HPPIR1==46)? If yes, keypress-WFI is viable.\n"
	"twwfi hp [ms]    - arm CNTHP (EL2 timer, INTID 26) + one WFI\n"
	"twwfi p  [ms]    - arm CNTP  (EL1 timer, INTID 30) + one WFI\n"
	"  If a WFI test hangs, power-cycle. probe/spi/gpio/dump never hang."
);
