# Power consumption on the RK3399 Chromebook (gru/kevin)

The typewriter runs bare in U-Boot, which has no OS power management (no cpufreq
governor, no cpuidle). This note explains why it drew far more power than Linux,
and the two changes that fix it — **neither of which is typewriter C code**;
they are U-Boot config.

---

## TL;DR

Six levers. The idle-power ones are Fix 6 (real WFI sleep) and Fix 5 (dim +
lengthen the sleep when idle); the rest are supporting.

1. **`CONFIG_ARMV8_UDELAY_EVENT_STREAM=y`** — makes plain `udelay()` sleep with
   `WFE` (event stream) instead of busy-spinning. **Confirmed EL2 on gru/kevin.**
   NB: this is a self-waking ~5 µs spin, good for short delays but *not* real
   idle for the key-wait loop — that's what Fix 6 replaced it with.
2. **CPU frequency 600 MHz → 408 MHz** — fixed lower OPP; less active power/heat.
3. **Dim the backlight** (`^-` / `^]`, default 40 %) — a big share of idle draw.
   Needs the backlight PWM config chain (`DM_PWM` + `PWM_CROS_EC` + …).
4. **Throttle the idle loop** (Fix 4) — EC button/lid poll cut from 100 Hz to
   5 Hz; less EC/SPI traffic between keystrokes.
5. **Idle power-saving mode** (Fix 5) — after 20 s idle, dim + `[power saving]` +
   stretch the sleep to 500 ms. Any key wakes and that key still types.
6. **Real WFI idle** (Fix 6) — **the key fix.** Each nap is now a true `WFI`
   sleep woken by an armed CNTP timer (or a keypress IRQ), so the core actually
   clock-gates. The earlier "lazy poll" was a WFE busy-spin that never slept.
   Modelled on Linux, which idles this board the same way (WFI + arch_timer).

Measured before any change: **~20 %/hr battery, warm CPU** (vs Linux ~6 %/hr,
9 h life). The WFE "throttle" (Fixes 4–5) alone did *not* help (35 min → 9 %,
~15 %/hr) because the CPU never actually slept — Fix 6 is what addresses that.
Reference: a suspected further draw (Linux and U-Boot) is the **WiFi module left
powered but unmanaged** — not yet addressed (would need regulator/PMIC gating).

---

## Why it was hot / thirsty

The editor waits for a keypress in a poll loop (`cmd_tw.c`, `tw_read_key`):

```c
while (!tstc()) {
    schedule();
    udelay(10000);   /* 10 ms between polls */
}
```

