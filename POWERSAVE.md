# Power consumption on the RK3399 Chromebook (gru/kevin)

The typewriter runs bare in U-Boot, which has no OS power management (no cpufreq
governor, no cpuidle). Out of the box it drew ~3× Linux's idle power (~20 %/hr
vs ~6 %/hr, warm CPU). This note describes the changes that fix it.

> The long list of things that **didn't** work (the WFI freeze chain, the wrong
> conclusions, PSCI CPU_SUSPEND, an abandoned "power-saving mode") lives in
> **POWERSAVE_FAILED_EFFORTS.md**. This file keeps only the working design.

---

## TL;DR — the levers that work

1. **Real WFI deep idle** *(the big one)*. The editor's idle nap is a true `WFI`
   that clock-gates the core until an interrupt, exactly how Linux idles this
   board. Woken by the NonSecure arch-timer (CNTP) tick and, early, by any
   cros_ec keypress / power button / lid. In `cmd_tw.c` (`tw_idle_nap`).
2. **CPU frequency 600 → 408 MHz** — U-Boot's fixed clock lowered to Linux's
   idle OPP. Less active power/heat. (U-Boot core change, `clk_rk3399.c`.)
3. **Dim the backlight** (`^-` / `^]`, default 40 %) — the panel is a big share
   of idle draw; U-Boot brings it up full-bright and never lowers it.
4. **Throttle the idle loop** — the EC power-button/lid poll is a SPI
   transaction; do it every 200 ms, not every nap.
