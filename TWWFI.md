# `twwfi` — the RK3399 idle/power debug command

`twwfi` is a U-Boot shell command (defined in `cmd_twwfi.c`) used to investigate
and prove out the typewriter's low-power idle on gru/kevin (RK3399, running at
EL2 under coreboot + bl31). It began as a **W**ait-**F**or-**I**nterrupt probe
(hence the name) and grew into a general "what is this SoC doing / drawing"
toolbox: CPU frequency, power-domain state, GIC/timer interrupt tracing, and the
actual WFI wake paths.

It is a **debug/bench tool**, not part of the editor. Build it only while
investigating power; it can be dropped from production. The findings it produced
are written up in [POWERSAVE.md](POWERSAVE.md); this doc is the operator's manual
for the command itself.

> **Board specifics baked in.** All register addresses (GIC, PMU, CRU, GPIO0) are
> RK3399 gru/kevin values, verified against bl31 (the vendored
> `arm-trusted-firmware` tree) and by `md`/`mw` on the board. On any other SoC the
> addresses are wrong and the command is meaningless.

---

## Safety classes

Every subcommand is one of two kinds. **Know which before you run it.**

| Class | Meaning | Subcommands |
|---|---|---|
| **SAFE** | Reads only (or polls, never sleeps). Cannot hang. Re-runnable freely. | `dump` (no arg), `pmu`, `cpuinfo`, `probe`, `spi`, `gpio` |
| **MAY HANG** | Does a real WFI or writes power/clock hardware. If a path is broken the prompt never returns — **power-cycle** to recover (you lose the shell, not an editing session). | `gate`, `keystroke`, `irq`, `ecwake`, `suspend`, `p`, `hp` |

The `keystroke`/`irq`/`ecwake` paths are the *working* WFI recipe (the same one
the editor uses) and return normally; `p`/`hp`/`suspend` are the *broken/legacy*
experiments kept as evidence and will freeze. See each entry below.

---

## Quick reference

```
twwfi                 - dump EL / timer / GIC state (SAFE)
twwfi pmu             - CPU + peripheral power-domain state (SAFE)
twwfi cpuinfo         - live CPU cluster frequencies (SAFE)
twwfi gate [N]        - power-gate editor-unused domain N (or all); MAY HANG
twwfi keystroke [ms]  - the editor's real idle-nap loop until a key (works)
twwfi irq [ms]        - take a timer IRQ at EL2 + WFI (works)
twwfi ecwake          - EC keypress/lid/power IRQ wakes WFI, no timer (works)
twwfi suspend [ms]    - PSCI CPU_SUSPEND via bl31 (HANGS - evidence)
twwfi probe [p|hp]    - arm timer + poll (no WFI), trace propagation (SAFE)
twwfi spi [n]         - read an SPI's group/priority in the GICD (SAFE)
twwfi gpio            - EC GPIO IRQ poll ~5 s, press keys (SAFE)
twwfi p|hp [ms]       - arm CNTP/CNTHP + one raw WFI (HANGS - no IMO/handler)
```

`[ms]` defaults to 300 (or 5000 for `keystroke`). `[N]`/`[n]` are integers.

---

## The SAFE commands (start here)

### `twwfi` — state dump

No argument. Prints the current exception level, the arch-timer
(`CNTFRQ`/`CNTPCT`, read twice to prove it advances), `CNTHCTL_EL2`, and the
GICv3 redistributor/CPU-interface state (enables, group, priority, `ICC_*`). This
is the "where am I" snapshot — EL2, timer alive, GIC visible — before any deeper
probe. Reads only.

### `twwfi cpuinfo` — live CPU frequency

Decodes the **actual** CPU cluster clocks from the CRU (no hand-math on `md`):

- **CPUL** = the A53 little cluster = **CPU0, the core the typewriter runs on**,
  clocked from APLL_L.
- **CPUB** = the A72 big cluster, from APLL_B.

For each it reads APLL CON0–3 + the core mux/divider (`CLKSEL_CON0`/`CON2`) and
prints the divisors, PLL lock + mode (flags slow/bypass = running off the 24 MHz
OSC), the PLL output, and the final core MHz.

