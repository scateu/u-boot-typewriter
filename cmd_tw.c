// SPDX-License-Identifier: GPL-2.0+
/*
 * cmd_tw.c - the `typewriter` command: a Nano-style, modeless, full-screen
 * editor on U-Boot's video framebuffer, with a built-in Wubi 86 IME.
 *
 * This file owns the U_BOOT_CMD entry point, the key reader (cooked ASCII plus
 * arrow/nav escape sequences), the editing primitives over the codepoint line
 * model, the Ctrl-key command dispatch, the Wubi composition state machine, and
 * the bottom-line prompts (^S save, ^W where-is, ^X exit-if-modified).
 *
 * Rendering, framebuffer setup, and the embedded glyph/table data live in the
 * sibling files (cmd_tw_video.c, cmd_tw_fs.c, ime_table.c, font_data.c,
 * wubi_embed.c).
 */
#include <command.h>
#include <stdio.h>
#include <vsprintf.h>
#include <u-boot/schedule.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/psci.h>
#include <irq_func.h>
#include <dm.h>
#include <cros_ec.h>
#include <asm/system.h>
#include <time.h>
#if defined(CONFIG_ARM64) && defined(__aarch64__)
#include <asm/io.h>       /* readl/writel/setbits_le32 for the WFI GIC setup */
#include <asm/gic.h>      /* ICC_* sysreg encodings */
#define _TW_STR(x) #x
#define TW_STR(x)  _TW_STR(x)
#endif
#include "cmd_tw.h"
#include "wubi_embed.h"

/* --- from cmd_tw_fs.c --- */
int tw_fs_probe(struct tw_fs *fs, const char *iftype, const char *dev_part,
		const char *fstype);
int tw_file_load(struct tw_state *s, const char *path);
int tw_file_save(struct tw_state *s);
int tw_list_files(struct tw_state *s);

/* --- from cmd_tw_video.c --- */
int  tw_video_init(struct tw_state *s);
void tw_render(struct tw_state *s);
int  tw_cp_cols(u32 cp);
int  tw_line_rows(struct tw_state *s, int fr);
int  tw_backlight_set(int pct);   /* set brightness 0..100, returns applied */
int  tw_backlight_step(int delta);/* step by delta (+/-), returns applied */

static struct tw_state g_tw;

/* ------------------------------------------------------------ WFI idle --- */
/*
 * Low-power idle = real WFI deep sleep, woken by an actually-TAKEN NonSecure
 * physical IRQ - the same way Linux idles this board. Two wake sources are
 * armed: the CNTP timer (INTID 30) as a battery backstop, and the cros_ec line
 * (gpio0 PA1 → INTID 46) which fires for a key / power button / lid.
 *
 * The long investigation (see POWERSAVE.md + the `twwfi` command) landed here:
 * WFI wakes only if the IRQ is actually TAKEN, which needs three things U-Boot
 * doesn't set up by default -
 *   1. HCR_EL2.IMO = 1     - route physical IRQ to EL2 (U-Boot only sets AMO,
 *                            so IRQs targeted EL1 and our EL2 WFI never woke);
 *   2. the GIC enabled for NS Group1 (GICD_CTLR.EnableGrp1NS + ICC_IGRPEN1_EL1
 *      + the CNTP PPI + INTID 46 + ICC_PMR open);
 *   3. an EL2 IRQ handler that acks (ICC_IAR1) + EOIs (ICC_EOIR1) and silences
 *      the source (disable CNTP; mask gpio0 PA1 - the EC line is level-held).
 * Then: arm CNTP (IMASK=0), unmask PSTATE.I, WFI - the taken IRQ wakes it.
 * bl31 leaves IRQs un-routed to EL3 in normal running, so taking them at EL2
 * works. (Verified: `twwfi irq`/`twwfi keystroke` return on the armed event.)
 */
#if defined(CONFIG_ARM64) && defined(__aarch64__)
#define TW_GICD_CTLR        0xfee00000UL      /* GIC distributor control */
#define TW_GICD_ISENABLER   0xfee00100UL      /* GICD set-enable (SPIs, 1 bit/INTID) */
#define TW_GICD_ICPENDR     0xfee00280UL      /* GICD clear-pending (1 bit/INTID) */
#define TW_GICR_ISENABLER0  0xfef10100UL      /* CPU0 redistributor SGI frame */
#define TW_GICR_ICPENDR0    0xfef10280UL      /* CPU0 redist clear-pending (PPIs) */

/*
 * The cros_ec interrupt line (gpio0 PA1, "ec-interrupt" in the DT) -> GIC_SPI 14
 * = INTID 46. The EC asserts it for ANY event: a key scan (MKBP), the power
 * button, or a lid-close. We enable it as a WFI wake source so a keypress wakes
 * the idle WFI instantly (and power/lid wake it too - both are handled by the
 * host-event poll on the very next loop iteration). GPIO0 v1 controller regs:
 */
#define TW_GPIO0_BASE       0xff720000UL
#define TW_GPIO_INTEN       (TW_GPIO0_BASE + 0x30)
#define TW_GPIO_INTMASK     (TW_GPIO0_BASE + 0x34)
#define TW_GPIO_INTTYPE     (TW_GPIO0_BASE + 0x38)   /* 0=level, 1=edge */
#define TW_GPIO_INT_POL     (TW_GPIO0_BASE + 0x3c)   /* 0=active-low */
#define TW_GPIO_PORTA_EOI   (TW_GPIO0_BASE + 0x4c)
#define TW_EC_PA1_BIT       (1U << 1)                /* PA1 = ec-interrupt pin */
#define TW_EC_GPIO_INTID    46                       /* GPIO0 bank = GIC_SPI 14 */

/*
 * Minimal EL2 vector table. Only the Current-EL-SPx IRQ slot (offset 0x280) has
 * a handler; it acks the pending interrupt, silences BOTH possible wake sources
 * (disables CNTP so the timer de-asserts; masks gpio0 PA1 so the level-triggered
 * EC line stops asserting), EOIs, and returns. Every other slot just eret (we
 * only ever expect the timer or the EC IRQ while napping, with all other
 * exceptions still impossible). 2 KiB aligned.
 *
 * Masking PA1 is essential: the EC line is level-triggered and stays asserted
 * until its event is consumed, so without the mask the IRQ re-fires forever and
 * WFI never settles. tw_idle_nap re-unmasks it after each wake.
 */
extern char tw_idle_vectors[];
asm(
"	.pushsection .text.tw_idle_vec, \"ax\"		\n"
"	.align 11					\n"
"tw_idle_vectors:					\n"
"	.align 7\n eret\n"	/* SP0 sync  */
"	.align 7\n eret\n"	/* SP0 irq   */
"	.align 7\n eret\n"	/* SP0 fiq   */
"	.align 7\n eret\n"	/* SP0 err   */
"	.align 7\n eret\n"	/* SPx sync  */
"	.align 7					\n"	/* SPx IRQ - ours */
"	stp	x0, x1, [sp, #-32]!			\n"
"	str	x2, [sp, #16]				\n"
"	mrs	x0, S3_0_C12_C12_0			\n"	/* ICC_IAR1_EL1 ack */
"	msr	cntp_ctl_el0, xzr			\n"	/* disable CNTP     */
"	movz	x1, #0xff72, lsl #16			\n"	/* gpio0 base 0xff720000 */
"	ldr	w2, [x1, #0x34]				\n"	/* GPIO_INTMASK */
"	orr	w2, w2, #0x2				\n"	/* mask PA1 (bit1) */
"	str	w2, [x1, #0x34]				\n"
"	dsb	sy					\n"
"	msr	S3_0_C12_C12_1, x0			\n"	/* ICC_EOIR1_EL1 EOI */
"	isb						\n"
"	ldr	x2, [sp, #16]				\n"
"	ldp	x0, x1, [sp], #32			\n"
"	eret						\n"
"	.align 7\n eret\n"	/* SPx fiq   */
"	.align 7\n eret\n"	/* SPx err   */
"	.align 7\n eret\n	.align 7\n eret\n"	/* lower a64 sync/irq */
"	.align 7\n eret\n	.align 7\n eret\n"	/* lower a64 fiq/err  */
"	.align 7\n eret\n	.align 7\n eret\n"	/* lower a32 sync/irq */
"	.align 7\n eret\n	.align 7\n eret\n"	/* lower a32 fiq/err  */
"	.popsection					\n"
);

/* One-time: enable the GIC path (persistent, harmless to leave on). VBAR_EL2 and
 * HCR_EL2.IMO are NOT changed here - they're swapped per-nap (below) so U-Boot's
 * normal exception handling is untouched except during the brief WFI window. */
