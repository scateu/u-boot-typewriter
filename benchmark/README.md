# benchmark/ — power measurement helpers

Scripts used while characterizing typewriter idle power on the Kevin (RK3399)
Chromebook. Two kinds: some run in **Linux on the target** (read the same EC
smart-battery gauge the U-Boot editor reads), some run on the **build host**.

The authoritative results these produced live in [`../POWERSAVE_CURRENT.md`](../POWERSAVE_CURRENT.md).
The on-board equivalents (measure from inside the editor, no external meter) are
the `twwfi` metering subcommands — see [`../TWWFI.md`](../TWWFI.md).

| Script | Runs on | What it does |
|---|---|---|
| `wfi_proof.sh` | target Linux (root) | The definitive "WFI doesn't matter" proof. A/B/C idle regimes: deep PSCI cluster-sleep vs WFI-only vs poll (no idle), via `cpuidle/state*/disable`. Averages battery current in each; restores all flags on exit. Expect all three within ~15 mA = noise. |
| `wfi-investigate.sh` | target Linux | Earlier exploratory version (idle-state + rail poking). Superseded by `wfi_proof.sh` for the clean A/B/C. |
| `power_read.sh` | target Linux | Quick 5-sample battery current (mA) + backlight + charge status. One-liner for eyeballing draw. |
| `flash.sh` | build host | `flashrom -p internal -w <rom> && reboot` — reflash the coreboot+U-Boot image. Destructive/reboots; run by hand. |

## Measuring on battery — the one rule

The SBS gauge reports **battery** current, so **unplug AC** before any
measurement. On AC the reading is charge current and every delta is meaningless.
Confirm `status` reads `Discharging`.

The gauge updates ~1 Hz and is ±10–20 mA noisy — average ≥5–8 samples and treat
anything under ~15 mA as noise.

## Why there's a Linux path at all

The target's Linux (same board, same gauge) was the reference for every U-Boot
power lever: it's where the 408 MHz→0.80 V OPP, DDR-400 rate, and centerlogic
voltage points were read, and where "idle depth is a wash" and "the ~3%/hr gap is
not CPU-side" were proven. See `../POWERSAVE_CURRENT.md` for the cross-checks.

## Summary of what was measured (see ../POWERSAVE_CURRENT.md for detail)

- **DDR 928→400 MHz ≈ 78 mA** — biggest firmware lever (shipped).
- **Backlight ≈ 50–83 mA** — user `^-`/`^]`, biggest overall (default 10%).
- **Peripheral domain gate (10 domains) ≈ 23 mA** + **USB2 clock-gate ≈ 4 mA** — shipped at startup.
- **WFI / CPU idle depth ≈ 0** — proven a wash (`wfi_proof.sh`, `twwfi wfibench`).
- GPU rail: not gateable (DM refuses). Centerlogic: already at parity. SDIOAUDIO: wedges.
