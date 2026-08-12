# Power / current-draw measurements — Kevin (RK3399)

Battery current read from the SBS smart-battery gauge (`sbs-9-000b`, reg 0x0A
`Current()`, signed mA, negative = discharging). In U-Boot this is read via the
EC I2C tunnel in the `typewriter` command (Ctrl-T status line, and
`twwfi cpuinfo`). In Linux it's `/sys/class/power_supply/sbs-9-000b/current_now`
(µA). Same physical gauge on both sides, so the numbers are directly comparable.

All figures are on **battery (AC unplugged)**, discharging. Readings are noisy
(±10–20 mA); treat single samples as approximate.

## MASTER LEVER TABLE (all measured, most→least, 2026-08)

Every power lever investigated, with its measured battery-current contribution.
"Noise floor" = ±15 mA (SBS gauge, ~1 Hz). Sorted by size.

| Lever                          | Contribution | Status / notes                       |
|--------------------------------|--------------|--------------------------------------|
| DDR 928 → 400 MHz              | **~78 mA**   | SHIPPED (`tw_set_ddr_400`). Biggest firmware lever. Linux-confirmed. |
| Backlight (per ~20% step)      | **~50–83 mA**| user `^-`/`^]`; ~50 mA for the whole LED at 40%. Largest overall. |
| USB dongle attached (VBUS)     | ~24–51 mA    | external device VBUS; only if plugged in; NOT gateable (see below). |
| USB3 controller/PHY domain     | **~14 mA**   | SHIPPED (`tw_gate_usb3`). Borderline but consistent + free. |
| **WFI vs busy-spin (deep idle)** | **~8–11 mA** | **NOISE — the WFI idle saves only a few mA. Correct + harmless, but NOT a power lever.** |
| CPU 600→408 MHz + 0.90→0.80 V  | ~few mA      | below noise for power; done for HEAT, not battery. |
| PSCI cpu-sleep / cluster-sleep | **0 mA**     | even gating the whole A53 cluster+L2 saves nothing. |
| GPU domain gate / GPU rail     | ~0 / refused | domain gate ~0; rail disable refused by DM. |
| WiFi/BT, GPU freq, audio rail  | ~0–3 mA      | noise; WiFi already off. |

**Takeaway on WFI specifically:** the deep-WFI idle path (`tw_idle_nap`, EC-IRQ
wake, no timer) works perfectly and is worth keeping for responsiveness/cleanliness,
but its *power* contribution is only ~8–11 mA — inside the noise floor. The A53
core simply isn't where the battery goes (rail doesn't gate on WFI; see the WFI
sections below for the two-stack proof). Do not expect WFI tuning to move battery.

## U-Boot `typewriter`, idle (deep WFI)

Build with DDR-400 + CPU-408 wired into typewriter startup unless noted.

| Config                                   | Current  |
|------------------------------------------|----------|
| 40% brightness + USB network card        | -621 mA  |
| 40% brightness, no card                  | -597 mA  |
| 20% brightness, no card                  | -514 mA  |
| 20% brightness, DDR 928 MHz              | -569 mA  |
| 20% brightness, DDR 400 MHz              | -518 mA  |

### Isolated levers (U-Boot)

| Lever                          | Delta      | Notes                                    |
|--------------------------------|------------|------------------------------------------|
| DDR 928 → 400 MHz              | **~51 mA** | 569→518 @ 20%. Always-on. Wired in.      |
| Backlight 40% → 20%            | **~83 mA** | 597→514. Biggest single measured lever.  |
| USB network card (remove)      | ~24 mA     | 621→597. External peripheral.            |
| CPU 600→408 MHz + 0.90→0.80 V  | below noise| Idle WFI core draws little regardless.   |

DDR at 400 MHz is confirmed to move real power (not just the reported GET_RATE),
and matches Linux's idle DDR rate — so that lever is at parity with Linux.

## Linux (mainline, schedutil), idle — THE TARGET

Backlight at 40% (`brightnessctl` = raw 1638 / 4095).

| Config                          | Current   |
|---------------------------------|-----------|
| 40% brightness + USB adapter    | -465 mA   |
| **40% brightness, no adapter**  | **-366 mA** ← target |

The USB network adapter costs **~100 mA** (465→366), NOT the ~24 mA seen in the
earlier U-Boot readings — earlier measurements were confounded by it. **-366 mA
is the real Linux idle draw and the number to pursue.**

Static-boot context (read 2026-08-12, ~1 min uptime, some boot churn still
active so true idle marginally lower):

| Metric                     | Value                        |
|----------------------------|------------------------------|
| Battery voltage            | 8.186 V                      |
| Backlight                  | 1638 / 4095 (raw, type=raw)  |
| DDR (`memory-controller`)  | 400 MHz (simple_ondemand)    |
| GPU devfreq                | 500 MHz                      |
| CPU cores online           | 6 (0-5), schedutil           |
| CPU freq                   | little 1200 MHz / big 1416 MHz |
| Load avg                   | 0.47                         |

## IMPORTANT: U-Boot and Linux drive DIFFERENT backlight controllers