5. **`CONFIG_ARMV8_UDELAY_EVENT_STREAM=y`** — makes plain `udelay()` (used
   elsewhere in U-Boot) sleep with WFE instead of hot-spinning. Not the editor's
   idle path (that's #1), but a good default. **Requires EL2** — confirmed.

---

## Fix 1 — real WFI deep idle

`tw_idle_nap()` (`cmd_tw.c`) puts the core into a genuine `WFI` sleep between
keystrokes, woken by the NonSecure physical timer (CNTP, INTID 30). This is the
same mechanism Linux uses (cpuidle state 0 = *"ARM WFI"*).

The catch — and why this took real work — is that U-Boot does none of the
interrupt setup a `WFI` needs to be woken. Four things must be in place:

1. **`HCR_EL2.IMO = 1`** — route physical IRQs to **EL2** (where U-Boot runs).
   U-Boot's `start.S` sets `HCR_EL2.AMO` (SError) but **not IMO**, so by default
   IRQs target EL1 and an EL2 `WFI` is never the interrupt's target → never
   wakes. This was the single missing piece behind a long chain of freezes.
2. **The GIC forwards NonSecure Group1** — `GICD_CTLR.EnableGrp1NS` (distributor)
   + `ICC_IGRPEN1_EL1 = 1` (CPU interface) + `ICC_PMR = 0xff` (priority mask
   open) + enable the CNTP PPI (INTID 30) in the redistributor. bl31 leaves all
   of these off for the normal world. (The arch timer is **not** secure — bl31
   only claims INTID 29, the EL3 timer.)
3. **An EL2 IRQ handler** that acks + EOIs the timer, so the taken interrupt is
   serviced (U-Boot's default `do_irq` just panics).
4. **`PSTATE.I` unmasked** during the `WFI` so the IRQ is actually *taken*.

### How `tw_idle_nap` does it

Once (`tw_wfi_setup`, first call):
`GICD_CTLR.EnableGrp1NS` + CNTP PPI enable + `ICC_PMR = 0xff` +
`ICC_IGRPEN1_EL1 = 1`. These are harmless to leave on.

Per nap:
```
save VBAR_EL2, HCR_EL2
VBAR_EL2  = tw_idle_vectors      # minimal EL2 vector table (below)
HCR_EL2  |= IMO                  # route phys IRQ to EL2
arm CNTP: cntp_tval_el0 = ms·rate ; cntp_ctl_el0 = ENABLE (IMASK=0)
msr daifclr, #2                  # PSTATE.I = 0 -> take IRQs
wfi                              # deep sleep until timer / keypress IRQ
msr daifset, #2                  # re-mask
cntp_ctl_el0 = 0                 # disable timer
restore VBAR_EL2, HCR_EL2
```

`tw_idle_vectors` is a minimal 2 KiB-aligned EL2 vector table; only the
Current-EL-SPx IRQ slot has a handler — `ack (ICC_IAR1) → disable CNTP → EOI
(ICC_EOIR1) → eret` (saving/restoring x0/x1). `VBAR_EL2` and `HCR_EL2.IMO` are
swapped **per nap** and restored after, so the rest of U-Boot keeps its own
exception handling; only the brief WFI window takes IRQs.

Result: the core genuinely clock-gates between keystrokes. arm64-only; host and
non-arm64 builds fall back to a plain `udelay`.

> How it was found: a long debug session (see POWERSAVE_FAILED_EFFORTS.md and the
> `twwfi` command). The clincher was `twwfi irq` returning after its armed
> interval once `HCR_EL2.IMO=1` + a real handler were added — the NXP i.MX forum
> advice ("open the I-bit, vectors, GIC, handler") was right all along.

---

## Fix 2 — CPU frequency 600 → 408 MHz

U-Boot's RK3399 clock driver (`drivers/clk/rockchip/clk_rk3399.c`, `rkclk_init`)
fixes both CPU clusters at **600 MHz**. Linux scales 408 MHz–1.5 GHz and idles at
408. Dropping U-Boot to **408 MHz** matches Linux's idle point and cuts active
power/heat. It complements Fix 1: WFI removes the "never sleeps" cost; the lower
clock reduces the cost of the small time the core *is* awake.

> `clk dump` here does **not** list the ARM cluster clocks (only `xin24m` + RTC),
> so verify by temperature / battery drain, not the shell.

---

## Fix 3 — dim the panel backlight (`^-` / `^]`)

U-Boot brings the LCD up at full brightness and never lowers it, so it burns the
same power idle or not. The typewriter controls it:

- Starts at **`TW_BACKLIGHT_DEFAULT` = 40 %** (`cmd_tw.h`).
- **`^-`** dims 20 %, **`^]`** brightens 20 %. Floor `TW_BACKLIGHT_MIN = 20 %`
  (never 0 / `BACKLIGHT_OFF`, which powers the PWM down and froze the panel).
- `tw_backlight_set()` / `tw_backlight_step()` in `cmd_tw_video.c`.

> Keys: Ctrl+punctuation is keymap-dependent. Verified on this EC keyboard,
> `^-` = `0x1F`, `^]` = `0x1D`; `^=` types a literal `=`, so brighten is `^]`.

### The backlight config chain must be built in

The control path is a chain, and *every* link needs its Kconfig or it silently
no-ops:

```
rk_edp (eDP) --rockchip,panel--> UCLASS_PANEL (simple-panel)
            --backlight phandle--> pwm-backlight --pwms--> cros-ec PWM
```

| Link          | Config symbol          | Notes                                  |
|---------------|------------------------|----------------------------------------|
| simple panel  | `CONFIG_SIMPLE_PANEL`  | `default y` w/ PANEL+BACKLIGHT+DM_GPIO |
| pwm backlight | `CONFIG_BACKLIGHT_PWM` | `default y` w/ BACKLIGHT+DM_PWM        |
| PWM core      | `CONFIG_DM_PWM`        | **often dropped — the usual culprit**  |
| cros-ec PWM   | `CONFIG_PWM_CROS_EC`   | gru backlight PWM lives on the EC      |

Add to the durable libreboot config (`$LB/config/u-boot/gru_kevin/config/default`):
```
CONFIG_DM_PWM=y
CONFIG_PWM_CROS_EC=y
CONFIG_BACKLIGHT_PWM=y
CONFIG_SIMPLE_PANEL=y
```
Verify after boot: `dm tree | grep -i "panel\|backlight\|pwm"` should list a
panel + backlight + pwm device. If absent, `^-`/`^]` will appear to do nothing.

---

## Fix 5 — `CONFIG_ARMV8_UDELAY_EVENT_STREAM=y`

Makes U-Boot's `__udelay()` (`arch/arm/cpu/armv8/generic_timer.c`) sleep the core
in **WFE** via the ARMv8 event stream (self-wakes every ~5 µs) instead of a full
busy-spin, whenever `current_el() >= 2`. The editor's idle path is real WFI
(Fix 1), so this mainly helps other `udelay()` callers — but it's a cheap, safe
default. Set it in the durable defconfig:

```
# $LB/config/u-boot/gru_kevin/config/default
CONFIG_ARMV8_UDELAY_EVENT_STREAM=y
```

**EL2 requirement (confirmed).** The event-stream path (and Fix 1's IMO routing)
only work at EL2+. gru/kevin runs U-Boot at **EL2**: coreboot's bl31 hands the
payload to EL2 non-secure and `CONFIG_ARMV8_SWITCH_TO_EL1` is off. The startup
line prints it (`[ Read N lines - EL2 ]`); if it ever shows EL1, idle power will
regress — check that first.

---

## Cycle-time / wake reference

The editor's key-wait loop (`tw_read_key`, `cmd_tw.c`) sleeps in `WFI` and wakes
on interrupts or on its own timed deadlines. What wakes / polls, how often, and
by what mechanism:

| Event                         | Rate / latency   | Constant           | Method |
|-------------------------------|------------------|--------------------|--------|
| **Keystroke**                 | instant (IRQ)¹   | `TW_KEY_NAP_MS` 25 ms | cros_ec keyboard IRQ (INTID 46) wakes WFI early; worst case, next 25 ms timer tick |
| **Power button**              | ≤ 200 ms         | `TW_EC_POLL_MS` 200 ms | polled: EC **host-event** flags (`cros_ec_get_host_events`), *not* the MKBP FIFO |
| **Lid close**                 | ≤ 200 ms         | `TW_EC_POLL_MS` 200 ms | same EC host-event poll (`EC_HOST_EVENT_LID_CLOSED`) |
| **Battery % (title `BAT:`)**  | 30 s             | `TW_BATT_POLL_MS` 30 s | polled: one EC `CHARGE_STATE` transaction; title repaints only if the % changed |
| **WFI wake tick**             | 25 ms            | `TW_KEY_NAP_MS` 25 ms | armed CNTP timer (INTID 30) — bounds keystroke latency, re-checks the polls |

¹ A keypress asserts the cros_ec interrupt (gpio0 PA1 → GIC INTID 46, NonSecure
  Group1), which wakes the `WFI` immediately; the 25 ms tick is only the fallback
  ceiling. Power button and lid are **polled** (not IRQ-woken) because they share
  the EC host-event channel we read every 200 ms — well under human reaction time.

Rationale for the values:
- **25 ms nap** — worst-case key-to-read latency, below perception; also the WFI
  re-check interval.
- **200 ms EC poll** — each is a SPI transaction to the EC; decoupled from the
  fast keyboard path so the bus isn't clocked every nap.
- **30 s battery** — battery moves ~1 %/several-min; near-free (repaint only on
  change).

---

## What did NOT matter

- **The device tree / `.dtb`** — describes hardware, not CPU run-mode/idle. Not
  the cause of the draw.
- **Multiple CPU cores** — U-Boot proper runs on CPU0 only; the others are parked
  by the earlier boot stage.

---

## Measuring

No `cpu`/cpufreq readout in U-Boot here, so measure indirectly:
1. **CPU temperature** — idle a few minutes; cooler = WFI idle engaged.
2. **Battery drain** — battery % over 15–30 min idle → %/hr. Target: from
   ~20 %/hr toward Linux's ~6 %/hr.
3. **Responsiveness** — typing must still feel instant (a keypress IRQ wakes WFI
   immediately).

---

## References

- `cmd_tw.c` — `tw_idle_nap()` / `tw_wfi_setup()` / `tw_idle_vectors` (Fix 1);
  `tw_read_key()` key-wait loop; `^-`/`^]` brightness keys.
- `cmd_tw_video.c` — `tw_backlight_set()` / `tw_backlight_step()` (Fix 3).
- `cmd_twwfi.c` — the `twwfi` idle/WFI diagnostic command (evidence trail).
- `arch/arm/cpu/armv8/start.S` — sets `HCR_EL2.AMO` (not IMO — the Fix 1 gap).
- `arch/arm/cpu/armv8/generic_timer.c` — `__udelay()` event-stream path (Fix 5).
- `drivers/clk/rockchip/clk_rk3399.c` — `rkclk_init()` CPU frequency (Fix 2).
- `plat/rockchip/rk3399/rk3399_def.h` (ATF) — secures INTID 29 only, not 30.
- **POWERSAVE_FAILED_EFFORTS.md** — the dead ends and wrong turns.
