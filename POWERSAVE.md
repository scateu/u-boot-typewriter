# Power consumption on the RK3399 Chromebook (gru/kevin)

The typewriter runs bare in U-Boot, which has no OS power management (no cpufreq
governor, no cpuidle). This note explains why it drew far more power than Linux,
and the two changes that fix it — **neither of which is typewriter C code**;
they are U-Boot config.

---

## TL;DR

Two levers, both U-Boot **config** (no `cmd_tw.c` change):

1. **`CONFIG_ARMV8_UDELAY_EVENT_STREAM=y`** — makes U-Boot's `udelay()` (which
   the editor's key-wait loop spins in while idle) **sleep the CPU with `WFE`**
   via the ARM event stream instead of busy-spinning. This is the big idle-power
   win, and it is **safe** (see "Why WFE, not WFI" below).
2. **CPU frequency 600 MHz → 408 MHz** — U-Boot clocks the RK3399 at a fixed
   600 MHz; dropping to 408 MHz (Linux's lowest OPP) cuts active power and heat.

Measured before any change: **~20 %/hr battery, warm CPU** (vs Linux ~6 %/hr,
9 h life). The busy-spin was the dominant cause.

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

### Why WFE, not WFI (this is the crucial part)

We first tried `WFI` directly in the key loop. **It froze the board.** On this
platform U-Boot has no periodic timer *interrupt* and the console is polled, so
`WFI` halted the core and *nothing ever woke it* → dead typewriter.

`WFE` is different: it wakes on the ARM **event stream**, a signal the
architectural timer generates **unconditionally** on a fixed period once
`CNTHCTL_EL2.EVNT_EN` is set (the event-stream code sets it). No interrupt, no
GIC, no handler — the wake is guaranteed by the timer hardware. So `WFE` can
**never hang** the way `WFI` did. This is why the fix is "enable the event
stream," not "put WFI in the loop."

### Requirement: EL2 or above

The event-stream path only runs when `current_el() >= 2` (see the `if` above).
On gru/kevin, U-Boot runs at EL2/EL3 (consistent with PSCI SMC working), so it
applies. If a build somehow ran U-Boot at EL1, `__udelay()` would silently fall
back to the busy-poll (no harm, but no saving) — if power doesn't improve after
enabling the option, check the exception level first.

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

- **WFI in the key loop → froze the board.** No wake source (polled console, no
  timer IRQ in U-Boot proper). Reverted. The lesson: use **WFE + event stream**,
  which self-wakes, not WFI.
- A full **timer-interrupt + GIC** setup would also let WFI sleep, but it is
  much more involved and risky; the event stream achieves the same idle with a
  single config option and no interrupt plumbing.

---

## References

- `arch/arm/cpu/armv8/generic_timer.c` — `__udelay()` event-stream path.
- `arch/arm/cpu/armv8/Kconfig` — `CONFIG_ARMV8_UDELAY_EVENT_STREAM`.
- `arch/arm/include/asm/system.h` — `wfe()` / `wfi()`.
- `drivers/clk/rockchip/clk_rk3399.c` — `rkclk_init()` CPU frequency presets.
- `cmd_tw.c` — `tw_read_key()` key-wait loop (unchanged; benefits from Fix 1).