They are NOT the same PWM on different scales — they are different hardware:

- **Linux** drives the **RK3399 SoC PWM** (`ff420000.pwm`), raw 0-4095, linear
  (DT has no `brightness-levels` table). 40% = raw 1638.
- **U-Boot typewriter** drives the **ChromeOS EC PWM** (`CONFIG_PWM_CROS_EC`)
  via the U-Boot backlight/panel uclass, as a **0-100 percent**
  (`tw_backlight_set(pct)` → `backlight_set_brightness` / `panel_set_backlight`).

So there is NO reason U-Boot "%" ≈ Linux "%". Confirmed by eye: **Linux 40%
(raw 1638) looks visibly darker than typewriter's old 20% floor.** So the
earlier -518 mA "U-Boot 20%" reading was at a genuinely BRIGHTER panel than
Linux's -366 mA at 40%. A large part of the apparent firmware gap was just more
light, not efficiency.

To compare fairly you must match brightness **by eye** (dim typewriter until the
panel visually matches Linux), not by number — the numbers are on unrelated
scales.

### Brightness config change (2026-08-12) to enable a fair comparison

Typewriter previously clamped at `TW_BACKLIGHT_MIN = 20%`, which was still
brighter than Linux 40% — so it could not be dimmed to match. Lowered to allow
much darker:

| Constant               | Old | New | Note                                   |
|------------------------|-----|-----|----------------------------------------|
| `TW_BACKLIGHT_MIN`     | 20  | 5   | nonzero floor safe; only 0/OFF froze it |
| `TW_BACKLIGHT_STEP`    | 20  | 5   | finer `^-`/`^]` control                |
| `TW_BACKLIGHT_DEFAULT` | 40  | 35  | starts dimmer                          |

(The historical freeze was at literal 0 / `BACKLIGHT_OFF`, which powers the PWM
and its regulator fully down. A low nonzero % keeps them alive.)

## Comparison at matched DDR (both 400 MHz), no USB adapter

| Stack   | Current  | Backlight                    | CPU                        |
|---------|----------|------------------------------|----------------------------|
| Linux   | **-366 mA** | RK3399 PWM raw 1638 (40%)  | 6 cores, 1.2/1.4 GHz, idled |
| U-Boot  | (to re-measure at matched brightness) | EC PWM %, dimmable to 5% | 1 core, 408 MHz, deep WFI |

The old U-Boot readings (-518 @ 20%) were at higher brightness AND with the
adapter confound removed only partly — they are NOT comparable to -366 and
should not be read as "U-Boot is worse". Re-measure typewriter dimmed to match
Linux visually, no adapter, to get the true firmware delta.

Linux drawing -366 mA while running 6 faster cores is consistent with the
"Linux is cooler" fact: it's **idle residency** (cpuidle parks cores/rails/PLLs
per idle cycle), not core count or clock. Typewriter already deep-WFIs one core;
remaining gap is (a) brightness (now matchable) and (b) rails/clocks Linux gates
at idle that U-Boot leaves on.

## WFI on/off is a WASH (measured both stacks) — the CPU is NOT the lever

`twwfi wfibench` (busy-spin vs CNTP-woken deep WFI, same brightness/DDR/freq):

| Stack           | busy-spin | deep WFI | WFI saves            |
|-----------------|-----------|----------|----------------------|
| U-Boot (raw)    | -906 mA   | -895 mA  | ~11 mA (< noise)     |
| Linux           | -463 mA   | -455 mA  | ~8 mA  (< noise)     |

Both stacks agree: idling the core via WFI saves ~8-11 mA, **inside the ±15 mA
gauge noise = effectively zero**. This is the SILICON, not a firmware bug: the
A53 core rail does NOT power-gate on WFI (`PMU_CORE_PM_CON0` core_pm_en=0), so a
"sleeping" core still burns its rail; only the tiny dynamic switching current is
saved. The typewriter WFI path is correct (it sleeps and wakes properly) — it
just doesn't buy power here. **Do not chase CPU idle/freq/voltage for battery.**

(The raw-U-Boot -906 vs typewriter's ~-518 gap is backlight + shell busy-work,
not WFI — again pointing at backlight as the real lever.)

## WHY WFI saves nothing — full idle breakdown (Linux, 2026-08, measured)

Dug into it on the live Linux board (sbs gauge, 8-sample averages, reversible
toggles). Linux idle ~ -473 mA. Isolated every lever:

| Lever                                   | Delta   | Verdict                    |
|-----------------------------------------|---------|----------------------------|
| WFI vs busy-spin                        | ~8 mA   | noise                      |
| WFI-only vs PSCI cpu-sleep vs cluster-sleep | **0 mA** (-476 all three) | gating the whole cluster+L2 saves nothing |
| GPU 500→200 MHz                         | ~3 mA   | nothing                    |
| WiFi down                               | ~2 mA   | nothing                    |
| **Backlight 40% → OFF**                 | **~50 mA** | real (LED backlight)    |
| **DDR 400 → 928 MHz**                   | **~78 mA** | real — biggest knob     |
| **Floor with backlight OFF**            | **~423 mA** | DDR + eDP link + always-on rails + PMIC + EC |

