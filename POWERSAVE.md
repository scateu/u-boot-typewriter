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

The editor's key-wait loop (`tw_read_key`, `cmd_tw.c`) has just two states:

1. **Active window** — for `TW_ACTIVE_WINDOW_MS` (2 s) after the last keypress, a
   cheap `TW_ACTIVE_POLL_MS` (25 ms) poll, no deep sleep. This spans the natural
   think-pauses between keys and, crucially, keeps U-Boot's input layer polled
   while a key is **held** so its auto-repeat fires — a held key emits no fresh
   IRQ, so a WFI would never wake for the repeat. Each key re-arms the window.
2. **Deep sleep** — once the window lapses, a `WFI` with **no timer armed**: the
   CPU sleeps until the cros_ec IRQ (INTID 46) fires. There is no periodic wake at
   all — an idle typewriter draws its true floor until you touch it.

What wakes it, and by what mechanism:

| Event            | Latency          | Method |
|------------------|------------------|--------|
| **Keystroke**    | instant (IRQ)    | cros_ec IRQ (gpio0 PA1 → INTID 46) wakes the WFI; `tstc()` returns the key. No EC-SPI on this path. For 2 s after a key, a 25 ms poll (not WFI) catches the next byte / auto-repeat. |
| **Power button** | instant (IRQ)    | same INTID 46 wakes the WFI; on a no-key wake, one `cros_ec_get_host_events()` read decodes it → save + power off |
| **Lid close**    | instant (IRQ)    | same INTID 46 wake; EC host-event flag (`EC_HOST_EVENT_LID_CLOSED`) |
| **Battery %**    | on demand (^T)   | not polled — `Ctrl-T` reads the gauge once and shows it; keeping it off a timer is what lets the CPU sleep indefinitely |

The EC multiplexes key, power button, and lid onto its single interrupt line
(gpio0 PA1 → GIC INTID 46, NonSecure Group1), so any of the three wakes the WFI
immediately. A key surfaces as a console byte (`tstc`/`getchar`); power/lid do not
(they latch host-event flags on a separate channel), so after a no-key wake the
loop does **one** host-event read to catch them. That read is a slow EC SPI
transaction, but it runs only on a genuine no-key wake — never during typing
(keys short-circuit via `tstc`, and the active window polls without touching the
EC) — so it never lags keystrokes.

There is deliberately **no backstop timer**: a lost IRQ edge would leave the
editor asleep until the next real keypress (which itself wakes it). On a
typewriter that trade — lowest possible idle power vs. a theoretical missed edge
that a keypress recovers — is the right one.

Rationale for the values:
- **2 s active window** — covers the think-pauses between keys and the whole span
  of a held key (auto-repeat needs the input layer polled every ~30 ms; a shorter
  window would drop back to WFI mid-hold and repeat would die). Each key re-arms
  it, so sustained input keeps the CPU in the cheap poll; 2 s after you truly stop,
  it deep-sleeps. The 2 s of 25 ms-polling after each key is negligible power
  versus the deep-WFI floor it returns to.
- **25 ms poll** — was the old worst-case key latency; imperceptible for typing,
  and faster than the 30 ms auto-repeat rate so no repeat slot is missed.

---

## What did NOT matter

- **The device tree / `.dtb`** — describes hardware, not CPU run-mode/idle. Not
  the cause of the draw.
