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
   was never set. The explicit probe in `^Q` handles this. U-Boot's builtin
   `poweroff` would instead need `CONFIG_SYSRESET_PSCI=y` to get it probed.

3. **Does the builtin `poweroff` work?** With the node present and
   `CONFIG_SYSRESET_PSCI=y`, `=> poweroff` should power the board off — a quick
   way to confirm the PSCI path independent of the editor.

4. **Wake behaviour.** PSCI `SYSTEM_OFF` is a normal system-off — the **power
   button turns the board back on**. (This is unlike an EC "ship mode" battery
   cutoff, which would wake only on AC.)

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