Linux cpuidle has 3 states (`psci_idle` driver): state0 WFI, state1 cpu-sleep
(PSCI core power-gate), state2 cluster-sleep (PSCI cluster+L2 gate). At idle the
menu governor picks cluster-sleep ~95% of the time (deepest). Yet disabling
states 1+2 to force WFI-only gave the SAME -476 mA. So even genuinely
power-gating the A53 cluster via bl31 saves nothing measurable.

**Conclusion: the A53 cluster is NOT where the power goes** — it draws single-digit
mA at idle (hence 35°C, "Linux is cooler" = CPU genuinely idle). No CPU-idle
mechanism (WFI, cpu-sleep, cluster-sleep, freq, voltage) can touch the ~423 mA
floor because that floor is DDR + the display/eDP link + always-on rails (pp900_ap,
pp1800, pp3300_disp, pp1200_lpddr) + PMIC quiescent + the EC. This CLOSES the
CPU-idle question: it is measured, not theorized.

The two real levers are already captured in typewriter: **DDR 400 (~78 mA,
shipped)** and **backlight (~50-83 mA, user `^-`/`^]`)**. The remaining ~423 mA
is bl31/PMIC/rail-sequencing territory (system-suspend), a much larger effort
with uncertain payoff — not a CPU-idle tweak.

Rails enabled at Linux idle (for reference, from /sys/class/regulator): pp3300_disp,
pp3300_wifi_bt, wlan_pd_n, ppvar_gpu 0.85V, ppvar_bigcpu 0.97V, ppvar_litcpu 0.80V,
pp1200_lpddr, pp1500_ap_io, pp3000, pp900_ap.

## USB3 domain gate — SHIPPED (~14 mA, measured 2026-08)

`twwfi gate 8` (USB3 = PWRDN bit 27 / bus-idle bit 12) self-metered:

| Condition        | before | while gated | saved  |
|------------------|--------|-------------|--------|
| with USB dongle  | -518   | -504        | ~14 mA |
| without dongle   | -467   | -453        | ~14 mA |

Saving is IDENTICAL with/without the dongle → the ~14 mA is the USB3
controller/PHY DOMAIN itself (U-Boot leaves it powered though the editor's
keyboard is cros-ec, not USB). The dongle's own ~51 mA (518 vs 467) rides on top
as **VBUS to the attached device — NOT a PMU domain**, so domain-gating can't
reclaim it (would need the port's VBUS regulator, separate effort; only worth it
if a device stays plugged in while writing).

Shipped: `tw_gate_usb3()` in cmd_tw.c runs at editor startup (after DDR-400),
replicating bl31's pmu_set_power_domain (bus-idle req → PWRDN_CON). Best-effort
with timeout-abort/back-out, so it can only ever be a no-op, never a startup
wedge. Permanent for the session (no restore).

## GPU rail — NOT gateable (tested 2026-08, closed)

`twwfi gpurail` tried `regulator_disable(ppvar_gpu_pwm)`: the DM **refused** (rail
is always-on / boot-on or has other consumers). Combined with the GPU *domain*
gate (twwfi gate 0) showing ~0 earlier, the GPU is fully off the table — no
shippable lever. (Reported voltage "9V" was a µV/display artifact; the rail is
~0.9 V / 900000 µV as the Linux dump confirmed — nothing is actually at 9 V.)

## Rail hunt: EXHAUSTED

Every enabled rail on this board is now classified:
- **Load-bearing (cannot touch):** pp1200_lpddr (DRAM), pp900_ap / ppvar_logic /
  ppvar_centerlogic (SoC/NoC/DDR-ctrl), pp1500_ap_io / pp1800 / pp3300 (I/O
  banks incl. EC), ppvar_litcpu (our CPU), pp3300_disp + backlight (screen),
  pp3000 / pp3000_sd_slot / ppvar_sd_card_io (microSD for ^S/^R).
- **Already off / not a contributor:** pp3300_wifi_bt + wlan_pd_n (WiFi GPIOs
  at 0, disable refused -114), pp1800_audio / p3.3v_dig (µA-scale, unused).
- **Gateable, tested:** USB3 domain = ~14 mA (SHIPPED). GPU domain = ~0. GPU rail
  = refused. ppvar_bigcpu (A72 cores off, rail at 0.97 V) — off-domain leakage,
  Linux keeps it HIGHER (1.15 V) when A72 offlined, so not an obvious win; left
  alone.

Net: the ONLY reclaimable rail/domain from the editor was USB3 (~14 mA). The
remaining ~450 mA is DDR + display/eDP link + core/logic rails — reachable only
by bl31 system-suspend / rail-sequencing (large effort, uncertain payoff), NOT by
flipping a regulator or gating a domain from U-Boot.

## Biggest reachable lever going forward

Backlight is the largest measured lever, now dimmable to 5%. Candidate feature:
**auto-dim on idle** — drop to the floor after N s of no keystrokes, restore on
next key (reuse the same idle hook that drives deep WFI).