static void tw_wfi_setup(void)
{
	static int done;
	unsigned int word = TW_EC_GPIO_INTID / 32, bit = TW_EC_GPIO_INTID % 32;

	if (done)
		return;
	done = 1;
	setbits_le32((void *)TW_GICD_CTLR, (1U << 1));   /* EnableGrp1NS */
	dsb();
	writel(1U << 30, (void *)TW_GICR_ISENABLER0);    /* INTID 30 CNTP PPI */
	dsb();
	asm volatile("msr " TW_STR(ICC_PMR_EL1) ", %0" : : "r" (0xffUL));
	asm volatile("msr " TW_STR(ICC_IGRPEN1_EL1) ", %0" : : "r" (1UL));
	isb();

	/*
	 * EC keypress/power/lid wake: gpio0 PA1 as a level, active-low IRQ +
	 * INTID 46 enabled in the distributor. The cros_ec driver owns this pin
	 * only as a DATA input (it reads the level for tstc/MKBP); it never
	 * touches these interrupt-control registers, so there's no conflict.
	 */
	clrbits_le32((void *)TW_GPIO_INTTYPE, TW_EC_PA1_BIT);   /* 0 = level */
	clrbits_le32((void *)TW_GPIO_INT_POL, TW_EC_PA1_BIT);   /* 0 = active-low */
	clrbits_le32((void *)TW_GPIO_INTMASK, TW_EC_PA1_BIT);   /* unmask */
	setbits_le32((void *)TW_GPIO_INTEN,   TW_EC_PA1_BIT);   /* enable */
	writel(1U << bit, (void *)(TW_GICD_ISENABLER + word * 4));
	dsb();
}

/*
 * One deep WFI: sleep until an EC event (key / power button / lid, all on INTID
 * 46). If `ms` is non-zero a CNTP backstop timer (INTID 30) is also armed and
 * wakes it after `ms`; `ms == 0` means NO timer - sleep purely until an EC IRQ
 * (the typewriter's idle: no periodic wake, lowest power). The taken IRQ wakes
 * it; the handler silences the source. This is where nearly all time is spent.
 */
static void tw_idle_nap(unsigned int ms)
{
	unsigned long ticks = (get_tbclk() / 1000) * ms;
	unsigned long vbar_save, hcr_save;

	tw_wfi_setup();

	/* Point VBAR_EL2 at our minimal IRQ vector and route phys IRQ to EL2
	 * (HCR_EL2.IMO=1) - only for the duration of this nap, then restore, so
	 * the rest of U-Boot keeps its own vectors / IRQ routing. */
	asm volatile("mrs %0, vbar_el2" : "=r" (vbar_save));
	asm volatile("mrs %0, hcr_el2"  : "=r" (hcr_save));
	asm volatile("msr vbar_el2, %0" : : "r" ((unsigned long)tw_idle_vectors));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save | (1UL << 4)));
	isb();

	/* Arm CNTP only if a backstop was requested (ms != 0); otherwise leave the
	 * timer off so the EC IRQ is the sole wake source. Take IRQs, WFI. The
	 * handler acks/disables CNTP/masks the EC line/EOIs. */
	if (ms) {
		asm volatile("msr cntp_tval_el0, %0" : : "r" (ticks));
		asm volatile("msr cntp_ctl_el0, %0" : : "r" (1UL));
	} else {
		asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));
	}
	isb();
	asm volatile("msr daifclr, #2");     /* PSTATE.I = 0 -> IRQ taken */
	wfi();
	asm volatile("msr daifset, #2");     /* re-mask */
	asm volatile("msr cntp_ctl_el0, %0" : : "r" (0UL));   /* ensure off */

	/* Restore U-Boot's vectors + IRQ routing. */
	asm volatile("msr vbar_el2, %0" : : "r" (vbar_save));
	asm volatile("msr hcr_el2, %0"  : : "r" (hcr_save));
	isb();

	/*
	 * If the EC IRQ woke us, the handler masked gpio0 PA1 to quiet the level
	 * line. Clear the GPIO latch and re-unmask it so the next nap can wake on
	 * the next event, and clear any pending INTID 30/46 so it starts clean.
	 * (The cros_ec keyboard reads the pin's DATA level for tstc, unaffected by
	 * the interrupt mask, so keys pressed during the wake are still seen.)
	 */
	writel(TW_EC_PA1_BIT, (void *)TW_GPIO_PORTA_EOI);
	clrbits_le32((void *)TW_GPIO_INTMASK, TW_EC_PA1_BIT);
	writel(1U << 30, (void *)TW_GICR_ICPENDR0);                 /* CNTP PPI */
	writel(1U << (TW_EC_GPIO_INTID % 32),
	       (void *)(TW_GICD_ICPENDR + (TW_EC_GPIO_INTID / 32) * 4)); /* SPI 46 */
	dsb();
}

/* --------------------------------------------------- CPU little-cluster clk --- */
/*
 * Drop CPU0 (A53 little cluster) to 408 MHz at editor startup. The board's
 * rkclk_init leaves it at 600 MHz; a typewriter needs far less, and a lower
 * core clock trims (a little) dynamic power.
 *
 * Self-contained: pokes the RK3399 CRU APLL_L registers directly (no dependency
 * on the U-Boot clk driver, so this builds against a stock U-Boot tree). We use
 * the SAME safe sequence U-Boot's rkclk_set_pll does: force the PLL to slow
 * (bypass) mode so the core runs off the 24 MHz OSC while we reprogram, set the
 * divisors, wait for PLL lock, switch back to normal mode. Glitch-free even
 * though we're executing on that very core.
 *
 * 408 MHz = 24MHz * fbdiv / (refdiv * postdiv1 * postdiv2)
 *         = 24 * 68 / (1 * 2 * 2). VCO = 24*68/1 = 1632 MHz (in 800-2000 range).
 * Then select APLL_L as the core mux and set the core divider to /1 so CPU0
 * runs at the full 408 MHz PLL output.
 *
 * Verify after flashing with `md.l 0xff760000 2` -> expect 00000044 00002201.
 * Not restored on editor exit (the shell keeps 408 too).
 */
#define TW_CRU_BASE          0xff760000UL
#define TW_APLL_L_CON0       (TW_CRU_BASE + 0x00)   /* [11:0] fbdiv */
#define TW_APLL_L_CON1       (TW_CRU_BASE + 0x04)   /* refdiv/postdiv1/postdiv2 */
#define TW_APLL_L_CON2       (TW_CRU_BASE + 0x08)   /* [31] lock status */
#define TW_APLL_L_CON3       (TW_CRU_BASE + 0x0c)   /* [9:8] mode, [3] dsmpd */
#define TW_CRU_CLKSEL_CON0   (TW_CRU_BASE + 0x100)  /* little-core mux + div */
/* RK3399 CRU regs are write-masked: high 16 bits enable the low-16-bit write. */
#define TW_WMSK(mask, val)   (((mask) << 16) | ((val) & (mask)))
#define TW_PLL_MODE_SLOW     0
#define TW_PLL_MODE_NORM     1

static void tw_set_cpu_408(void)
{
	/* 408 MHz little-cluster PLL divisors. */
	const unsigned int fbdiv = 68, refdiv = 1, postdiv1 = 2, postdiv2 = 2;
	unsigned int spin = 100000;

	/* 1. PLL -> slow/bypass mode: core now on 24 MHz OSC, safe to reprogram.
	 *    (CON3 bits[9:8] = mode.) */
	writel(TW_WMSK(0x3U << 8, TW_PLL_MODE_SLOW << 8), (void *)TW_APLL_L_CON3);
	/*    integer mode: CON3 bit[3] DSMPD = 1. */
	writel(TW_WMSK(0x1U << 3, 0x1U << 3), (void *)TW_APLL_L_CON3);

	/* 2. divisors. CON0[11:0]=fbdiv; CON1[5:0]=refdiv,[10:8]=pd1,[14:12]=pd2.
	 *    Mask 0x773f = postdiv2(14:12) | postdiv1(10:8) | refdiv(5:0), exactly
	 *    the fields U-Boot's rkclk_set_pll touches. */
	writel(TW_WMSK(0xfffU, fbdiv), (void *)TW_APLL_L_CON0);
	writel(TW_WMSK(0x773fU,
		       refdiv | (postdiv1 << 8) | (postdiv2 << 12)),
	       (void *)TW_APLL_L_CON1);

	/* 3. wait for PLL lock (CON2 bit31), bounded so we can't hang forever. */
	while (!(readl((void *)TW_APLL_L_CON2) & (1U << 31)) && --spin)
		udelay(1);

	/* 4. back to normal mode: core now runs off the relocked 408 MHz PLL. */
	writel(TW_WMSK(0x3U << 8, TW_PLL_MODE_NORM << 8), (void *)TW_APLL_L_CON3);

	/* 5. little-core mux -> APLL_L (CLKSEL_CON0 [7:6]=0) and core div -> /1
	 *    ([4:0]=0), so CPU0 = full PLL output = 408 MHz. */
	writel(TW_WMSK((0x3U << 6) | 0x1fU, 0), (void *)TW_CRU_CLKSEL_CON0);
}

#else   /* host / non-arm64: plain delay (WFE-equivalent) */
static void tw_idle_nap(unsigned int ms) { udelay(ms * 1000); }
static void tw_set_cpu_408(void) { }
#endif

/* ----------------------------------------------------------- key input --- */
/* Read one logical keypress: cooked ASCII (0x00-0xFF) or an extended KEY_*
 * constant (> 0xFF) parsed from an ANSI arrow/nav escape sequence. Mirrors the
 * ved reference's parser; input comes from U-Boot's console (serial or USB kbd
 * routed to stdin). */
