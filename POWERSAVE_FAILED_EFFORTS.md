# Power-saving: failed efforts and dead ends (gru/kevin)

The working power design is in **POWERSAVE.md**. This file records the wrong
turns during the WFI-idle investigation so nobody repeats them. The short
version: getting real WFI deep idle took a long chain of freezes and several
confidently-wrong conclusions before the actual fix (`HCR_EL2.IMO=1` + a real
EL2 IRQ handler — see POWERSAVE.md "Real WFI deep idle").

Every WFI test cost a hard power-cycle (the board froze on a bad WFI), so a
standalone `twwfi` command was built to test a single armed WFI, plus a *safe*
no-WFI `twwfi probe` that arms the timer, polls the counter, and reads how far
the interrupt propagated. Much of the debugging was done live from the U-Boot
shell with `md`/`mw` (no reflash).

## The WFI freeze chain (each fixed a real bug, none was sufficient alone)

A bare `WFI` in the key loop froze the board. Root reasons, found in order:

1. **Nothing generates an interrupt by default.** U-Boot's console is polled
   (`tstc()`), the generic timer is only *read* (`get_ticks()` polls
   `CNTPCT_EL0`), no GIC driver is initialised, and `do_irq()` just `panic()`s.
   So a bare WFI waits for an event that never comes → permanent halt.
2. **Arch-timer PPI disabled.** `GICR_ISENABLER0 = 0x20004000` — bit 30 (INTID
   30, CNTP) clear. Enabled it. Still froze.
3. **Timer armed with `IMASK=1`.** The arch timer only asserts its interrupt
   when `ENABLE=1 && ISTATUS=1 && IMASK=0`; with `IMASK=1` it never fires.
   Fixed to `IMASK=0`. Still froze.
4. **`ICC_IGRPEN1 = 0`** — Group 1 disabled at the GIC CPU interface. Enabled
   it. Still froze.
5. **`GICD_CTLR.EnableGrp1NS` (bit 1) clear** — `GICD_CTLR = 0x35` = `EnableGrp0
   | EnableGrp1S | ARE_S | ARE_NS`, so the *distributor* never forwarded
   NonSecure Group1 interrupts. Enabling it got the interrupt fully deliverable:
   the safe probe showed **`ICC_HPPIR1` went 1023 → 30**.

Yet a WFI that left `PSTATE.I` **masked** and had **no handler** *still* hung,
even with the interrupt deliverable and force-pending. That was the crux
misunderstanding (see below).

## Wrong conclusions we reached (and why they were wrong)

- **"The arch timer is a secure interrupt owned by bl31."** False. ATF
  (`plat/rockchip/rk3399/rk3399_def.h`) secures only INTID **29** (the EL3
  physical timer) + two SGIs. INTID **30** (CNTP, NS timer) is Group1-NonSecure
  and usable by us. The `IGROUPR0` bit30=1 / `IGRPMODR0` bit30=0 dumps confirm it.
- **"Priority `0x80` is in the secure half, so NS can't see it."** False. The
  blocker was the *group-forwarding* enable (`EnableGrp1NS`), not priority;
  `0x80` < `ICC_PMR = 0xf8` passes fine.
- **"A masked, un-taken pending interrupt wakes WFI."** False on this board. The
  ARM ARM allows a pending IRQ as a WFI wake-up event regardless of `PSTATE.I`
  *only if the interrupt actually targets this PE/EL*. Because U-Boot never set
  `HCR_EL2.IMO`, physical IRQs targeted **EL1, not our EL2** — so our EL2 WFI was
  never the target and never woke. The real fix was to route IRQ to EL2 (IMO=1)
  and *take* it with a handler.
- **"WFI is held at EL3 (`SCR_EL3.IRQ`) — only PSCI can idle."** False. bl31 does
  *not* route IRQs to EL3 in normal running (that's why its own
  `rockchip_cpu_standby` has to *set* `SCR_EL3.IRQ` before its WFI). With
  `HCR_EL2.IMO=1` the NS IRQ targets our EL2 and we can take it directly.

## PSCI `CPU_SUSPEND` — also a dead end

Believing WFI was walled off at EL3, we tried the "ask bl31 to idle" route:
`PSCI CPU_SUSPEND` (STANDBY, `power_state=0`), which makes bl31 do the WFI at
EL3 (where it sets `SCR_EL3.IRQ`) — the path Linux's `cpu-sleep`/`cluster-sleep`
use. It **hung too**, proven from the shell with a NonSecure interrupt
force-pending:

```
mw.l 0xfee00000 0x37          # GICD_CTLR |= EnableGrp1NS  -> 0x37
mw.l 0xfef10100 0x40000000    # enable INTID 30 PPI        -> 0x60004000
mw.l 0xfef10200 0x40000000    # FORCE INTID 30 pending     -> 0x40000000
twwfi suspend                 # PSCI CPU_SUSPEND(STANDBY) ... HANGS
```

bl31's standby only wakes on the wake sources Rockchip built it for (its own
PMU/GPIO path), not our arbitrary NS timer. This route was abandoned once the
direct `HCR_EL2.IMO` + take-the-IRQ approach worked (`twwfi irq` returned).

## Idle "power-saving mode" — built, then removed

An earlier feature: after 20 s idle, dim the backlight, show `[power saving]`,
and poll "lazily" at 500 ms. It was built on the belief that a longer `udelay`
nap sleeps the CPU. **It doesn't** — the event-stream `udelay` is a WFE spin
that self-wakes every ~5 µs (see POWERSAVE.md Fix 1), so a "500 ms lazy nap" was
a 500 ms busy spin. Measured: no idle-power benefit (~15 %/hr, unchanged). It
added dim/wake/swallow logic and a `[power saving]` state for zero CPU-idle
gain, so it was removed. (Real WFI idle, added later, is what actually sleeps
the core — and needs no separate "mode": every nap is a real sleep.)

## `twwfi` diagnostic command

`cmd_twwfi.c` stays in the tree as the evidence trail and a re-test tool:

```
twwfi              - dump EL / CNTFRQ / CNTPCT / CNTHCTL_EL2 / GIC state (safe)
twwfi probe [p|hp] - arm timer, poll (NO WFI), report how far the IRQ got (safe)
twwfi spi [n]      - read an SPI's group/priority in the GICD (safe)
twwfi gpio         - set up the EC GPIO IRQ (INTID 46), poll ~5 s, press keys
twwfi suspend [ms] - PSCI CPU_SUSPEND(STANDBY) — HANGS here (evidence)
twwfi irq [ms]     - TAKE the IRQ (HCR_EL2.IMO=1 + handler) + WFI — THE fix,
                     returns after ~ms; this is what tw_idle_nap now does
twwfi p|hp [ms]    - raw armed WFI, masked — hangs here (no IMO/handler)
```
