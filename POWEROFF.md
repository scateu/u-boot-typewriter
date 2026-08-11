# Power-off on the RK3399 Chromebook (gru/kevin)

How the typewriter (`^Q`) powers the machine off, and why U-Boot's built-in
`poweroff` command needed fixing on this board.

---

## TL;DR

- `^Q` opens **`Save & ...  Y) power off   B) boot OS   N) cancel`**:
  - **Y** — save + sync, then power off via **PSCI `SYSTEM_OFF`** (SMC to
    coreboot's bl31); the same mechanism Linux uses, **power-button-wakeable**.
  - **B** — save, then boot the OS (`bootflow scan -lb`).
  - **N** / Esc — cancel.
- Power-off out of the box did **not** work: U-Boot's `poweroff` printed
  `poweroff ...` and did nothing. **The one thing needed is to PROBE the PSCI
  firmware driver** before calling `SYSTEM_OFF` — U-Boot binds it but never
  probes it (`CONFIG_SYSRESET_PSCI` is off), so the SMC conduit was never set.
- **No device-tree patch is required.** The `/psci` node already exists in
  `rk3399-base.dtsi` (pulled in via `rk3399.dtsi`). An earlier version of this
  doc told you to add the node to `rk3399-gru-u-boot.dtsi`; that was
  **redundant** — the node was always present; only the probe was missing.

---

## Why it didn't work, and the fix

`poweroff` (and `^Q`) ultimately call
`invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF, ...)`, which does an `smc` to the
secure monitor (coreboot's bl31), which powers the board off. Two things were
missing.

### The `/psci` DT node already exists (no patch needed)

U-Boot's PSCI driver (`drivers/firmware/psci.c`) learns the call conduit (SMC
vs HVC) from a `/psci` DT node's `method` property. That node **is already in
the device tree** — `rk3399-base.dtsi` (included via `rk3399.dtsi`) defines:

```dts
psci {
	compatible = "arm,psci-1.0";
	method = "smc";
};
```

You can confirm it's in the built control DTB from the U-Boot shell:

```
=> fdt addr $fdtcontroladdr
=> fdt print /psci        # shows compatible = "arm,psci-1.0", method = "smc"
```

So **no edit to `rk3399-gru-u-boot.dtsi` is required.** (An earlier revision of
this document added the node there; it was harmless but redundant and has been
removed.)

Linux uses the same node — `/proc/device-tree/psci/method` = `smc`, and
`dmesg | grep -i psci` shows `PSCIv1.1 detected in firmware`.

### The actual fix: probe the PSCI firmware driver

With the node present, `poweroff` *still* did nothing. `dm tree` showed the
driver **bound but not probed**:

```
=> dm tree
 firmware      0  [ ]   psci                          psci
```

The `[ ]` (not `[+]`) means `psci_probe()` never ran, so `psci_method` stayed
unset and `invoke_psci_fn()` made no SMC (every call looked like `-8`). U-Boot
only auto-probes the PSCI driver when `CONFIG_SYSRESET_PSCI` is enabled (it is
**not** on this build). So `^Q` **probes it explicitly** before the call:

```c
struct udevice *psci;
uclass_get_device_by_name(UCLASS_FIRMWARE, "psci", &psci);  /* runs psci_probe */
disable_interrupts();
invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF, 0, 0, 0);            /* powers off */
```

`psci_probe()` reads `method = "smc"`, sets the conduit, and the subsequent
`SYSTEM_OFF` powers the board off cleanly (button-wakeable).

> Alternative to the explicit probe: enable **`CONFIG_SYSRESET_PSCI=y`** in the
> U-Boot config. Then the PSCI sysreset device is registered and probed
> normally, U-Boot's builtin `poweroff` works on its own, and the explicit
> probe in `^Q` becomes redundant (but harmless). With the DT node in place,
> either approach works.

---

## What `^Q` does

`^Q` opens a confirm line: **`Save & ...  Y) power off   B) boot OS   N) cancel`**
(so an accidental press does nothing until you choose). All three first
**save + sync** the buffer if it's writable and modified — and if that save
**fails, the action is aborted** (`[ Save failed ... ]`) so edits are never
lost. U-Boot FAT writes are synchronous/write-through, so once the save
returns the data is on the card (a 200 ms settle is added before power-off as
belt-and-braces).

- **Y** — power off via `invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF)` (after probing
  PSCI). The board powers down; **press the power button to turn it back on.**
- **B** — hand off to the OS with `run_command("bootflow scan -lb", 0)` — the
  libreboot default that scans partitions (incl. `extlinux.conf`) and boots.
  If nothing bootable is found it returns to the editor.
- **N** / Esc — cancel.

The **physical power button** does the same as **Y** (save + power off), from any
mode or prompt — see below.

---

## Power button & lid-close in the editor  ✅ CONFIRMED WORKING

Goal: pressing the laptop's power button **or closing the lid** while in the
typewriter saves and powers off, instead of doing nothing (or a hard EC cutoff).
**Verified on gru/kevin: a short press of the physical power button saves the
buffer and powers the board off, and typing is unaffected.** Lid-close uses the
same mechanism (`EC_HOST_EVENT_LID_CLOSED`).

### The wrong way (don't): polling the MKBP event queue

The obvious approach is `cros_ec_get_next_event()` in the key-wait loop, checking
for `EC_MKBP_EVENT_BUTTON`. **This breaks the keyboard.** On gru/kevin the
keyboard is *also* a cros_ec (`CROS_EC_KEYB`); its read path
(`cros_ec_kbc_check` → `check_for_keys`) pulls from the **same MKBP event FIFO**
and discards everything that isn't a key-matrix scan. So:

- polling the FIFO ourselves races the keyboard and **steals/reorders keystrokes**
  (observed: backspace repeats and deletes many chars, keys go missing); and
- the keyboard driver **silently eats button events** anyway, so we'd rarely see
  one even if we won the race.

### The right way: EC host-event flags

The EC also exposes a **latched host-event bitmask** — a separate channel the
keyboard driver never touches. The power button latches
`EC_HOST_EVENT_POWER_BUTTON` and closing the lid latches
`EC_HOST_EVENT_LID_CLOSED`. We poll both (`TW_POWEROFF_EVENTS`) with
`cros_ec_get_host_events()` (which reads the **B copy**, `EC_CMD_HOST_EVENT_GET_B`,
distinct from the ACPI/SMI main copy) and clear the bits after seeing them, so
they're edge-triggered. Reading host events is a normal host command — it does
**not** drain the key FIFO, so typing is unaffected.

Implementation (all in `cmd_tw.c`, under `#if CONFIG_IS_ENABLED(CROS_EC)`):
`tw_poweroff_events_clear()` clears the latches at editor start (so the
press/keystroke that launched us — or the lid already being where it is — doesn't
fire an instant power-off); `tw_poweroff_event_pending()` polls + clears them each
key-wait tick; a hit returns a synthetic `KEY_POWER_BTN` handled exactly like
`^Q` → Y.

`tw_poweroff()` **also** calls `tw_poweroff_events_clear()` first thing, before it
even saves. Why: these are *latched* flags, and if the poweroff doesn't actually
happen (PSCI unavailable at EL3, or the save fails and we return), a still-set
latch would re-fire `KEY_POWER_BTN` on the very next key-wait tick and spin.
Clearing up front makes each trigger a single attempt — press (or close the lid)
again to retry.

---

## Battery % (title bar) — and why it needed a private EC command path

The **title bar** shows the battery percentage (near the `[En]`/`[Wubi]` chip).
It is read from the EC **on demand** — pressing `Ctrl-T` runs one EC SPI command,
updates `s->batt_pct`, and repaints the title. It is deliberately NOT polled on a
timer: a periodic poll would force the CPU to wake out of deep WFI at that
cadence, defeating the idle-power design (see POWERSAVE.md). Getting the read
working hit two EC quirks worth recording:

1. **U-Boot's `cros_ec_read_batt_charge()` is buggy.** It does `if (ret) return
   ret`, but `ret` is `ec_command()`'s **byte count** (positive on success), not
   an error — so a good read is reported as e.g. `-20`. Every *other* caller in
   `cros_ec.c` checks `if (ret != sizeof(resp))` / `if (ret < 0)`; this one got
   it wrong.

2. **The EC speaks protocol v3.** The obvious workaround — call the transport op
   `dm_cros_ec_get_ops(ec)->command` directly — only implements **v2** and returns
   `-1` on a v3 EC. The correct v3 framing (`send_command_proto3`,
   `create_proto3_request`, `handle_proto3_response`) is all **static** in
   `cros_ec.c`; there is no public raw-command entry point.

Rather than patch U-Boot core, the typewriter reimplements the v3 transaction in
`tw_ec_command()` (`cmd_tw.c`), built only on **public** pieces: `struct
cros_ec_dev` (its `din`/`dout` buffers) via `dev_get_uclass_priv()`, the transport
`->packet` op, `cros_ec_calc_checksum()`, and the `ec_host_request`/
`ec_host_response` structs. It builds the 8-byte request header + checksum, calls
`->packet`, validates the response header/checksum, and returns the payload. Falls
back to the v2 `->command` op if the EC isn't v3. This keeps all Chromebook-EC
specifics inside the typewriter — U-Boot is untouched.

---

## A config trap: `SYSRESET_PSCI` breaks the SPL/TPL link

The shell `poweroff` command needs a `UCLASS_SYSRESET` provider, which comes from
`CONFIG_SYSRESET_PSCI=y`. **But enabling it broke the libreboot build:**

```
sysreset_psci.c: undefined reference to `psci_sys_reset' / `psci_sys_poweroff'
make: *** [tpl/u-boot-tpl] Error
```

Why: `psci.o` (which defines those symbols) is built per-phase, gated by
`CONFIG_$(PHASE_)ARM_PSCI_FW`. `SYSRESET_PSCI` `select`s `SPL_ARM_PSCI_FW` for SPL
but there is **no `TPL_ARM_PSCI_FW` symbol at all** — yet this config has
`TPL_SYSRESET=y`, so `sysreset_psci.o` gets compiled into TPL with no backend →
undefined reference. TPL/SPL never power off, so this is pure collateral damage.

**We don't use shell `poweroff` here** (the editor's `^Q`/power-button path calls
`invoke_psci_fn` directly, no sysreset uclass needed), so the fix is simply to
**leave `CONFIG_SYSRESET_PSCI` off**. If you ever do want the shell command, also
turn off `SPL_SYSRESET`/`TPL_SYSRESET` so `SYSRESET_PSCI` only lands in main
U-Boot.

---

## Debugging

If `^Q` (or `poweroff`) doesn't power off:

1. **Is the `/psci` node in the DTB?** (should already be, from
   `rk3399-base.dtsi` — no patch needed):
   ```
   => fdt addr $fdtcontroladdr
   => fdt print /psci
   ```
   It shows `compatible = "arm,psci-1.0"` and `method = "smc"`. If it's somehow
   "not found", your DT is unusual — the node can be added to
   `rk3399-gru-u-boot.dtsi`, but that shouldn't be necessary here.

2. **Is the PSCI driver probed?** (this is the usual culprit)
   ```
   => dm tree
   ```
   `firmware 0 [ ] psci` (empty brackets) = bound-but-unprobed → the conduit
   was never set. The explicit probe in `^Q` handles this.

3. **What exception level are we at?** PSCI is a call *up* to firmware, so it
   only works from **EL2 or EL1** — from **EL3** `psci_probe()` bails with
   `-EINVAL` (nothing above EL3 to SMC into) and every call is a no-op. `^Q` now
   prints this on failure:
   ```
   [ Poweroff unavailable: EL3 probe=-22 - need SYSRESET_PSCI + EL<3 ]
   ```
   If you see `EL3`, that's the problem — U-Boot must be entered below EL3 (it
   normally is on this board via coreboot's bl31; a change there can regress it).
   `probe=` is the return of the driver probe (0 = ok).

4. **Wake behaviour.** PSCI `SYSTEM_OFF` is a normal system-off — the **power
   button turns the board back on**. (This is unlike an EC "ship mode" battery
   cutoff, which would wake only on AC.)

> Note: the builtin shell `poweroff` is **not** used here and `CONFIG_SYSRESET_PSCI`
> is intentionally left **off** (it breaks the SPL/TPL link on this config — see
> "A config trap" above). The editor's `^Q`/power-button path calls
> `invoke_psci_fn` directly and needs no sysreset provider.

---

## History (why this took a while)

The power-off search went through several wrong turns before landing on PSCI;
recorded here so nobody repeats them:

- **Not a PMIC.** gru/kevin has no AP-accessible PMIC. Linux `i2cdetect` shows
  no rk808 on any bus (only a TPM, audio codecs, touch controllers, and the
  smart battery behind the EC tunnel). So U-Boot's `rk8xx` PMIC sysreset path
  is irrelevant here.
- **Not the EC commands.** `cros_ec_reboot(EC_REBOOT_HIBERNATE)` timed out
  without powering off; `EC_REBOOT_COLD` rebooted; variants 7/8 don't exist in
  this EC firmware (`-1`). EC **battery cutoff** *does* power off but wakes only
  on AC (ship mode) — not what we want.
- **The answer was PSCI**, matching how Linux powers off. The `/psci` node was
  present all along (`rk3399-base.dtsi`); the only missing piece was that
  U-Boot never *probed* the PSCI driver. All the EC/PMIC exploration, and the
  brief detour of adding a redundant `/psci` node to the board dtsi, were dead
  ends.

---

## Deploying

Only `cmd/cmd_tw.c` changes for this — **no device-tree edit needed.** Copy the
typewriter sources into the libreboot U-Boot tree and rebuild:

```sh
cd $LB                                   # libreboot source dir
cp /path/to/u-boot-typewriter/cmd_tw*.c /path/to/.../*.h  src/u-boot/default/cmd/
./mk -c u-boot gru_kevin
./mk -b u-boot gru_kevin
rm -f elf/u-boot/default/gru_kevin/default/*   # clear staged ELF so coreboot re-pulls
./mk -b coreboot gru_kevin
# flash bin/gru_kevin/uboot_gru_kevin_libgfxinit_corebootfb.rom
```

(Optional: set `CONFIG_SYSRESET_PSCI=y` to also make U-Boot's builtin
`poweroff` work on its own; the `^Q` probe makes it unnecessary.)

---

## References

- `cmd_tw.c` — `tw_poweroff()`: save, probe PSCI, `invoke_psci_fn(SYSTEM_OFF)`.
- `arch/arm/dts/rk3399-gru-u-boot.dtsi` — the added `/psci` node.
- `drivers/firmware/psci.c` — `psci_probe()` (sets the SMC/HVC conduit from
  `method`), `invoke_psci_fn()`, and U-Boot's own `do_poweroff`.
- Linux `drivers/firmware/psci/psci.c` — `pm_power_off = psci_sys_poweroff`,
  the same `SYSTEM_OFF` call.