/*
 * Read the next byte of an escape sequence, waiting up to ~30 ms for it. The
 * bytes of an ESC-sequence / Meta chord arrive right after the ESC but not in
 * the same instant, so a single tstc() races the UART and drops the follow-up
 * (this was why M-w / M-0..9 "didn't work"). Returns -1 if nothing arrives
 * (i.e. it really was a bare ESC).
 */
static int tw_getch_timeout(void)
{
	int spins = 30;         /* ~30 ms at 1 ms/spin */

	while (!tstc() && spins-- > 0)
		udelay(1000);
	if (!tstc())
		return -1;
	return getchar();
}

static int tw_poweroff_event_pending(void);   /* defined below (EC section) */

/* Set the "stay awake" window: keep polling cheaply (no deep WFI) until this ms
 * deadline. Re-armed only on real keyboard input, so bursty typing and multi-
 * byte escape sequences don't pay a sleep/wake cycle per byte. After it lapses
 * (TW_ACTIVE_WINDOW_MS past the last keypress) the loop drops to deep WFI. */
static unsigned long tw_active_until;

static inline void tw_stay_awake(void)
{
	tw_active_until = get_timer(0) + TW_ACTIVE_WINDOW_MS;
}

static int tw_read_key(void)
{
	int c;

	/*
	 * Wait for an event. Two states:
	 *
	 *  1. Active window - for TW_ACTIVE_WINDOW_MS (2 s) after the last keypress,
	 *     poll cheaply every TW_ACTIVE_POLL_MS. This spans think-pauses between
	 *     keys and, crucially, keeps the input layer polled while a key is HELD
	 *     so its auto-repeat fires (a held key emits no fresh IRQ, so a WFI would
	 *     never wake for the repeat). Each key re-arms the window.
	 *
	 *  2. Deep sleep - once the window lapses with nothing pending, drop into a
	 *     WFI with NO timer armed: the CPU sleeps until the cros_ec IRQ (INTID
	 *     46) fires. A key, the power button, and the lid all ride that one line,
	 *     so any of them wakes it instantly. There is no periodic wake at all -
	 *     an idle typewriter draws its true floor until you touch it.
	 *
	 * A key surfaces as a console byte (tstc/getchar). The power button and lid
	 * do NOT - they latch EC host-event flags on a separate channel - so once we
	 * wake with no key pending we do a single host-event read to catch them and
	 * power off. That read is a slow EC SPI transaction, but it only runs on a
	 * genuine no-key wake (never during typing: keys short-circuit via tstc, and
	 * the active window polls without touching the EC), so it never lags keys.
	 */
	while (!tstc()) {
		unsigned long now = get_timer(0);

		/* Active window: cheap poll, no deep sleep, no EC access. Polling
		 * (not WFI) here keeps the input layer's auto-repeat serviced while a
		 * key is held - a held key emits no new IRQ, so a WFI would never wake
		 * for the repeat. */
		if (now < tw_active_until) {
			schedule();
			udelay(TW_ACTIVE_POLL_MS * 1000);
			continue;
		}

		/* Window lapsed. Before sleeping, decode a possible power/lid press
		 * (the only non-key events); they share the WFI's INTID 46 wake. */
		if (tw_poweroff_event_pending())
			return KEY_POWER_BTN;

		schedule();               /* keep any housekeeping fed */
		tw_idle_nap(0);           /* no timer: sleep until an EC IRQ */
	}

	/* A key is pending: activity, so keep the CPU awake for another window -
	 * covers fast typing and the trailing bytes of an escape/Meta sequence. */
	tw_stay_awake();

	c = getchar();

	if (c != KEY_ESC)
		return c;

	/* ESC alone, or the start of a CSI/SS3 sequence (arrows/nav) or a Meta
	 * chord (M-x arrives as ESC then x). */
	c = tw_getch_timeout();
	if (c < 0)
		return KEY_ESC;         /* bare ESC */

	if (c == '[' || c == 'O') {
		c = tw_getch_timeout();
		switch (c) {
		case 'A': return KEY_ARROW_UP;
		case 'B': return KEY_ARROW_DOWN;
		case 'C': return KEY_ARROW_RIGHT;
		case 'D': return KEY_ARROW_LEFT;
		case 'H': return KEY_HOME_SEQ;
		case 'F': return KEY_END_SEQ;
		case '1': case '2': case '3':
		case '4': case '5': case '6': {
			int sub = c;
			int c4 = tw_getch_timeout();

			if (c4 == '~') {
				if (sub == '5') return KEY_PAGE_UP;
				if (sub == '6') return KEY_PAGE_DOWN;
				if (sub == '3') return KEY_DELETE;
				if (sub == '1') return KEY_HOME_SEQ;
				if (sub == '4') return KEY_END_SEQ;
			}
			return KEY_ESC;
		}
		default:
			return KEY_ESC;
		}
	}

	/* Meta (Alt) chord: ESC followed by a letter. NOTE: Meta keys don't work
	 * on the target hardware/terminal, so these are effectively dormant - kept
	 * wired (word motion / find) in case a future terminal delivers them. The
	 * M-0..9 file slots were removed; use the ^R picker to switch files. */
	switch (c) {
	case 'f': case 'F': return KEY_META_F;
	case 'b': case 'B': return KEY_META_B;
	case 'd': case 'D': return KEY_META_D;
	case 'w': case 'W': return KEY_META_W;
	default:            return KEY_ESC;
	}
}

static void tw_status(struct tw_state *s, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(s->status_msg, sizeof(s->status_msg), fmt, ap);
	va_end(ap);
}

static void tw_clear_status(struct tw_state *s)
{
	s->status_msg[0] = '\0';
}

/* ---------------------------------------------------------- scrolling ---- */
/*
 * Keep the cursor's logical line visible, counting SCREEN rows (soft-wrapped),
 * not logical lines - a wrapped long line can occupy several rows, so a
 * screenful holds fewer lines than text_rows. If the cursor line is above the
 * viewport, snap scroll_top to it; if the rows from scroll_top through the
 * cursor line exceed the screen, advance scroll_top until they fit.
 */
static void tw_scroll_adjust(struct tw_state *s)
{
	if (s->cur_row < s->scroll_top)
		s->scroll_top = s->cur_row;
	if (s->scroll_top < 0)
		s->scroll_top = 0;

	for (;;) {
		int rows = 0, fr;

		for (fr = s->scroll_top; fr <= s->cur_row; fr++)
			rows += tw_line_rows(s, fr);
		if (rows <= s->text_rows || s->scroll_top >= s->cur_row)
			break;
		s->scroll_top++;        /* cursor line overflows: scroll down */
	}
}

static void tw_clamp_col(struct tw_state *s)
{
	if (s->cur_col > s->line_len[s->cur_row])
		s->cur_col = s->line_len[s->cur_row];
	if (s->cur_col < 0)
		s->cur_col = 0;
}

/* ------------------------------------------------------ editing prims ---- */
static void tw_insert_cp(struct tw_state *s, u32 cp)
{
	int row = s->cur_row;
	int i;

	if (s->line_len[row] >= TW_MAX_COLS - 1)
		return;                         /* line full */
	for (i = s->line_len[row]; i > s->cur_col; i--)
		s->lines[row][i] = s->lines[row][i - 1];
	s->lines[row][s->cur_col] = cp;
	s->line_len[row]++;
	s->cur_col++;
	s->dirty = 1;
}

static void tw_insert_newline(struct tw_state *s)
{
	int row = s->cur_row;
	int rest, i;

	if (s->num_lines >= TW_MAX_LINES)
		return;

	/* shift lines below down by one */
	for (i = s->num_lines; i > row + 1; i--) {
		memcpy(s->lines[i], s->lines[i - 1],
		       sizeof(u32) * TW_MAX_COLS);
		s->line_len[i] = s->line_len[i - 1];
	}
	s->num_lines++;

	/* move the tail of the current line onto the new line */
	rest = s->line_len[row] - s->cur_col;
	for (i = 0; i < rest; i++)
		s->lines[row + 1][i] = s->lines[row][s->cur_col + i];
	s->line_len[row + 1] = rest;
	s->line_len[row] = s->cur_col;

	s->cur_row++;
	s->cur_col = 0;
	s->dirty = 1;
}

static void tw_backspace(struct tw_state *s)
{
	int row = s->cur_row;
	int i, prev_len;

	if (s->cur_col > 0) {
		for (i = s->cur_col - 1; i < s->line_len[row] - 1; i++)
			s->lines[row][i] = s->lines[row][i + 1];
		s->line_len[row]--;
		s->cur_col--;
		s->dirty = 1;
		return;
	}
	if (row == 0)
		return;                         /* top of file */

	/* merge this line onto the end of the previous one */
	prev_len = s->line_len[row - 1];
	for (i = 0; i < s->line_len[row] &&
		    prev_len + i < TW_MAX_COLS - 1; i++)
		s->lines[row - 1][prev_len + i] = s->lines[row][i];
	s->line_len[row - 1] = prev_len + i;

	for (i = row; i < s->num_lines - 1; i++) {
		memcpy(s->lines[i], s->lines[i + 1],
		       sizeof(u32) * TW_MAX_COLS);
		s->line_len[i] = s->line_len[i + 1];
	}
	s->num_lines--;
	s->cur_row--;
	s->cur_col = prev_len;
	s->dirty = 1;
}

