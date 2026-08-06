# Power-off on the RK3399 Chromebook (gru/kevin)

How the typewriter powers the machine off, why U-Boot's built-in `poweroff`
command does **not** work on this board, and how to debug it.

---

## TL;DR

- U-Boot's generic **`poweroff` command is a no-op on gru/kevin** — it prints
  `poweroff ...` and nothing happens.
- Reason: **this board has no PMIC.** Power is controlled by the **ChromeOS
  Embedded Controller (EC)**. `poweroff` looks for a PMIC-style power-off
  device and finds none.
- The typewriter's **`^Q`** key powers off correctly: it **saves + syncs**,
  then tells the **EC to hibernate** (`cros_ec_reboot(EC_REBOOT_HIBERNATE)`).
- **Wake the machine with the physical power button.**

---

## Why the built-in `poweroff` fails

`poweroff` (in `cmd/boot.c` → `do_poweroff` in
`drivers/sysreset/sysreset-uclass.c`) walks every `UCLASS_SYSRESET` device
asking it to handle `SYSRESET_POWER_OFF`:

```c
puts("poweroff ...\n");
ret = sysreset_walk(SYSRESET_POWER_OFF);
```

On a typical Rockchip board the `rk8xx` **PMIC** driver registers an
`rk8xx_sysreset` device that answers `SYSRESET_POWER_OFF` (it sets the rk808's
software-off bit). That device is only bound if the PMIC's device-tree node has
`system-power-controller`.

**gru/kevin has no rk808 — no PMIC at all.** Its `rk3399-gru.dtsi` power tree is
entirely:

- `regulator-fixed` (GPIO-switched rails: pp1800, pp3300, pp5000, …),
- `pwm-regulator` / `vctrl-regulator` (CPU/GPU voltage via PWM),
- `regulator-gpio`,

plus a **`google,cros-ec-spi`** node — the ChromeOS EC. There is **no
`rk808`, no `pmic@1b`, no `system-power-controller`** anywhere. So
`sysreset_walk(SYSRESET_POWER_OFF)` finds no device and returns without doing
anything. This is why `poweroff` prints its banner and then silently continues.

> Dead end warning: do **not** try to "fix" this by adding
> `system-power-controller` or an rk808 node. There is no such chip on the
> board. Several earlier debugging attempts chased a PMIC that doesn't exist.

### How we proved it

| Check | Result | Meaning |
|---|---|---|
| `poweroff` | prints `poweroff ...`, no effect | command runs; no sysreset device claims POWER_OFF |
| `pmic list` | **empty** | no PMIC bound in U-Boot at all |
| grep DTS for `rk808` / `system-power-controller` | nothing | no PMIC node exists |
| `i2c probe` (bus 0) | only `0x20` (not `0x1b`) | rk808 (0x1b) absent |
| `crosec id` | `kevin_v1.10.120-…` | the **EC** is present and talking |
| `crosec events` | lists `power_button`, `battery_shutdown`, … | EC manages power |

Conclusion: power is the EC's job, so software power-off must go **through the
EC**, not a PMIC.

---

## How `^Q` powers off (the typewriter's path)

`^Q` in the editor runs `tw_poweroff()` (in `cmd_tw.c`). It is intentionally
**save-first**, because the whole point is not to lose the document:

1. **Save.** If the buffer is writable and modified, `tw_file_save()` writes it.
   If that save **fails, power-off is aborted** and a `[ Save failed - NOT
   powering off ]` message is shown — edits are never dropped.
2. **Sync/settle.** U-Boot's FAT/block writes are **synchronous / write-through**
   (there is no FS write-back cache to flush like on Linux — once `fs_write`
   returns, the bytes are on the card). A `mdelay(200)` is added as
   belt-and-braces before cutting power.
3. **EC battery cutoff.** Get the EC (`uclass_first_device_err(UCLASS_CROS_EC,
   …)`) and call:
   ```c
   cros_ec_battery_cutoff(ec, 0);   /* 0 = cut off NOW, not "at shutdown" */
   ```
   This sends `EC_CMD_BATTERY_CUT_OFF` — ChromeOS "ship mode" — telling the
   battery to **stop supplying power**, which fully powers the device down. It
   is the truest hardware power-off this EC exposes.

   **Why not `cros_ec_reboot(EC_REBOOT_HIBERNATE)`?** We tried it first. On the
   tested units the EC accepted `HIBERNATE` (6) and `HIBERNATE_CLEAR_AP_OFF` (7)
   but **did not power the AP off** — the EC has no `pmu` feature (`crosec
   features`) and hibernate expects the OS's AP-shutdown handshake, which a bare
   U-Boot payload doesn't perform. Worse, with `flags = 0`, `cros_ec_reboot()`
   polls the EC with `hello` for **3 seconds** ("wait for it to come back"),
   logs `EC did not return from reboot`, and **froze** U-Boot; a short
   power-button press then *reset* the board. Battery cutoff avoids that path
   entirely.

The `^Q` key opens a **"Save & power off? (Y/N)"** confirm first, so an
accidental press can't shut the machine off.

### ⚠️ Wake behavior after battery cutoff

After a battery cutoff the device is fully off, but it typically **powers back
on only when the charger / AC is plugged in** — not necessarily the power
button. This is inherent to "ship mode." If that's not what you want, the plain
alternative is the power-button long-press (which needs a ~6 s hold for full
off but wakes normally). `^Q` still always **saves your work first**; if the
cutoff doesn't take, it drops back to the editor with a "hold the power button"
message — no hang.

