# Power consumption on the RK3399 Chromebook (gru/kevin)

The typewriter runs bare in U-Boot, which has no OS power management (no cpufreq
governor, no cpuidle). This note explains why it drew far more power than Linux,
and the two changes that fix it — **neither of which is typewriter C code**;
they are U-Boot config.

---

## TL;DR

Four levers that helped, and one that was proven impossible:

1. **`CONFIG_ARMV8_UDELAY_EVENT_STREAM=y`** — makes `udelay()` sleep with `WFE`
   (event stream) instead of hot-spinning. **Confirmed EL2 on gru/kevin.** This
   is a self-waking ~5 µs WFE, not deep sleep, but it's the idle mechanism we
   use (see the WFI investigation for why deep WFI isn't available).
2. **CPU frequency 600 MHz → 408 MHz** — fixed lower OPP; less active power/heat.
3. **Dim the backlight** (`^-` / `^]`, default 40 %) — a big share of idle draw.
   Needs the backlight PWM config chain (`DM_PWM` + `PWM_CROS_EC` + …).
4. **Throttle the idle loop** (Fix 4) — EC button/lid poll cut from 100 Hz to
   5 Hz; less EC/SPI traffic between keystrokes.

**Deep WFI idle: not reachable from U-Boot here (proven).** Linux idles this
board with WFI woken by the arch-timer. We got the GIC fully configured from
NonSecure EL2 so the timer interrupt is deliverable (`ICC_HPPIR1 = 30`), but raw
WFI won't wake (that needs `SCR_EL3.IRQ`, which only EL3 sets). We then tried
the designed escape hatch — `PSCI CPU_SUSPEND`, where bl31 does the WFI at EL3 —
and it *also* hangs, even with a NonSecure interrupt force-pending (verified from
the shell with `md`/`mw`, no reflash). So deep idle needs bl31 changes we can't
make. We keep WFE. (An earlier idle "power-saving mode" that assumed a longer
`udelay` slept the CPU — it doesn't — was also removed.) See "Investigation:
real WFI idle" below.

Measured before any change: **~20 %/hr battery, warm CPU** (vs Linux ~6 %/hr,
9 h life). Reference: a suspected further draw (Linux and U-Boot) is the **WiFi
module left powered but unmanaged** — not yet addressed (needs regulator/PMIC
gating).

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

## Investigation: real WFI idle — and why it's impossible here

Deep sleep would be `WFI` (the core clock-gates until an interrupt), which is
what Linux uses to idle this board (cpuidle state 0 = *"ARM WFI"*, woken by
`arch_timer`). We tried hard to use it and **it cannot be done from where U-Boot
runs.** This section records the investigation so nobody repeats it.

Also note what this kills: an earlier idea to add an **idle "power-saving mode"**
(dim the backlight after N seconds, poll lazily at 500 ms, wake on a key) was
built on the belief that a longer `udelay` nap sleeps the CPU. It does not — the
event-stream `udelay` is a WFE spin that self-wakes every ~5 µs (Fix 1), so a
"500 ms lazy nap" is a 500 ms busy spin. Measured: no idle-power benefit
(~15 %/hr, unchanged). **That power-saving mode was removed** — it added
complexity (dim/wake/swallow logic, a `[power saving]` state) for zero CPU-idle
gain. WFE is the idle mechanism; the backlight is dimmed only on demand (`^-`).

### Why WFI needs a wake source, and the freezes

A bare `WFI` froze the board because U-Boot has no armed interrupt to wake it
(console is polled; the timer is only read, never set to fire an IRQ; no GIC
init). To use WFI we must arm a timer interrupt that becomes a WFI wake event.
Getting there was a chain of freezes, each debugged live from the U-Boot shell
with `md`/`mw` and a standalone `twwfi` test command (which does a single armed
WFI or a *safe, no-WFI* probe — see `cmd_twwfi.c`). What we found, in order:

1. **Arch-timer PPI disabled.** `GICR_ISENABLER0 = 0x20004000` — bit 30 (INTID
   30, the timer PPI) clear. Enabled it (`|= 1<<30`). Still froze.
2. **Timer armed with `IMASK=1`.** The timer only asserts its interrupt when
   `ENABLE=1 && ISTATUS=1 && IMASK=0`; with IMASK=1 it never fires. Fixed to
   `IMASK=0` and masked the IRQ at the CPU (`PSTATE.I`) instead. Still froze.
3. **`ICC_IGRPEN1 = 0`** — Group 1 disabled at the GIC CPU interface. Enabled
   it. Still froze.

At that point every "does WFI wake?" test cost a hard power-cycle, so `twwfi`
grew a **safe probe** (`twwfi probe`) that arms the timer, polls the counter
(no WFI), and reads how far the interrupt propagated. It reported:

```
timer CTL    = 0x5   ISTATUS=1        -> the timer fired
GICR_ISPENDR0= ...   bit30=1          -> it latched pending at the redistributor
ICC_HPPIR1   = 1023  (not 30)         -> the CPU interface presents NOTHING
```

So the interrupt was pending but the CPU interface wouldn't present it
(`HPPIR1 = 1023`). The missing piece turned out to be a **distributor** enable
that bl31 leaves off for the normal world:

```
GICD_CTLR = 0x35 = EnableGrp0 | EnableGrp1S | ARE_S | ARE_NS
                   -> EnableGrp1NS (bit1) is CLEAR: the distributor does not
                      forward NonSecure Group1 interrupts at all.
```

The arch timer is **not** secure. bl31 (`plat/rockchip/rk3399/rk3399_def.h`)
secures only INTID 29 (the EL3 physical timer) + two SGIs; INTID 30 (CNTP, the
NS timer) is Group1-NonSecure and ours to use. (An earlier draft of this doc
wrongly blamed a "secure priority half" — priority `0x80`/PMR `0xf8` was never
the blocker; the group-forwarding enable was.)

Setting all three NS enables — `GICD_CTLR.EnableGrp1NS`, `ICC_IGRPEN1_EL1`, and
the CNTP PPI — the safe probe finally showed **`ICC_HPPIR1 = 30`**: the timer
interrupt is now fully deliverable to our PE.

### ...and yet WFI STILL does not wake

With `HPPIR1 = 30` (interrupt deliverable, `ICC_RPR = 0xff` idle), a real armed
`WFI` **still hangs**. Everything the GIC can express from NonSecure is correct.
The reason is `SCR_EL3.IRQ`: a physical IRQ only wakes a lower-EL WFI when it is
routed to EL3, and **only EL3 can set that bit**. From NonSecure EL2, our WFI
can't be woken by an IRQ — full stop. This is confirmed by bl31's own code
(`plat/rockchip/common/plat_pm.c`, `rockchip_cpu_standby`), which for a STANDBY
CPU_SUSPEND does `write_scr_el3(scr | SCR_IRQ_BIT); wfi;` — it sets that bit
itself, precisely because a lower EL can't.

### The PSCI CPU_SUSPEND route — also a dead end (proven via md/mw)

So we tried the designed escape hatch: `PSCI CPU_SUSPEND` (STANDBY, level 0),
which makes **bl31** do the WFI at EL3 (where `SCR_EL3.IRQ` is set) and return on
a NonSecure interrupt — the same path Linux's `cpu-sleep`/`cluster-sleep` use.
It **also hangs.**

We proved this needs no reflash, entirely from the U-Boot shell with `md`/`mw`:

```
mw.l 0xfee00000 0x37          # GICD_CTLR |= EnableGrp1NS  -> reads 0x37
mw.l 0xfef10100 0x40000000    # enable INTID 30 PPI        -> reads 0x60004000
mw.l 0xfef10200 0x40000000    # FORCE INTID 30 pending     -> reads 0x40000000
twwfi suspend                 # PSCI CPU_SUSPEND(STANDBY) ... HANGS
```

With a NonSecure Group1 interrupt **already pending and deliverable**, bl31's
standby WFI *still* did not wake. So bl31's standby only wakes on the wake
sources Rockchip built it for (its own PMU/GPIO wake path), not an arbitrary NS
timer — and we can't change bl31 from U-Boot.

### Conclusion: keep WFE (proven end to end)

Deep idle is not reachable from U-Boot on this board. Both routes fail:
- **raw WFI at NS-EL2** — can't wake (needs `SCR_EL3.IRQ`, EL3-only);
- **PSCI CPU_SUSPEND** (bl31 does the WFI) — doesn't wake on our NS interrupt
  even when it's force-pending.

The GIC misconfig was real and fully solved (`HPPIR1` 1023 → 30); the remaining
wall is EL3/bl31 firmware behaviour. `tw_idle_nap()` stays a plain event-stream
`udelay` (WFE). The `twwfi` command remains in the tree as the diagnostic and
the evidence trail. (The i.MX7D NXP-forum thread in `ref/power_consumption/` is
the same shape: WFI-from-timer needs firmware/GIC
cooperation that isn't there.)

---

## Cycle-time reference

All periods are set in `cmd_tw.h`. The idle loop (`tw_read_key`) wakes every
"nap", checks the keyboard, and services the other polls on their own deadlines.

| What                          | Constant          | Value  |
|-------------------------------|-------------------|--------|
| WFE nap = keystroke latency   | `TW_KEY_NAP_MS`   | 25 ms  |
| Power button / lid poll       | `TW_EC_POLL_MS`   | 200 ms |
| Battery re-read (title `BAT:`)| `TW_BATT_POLL_MS` | 30 s   |

Each "nap" is a WFE sleep (event-stream `udelay`, Fix 1). The keyboard is checked
(`tstc()`) every nap, so typing latency is bounded by `TW_KEY_NAP_MS` (25 ms).

Notes on the choices:
- **25 ms keystroke nap** — worst-case latency from key press to it being read;
  well below perception.
- **200 ms button/lid poll** — each is an EC SPI transaction, so it's decoupled
  from the fast keyboard check; 200 ms is under human reaction time.
- **30 s battery** — battery moves ~1 %/several-min; the title only repaints when
  the integer % changes, so this is near-free.

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

- **WFI deep-idle → dead end (held at EL3 by firmware).** A long chain of
  freezes; the GIC side turned out fully solvable from NonSecure EL2 (enable
  `GICD_CTLR.EnableGrp1NS` + `ICC_IGRPEN1_EL1` + the CNTP PPI → `ICC_HPPIR1 = 30`,
  interrupt deliverable), but WFI *still* won't wake because IRQs are routed to
  EL3 / WFI is trapped by bl31. Full story in "Investigation: real WFI idle"
  above. We use **WFE + event stream** instead.
- **Idle "power-saving mode" (dim after 20 s + lazy 500 ms poll) → removed.** It
  assumed a longer `udelay` sleeps the CPU; it doesn't (WFE self-wakes every
  ~5 µs), so it saved no idle power while adding dim/wake/swallow complexity.
- A full **timer-interrupt + GIC** setup can't help either, since the timer IRQ
  itself is secure (see the i.MX8 case in `ref/power_consumption/` where a
  similar attempt hung for a related reason).

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