static void tw_delete_cp(struct tw_state *s)
{
	int row = s->cur_row;
	int i;

	if (s->cur_col >= s->line_len[row]) {
		/* at EOL: pull the next line up (join), like Nano's Del */
		if (row + 1 < s->num_lines) {
			s->cur_row++;
			s->cur_col = 0;
			tw_backspace(s);
		}
		return;
	}
	for (i = s->cur_col; i < s->line_len[row] - 1; i++)
		s->lines[row][i] = s->lines[row][i + 1];
	s->line_len[row]--;
	s->dirty = 1;
}

/*
 * readline C-k: kill from the cursor to end of line into the kill buffer.
 * When there's nothing to the right of the cursor (empty line, or cursor
 * already at EOL), C-k instead deletes the line break - joining the next line
 * up - so repeated C-k on an empty line eats lines rather than stalling.
 */
static void tw_kill_to_eol(struct tw_state *s)
{
	int row = s->cur_row, i, n;

	n = s->line_len[row] - s->cur_col;
	if (n <= 0) {
		/* nothing to kill on this line: pull the next line up (like Del
		 * at EOL). Does nothing on the very last line. */
		if (row + 1 < s->num_lines)
			tw_delete_cp(s);
		return;
	}
	for (i = 0; i < n; i++)
		s->cut[i] = s->lines[row][s->cur_col + i];
	s->cut_len = n;
	s->cut_valid = 1;
	s->line_len[row] = s->cur_col;    /* truncate at the cursor */
	s->dirty = 1;
}

/* readline C-y: yank (insert) the kill buffer at the cursor. */
static void tw_yank(struct tw_state *s)
{
	int row = s->cur_row, i, n;

	if (!s->cut_valid || s->cut_len == 0)
		return;
	n = s->cut_len;
	if (s->line_len[row] + n > TW_MAX_COLS - 1)
		n = TW_MAX_COLS - 1 - s->line_len[row];
	if (n <= 0)
		return;
	/* make room at the cursor */
	for (i = s->line_len[row] - 1; i >= s->cur_col; i--)
		s->lines[row][i + n] = s->lines[row][i];
	for (i = 0; i < n; i++)
		s->lines[row][s->cur_col + i] = s->cut[i];
	s->line_len[row] += n;
	s->cur_col += n;
	s->dirty = 1;
}

/* A "word" character for M-f/M-b/M-d/C-w: ASCII alphanumeric, or any codepoint
 * >= 0x80 (treat CJK/other as word content). */
static int tw_is_word_cp(u32 cp)
{
	if (cp >= 0x80)
		return 1;
	if (cp >= '0' && cp <= '9')
		return 1;
	if (cp >= 'a' && cp <= 'z')
		return 1;
	if (cp >= 'A' && cp <= 'Z')
		return 1;
	return 0;
}

/* Column of the start of the previous word on the current line (readline M-b:
 * skip non-word chars left, then skip the word). */
static int tw_prev_word_col(struct tw_state *s)
{
	int row = s->cur_row, c = s->cur_col;

	while (c > 0 && !tw_is_word_cp(s->lines[row][c - 1]))
		c--;
	while (c > 0 && tw_is_word_cp(s->lines[row][c - 1]))
		c--;
	return c;
}

/* Column just past the end of the next word (readline M-f). */
static int tw_next_word_col(struct tw_state *s)
{
	int row = s->cur_row, c = s->cur_col, len = s->line_len[row];

	while (c < len && !tw_is_word_cp(s->lines[row][c]))
		c++;
	while (c < len && tw_is_word_cp(s->lines[row][c]))
		c++;
	return c;
}

/* Delete the [from,to) range on the current line (from < to), leaving the
 * cursor at `from`. Used by C-w (delete word back) and M-d (kill word fwd). */
static void tw_delete_range(struct tw_state *s, int from, int to)
{
	int row = s->cur_row, i, n = to - from;

	if (n <= 0)
		return;
	for (i = to; i < s->line_len[row]; i++)
		s->lines[row][i - n] = s->lines[row][i];
	s->line_len[row] -= n;
	s->cur_col = from;
	s->dirty = 1;
}

/* --------------------------------------------------------- navigation ---- */
static void tw_move_left(struct tw_state *s)
{
	if (s->cur_col > 0) {
		s->cur_col--;
	} else if (s->cur_row > 0) {
		s->cur_row--;
		s->cur_col = s->line_len[s->cur_row];
	}
}

static void tw_move_right(struct tw_state *s)
{
	if (s->cur_col < s->line_len[s->cur_row]) {
		s->cur_col++;
	} else if (s->cur_row < s->num_lines - 1) {
		s->cur_row++;
		s->cur_col = 0;
	}
}

static void tw_move_up(struct tw_state *s)
{
	if (s->cur_row > 0) {
		s->cur_row--;
		tw_clamp_col(s);
	}
}

static void tw_move_down(struct tw_state *s)
{
	if (s->cur_row < s->num_lines - 1) {
		s->cur_row++;
		tw_clamp_col(s);
	}
}

/* ---------------------------------------------------------- searching ---- */
/* Find `pat` (ASCII, from the search prompt) starting just after the cursor.
 * Matches against the codepoint buffer treating ASCII cp == byte. Returns 1 and
 * moves the cursor on a hit, 0 otherwise. */
static int tw_search(struct tw_state *s, const char *pat)
{
	int plen = strlen(pat);
	int row = s->cur_row, col = s->cur_col + 1;
	int scanned = 0;

	if (plen == 0)
		return 0;

	while (scanned <= s->num_lines) {
		int len = s->line_len[row];
		int i;

		for (i = col; i + plen <= len; i++) {
			int j;

			for (j = 0; j < plen; j++)
				if (s->lines[row][i + j] != (u32)(unsigned char)pat[j])
					break;
			if (j == plen) {
				s->cur_row = row;
				s->cur_col = i;
				tw_scroll_adjust(s);
				return 1;
			}
		}
		row = (row + 1) % s->num_lines;
		col = 0;
		scanned++;
	}
	return 0;
}

/* ---------------------------------------------------------------- IME ---- */
static void tw_ime_reset(struct tw_ime *im)
{
	im->code_len = 0;
	im->code[0] = '\0';
	im->ncand = 0;
	im->page = 0;
}

static void tw_ime_relookup(struct tw_ime *im)
{
	im->ncand = 0;
	im->page = 0;
	if (im->ready && im->code_len)
		im->ncand = ime_lookup(&im->tab, im->code, im->code_len,
				       im->cand, TW_MAX_CANDS);
}

/* Commit candidate `idx` into the document: decode its UTF-8 to codepoints and
 * insert each through the normal edit path. */
static void tw_ime_commit(struct tw_state *s, int idx)
{
	const ime_cand *c;
	int off;

	if (idx < 0 || idx >= s->ime.ncand)
		return;
	c = &s->ime.cand[idx];
	off = 0;
	while (off < c->word_len) {
		u32 cp;
		unsigned char b = (unsigned char)c->word[off];

		if (b < 0x80) {
			cp = b; off += 1;
		} else if ((b & 0xE0) == 0xC0 && off + 1 < c->word_len) {
			cp = ((u32)(b & 0x1F) << 6) | (c->word[off + 1] & 0x3F);
			off += 2;
		} else if ((b & 0xF0) == 0xE0 && off + 2 < c->word_len) {
			cp = ((u32)(b & 0x0F) << 12) |
			     ((c->word[off + 1] & 0x3F) << 6) |
			     (c->word[off + 2] & 0x3F);
			off += 3;
		} else {
			cp = 0xFFFD; off += 1;
		}
		tw_insert_cp(s, cp);
	}
	tw_ime_reset(&s->ime);
}

/*
 * Feed one key to the Wubi composer. Returns 1 if consumed by the IME, 0 if the
 * caller should handle it as a normal editor key. Mirrors wubi-fep's
 * handle_cn_byte(), but commits into the document instead of a pty.
 */