### Build gating

The EC code is wrapped in `#if CONFIG_IS_ENABLED(CROS_EC)`. On a board without
a cros-EC, `^Q` still saves and shows *"No EC power-off on this board - use
button"* instead of failing to build. On gru/kevin the relevant configs are
already set:

```
CONFIG_CROS_EC=y
CONFIG_CROS_EC_SPI=y
CONFIG_I2C_CROS_EC_TUNNEL=y
```

### Waking up

`EC_REBOOT_HIBERNATE` powers the AP off; **press the power button to turn the
machine back on.** (The EC stays in a low-power state and watches the power
button — that's what `crosec events` showing `power_button` confirms.)

---

## Debugging

### 1. Is the EC alive in U-Boot?

At the U-Boot prompt (or before wiring anything, to sanity-check):

```
=> crosec id
kevin_v1.10.120-90210ee        <- good: U-Boot talks to the EC over SPI
=> crosec events               <- should list power_button, battery_shutdown, …
```

If `crosec id` errors or times out, the EC link is down and **no** software
power-off can work — check `CONFIG_CROS_EC*` and the EC's SPI node in the DT.

Safe read-only `crosec` subcommands for testing: `id`, `sku`, `events`.
**Avoid** `crosec reboot ...` while probing — some variants reset the whole AP.

### 2. Confirm there really is no PMIC

```
=> pmic list          <- empty on this board (expected)
=> i2c dev 0
=> i2c probe          <- 0x1b (rk808) will NOT appear
```

### 3. Does `^Q` save before powering off?

The data-safety guarantee is covered by a host test
(`.cc/functest.c`, "^Q poweroff"): a dirty buffer is written to the (mock) disk
*before* the EC call, and the save is aborted if it fails. To exercise on the
device:

1. Type some text (don't `^O`).
2. Press `^Q`, answer `Y`.
3. The status line briefly shows the write, then the board powers off.
4. Power on with the button, reopen the file (`^R` or relaunch) — your text
   should be there.

### 4. If `^Q` doesn't power off (observed on the tested units)

`EC_REBOOT_HIBERNATE` was accepted by the EC but the AP stayed powered. If you
want to try harder to make it power off, the change is one line in
`tw_poweroff()` (`cros_ec_reboot(ec, <cmd>, <flags>)`). Variants, in order to
try:

1. **`EC_REBOOT_HIBERNATE` + `EC_REBOOT_FLAG_ON_AP_SHUTDOWN`** — the current
   code. No freeze; hibernates on AP-shutdown (may not trigger from U-Boot).
2. **`EC_REBOOT_HIBERNATE_CLEAR_AP_OFF` (7)** + the same flag — hibernate and
   clear the AP-off flag so the power button wakes cleanly.
3. **plain `EC_CMD_REBOOT` (0x00D1, "die")** via a raw `ec_command` — a hard EC
   reset (likely reboots rather than powers off; matches the observed
   "short power-button press = reset").

Reality check: ChromeOS ECs hibernate as part of the OS's AP-shutdown sequence,
which a bare U-Boot payload doesn't perform, so none of these may cleanly power
the board off from the editor. The dependable power-off is the **power-button
long-press** (hardware off). `^Q`'s real value is that it **saves your work
first**; treat the actual power-down as best-effort.

**IMPORTANT:** always keep `EC_REBOOT_FLAG_ON_AP_SHUTDOWN` in the flags when
using a HIBERNATE cmd — without it, `cros_ec_reboot()` does a 3-second
hello-poll that freezes U-Boot when the EC doesn't answer.

### 5. If `^Q` does nothing / says "No EC"

- `[ No EC - power off with the button ]` → `uclass_first_device_err(
  UCLASS_CROS_EC, …)` failed: the EC isn't bound in U-Boot proper. Check
  `crosec id` (step 1) and the EC SPI node's `bootph-*` tags in the DT.
- Command not built (no `^Q` effect at all) → `CONFIG_CROS_EC` off, so the
  `#if CONFIG_IS_ENABLED(CROS_EC)` block compiled out; it will still save and
  tell you to use the button.

### 6. Serial vs. framebuffer

The typewriter owns the framebuffer, so its status messages appear **on the
screen's bottom bar**, not the serial console. `crosec`/`pmic`/`i2c` debugging
is done from the **U-Boot serial prompt** (exit the editor with `^X` first).

---

## Alternatives (no code)

- **Power button long-press** — the EC hardware-powers-off regardless of U-Boot
  or the OS. Always available.
- **`reset`** — reboots (via the rockchip cru sysreset), if you only want a
  restart.

---

## References

- `cmd_tw.c` — `tw_poweroff()`, the `^Q` key and confirm prompt.
- `drivers/misc/cros_ec.c` — `cros_ec_reboot()`.
- `include/ec_commands.h` — `enum ec_reboot_cmd` (`EC_REBOOT_HIBERNATE` = 6).
- `drivers/sysreset/sysreset-uclass.c` — the generic `do_poweroff` that can't
  find a device here.
- `drivers/power/pmic/rk8xx.c` — the PMIC sysreset path this board can't use
  (no rk808).
- `KNOWN_ISSUES.md` — the related eMMC FAT-write defect (also board-specific).