```
FOUT = 24MHz * fbdiv / (refdiv * postdiv1 * postdiv2)
CPU  = FOUT / (core_div + 1)
```

Use it to confirm a frequency change took. Example on gru/kevin: the shell shows
`CPUL 600 MHz`; after launching `typewriter` (which sets 408 MHz) it shows
`CPUL 408 MHz`. **`CPUB 600 MHz` is normal and harmless** — APLL_B's configured
rate, with both A72 cores powered off (nothing clocked from it). See
[POWERSAVE.md](POWERSAVE.md).

### `twwfi pmu` — power-domain map

Reads `PMU_PWRDN_ST` (bit set = domain OFF). Decodes:

- **CPU cores / clusters**: CPUL0–3 (A53), CPUB0–1 (A72), SCUL/SCUB (cluster
  L2+SCU). On this board only CPUL0 is ON; A72 cores are OFF.
- **Peripheral domains** (bits 8–31): TCPD0/1, CCI, PERILP/PERIHP, CENTER, VIO,
  GPU, VCODEC, VDU, RGA, IEP, VO, ISP0/1, HDCP, GMAC, EMMC, USB3, EDP, GIC, SD,
  SDIOAUDIO — each flagged `<- unused by editor` where a text editor never needs
  it.

Also prints `PMU_BUS_IDLE_ST` raw. **Caveat printed inline:** that idle register
reads all-0 (not-idle) at runtime for every bus regardless of activity — buses
only enter "idle" after bl31 requests it during suspend — so it does **not**
indicate which domains draw power. Bits/polarity come from bl31's
`pmu_powerdomain_id` / `pmu_bus_id` enums.

### `twwfi probe [p|hp]` — trace timer-IRQ propagation, no WFI

The safe precursor to a real WFI. Arms a timer (`p` = CNTP/INTID 30, the EL1
physical timer; `hp` = CNTHP/INTID 26, the EL2 timer), enables the GIC path
(`GICD_CTLR.EnableGrp1NS`, the PPI, `ICC_IGRPEN1`), then **polls the counter**
until the timer must have fired and reads back how far the interrupt got: timer
`ISTATUS`, redistributor pending, and `ICC_HPPIR1` (does the PE see the INTID, or
1023 = nothing deliverable?). Prints a layer-by-layer diagnosis. Never sleeps, so
it can't freeze — this is how the working WFI recipe was found.

### `twwfi spi [n]` — inspect a GIC SPI

Reads one shared peripheral interrupt (INTID ≥ 32, default 46 = the GPIO0 bank
carrying the cros_ec line) in the distributor: group/gmod (is it NS Group1 and
therefore deliverable to our EL2?), enable, priority, routing. Read-only; used to
check a candidate wake source before wiring it.

### `twwfi gpio` — watch the EC keypress line, no WFI

Sets up GPIO0 PA1 (the `ec-interrupt` line → INTID 46) as a level/active-low IRQ,
enables it at the GIC, then **polls ~5 s** while you press keys / the power button
/ the lid, reporting whether the GPIO asserts, whether INTID 46 goes pending, and
whether `ICC_HPPIR1` shows 46 (i.e. the PE would wake). No WFI, no freeze — the
safe proof that a keypress can wake WFI before committing to it.

---

## The WORKING WFI commands (these return normally)

These share the recipe the editor's idle actually uses. The key insight (see the
header comments in `cmd_twwfi.c` and `cmd_tw.c`): a physical IRQ only wakes an
EL2 WFI if it is **taken**, which needs three things U-Boot doesn't set by
default — `HCR_EL2.IMO=1` (route phys IRQ to EL2; U-Boot only sets AMO),
the NS-Group1 GIC path enabled (`GICD_CTLR.EnableGrp1NS` + the PPI/SPI +
`ICC_PMR` open + `ICC_IGRPEN1`), and a tiny EL2 IRQ vector that acks
(`ICC_IAR1`) + EOIs (`ICC_EOIR1`) and silences the source.

### `twwfi irq [ms]`
Installs a minimal EL2 vector, sets `HCR_EL2.IMO`, arms CNTP, unmasks PSTATE.I,
`wfi()`. The taken timer IRQ wakes it after `ms`. If the prompt returns, the
recipe works. **This is what the editor's idle nap does.**