static int tw_ime_key(struct tw_state *s, int key)
{
	struct tw_ime *im = &s->ime;

	if (im->mode != TW_IME_WUBI)
		return 0;

	if (key >= 'a' && key <= 'z') {
		if (im->code_len < IME_CODE_LEN) {
			im->code[im->code_len++] = (char)key;
			im->code[im->code_len] = '\0';
			tw_ime_relookup(im);
		}
		return 1;
	}

	if (key >= '1' && key <= '9' && im->code_len > 0) {
		int abs = im->page + (key - '1');

		if (abs < im->ncand && (key - '1') < TW_PAGE)
			tw_ime_commit(s, abs);
		return 1;
	}

	if (key == ' ') {
		if (im->code_len > 0 && im->ncand > 0) {
			tw_ime_commit(s, im->page);
			return 1;
		}
		return 0;               /* nothing pending: literal space */
	}

	if (key == KEY_BACKSPACE || key == KEY_BS) {
		if (im->code_len > 0) {
			im->code[--im->code_len] = '\0';
			tw_ime_relookup(im);
			return 1;
		}
		return 0;               /* nothing pending: edit document */
	}

	if ((key == '=' || key == '.') && im->code_len > 0) {
		if (im->page + TW_PAGE < im->ncand)
			im->page += TW_PAGE;
		return 1;
	}
	if ((key == '-' || key == ',') && im->code_len > 0) {
		if (im->page >= TW_PAGE)
			im->page -= TW_PAGE;
		return 1;
	}

	if (key == KEY_ESC) {
		if (im->code_len > 0) {
			tw_ime_reset(im);
			return 1;       /* swallow: "discard candidates" */
		}
		return 0;
	}

	if (key == KEY_ENTER || key == KEY_LF) {
		if (im->code_len > 0) {
			/* emit the raw typed letters, swallow the newline */
			int i;

			for (i = 0; i < im->code_len; i++)
				tw_insert_cp(s, (u32)(unsigned char)im->code[i]);
			tw_ime_reset(im);
			return 1;
		}
		return 0;
	}

	/* other printable ASCII with a pending code: commit first, then let the
	 * punctuation fall through to the editor. */
	if (key >= 0x20 && key < 0x7f && im->code_len > 0) {
		if (im->ncand > 0)
			tw_ime_commit(s, im->page);
		else
			tw_ime_reset(im);
		return 0;               /* punctuation still handled below */
	}

	return 0;
}

/* -------------------------------------------------------------- prompts -- */
static void tw_do_save(struct tw_state *s)
{
	if (!s->writable) {
		/* Read-only: either the user passed 'ro', or this is the eMMC
		 * (mmc 0), which is hard-locked because its FAT writes corrupt
		 * the card on this board. Use the microSD (mmc 1) to save. */
		tw_status(s, "[ Read-only (eMMC is locked; save on mmc 1) ]");
		return;
	}
	if (!s->filename[0]) {
		tw_status(s, "[ No file name ]");
		return;
	}
	if (tw_file_save(s) == 0)
		/* Show name + byte count so a corrupt/short write is visible
		 * on the framebuffer status line (no serial needed to debug). */
		tw_status(s, "[ Wrote %d line%s, %d bytes -> %s ]",
			  s->num_lines, s->num_lines == 1 ? "" : "s",
			  s->last_write_bytes, s->filename);
	else
		tw_status(s, "[ Error writing %s ]", s->filename);
}

/* --------------------------------------------------------- ChromeOS EC --- */
/*
 * Grab the ChromeOS EC device (cached). NULL if this board has no EC or the
 * driver isn't built in. The EC is how we reach the battery gauge on gru/kevin.
 */
#if CONFIG_IS_ENABLED(CROS_EC)
static struct udevice *tw_ec(void)
{
	static struct udevice *ec;
	static int tried;

	if (!tried) {
		tried = 1;
		if (uclass_first_device_err(UCLASS_CROS_EC, &ec))
			ec = NULL;
	}
	return ec;
}

/*
 * Issue one EC host command and return the response payload, WITHOUT going
 * through U-Boot's public cros_ec_* wrappers.
 *
 * Why we reimplement this: U-Boot's cros_ec_read_batt_charge() has a bug
 * (`if (ret)` treats ec_command's positive byte-count as an error), and the
 * correct internal path (send_command_proto3 + create/handle_proto3) is all
 * static in drivers/misc/cros_ec.c - there's no public raw-command API. Rather
 * than patch U-Boot core, we replicate the protocol framing here, on top of the
 * PUBLIC pieces: struct cros_ec_dev (with its din/dout buffers) via
 * dev_get_uclass_priv(), the transport ops (dm_cros_ec_get_ops), and
 * cros_ec_calc_checksum(). gru/kevin's EC negotiates protocol v3, so that's the
 * path we implement; we fall back to the v2 ->command op otherwise.
 *
 * Mirrors send_command_proto3(): build an ec_host_request header + data in
 * cdev->dout, call ops->packet(), then validate the ec_host_response header +
 * checksum in cdev->din. Returns payload length (>=0) and sets *dinp to the
 * payload, or a negative EC_RES_* / errno on failure.
 */
static int tw_ec_command(struct udevice *ec, int cmd, int cmd_version,
			 const void *dout, int dout_len,
			 uint8_t **dinp, int din_len)
{
	struct cros_ec_dev *cdev = dev_get_uclass_priv(ec);
	struct dm_cros_ec_ops *ops = dm_cros_ec_get_ops(ec);
	struct ec_host_request *rq;
	struct ec_host_response *rs;
	int out_bytes, in_bytes, rv;

	if (!cdev || !ops)
		return -EC_RES_ERROR;

	/* Protocol v2 fallback: the ->command op does its own framing. */
	if (cdev->protocol_version != 3) {
		if (!ops->command)
			return -EC_RES_INVALID_VERSION;
		return ops->command(ec, cmd, cmd_version, dout, dout_len,
				    dinp, din_len);
	}

	/* --- protocol v3 --- */
	if (!ops->packet)
		return -ENOSYS;

	out_bytes = dout_len + sizeof(*rq);
	in_bytes  = din_len + sizeof(*rs);
	if (out_bytes > (int)sizeof(cdev->dout))
		return -EC_RES_REQUEST_TRUNCATED;
	if (in_bytes > (int)sizeof(cdev->din))
		return -EC_RES_RESPONSE_TOO_BIG;

	/* Build request header + data, then set checksum so all bytes sum 0. */
	rq = (struct ec_host_request *)cdev->dout;
	rq->struct_version  = EC_HOST_REQUEST_VERSION;
	rq->checksum        = 0;
	rq->command         = cmd;
	rq->command_version = cmd_version;
	rq->reserved        = 0;
	rq->data_len        = dout_len;
	if (dout_len)
		memcpy(rq + 1, dout, dout_len);
	rq->checksum = (uint8_t)(-cros_ec_calc_checksum(cdev->dout, out_bytes));

	rv = ops->packet(ec, out_bytes, in_bytes);
	if (rv < 0)
		return rv;

	/* Validate response header + checksum. */
	rs = (struct ec_host_response *)cdev->din;
	if (rs->struct_version != EC_HOST_RESPONSE_VERSION || rs->reserved)
		return -EC_RES_INVALID_RESPONSE;
	if (rs->data_len > din_len)
		return -EC_RES_RESPONSE_TOO_BIG;
	if (cros_ec_calc_checksum(cdev->din, sizeof(*rs) + rs->data_len))
		return -EC_RES_INVALID_CHECKSUM;
	if (rs->result)
		return -(int)rs->result;

	*dinp = (uint8_t *)(rs + 1);
	return rs->data_len;
}

/*
 * Read the battery charge as a percentage (0..100) on success, or a negative
 * value on failure (TW_BATT_NO_EC = no EC, else a negative EC result code) -
 * the title bar hides any negative. Called on demand from Ctrl-T (one EC
 * transaction per press); there is no periodic battery poll (see POWERSAVE.md).
 */
static int tw_read_battery(void)
{
	struct udevice *ec = tw_ec();
	struct ec_response_charge_state resp;
	uint8_t reqcmd = CHARGE_STATE_CMD_GET_STATE;
	uint8_t *din = NULL;
	int len;

	if (!ec)
		return TW_BATT_NO_EC;

	len = tw_ec_command(ec, EC_CMD_CHARGE_STATE, 0,
			    &reqcmd, sizeof(reqcmd),
			    &din, sizeof(resp));
	if (len < 0)
		return len;                    /* -EC_RES_* : show the code */
	if (!din || len < (int)sizeof(resp.get_state))
		return -EC_RES_INVALID_RESPONSE;

	memcpy(&resp, din, sizeof(resp.get_state));
	return resp.get_state.batt_state_of_charge;
}

/*
 * Power-off triggers: the physical power button AND closing the lid. Detection
 * done RIGHT.
 *
 * The obvious approach - poll cros_ec_get_next_event() - is WRONG here: the
 * keyboard is also a cros_ec (CROS_EC_KEYB), and its read path
 * (cros_ec_kbc_check) pulls from the SAME MKBP event FIFO, discarding every
 * event that isn't a key-matrix scan. So (a) polling the FIFO ourselves steals
 * keystrokes, and (b) the keyboard driver silently eats button events anyway.
 *
 * Instead we use the EC HOST EVENT flags - a separate, latched bitmask channel
 * that the keyboard driver never touches. Both the power button and a lid-close
 * latch bits there (EC_HOST_EVENT_POWER_BUTTON / EC_HOST_EVENT_LID_CLOSED). We
 * read the B copy (cros_ec_get_host_events uses EC_CMD_HOST_EVENT_GET_B, distinct
 * from the ACPI/SMI main copy) and clear the bits after seeing them, so they
 * behave edge-triggered. Reading host events is a plain host command - it does
 * NOT drain the key FIFO, so typing is safe.
 */