- **Multiple CPU cores** — confirmed via `twwfi pmu`: only CPUL0 (A53) is
  powered; the other little cores and BOTH A72 big cores are already off (U-Boot
  never PSCI-CPU_ON's them). The big-cluster L2/SCU domain (SCUB) is still on but
  that's leakage bl31 won't cleanly let EL2 gate. Nothing to do CPU-side.
- **The idle WFI itself not sleeping** — ruled out by measurement. `twwfi
  napstats` instruments the editor's actual no-timer nap: a run showed
  `count=3, instant(<1ms)=0, slept=47623ms` (avg ~15.9 s/nap) — the core was
  genuinely halted the whole idle period. So flat battery draw is NOT the CPU
  busy-spinning; the deep WFI works. (Also cross-checked by `twwfi keystroke`.)

---

## CPU A53 operating point: 408 MHz + 0.80 V (the real heat fix)

CPU0 is the **A53 little cluster**, clocked from **APLL_L**, powered by the
**`ppvar_litcpu_pwm`** rail. The editor sets BOTH freq and voltage to the 408 MHz
operating point at startup (`tw_set_cpu_408()` in cmd_tw.c).

**Why voltage, not just frequency — the finding that mattered.** The board runs
noticeably HOTTER under the U-Boot editor than under Linux at idle (confirmed by
touch). Comparing against the live Linux box on the same hardware (see the
"Reference: live Linux measurements" section below) found the cause: at 408 MHz
Linux DVFS runs `ppvar_litcpu` at **0.80 V**, but U-Boot leaves it at **0.90 V**
(≈ the 1008 MHz OPP voltage). Same frequency, higher voltage → **(0.90/0.80)² =
1.27× the core power burned as heat for nothing.** That over-voltage is the heat.

**What the editor now does (both, safe order):**
1. Frequency 600 → **408 MHz** — self-contained CRU APLL_L pokes (no clk-driver
   dependency, core tree stays stock; same slow→reprogram→lock→normal sequence
   as `rkclk_set_pll`, glitch-free on the running core).
   408 MHz = 24 MHz × 68 / (1 × 2 × 2); VCO = 1632 MHz (in 800–2000).
2. Voltage 0.90 → **0.80 V** on `ppvar_litcpu_pwm` — DM regulator API
   (`regulator_get_by_platname` + `regulator_set_value`), **lower-only** guard.

Order matters: frequency dropped FIRST, then voltage — never under-volt for the
running clock. 0.80 V @ 408 MHz is the exact Linux OPP, proven stable on this
silicon (Linux + `twwfi litvolt` held it 10 s, no instability).

**Confirmed:**
- `twwfi cpuinfo` → CPUL 600 MHz at the shell, **408 MHz** after launching the editor.
- `twwfi litvolt` → set 0.80 V, held 10 s, recovered cleanly (safe).
- `md.l 0xff760000 2` → `00000044 00002201` (the 408 MHz PLL divisors).

**Checking the CPU voltage yourself (U-Boot shell):**
```
regulator dev ppvar_litcpu_pwm
regulator value            # prints the current core voltage in uV
```
Expect ~900000 (0.90 V) at the bare shell; ~800000 (0.80 V) after the editor ran.
(`ppvar_bigcpu_pwm` is the A72 rail — see the open lead below.)

Notes:
- **`CONFIG_SYS_CLK_FREQ` does nothing here** — mach-sunxi/legacy Kconfig the
  Rockchip clock path never reads. The real change is a runtime register write.
- **CPUB (A72) shows 600 MHz** in `twwfi cpuinfo` — APLL_B's *configured* rate;
  its cores are OFF (`twwfi pmu`), so nothing is clocked from it. **Open lead:**
  if `ppvar_bigcpu_pwm` is likewise over-volted while the A72 sits idle/off, that
  is more free heat to shave (the editor never uses the big cluster). Check its
  U-Boot value vs. the Linux idle value before touching it.
- **Subordinate cluster clocks** (ACLKM/PCLK_DBG/ATCLK) keep their boot-time
  dividers (set against 600), so they now run proportionally lower — within spec.
  Not restored on editor exit (shell keeps 408 MHz / 0.80 V). To revert, drop the
  `tw_set_cpu_408()` call.

---

## Unused-peripheral power-gating — tried, REMOVED (no benefit)

We tried gating the RK3399 peripheral domains a text editor never uses (GPU,
VCODEC, VDU, RGA, IEP, ISP0, ISP1, HDCP, USB3, GMAC) at editor startup, using
bl31's own sequence — request the domain's NoC bus idle (`PMU_BUS_IDLE_REQ` →
wait `_ST` & `_ACK`), then set its `PMU_PWRDN_CON` bit (→ wait `PMU_PWRDN_ST`).

> **Measured result: NO CHANGE.** Idle drain stayed at ~14 %/hr, identical to
> before. These domains were powered-but-idle *leakage*, negligible next to the
> real draw. The startup gating was **removed** — it added risk/complexity for no
> win. Don't re-chase PMU-domain gating for power on this board.

The bench tool `twwfi gate [N]` was kept for future meter-based testing (gates
one domain, holds ~15 s, restores; 0=GPU 1=VCODEC 2=VDU 3=RGA 4=IEP 5=ISP0
6=ISP1 7=HDCP 8=USB3 9=GMAC). Verified safe there — all ten gate + restore with
no hang — but with no measurable effect on drain.

### WiFi — also ruled out

`regulator disable pp3300_wifi_bt` refuses (-114); `regulator info` shows the rail
is neither always-on nor boot-on; `gpio status -a` shows both WiFi control pins
(`regulator-pp3300-wifi-bt.gpio`, `regulator-wlan-pd-n.gpio`) at output 0 — i.e.
WiFi/BT is already effectively off at idle. Not a contributor.

## Where the remaining ~14 %/hr actually is

After the WFI work (~20→14 %/hr), the leftover is **not** cheap unused blocks:
CPU is minimal (1 core), peripheral PMU domains gate to **0 delta**, WiFi is off.
By elimination it's the **load-bearing** draws:
- **Display panel + backlight** — on at full brightness the entire session. Almost
  certainly the biggest single item. Cheapest next experiment (no meter): dim to
  minimum (`Ctrl--`) or blank the panel on idle and compare %/hr. If that moves
  the needle, build "auto-dim/blank after N s idle."
- **DDR self-refresh + always-on core/logic rails** (`pp1200_lpddr`, `ppvar_logic`,
  `ppvar_centerlogic`, `pp900_ap`, …) — the machine doing its job; not disableable
  without hanging, and tuning needs a **power meter**. Don't guess-tune rails blind.

Bottom line: the kept win is the WFI idle work. Beyond it, the display is the one
remaining cheap lever; everything else needs instrumentation.

---

## Reference: live Linux measurements (same board)

The gru/kevin board dual-boots a full Linux (6.12) — the ~6 %/hr reference. SSH
onto it made several session-long assumptions measurable. Key findings:

**Linux runs at EL2** (`dmesg`: "All CPU(s) started at EL2"), same as the U-Boot
editor, over the same `smc` PSCI conduit. So EL2-vs-EL1 was never the blocker —
running the editor at EL1 would not help. (This killed a long EL1 detour.)

**PSCI deep CPU idle does NOT measurably cut current here.** Battery current
(`/sys/class/power_supply/sbs-9-000b/current_now`) at settled idle was ~633 mA
whether cluster-sleep was enabled OR forced off (WFI-only, like U-Boot). WiFi
down and backlight off also didn't move it. So the CPU *idle state depth* is not
the lever — a PSCI CPU_SUSPEND / context-save port into U-Boot would buy ~nothing.

**s2idle does not cleanly resume** on this board's Linux (rtcwake -m freeze hangs
on resume, needs power-cycle) — the one whole-system quiesce that *would* cut
DDR/rails is itself broken here. That's the TI "Why Won't My CPU Sleep" wake
problem; not reachable from a U-Boot editor.