### `twwfi ecwake`
Same recipe but **no timer** — only the EC line (INTID 46) can wake it. Press a
key / power button / lid; if the prompt returns, an EC event alone wakes WFI, so
the editor can sleep indefinitely with no periodic tick. (No timeout: if nothing
is pressed it waits forever — power-cycle.)

### `twwfi keystroke [ms]`
The editor's exact idle-nap loop, run until a key/power/lid event, with a default
`ms` of 5000 so it's human-observable. The board goes quiet (deep WFI) and wakes
on the event; it reports naps/avg to prove WFI actually slept (avg ≈ `ms` = good;
avg ≈ 0 with a huge nap count = WFI not sleeping). Decodes key vs power vs lid via
the cros_ec host-event channel.

---

## The MAY-HANG / evidence commands

### `twwfi gate [N]` — power-gate editor-unused peripheral domains

**RISKY (writes PMU power domains live).** Gates domain `N`, or all if `N`
omitted:

```
0=GPU 1=VCODEC 2=VDU 3=RGA 4=IEP 5=ISP0 6=ISP1 7=HDCP 8=USB3 9=GMAC
```

Uses bl31's own sequence per domain: request the NoC bus idle
(`PMU_BUS_IDLE_REQ` → wait `_ST` **and** `_ACK`), then power off (`PMU_PWRDN_CON`
→ wait `PMU_PWRDN_ST`). If a bus won't idle it **aborts that domain** (backs the
request out) rather than forcing it — forcing is the wedge risk. Then it holds
~15 s ("read the meter now"; press a key to end early) and **restores** in
reverse. Gate **one at a time** to bisect which domain wedges (the last name
printed before a hang is the culprit; power-cycle to recover).

> Verified on gru/kevin: all ten gate + restore with no hang. But gating them at
> editor startup gave **zero measured change** to idle drain — these domains are
> powered-but-idle leakage. Kept as a bench tool only. See POWERSAVE.md.

`bl31` normally does this during suspend with clocks quiesced; doing it live is
what makes it risky. USB3 is only safe to gate when input is the built-in cros_ec
keyboard (SPI) and storage is mmc/SD — **not** if booting/reading over USB.

### `twwfi suspend [ms]` — PSCI CPU_SUSPEND

**HANGS on this board — kept as evidence.** SMCs into bl31 requesting STANDBY
with a timer wake armed. The theory was bl31's standby handler sets
`SCR_EL3.IRQ` and does the WFI at EL3; in practice it freezes here. Documents why
the editor takes the IRQ at EL2 instead of delegating to PSCI.

### `twwfi p [ms]` / `twwfi hp [ms]` — raw WFI, no handler

**HANG — kept as evidence.** Arm CNTP (`p`) or CNTHP (`hp`) and do a bare `wfi()`
*without* `HCR_EL2.IMO` or an EL2 vector. Because the IRQ isn't taken at EL2, it
never wakes — the failing case that motivated the `irq` recipe above. Power-cycle.

---

## Typical session

```
twwfi                 # sanity: EL2? timer ticking? GIC visible?
twwfi cpuinfo         # what speed is CPU0 actually at?
twwfi pmu             # which cores/domains are powered?
twwfi probe p         # (safe) does the CNTP IRQ reach the PE?
twwfi irq             # (works) confirm a taken timer IRQ wakes WFI
twwfi ecwake          # (works) confirm a keypress alone wakes WFI
twwfi keystroke       # (works) run the editor's real idle nap
# only if chasing peripheral power, one at a time, meter attached:
twwfi gate 8          # gate USB3, hold 15 s, restore
```

## Provenance

Register addresses and bit meanings are cross-checked against the vendored
`arm-trusted-firmware/plat/rockchip/rk3399` headers (`pmu_regs.h`, `pmu_bits.h`,
`soc.h`, `addressmap_shared.h`) and the U-Boot Rockchip clock driver
(`drivers/clk/rockchip/clk_rk3399.c`, `arch-rockchip/cru_rk3399.h`). Key bases:
GIC dist `0xfee00000`, CPU0 redist `0xfef10000`, PMU `0xff310000`, CRU
`0xff760000`, GPIO0 `0xff720000`.