#define TW_POWEROFF_EVENTS  (EC_HOST_EVENT_MASK(EC_HOST_EVENT_POWER_BUTTON) | \
			     EC_HOST_EVENT_MASK(EC_HOST_EVENT_LID_CLOSED))

/* Clear the latched power-off host events (power button + lid closed). Called
 * at editor start (so the press/lid-state that launched us doesn't fire an
 * immediate poweroff) AND right before we attempt poweroff (so a stuck/latched
 * event can't re-fire in a loop if the poweroff itself doesn't take, e.g. EL3). */
static void tw_poweroff_events_clear(void)
{
	struct udevice *ec = tw_ec();

	if (ec)
		cros_ec_clear_host_events(ec, TW_POWEROFF_EVENTS);
}

/* Return 1 if a power-off trigger (power button pressed OR lid closed) fired
 * since the last check (latched host event), else 0. Clears the latch so it's
 * edge-triggered. Never touches the keyboard's MKBP FIFO, so it's safe to call
 * in the key-wait loop. */
static int tw_poweroff_event_pending(void)
{
	struct udevice *ec = tw_ec();
	uint32_t events = 0;

	if (!ec)
		return 0;
	if (cros_ec_get_host_events(ec, &events))
		return 0;
	if (!(events & TW_POWEROFF_EVENTS))
		return 0;
	cros_ec_clear_host_events(ec, TW_POWEROFF_EVENTS);  /* consume latch */
	return 1;
}
#else
static int tw_read_battery(void) { return TW_BATT_NO_EC; }
static void tw_poweroff_events_clear(void) { }
static int tw_poweroff_event_pending(void) { return 0; }
#endif

static int tw_save_before_exit(struct tw_state *s)
{
	if (s->writable && s->dirty && s->filename[0]) {
		if (tw_file_save(s) != 0) {
			tw_status(s, "[ Save failed - aborted ]");
			return -1;
		}
	}
	return 0;
}

/*
 * Save, then hand control to the OS boot flow (`bootflow scan -lb` - the
 * libreboot default that scans partitions incl. extlinux.conf and boots). If
 * a bootable target is found this does not return; otherwise it falls back to
 * the editor.
 */
static void tw_boot(struct tw_state *s)
{
	if (tw_save_before_exit(s) != 0)
		return;
	tw_status(s, "[ Booting... ]");
	tw_render(s);
	/* Clear the framebuffer first so the editor's screen isn't left behind
	 * the OS's boot output/menu - a clean handoff. Then scan + boot. */
	run_command("cls", 0);
	run_command("bootflow scan -lb", 0);
	/* Only here if nothing booted - repaint the editor over the cleared
	 * screen so we don't leave a blank display. */
	s->first_paint = 1;
	tw_status(s, "[ No bootable OS found ]");
}

/*
 * Save, let the write settle, then power the board off via PSCI SYSTEM_OFF
 * (SMC to coreboot's bl31 - button-wakeable, exactly how Linux powers off).
 * The PSCI driver is probed explicitly first; see the body + POWEROFF.md.
 */
static void tw_poweroff(struct tw_state *s)
{
	/* 0. Consume the latched power-button / lid-closed host events NOW, before
	 * we try to power off. If the poweroff doesn't actually take (e.g. PSCI is
	 * unavailable at EL3), a still-latched event would otherwise re-fire
	 * KEY_POWER_BTN on the next key-wait tick and loop. Clearing here makes it
	 * a single attempt; the user can press again. */
	tw_poweroff_events_clear();

	/* 1. flush the document to disk if there's anything to save. */
	if (tw_save_before_exit(s) != 0)
		return;

	/* 2. U-Boot's FAT/block writes are synchronous (write-through), so once
	 * tw_file_save() returned the data is on the card. Small settle delay as
	 * belt-and-braces before we cut power. */
	mdelay(200);

	/*
	 * 3. Power off via PSCI SYSTEM_OFF - an SMC call to ARM Trusted Firmware
	 * (coreboot's bl31). This is exactly how Linux powers this board off,
	 * and it is button-wakeable (unlike the EC "ship mode" battery cutoff,
	 * which wakes only on AC).
	 *
	 * Two things were needed to make it work, both now in place:
	 *  a) a /psci node with `method = "smc"` in the U-Boot device tree
	 *     (added in rk3399-gru-u-boot.dtsi) - without it the PSCI driver
	 *     couldn't learn the conduit and every SMC returned -8;
	 *  b) the PSCI firmware driver must be PROBED - U-Boot binds it but
	 *     doesn't auto-probe (CONFIG_SYSRESET_PSCI is off), so we probe it
	 *     explicitly here, which runs psci_probe() and sets the conduit.
	 *
	 * With both done, SYSTEM_OFF powers the board off and never returns. If
	 * for some reason it does return (PSCI unavailable), we fall through to
	 * the hold-the-button message rather than hanging.
	 */
	tw_render(s);         /* show the status before we go dark */
	{
		struct udevice *psci = NULL;
		int probe, el;

		/* Probe the PSCI firmware driver: this reads the DT `method`
		 * ("smc") and sets the conduit. It FAILS (returns -EINVAL) if we
		 * run at EL3 - PSCI is a call UP to firmware, so from EL3 there's
		 * nothing above to call. If poweroff does nothing, the EL shown
		 * here is the first thing to check (need EL < 3, i.e. EL2/EL1). */
		probe = uclass_get_device_by_name(UCLASS_FIRMWARE, "psci", &psci);
		el = current_el();

		if (probe || el == 3) {
			tw_status(s, "[ Poweroff unavailable: EL%d probe=%d - "
				     "need SYSRESET_PSCI + EL<3 ]", el, probe);
			return;
		}

		disable_interrupts();
		invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF, 0, 0, 0);
		enable_interrupts();
	}
	tw_status(s, "[ Power off failed - hold the power button ]");
}

/*
 * Switch the buffer to `name` on the current device (used by ^R open and the
 * M-0..M-9 slot keys). AlphaSmart-style: if the current buffer is writable and
 * modified, auto-save it first, then load the new file. Cursor/scroll reset.
 * The device (s->fs) is unchanged - only the filename changes.
 */
static void tw_switch_file(struct tw_state *s, const char *name)
{
	int saved = 0;

	if (!name || !name[0])
		return;

	/* auto-save the outgoing buffer if we can and it changed */
	if (s->writable && s->dirty && s->filename[0]) {
		if (tw_file_save(s) != 0) {
			tw_status(s, "[ Save of %s failed - not switching ]",
				  s->filename);
			return;                 /* don't lose edits on a bad save */
		}
		saved = 1;
	}

	strncpy(s->filename, name, TW_MAX_FILENAME - 1);
	s->filename[TW_MAX_FILENAME - 1] = '\0';

	if (tw_file_load(s, s->filename) < 0) {
		tw_status(s, "[ Cannot open %s ]", s->filename);
		return;
	}
	if (s->num_lines == 0) {
		s->num_lines = 1;
		s->line_len[0] = 0;
	}
	s->cur_row = 0;
	s->cur_col = 0;
	s->scroll_top = 0;
	s->dirty = 0;
	s->first_paint = 1;             /* full repaint for the new content */

	tw_status(s, "[ %s%s - %d line%s ]", saved ? "Saved & opened " : "Opened ",
		  s->filename, s->num_lines, s->num_lines == 1 ? "" : "s");
}

/* ^R: list files on the current device and enter the arrow-select picker. If
 * the directory is empty or unreadable, fall back to a typed "File to open:". */
static void tw_picker_open(struct tw_state *s)
{
	if (tw_list_files(s) > 0) {
		s->prompt = TW_PROMPT_PICK;
		/* pre-select the current file if it's in the list */
		for (int i = 0; i < s->pick_count; i++)
			if (!strcmp(s->pick_name[i], s->filename)) {
				s->pick_sel = i;
				break;
			}
	} else {
		s->prompt = TW_PROMPT_OPEN;     /* nothing to pick: type a name */
		s->prompt_ans[0] = '\0';
		s->prompt_len = 0;
		tw_status(s, "[ No files - type a name ]");
	}
}

static void tw_prompt_start(struct tw_state *s, int which)
{
	s->prompt = which;
	s->prompt_len = 0;
	s->prompt_ans[0] = '\0';
	if (which == TW_PROMPT_SAVE && s->filename[0]) {
		strncpy(s->prompt_ans, s->filename, TW_CMD_BUF - 1);
		s->prompt_ans[TW_CMD_BUF - 1] = '\0';
		s->prompt_len = strlen(s->prompt_ans);
	}
}