**The A53 OPP table** (little cluster, from Linux DT `opp-table-0`) — freq → volt:

| MHz | Volt | | MHz | Volt |
|----:|-----:|-|----:|-----:|
| 408 | 0.800 V | | 1008 | 0.900 V |
| 600 | 0.825 V | | 1200 | 0.975 V |
| 816 | 0.850 V | | 1512 | 1.150 V |

At 408 MHz Linux sets `ppvar_litcpu` = **0.80 V**; U-Boot left it at **0.90 V**
(the ~1008 MHz point). That 0.90→0.80 V gap is the editor's over-voltage / heat,
now fixed (see the CPU operating-point section above). The A72 table
(`opp-table-1`) tops out at 2016 MHz / 1.25 V; its idle rail was ~0.975 V.

**Temperatures (Linux, settled idle):** `cpu-thermal` ~36 °C, `gpu`/`bigcpu` ~35 °C
— the target the editor should approach after the voltage fix.

**Reading current/voltage on Linux (for future comparisons):**
```
cat /sys/class/power_supply/sbs-9-000b/current_now   # uA, live battery current
cat /sys/class/thermal/thermal_zone*/temp            # m°C per zone
for r in /sys/class/regulator/regulator.*; do echo "$(cat $r/name)=$(cat $r/microvolts)"; done
```
The same SBS battery gauge is reachable from U-Boot via the EC (see
`twwfi cpuinfo`'s board-current line) — the one real power instrument we have.

---

## Measuring

The `twwfi` command (see **[TWWFI.md](TWWFI.md)**) is the on-board instrument for
all of this: `twwfi cpuinfo` (live CPU MHz), `twwfi pmu` (powered domains), and
the `probe`/`irq`/`ecwake`/`keystroke` WFI-wake proofs. Beyond registers, measure
power indirectly:
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
