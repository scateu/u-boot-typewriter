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
	 * CRITICAL: arm with IMASK=0. The timer only ASSERTS its interrupt when
	 * ENABLE=1 && ISTATUS=1 && IMASK=0. With IMASK=1 (our earlier bug) the
	 * output is masked at the source, so nothing ever becomes pending and WFI
	 * never wakes. We keep PSTATE.I MASKED (DAIF.I=1) so the fired interrupt
	 * WAKES wfi but is not TAKEN as an exception (U-Boot's do_irq panics), then
	 * we disable the timer to drop the assertion.
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

static int do_twwfi(struct cmd_tbl *cmdtp, int flag, int argc,
		    char *const argv[])
{
	unsigned int ms = 300;

	if (argc < 2) {
		dump_state();
		printf("\nRun `twwfi hp` (EL2 timer) or `twwfi p` (EL1 timer) to"
		       " test a single WFI.\n");
		return 0;
	}
	if (argc >= 3)
		ms = simple_strtoul(argv[2], NULL, 10);

	if (!strcmp(argv[1], "hp"))
		return do_wfi_test(1, ms);
	if (!strcmp(argv[1], "p"))
		return do_wfi_test(0, ms);

	printf("usage: twwfi [hp|p] [ms]\n");
	return CMD_RET_USAGE;
}

U_BOOT_CMD(
	twwfi, 3, 0, do_twwfi,
	"WFI-wake test (timer idle debug)",
	"            - dump EL/timer/GIC state (safe, no WFI)\n"
	"twwfi hp [ms] - arm CNTHP (EL2 timer, INTID 26) + one WFI\n"
	"twwfi p  [ms] - arm CNTP  (EL1 timer, INTID 30) + one WFI\n"
	"  If the prompt returns, that timer wakes WFI. If it hangs, power-cycle."
);