/* Handle a key while a bottom-line prompt owns input. */
static void tw_prompt_key(struct tw_state *s, int key)
{
	/* ^R file picker: arrow / ^P / ^N to move, Enter opens, Esc cancels. */
	if (s->prompt == TW_PROMPT_PICK) {
		switch (key) {
		case KEY_ARROW_UP:
		case KEY_CTRL_P:
			if (s->pick_sel > 0)
				s->pick_sel--;
			break;
		case KEY_ARROW_DOWN:
		case KEY_CTRL_N:
			if (s->pick_sel < s->pick_count - 1)
				s->pick_sel++;
			break;
		case KEY_PAGE_UP:
			s->pick_sel -= 8;
			if (s->pick_sel < 0)
				s->pick_sel = 0;
			break;
		case KEY_PAGE_DOWN:
			s->pick_sel += 8;
			if (s->pick_sel > s->pick_count - 1)
				s->pick_sel = s->pick_count - 1;
			break;
		case KEY_ENTER:
		case KEY_LF: {
			char name[TW_PICK_NAMELEN];

			strncpy(name, s->pick_name[s->pick_sel], sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
			s->prompt = TW_PROMPT_NONE;
			tw_switch_file(s, name);
			return;
		}
		case KEY_ESC:
		case KEY_CTRL_X:
			s->prompt = TW_PROMPT_NONE;
			tw_status(s, "[ Cancelled ]");
			break;
		}
		return;
	}

	if (s->prompt == TW_PROMPT_EXIT) {
		if (key == 'y' || key == 'Y') {
			tw_do_save(s);
			if (!s->dirty)
				s->quit = 1;
			s->prompt = TW_PROMPT_NONE;
		} else if (key == 'n' || key == 'N') {
			s->quit = 1;
			s->prompt = TW_PROMPT_NONE;
		} else if (key == 'c' || key == 'C' || key == KEY_ESC) {
			s->prompt = TW_PROMPT_NONE;
		}
		return;
	}

	if (s->prompt == TW_PROMPT_POWEROFF) {
		if (key == 'y' || key == 'Y') {
			s->prompt = TW_PROMPT_NONE;
			tw_poweroff(s);        /* saves, syncs, powers off (or reports) */
		} else if (key == 'b' || key == 'B') {
			s->prompt = TW_PROMPT_NONE;
			tw_boot(s);            /* saves, then boots the OS (or reports) */
		} else if (key == 'n' || key == 'N' || key == KEY_ESC) {
			s->prompt = TW_PROMPT_NONE;
			tw_status(s, "[ Cancelled ]");
		}
		return;
	}

	if (key == KEY_ENTER || key == KEY_LF) {
		int which = s->prompt;

		s->prompt = TW_PROMPT_NONE;
		if (which == TW_PROMPT_SAVE) {
			if (s->prompt_ans[0]) {
				strncpy(s->filename, s->prompt_ans,
					TW_MAX_FILENAME - 1);
				s->filename[TW_MAX_FILENAME - 1] = '\0';
			}
			tw_do_save(s);
		} else if (which == TW_PROMPT_SEARCH) {
			strncpy(s->search_last, s->prompt_ans, TW_CMD_BUF - 1);
			s->search_last[TW_CMD_BUF - 1] = '\0';
			if (!tw_search(s, s->search_last))
				tw_status(s, "[ \"%s\" not found ]",
					  s->search_last);
		} else if (which == TW_PROMPT_OPEN) {
			if (s->prompt_ans[0])
				tw_switch_file(s, s->prompt_ans);
		}
		return;
	}
	if (key == KEY_ESC) {
		s->prompt = TW_PROMPT_NONE;
		tw_status(s, "[ Cancelled ]");
		return;
	}
	if ((key == KEY_BACKSPACE || key == KEY_BS) && s->prompt_len > 0) {
		s->prompt_ans[--s->prompt_len] = '\0';
		return;
	}
	if (key >= 0x20 && key < 0x7f && s->prompt_len < TW_CMD_BUF - 1) {
		s->prompt_ans[s->prompt_len++] = (char)key;
		s->prompt_ans[s->prompt_len] = '\0';
	}
}

/*
 * Set the renderer's dirty flags by diffing state around a key dispatch. This
 * keeps the fast incremental renderer correct without threading dirty-marking
 * through every editing primitive: if a change affects the whole text area
 * (line count changed, or the buffer scrolled) we repaint it all; the title
 * bar repaints when the filename/modified/mode indicator changes; the bottom
 * bar repaints when the IME composition, prompt, or status message changes.
 * The current + previous cursor rows are always repainted by tw_render itself.
 */
struct tw_snap {
	int num_lines, scroll_top, dirty, has_name;
	int ime_mode, code_len, page, ncand;
	int prompt, has_status, pick_sel;
	int cur_row, cur_len;   /* to detect an in-line content edit */
};

static void tw_snapshot(struct tw_state *s, struct tw_snap *o)
{
	o->num_lines = s->num_lines;
	o->scroll_top = s->scroll_top;
	o->dirty = s->dirty;
	o->has_name = s->filename[0] != '\0';
	o->ime_mode = s->ime.mode;
	o->code_len = s->ime.code_len;
	o->page = s->ime.page;
	o->ncand = s->ime.ncand;
	o->prompt = s->prompt;
	o->has_status = s->status_msg[0] != '\0';
	o->pick_sel = s->pick_sel;
	o->cur_row = s->cur_row;
	o->cur_len = s->line_len[s->cur_row];
}

static void tw_mark_dirty(struct tw_state *s, const struct tw_snap *o)
{
	/* Repaint the whole text area on any content change: line count or
	 * scroll changed (join/split/scroll), or - while staying on the same
	 * line - that line's length changed (insert/delete/tab/kill/yank). A
	 * pure cursor move (incl. vertical, which changes cur_row but no
	 * content) leaves these equal, so the renderer only moves the caret and
	 * doesn't flicker. */
	if (s->num_lines != o->num_lines || s->scroll_top != o->scroll_top ||
	    (s->cur_row == o->cur_row &&
	     s->line_len[s->cur_row] != o->cur_len))
		s->dirty_all = 1;
	if (s->dirty != o->dirty ||
	    (s->filename[0] != '\0') != o->has_name ||
	    s->ime.mode != o->ime_mode)
		s->dirty_title = 1;
	if (s->ime.mode != o->ime_mode || s->ime.code_len != o->code_len ||
	    s->ime.page != o->page || s->ime.ncand != o->ncand ||
	    s->prompt != o->prompt ||
	    (s->status_msg[0] != '\0') != o->has_status ||
	    s->status_msg[0] != '\0' ||
	    s->pick_sel != o->pick_sel)         /* picker selection moved */
		s->dirty_bar = 1;
}

/* --------------------------------------------------------- key dispatch -- */
static void tw_handle_key(struct tw_state *s, int key)
{
	struct tw_snap snap;

	tw_snapshot(s, &snap);

	/* Power-off trigger (power button pressed or lid closed): save + power
	 * off (like ^Q -> Y), from any mode or prompt. Handled before all else. */
	if (key == KEY_POWER_BTN) {
		s->prompt = TW_PROMPT_NONE;
		tw_poweroff(s);
		return;
	}

	if (s->prompt != TW_PROMPT_NONE) {
		tw_prompt_key(s, key);
		tw_mark_dirty(s, &snap);
		return;
	}

	tw_clear_status(s);

	/* Ctrl-Space toggles the IME in any state. */
	if (key == KEY_CTRL_SPACE) {
		s->ime.mode = (s->ime.mode == TW_IME_WUBI && s->ime.ready)
			      ? TW_IME_OFF
			      : (s->ime.ready ? TW_IME_WUBI : TW_IME_OFF);
		tw_ime_reset(&s->ime);
		tw_mark_dirty(s, &snap);
		return;
	}

	/* Brightness step works in any mode (before the composer, like ^Space).
	 * ^- dims, ^= brightens, by TW_BACKLIGHT_STEP percent. */
	if (key == KEY_CTRL_MINUS || key == KEY_CTRL_EQUAL) {
		int v = tw_backlight_step(key == KEY_CTRL_EQUAL
					  ? TW_BACKLIGHT_STEP : -TW_BACKLIGHT_STEP);
		tw_status(s, "brightness %d%%", v);
		tw_mark_dirty(s, &snap);
		return;
	}

	/* Let the Wubi composer consume the key first (a-z, digits, space,
	 * paging, backspace-while-composing, ...). */
	if (tw_ime_key(s, key)) {
		tw_mark_dirty(s, &snap);
		return;
	}

	switch (key) {
	case KEY_CTRL_S:            /* save (one-handed) */
		tw_prompt_start(s, TW_PROMPT_SAVE);
		break;
	case KEY_CTRL_T: {         /* battery % on demand (bottom status only) */
		int b = tw_read_battery();

		if (b == TW_BATT_NO_EC)
			tw_status(s, "[ Battery: no EC ]");
		else if (b < 0)
			tw_status(s, "[ Battery: read failed (%d) ]", b);
		else
			tw_status(s, "[ Battery: %d%% ]", b);
		break;
	}
	case KEY_CTRL_R:            /* open a file: arrow-select picker */
		tw_picker_open(s);
		break;
	case KEY_CTRL_Q:            /* save + power off / boot OS (Y/B/N) */
		tw_prompt_start(s, TW_PROMPT_POWEROFF);
		break;
	case KEY_CTRL_X:
		/* Only offer "save modified buffer?" when saving is possible.
		 * Read-only: quit immediately (edits can't be written anyway). */
		if (s->dirty && s->writable)
			tw_prompt_start(s, TW_PROMPT_EXIT);
		else
			s->quit = 1;
		break;
	/* readline kill / yank */
	case KEY_CTRL_K:
		tw_kill_to_eol(s);
		break;
	case KEY_CTRL_Y:
		tw_yank(s);
		break;
	case KEY_CTRL_W: {          /* delete word backward */
		int from = tw_prev_word_col(s);

		tw_delete_range(s, from, s->cur_col);
		break;
	}
	case KEY_META_D: {          /* kill word forward */
		int to = tw_next_word_col(s);

		tw_delete_range(s, s->cur_col, to);
		break;
	}
	case KEY_META_W:            /* search (moved off C-w) */
		tw_prompt_start(s, TW_PROMPT_SEARCH);
		break;

	/* readline cursor motion */
	case KEY_CTRL_A:
	case KEY_HOME_SEQ:
		s->cur_col = 0;
		break;
	case KEY_CTRL_E:
	case KEY_END_SEQ:
		s->cur_col = s->line_len[s->cur_row];
		break;
	case KEY_CTRL_B:
	case KEY_ARROW_LEFT:  tw_move_left(s);  break;
	case KEY_CTRL_F:
	case KEY_ARROW_RIGHT: tw_move_right(s); break;
	case KEY_CTRL_P:
	case KEY_ARROW_UP:    tw_move_up(s);    break;
	case KEY_CTRL_N:
	case KEY_ARROW_DOWN:  tw_move_down(s);  break;
	case KEY_META_B:      s->cur_col = tw_prev_word_col(s); break;
	case KEY_META_F:      s->cur_col = tw_next_word_col(s); break;

	case KEY_PAGE_UP: {
		int i;

		for (i = 0; i < s->text_rows; i++)
			tw_move_up(s);
		break;
	}
	case KEY_PAGE_DOWN: {
		int i;

		for (i = 0; i < s->text_rows; i++)
			tw_move_down(s);
		break;
	}

	case KEY_ENTER:
	case KEY_LF:
		tw_insert_newline(s);
		break;
	case KEY_BACKSPACE:
	case KEY_BS:
		tw_backspace(s);
		break;
	case KEY_CTRL_D:
	case KEY_DELETE:
		tw_delete_cp(s);
		break;

	case KEY_TAB: {
		/* Expand Tab to spaces up to the next tab stop (every TW_TABW
		 * columns). Spaces render cleanly in the fixed font and keep the
		 * column model simple; a literal tab codepoint would not. */
		int n = TW_TABW - (s->cur_col % TW_TABW);

		while (n-- > 0)
			tw_insert_cp(s, (u32)' ');
		break;
	}

	default:
		/* printable ASCII typed literally (English mode, or punctuation
		 * that fell through the IME after a commit) */
		if (key >= 0x20 && key < 0x7f)
			tw_insert_cp(s, (u32)key);
		break;
	}

	tw_clamp_col(s);
	tw_scroll_adjust(s);
	tw_mark_dirty(s, &snap);
}

/* --------------------------------------------------------------- run ----- */
static void tw_bind_ime(struct tw_state *s)
{
	s->ime.ready = 0;
	if (ime_table_open_mem(&s->ime.tab, wubi_tab, wubi_tab_len,
			       IME_SCHEME_WUBI) == 0)
		s->ime.ready = 1;
	/* Start in English; Ctrl-Space toggles to Wubi when the table bound. */
	s->ime.mode = TW_IME_OFF;
	tw_ime_reset(&s->ime);
}

static void tw_print_usage(struct cmd_tbl *cmdtp)
{
	/* Print the same text as `help typewriter`, then return to the shell. */
	if (cmdtp)
		cmd_usage(cmdtp);
}

/*
 * Is this the eMMC (mmc device 0)? Its FAT write path corrupts the card on this
 * board, so it is hard-locked read-only regardless of the requested mode.
 * `devpart` is "<dev>[:<part>]"; we match device index 0 exactly (so "0", "0:1"
 * lock, but "10:1" does not).
 */
static int tw_is_emmc(const char *iftype, const char *devpart)
{
	return !strcmp(iftype, "mmc") &&
	       devpart[0] == '0' &&
	       (devpart[1] == ':' || devpart[1] == '\0');
}

static int do_typewriter(struct cmd_tbl *cmdtp, int flag,
			 int argc, char *const argv[])
{
	struct tw_state *s = &g_tw;
	const char *fstype = NULL;
	/* Defaults for a bare `typewriter`: the microSD, which has a working
	 * FAT write path on this board (the eMMC's is broken - see
	 * KNOWN_ISSUES). The microSD is writable by default. */
	const char *iftype  = "mmc";
	const char *devpart = "1:1";
	const char *fname   = "a.txt";
	int want_ro = 0;
	int i;

	/* `typewriter -h` / `--help`: print usage to the console and return
	 * without launching the editor (which would take over the framebuffer). */
	if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		tw_print_usage(cmdtp);
		return CMD_RET_SUCCESS;
	}

	/* Argument shapes:
	 *   typewriter                        -> default: mmc 1:1 a.txt (RW)
	 *   typewriter <if> <dev:part> <file> [fstype] [ro]  -> explicit
	 * Anything with 1-3 args (an incomplete file spec) is a usage error. */
	if (argc >= 4) {
		iftype  = argv[1];
		devpart = argv[2];
		fname   = argv[3];
		/* Trailing optional args, in any order: the literal token "ro"
		 * forces read-only; any other trailing token is the fs type.
		 * (Devices are writable by default; only the eMMC below is
		 * hard-locked read-only regardless.) */
		for (i = 4; i < argc; i++) {
			if (!strcmp(argv[i], "ro"))
				want_ro = 1;
			else
				fstype = argv[i];
		}
	} else if (argc != 1) {
		return CMD_RET_USAGE;
	}

	memset(s, 0, sizeof(*s));
	s->batt_pct = -1;   /* unknown until first Ctrl-T (hidden in title) */

	/*
	 * Writable unless the user forced `ro`, EXCEPT the eMMC (mmc 0:*) is
	 * ALWAYS read-only: its FAT write path corrupts the card on this board
	 * (see KNOWN_ISSUES), so we hard-lock it no matter what was requested.
	 */
	s->writable = !want_ro;
	if (tw_is_emmc(iftype, devpart))
		s->writable = 0;

	if (tw_video_init(s) != 0) {
		printf("typewriter: no usable video framebuffer.\n");
		printf("  Enable CONFIG_VIDEO and a display driver on this board.\n");
		return CMD_RET_FAILURE;
	}

	if (tw_fs_probe(&s->fs, iftype, devpart, fstype) < 0) {
		printf("typewriter: cannot access %s %s\n", iftype, devpart);
		return CMD_RET_FAILURE;
	}

	strncpy(s->filename, fname, TW_MAX_FILENAME - 1);
	s->filename[TW_MAX_FILENAME - 1] = '\0';

	if (tw_file_load(s, s->filename) < 0) {
		printf("typewriter: cannot load %s\n", s->filename);
		return CMD_RET_FAILURE;
	}
	if (s->num_lines == 0) {
		s->num_lines = 1;
		s->line_len[0] = 0;
	}

	tw_bind_ime(s);
	tw_poweroff_events_clear();  /* drop any power-btn/lid latch from launch */
	tw_set_cpu_408();            /* little cluster 600 -> 408 MHz (low power) */
	/* Show the exception level in the startup line: the event-stream WFE idle
	 * (low power) only engages at EL>=2. EL2 = good; EL1 would mean the idle
	 * loop is busy-spinning and power-saving isn't active. See POWERSAVE.md. */
	tw_status(s, "[ Read %d line%s%s - EL%d ]", s->num_lines,
		  s->num_lines == 1 ? "" : "s",
		  s->writable ? "" : " - read-only", current_el());

	while (!s->quit) {
		tw_render(s);
		tw_handle_key(s, tw_read_key());
	}

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	typewriter, 6, 0, do_typewriter,
	"Nano-like editor with Wubi Chinese input (framebuffer)",
	"                        - default: open mmc 1:1 a.txt (writable)\n"
	"typewriter -h                        - show this help\n"
	"typewriter <iftype> <dev:part> <filename> [fstype] [ro]\n"
	"  iftype  : mmc usb virtio nvme sata\n"
	"  dev:part: 0:1  1:2  ...\n"
	"  filename: full path on the filesystem\n"
	"  fstype  : fat ext4  (optional, auto-detect)\n"
	"  ro      : open read-only (writable by default)\n"
	"  NOTE: mmc 0 (eMMC) is ALWAYS read-only - its FAT writes\n"
	"        corrupt the card; save on mmc 1 (microSD) instead.\n"
	"Keys (readline-style):\n"
	"  ^S save  ^R open (pick)  ^X exit  ^Q power off / boot OS  ^G help\n"
	"  ^T battery %  ^B/^F char  ^P/^N line  ^A/^E bol/eol  arrows move\n"
	"  ^D del  ^W del-word-back  ^K kill-eol  ^Y yank\n"
	"  ^- dim / ^] brighten backlight (20% steps)\n"
	"  ^Space toggle Wubi/English; in Wubi: a-z code,\n"
	"  1-9/Space commit, =/- page, Esc cancel"
);