U-Boot's console is **polled** — there is no "sleep until a key" primitive — so
the loop runs ~99 % of the time (you're not typing most of the time). The
problem was **what `udelay()` does**: by default it is a **busy-spin** — the
core executes the delay loop at full clock the entire 10 ms, never sleeping.

Linux idles the *same* core in **WFI** ~99 % of the time (near-zero power) and
scales frequency down. So U-Boot burned ~3× the power and ran warm, even though
"nothing was happening." **Duty cycle (spinning vs sleeping) dominated; the
clock frequency was the smaller half.**

---

## Fix 1 — event-stream `udelay` (the big one)

U-Boot already contains the mechanism; it's just off by default on this board.
With **`CONFIG_ARMV8_UDELAY_EVENT_STREAM=y`**, `__udelay()`
(`arch/arm/cpu/armv8/generic_timer.c`) becomes:

```c
if (current_el() >= 2) {
    /* enable the architectural timer's EVENT STREAM ... */
    while (get_ticks() + (1 << event_period) <= target)
        wfe();          /* SLEEP until the next event-stream tick */
}
while (get_ticks() <= target) ;   /* fine-grained tail, polled */
```

So the editor's existing `udelay(10000)` now **sleeps the core in `WFE`** for
almost the whole 10 ms instead of spinning. **No typewriter code changes** — the
same loop, but the delay no longer burns the CPU.

### Why WFE, not WFI — root cause of the WFI freeze

We first tried `WFI` directly in the key loop. **It froze the board.** Here is
exactly why, confirmed against the U-Boot source and the ARMv8 architecture.

**What wakes a WFI.** Per the ARM ARM, `WFI` completes on a *WFI wake-up event*:
a pending physical **IRQ / FIQ / SError / debug** event — and notably this
happens **even if that interrupt is masked** in `PSTATE.DAIF`. Masking only stops
the exception from being *taken*; it does not stop the pending interrupt from
*waking* WFI. So "interrupts are masked" is **not** why WFI hung.

**The actual cause: nothing ever generates an interrupt.** In U-Boot proper on
this board there is *no interrupt source at all*:
- The console/keyboard is **polled** (`tstc()`), never IRQ-driven.
- The generic timer is used **only by polling `CNTPCT_EL0`** (`get_ticks()` in
  `arch/arm/cpu/armv8/generic_timer.c`); U-Boot never programs `CNTP_CTL_EL0` /
  `CNTP_TVAL_EL0` to *fire* a timer interrupt.
- No GIC driver is compiled for rk3399 in U-Boot proper (the GICv3 exists in the
  DT and hardware, but nothing initialises it), and `arch/arm/lib/interrupts_64.c`
  has `enable_interrupts()` / `disable_interrupts()` as **no-ops** with `do_irq()`
  set to `panic()`.

So `WFI` puts the core to sleep waiting for an event that is **never produced** →
permanent halt. The lesson: WFI on bare-metal U-Boot needs an interrupt you have
explicitly armed; there is none here.

**Why WFE works instead.** `WFE` wakes on all the WFI events **plus** an *event*,
and the ARMv8 **event stream** makes the architectural timer emit a periodic
event automatically once `CNTHCTL_EL2.EVNT_EN` is set (which the event-stream
`udelay` path does). Period is `2^EVNTI` counter ticks — the code uses
`EVNTI = 7` → every 128 ticks; at RK3399's 24 MHz timer that is **~5.3 µs**. So
the core naps in a low-power state and self-wakes every ~5 µs with **no
interrupt, no GIC, no handler** — it can never hang the way WFI did.

**Could we make WFI work (deeper sleep)?** In principle yes: arm the generic
timer to raise a periodic **CNTP interrupt**, route the timer PPI through the GIC
to the CPU (`GICD` group-enable + `ICC_*` CPU interface), and replace the
panicking `do_irq` with a handler that just acknowledges the timer. WFI is deeper
than WFE (the core can gate its clock fully until the IRQ). But it is a lot of
fragile new plumbing — and the i.MX8 report in `ref/power_consumption/` shows how
it goes wrong (their timer PPI never reached the CPU because `GICD_CTLR.
EnableGrp1NS` wouldn't set, so *their* WFI hung too). The WFE event stream gets
most of the benefit for one config bit and zero interrupt code, so we stay with
it. Arming a timer IRQ is a possible future optimisation, not a fix that's needed.

### Requirement: EL2 or above  — CONFIRMED EL2 on gru/kevin

The event-stream path only runs when `current_el() >= 2` (see the `if` above).
**We confirmed gru/kevin runs U-Boot at EL2**, two ways:
- Config: coreboot uses ARM Trusted Firmware (`ARM64_USE_ARM_TRUSTED_FIRMWARE=y`),
  bl31 runs at EL3 and hands the payload (U-Boot) to EL2 non-secure; and U-Boot's
  `CONFIG_ARMV8_SWITCH_TO_EL1` is **off**, so it stays at EL2.
- Runtime: the typewriter prints the level in its startup status line
  (`[ Read N lines - EL2 ]`) via `current_el()`, and `^Q` prints it on any
  poweroff failure. Look for **EL2**.

So the event-stream WFE idle *is* active. If a build ever showed **EL1** there,
`__udelay()` would silently fall back to busy-poll (no saving) — that's the first
thing to check if idle power regresses.

### Enabling it

It's a U-Boot config option, so set it in the **durable** libreboot defconfig so
it survives config regeneration:

```
# $LB/config/u-boot/gru_kevin/config/default
CONFIG_ARMV8_UDELAY_EVENT_STREAM=y
```

Or, for a quick test, on the live config (wiped by the next `./mk -m u-boot`):

```sh
cd $LB/src/u-boot/default
./scripts/config -e ARMV8_UDELAY_EVENT_STREAM
make olddefconfig
grep ARMV8_UDELAY_EVENT_STREAM .config     # -> CONFIG_ARMV8_UDELAY_EVENT_STREAM=y
```

Then rebuild U-Boot + coreboot (clean U-Boot + clear the staged ELF so coreboot
re-pulls it):

```sh
cd $LB
./mk -c u-boot gru_kevin
./mk -b u-boot gru_kevin
rm -f elf/u-boot/default/gru_kevin/default/*
./mk -b coreboot gru_kevin
```

---

## Fix 2 — CPU frequency 600 → 408 MHz

U-Boot's RK3399 clock driver (`drivers/clk/rockchip/clk_rk3399.c`, `rkclk_init`)
sets both CPU clusters to a fixed **600 MHz** at init (the lowest preset it
defines). Linux scales between **408 MHz and 1.5 GHz** and sits at 408 when
idle. Dropping U-Boot to **408 MHz** matches Linux's idle point and reduces
active power and heat.

This is a clock-driver change (not typewriter code, and it touches U-Boot
core), done separately. It complements Fix 1: the event stream removes the
"never sleeps" cost; the lower clock reduces the cost of the (small) time the
core *is* awake.

> Note: `clk dump` on this board does **not** list the ARM cluster clocks
> (`armclkl`/`armclkb`) — it only shows `xin24m` and the RTC — so you can't read
> the running CPU frequency from the U-Boot shell here. Verify the effect by
> temperature / battery drain instead.

---

## Fix 3 — dim the panel backlight (`^-` / `^=`)

If Fix 1 + Fix 2 did **not** move the needle (event stream confirmed set, drain
still ~18 %/hr), the dominant draw is almost certainly the **LCD backlight**, not
the CPU. U-Boot brings the panel up at **full brightness** and never lowers it,
so it burns the same power whether you're typing or idle.

The typewriter now controls it:

- Starts at **`TW_BACKLIGHT_DEFAULT` = 60 %** (see `cmd_tw.h`).
- **`^-`** (Ctrl-minus) dims 20 %, **`^=`** (Ctrl-equal) brightens 20 %.
- **0 %** fully powers the backlight PWM down (`BACKLIGHT_OFF`); the next `^=`
  brings it back. A status line shows the current percent.

Implementation is confined to the typewriter: `tw_backlight_set()` /
`tw_backlight_step()` in `cmd_tw_video.c`, keys handled in `cmd_tw.c`.

### THE CATCH — the backlight config chain must be built in

This does **nothing** unless U-Boot actually binds a controllable backlight
device. On gru/kevin the control path is a **chain**, and *every* link needs its
Kconfig or the whole thing silently no-ops:

```
rk_edp (eDP) --rockchip,panel--> UCLASS_PANEL (simple-panel)
            --backlight phandle--> pwm-backlight --pwms--> cros-ec PWM
```

| Link            | Config symbol         | Notes                                  |
|-----------------|-----------------------|----------------------------------------|
| simple panel    | `CONFIG_SIMPLE_PANEL` | `default y` w/ PANEL+BACKLIGHT+DM_GPIO |
| pwm backlight   | `CONFIG_BACKLIGHT_PWM`| `default y` w/ BACKLIGHT+DM_PWM        |
| PWM core        | `CONFIG_DM_PWM`       | **often dropped — the usual culprit**  |
| cros-ec PWM     | `CONFIG_PWM_CROS_EC`  | gru backlight PWM lives on the EC      |

The stock `chromebook_kevin_defconfig` sets `PWM_CROS_EC`, `PWM_ROCKCHIP`, and
`REGULATOR_PWM`. **A libreboot config that omits `DM_PWM`/`PWM_CROS_EC` cannot
control the backlight at all** — the code above finds no device and does nothing.
Add to your libreboot U-Boot config:

```
# $LB/config/u-boot/gru_kevin/config/default
CONFIG_DM_PWM=y
CONFIG_PWM_CROS_EC=y
CONFIG_BACKLIGHT_PWM=y
CONFIG_SIMPLE_PANEL=y
```

Verify in the U-Boot shell after boot: `dm tree | grep -i "panel\|backlight\|pwm"`
should list a panel + backlight + pwm device. If they're absent, the config
didn't take and `^-`/`^=` will appear to do nothing.

> Note on the keys: Ctrl chording punctuation is keymap-dependent. `^-` is bound
> to `0x1F` and `^=` to `0x1D` (the bytes most US layouts emit). If a keypress
> shows no brightness change on the real EC keyboard, tell me the raw byte and
> I'll rebind — the values live in `cmd_tw.h` (`KEY_CTRL_MINUS`/`KEY_CTRL_EQUAL`).

---

## Fix 4 — throttle the idle key-wait loop

Fix 1 makes each `udelay()` a WFE sleep, but the *loop around it* still does work
every iteration. Two things ran too often in the editor's key-wait loop
(`tw_read_key`, `cmd_tw.c`):

- **The EC host-event poll** (power button / lid) was a **SPI transaction to the
  EC every ~10 ms (100 Hz)** — clocking the bus and waking the EC MCU constantly.
- The nap itself was only **10 ms**, so the core woke 100×/s even when idle.

Changes (constants in `cmd_tw.h`):
- `TW_KEY_NAP_MS` **10 → 25 ms** — the per-iteration WFE nap. This is the
  worst-case keystroke latency (still imperceptible), and the core now wakes
  ~40×/s instead of 100×/s.
- `TW_EC_POLL_MS` **= 200 ms** — the EC button/lid poll is now throttled to once
  per 200 ms (~8× fewer EC/SPI transactions). Human reaction time is ~150 ms, so
  a button press or lid close is still caught effectively instantly.

The keyboard is still checked (`tstc()`) every nap, so typing latency is bounded
by `TW_KEY_NAP_MS` (25 ms), not the EC interval. Battery %/backlight are
unaffected (battery already polls at 30 s).

---

## Fix 5 — idle power-saving mode (dim + go lazy)

The biggest idle lever, modelled on how terminals go idle: after
`TW_IDLE_SAVE_MS` (20 s) with no keystroke the editor enters **power-saving
mode** and stays there until you press a key.

While saving:
- the **backlight dims to the minimum** (`tw_backlight_dim()` — remembers your
  chosen level, doesn't clobber it);
- the title bar shows **`[power saving]`** and all chrome bars go black;
- the idle loop goes **lazy**: WFI nap and EC poll both stretch to
  `TW_SAVE_NAP_MS` (500 ms) instead of 25/200 ms, so the core (real WFI sleep,
  Fix 6) and the EC/SPI bus are almost entirely quiet.

Waking: **any key** wakes it, and **that key still types** — the wait ends
normally, the key handler notices power-saving is on, wakes (restores brightness
+ normal cadence, repaints), then processes the key. So no keystroke is lost; you
only pay the ≤500 ms wake latency on that first key.

Power button / lid still work while saving (checked on the 500 ms lazy poll, so
up to ~0.5 s latency there — fine for poweroff).

Tunables in `cmd_tw.h`: `TW_IDLE_SAVE_MS` (idle timeout), `TW_SAVE_NAP_MS` (lazy
cadence). Implementation: the wait loop (`tw_read_key`) tracks `last_input` and
returns synthetic `KEY_PWRSAVE` / `KEY_PWRSAVE_WAKE`; the handler flips
`s->power_saving`, dims/restores, and toggles the loop's lazy flag.

---

## Fix 6 — REAL WFI idle (the nap actually sleeps now)

**Correcting an earlier mistake.** Fixes 4–5 made the idle loop *nap* in
`udelay()`, believing that was low-power because of the event stream (Fix 1).
It is not, for the loop's purpose: the event-stream `udelay` is a **WFE spin that
self-wakes every ~5 µs** (EVNTI=7 → 128 ticks at 24 MHz). So a "500 ms nap" was
really a 500 ms *busy spin* — the core never slept, it just did the EC/keyboard
work less often. Measured: 35 min → 9 % (~15 %/hr), i.e. **no CPU idle benefit**.
(Thanks to the ARM review that caught this.)

**The real fix: WFI woken by an armed timer whose PPI is enabled.** Linux on this
board proves the path — cpuidle state 0 is *"ARM WFI"*, woken by `arch_timer`
(GICv3 **INTID 30**, the non-secure physical timer `CNTP`), CPUs at **EL2**. The
editor now naps with:

```
enable INTID 30 in the GIC redistributor (once)      ; GICR_ISENABLER0 |= 1<<30
arm CNTP to fire in `ms`  (cntp_tval_el0 = ms·rate, cntp_ctl_el0 = ENABLE|IMASK)
wfi()                     ; core clock-gates until timer expiry OR a keypress IRQ
disable CNTP
```

### Why the FIRST attempt froze (debugged live via `md`/`mw`)

The first version did only "arm CNTP + WFI" and **hung**. Reading the GIC from
the U-Boot shell found the cause:

```
=> md.l 0xfef10100 1      # GICR_ISENABLER0 (CPU0 redistributor)
   0xfef10100: 20004000   # bit 30 CLEAR -> INTID 30 (arch timer PPI) DISABLED
```

The `CNTP` condition fired, but with its PPI **disabled in the redistributor**
the GICv3 never forwarded it, so **no WFI wake-up event reached the core** → it
slept forever. Linux avoids this because its `arch_timer` driver enables the PPI
(`enable_percpu_irq`); we never did. Enabling bit 30 live confirmed the fix:

```
=> mw.l 0xfef10100 0x40000000   # write-1-to-set bit 30
=> md.l 0xfef10100 1
   0xfef10100: 60004000         # bit 30 now set; board stayed alive (no storm)
```

So `tw_wfi_nap()` writes `GICR_ISENABLER0 |= (1<<30)` once (`tw_timer_ppi_enable`)
before its first WFI. We keep the timer IRQ **masked** (`CNTP_CTL.IMASK=1`) and
leave DAIF masked: a *pending enabled* interrupt still wakes WFI regardless of the
PSTATE mask, but is never **taken**, so U-Boot's panicking `do_irq` never runs and
no handler is needed (bl31 already brought the GICv3 up). A cros_ec keypress IRQ
is likewise a wake event (early wake); if not, the timer tick wakes us within
`ms`. No self-test / fallback is needed now that the freeze cause is fixed at the
source.

The redistributor address (`GICR_ISENABLER0 = 0xfef10100` for CPU0 on rk3399) was
verified by the `md`/`mw` session above; it is a `#define` in `cmd_tw.c`.

Host-build note: the real `mrs`/`msr`/`wfi` + `writel` are compiled only under
`CONFIG_ARM64 && __aarch64__`; the `.cc` functest (and any non-arm64 build) uses
a `udelay` stub — important on Apple-Silicon hosts where `__aarch64__` is true
but EL0 can't run these.

Net effect: awake, 25 ms naps become real WFI sleeps (same latency ceiling);
power-saving 500 ms naps become **real 500 ms sleeps** — the core wakes twice a
second, checks, sleeps again. This is the change that should move idle drain
toward Linux's ~6 %/hr.

---

## Cycle-time reference

All periods are set in `cmd_tw.h`. The idle loop (`tw_read_key`) wakes every
"nap", checks the keyboard, and services the other polls on their own deadlines.

| What                          | Constant           | Awake    | Power-saving |
|-------------------------------|--------------------|----------|--------------|
| WFI nap = keystroke latency   | `TW_KEY_NAP_MS`    | 25 ms    | 500 ms¹      |
| Power button / lid poll       | `TW_EC_POLL_MS`    | 200 ms   | 500 ms¹      |
| Battery re-read (title `BAT:`)| `TW_BATT_POLL_MS` / `TW_BATT_SAVE_MS` | 30 s | 60 s² |
| Idle before power-saving      | `TW_IDLE_SAVE_MS`  | 20 s     | —            |
| Lazy nap/poll while saving    | `TW_SAVE_NAP_MS`   | —        | 500 ms       |

Each "nap" is a real WFI sleep (Fix 6), woken by the armed CNTP timer at the nap
period or early by a keypress IRQ — the core is genuinely asleep in between, not
spinning.

¹ In power-saving mode the nap and the EC (button/lid) poll both run at
  `TW_SAVE_NAP_MS` (500 ms). So a keypress, power button, or lid close is noticed
  within ~0.5 s — and a keypress *wakes* the editor (restoring the 25 ms / 200 ms
  awake cadence) and **still types**, so no keystroke is lost.

² Battery keeps refreshing while saving (the dimmed screen is still readable),
  just at 60 s instead of 30 s. The `KEY_REFRESH` handler only updates the title
  — it does not leave power-saving. The 20 s idle timer only runs while awake and
  resets on every keystroke.

Notes on the choices:
- **25 ms keystroke nap** — worst-case latency from key press to it being read;
  well below perception, and one real WFI sleep per nap.
- **200 ms button/lid poll** — each is an EC SPI transaction, so it's decoupled
  from the fast keyboard check; 200 ms is under human reaction time.
- **30 s / 60 s battery** — battery moves ~1 %/several-min; the title only
  repaints when the integer % changes, so this is near-free either way. Slower
  (60 s) while saving since a dim, idle screen doesn't need a snappy gauge.
- **20 s → power-saving** — terminals go idle around this mark (the reference
  point: most stop blinking the cursor ~15 s). Short enough to save power during
  a real pause, and since the waking key still types, an early trip is harmless.

---

## What did NOT matter

- **The device tree / `.dtb`.** The DT describes hardware; it does not set the
  CPU's run mode or idle behaviour. Display and PSCI work, so the DTB is fine.
  It was not the cause of the power draw.
- **Multiple CPU cores.** U-Boot proper runs on a single core (CPU0); the
  others are left parked by the earlier boot stage. Not a spinning-cores
  problem. (There is no `cpu` command in this build to enumerate them; enable
  `CONFIG_CMD_CPU` if you want `cpu list`/`cpu detail`.)

---

## Measuring

There's no `cpu`/cpufreq readout in U-Boot here, so measure indirectly:

1. **CPU temperature** — let the typewriter sit idle a few minutes; feel the
   bottom / read the sensor. Cooler after Fix 1 = the event stream engaged.
2. **Battery drain** — note battery % over 15–30 min of idle and extrapolate to
   %/hr. Target: down from ~20 %/hr toward Linux's ~6 %/hr.
3. **Responsiveness** — typing must still feel instant. `udelay(10000)` still
   returns after 10 ms; it just sleeps instead of spinning, so latency is
   unchanged.

---

## History / dead ends

- **WFI in the key loop → froze the board.** Root cause: U-Boot proper has *no*
  interrupt source (polled console, timer only polled via `CNTPCT`, no GIC init),
  so the WFI wake event is never produced — masking is a red herring. Full
  analysis in "Why WFE, not WFI — root cause of the WFI freeze" above. Fix: use
  **WFE + event stream**, which self-wakes from the timer with no interrupt.
- A full **timer-interrupt + GIC** setup would let WFI sleep deeper, but it is
  much more involved and risky (see the i.MX8 case in `ref/power_consumption/`
  where the same attempt hung); the event stream achieves near-equivalent idle
  with one config option and no interrupt plumbing.

---

## References

- `arch/arm/cpu/armv8/generic_timer.c` — `__udelay()` event-stream path.
- `arch/arm/cpu/armv8/Kconfig` — `CONFIG_ARMV8_UDELAY_EVENT_STREAM`.
- `arch/arm/include/asm/system.h` — `wfe()` / `wfi()`.
- `drivers/clk/rockchip/clk_rk3399.c` — `rkclk_init()` CPU frequency presets.
- `cmd_tw.c` — `tw_read_key()` key-wait loop (unchanged; benefits from Fix 1);
  `^-`/`^=` brightness keys in `tw_handle_key()`.
- `cmd_tw_video.c` — `tw_backlight_set()` / `tw_backlight_step()` (Fix 3).
- `drivers/video/panel-uclass.c` — `panel_set_backlight()` (panel → backlight).
- `drivers/video/simple_panel.c` — resolves the panel's `backlight` phandle.
- `drivers/video/rockchip/rk_edp.c` — `panel_enable_backlight()` on eDP enable.
- `include/backlight.h` — `BACKLIGHT_OFF` (-1), `backlight_set_brightness()`.
