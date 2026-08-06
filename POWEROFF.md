# Power-off on the RK3399 Chromebook (gru/kevin)

How the typewriter (`^Q`) powers the machine off, and why U-Boot's built-in
`poweroff` command needed fixing on this board.

---

## TL;DR

- `^Q` in the editor **saves + syncs**, then powers the board off via **PSCI
  `SYSTEM_OFF`** — an SMC call to ARM Trusted Firmware (coreboot's bl31). This
  is the same mechanism Linux uses, and it is **power-button-wakeable**.
- Out of the box this did **not** work: U-Boot's `poweroff` just printed
  `poweroff ...` and did nothing. The fix has **two parts**, both required:
  1. add a `/psci` node (`method = "smc"`) to U-Boot's device tree
     (`rk3399-gru-u-boot.dtsi`);
  2. **probe** the PSCI firmware driver before calling `SYSTEM_OFF` (U-Boot
     binds it but doesn't auto-probe it).

---

## Why it didn't work, and the fix

`poweroff` (and `^Q`) ultimately call
`invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF, ...)`, which does an `smc` to the
secure monitor (coreboot's bl31), which powers the board off. Two things were
missing:

### 1. The `/psci` device-tree node

U-Boot's PSCI driver (`drivers/firmware/psci.c`) learns the call conduit (SMC
vs HVC) from a `/psci` DT node's `method` property. **U-Boot's device tree for
gru/kevin had no `/psci` node at all**, so `psci_method` was never set and
`invoke_psci_fn()` made no SMC — every PSCI function returned `-8`
(NOT_SUPPORTED-looking garbage).

Linux works because *its* device tree has the node (confirmed on the running
system):

```
$ cat /proc/device-tree/psci/method        # -> smc
$ cat /proc/device-tree/psci/compatible     # -> arm,psci-1.0
$ dmesg | grep -i psci                       # -> PSCIv1.1 detected in firmware
```

Fix: add the same node to `arch/arm/dts/rk3399-gru-u-boot.dtsi`, inside the
root `/ { }` block:

```dts
	psci {
		compatible = "arm,psci-1.0", "arm,psci-0.2";
		method = "smc";
	};
```

Verify it landed in the built control DTB, from the U-Boot shell:

```
=> fdt addr $fdtcontroladdr
=> fdt print /psci
```

### 2. Probe the PSCI firmware driver

Even with the node present, `poweroff` still did nothing. `dm tree` showed the
driver **bound but not probed**:

```
=> dm tree
 firmware      0  [ ]   psci                          psci
```

The `[ ]` (not `[+]`) means `psci_probe()` never ran, so `psci_method` stayed
unset. U-Boot only auto-probes the PSCI driver when `CONFIG_SYSRESET_PSCI` is
enabled (it is **not** on this build). So `^Q` **probes it explicitly** before
the call:

```c
struct udevice *psci;
uclass_get_device_by_name(UCLASS_FIRMWARE, "psci", &psci);  /* runs psci_probe */
disable_interrupts();
invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF, 0, 0, 0);            /* powers off */
```

`psci_probe()` reads `method = "smc"` from the node, sets the conduit, and the
subsequent `SYSTEM_OFF` powers the board off cleanly.

> Alternative to the explicit probe: enable **`CONFIG_SYSRESET_PSCI=y`** in the
> U-Boot config. Then the PSCI sysreset device is registered and probed
> normally, U-Boot's builtin `poweroff` works on its own, and the explicit
> probe in `^Q` becomes redundant (but harmless). With the DT node in place,
> either approach works.

---

## What `^Q` does

1. **Save.** If the buffer is writable and modified, save it. If the save
   **fails, power-off is aborted** (a `[ Save failed ... ]` message) so edits
   are never lost.
2. **Settle.** `mdelay(200)` — U-Boot FAT writes are synchronous/write-through,
   so the data is already on the card; this is belt-and-braces.
3. **Power off.** Probe PSCI, then `invoke_psci_fn(PSCI_0_2_FN_SYSTEM_OFF)`.
   The board powers down. **Press the power button to turn it back on.**

`^Q` opens a **"Save & power off? (Y/N)"** confirm first, so an accidental
press can't shut the machine off.

---

## Debugging

If `^Q` (or `poweroff`) doesn't power off:

1. **Is the `/psci` node in the DTB?**
   ```
   => fdt addr $fdtcontroladdr
   => fdt print /psci
   ```
   It must show `compatible = "arm,psci-1.0" ...` and `method = "smc"`. If
   "not found", the dtsi edit didn't make it into the build — clean-rebuild
   U-Boot (see below), don't forget to clear the staged ELF so coreboot
   re-pulls it.

2. **Is the PSCI driver probed?**
   ```
   => dm tree
   ```
   Look for `firmware 0 [+] psci`. `[ ]` = bound-but-unprobed (the explicit
   probe in `^Q` handles this; U-Boot's builtin `poweroff` would need
   `CONFIG_SYSRESET_PSCI`).

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
- **The answer was PSCI**, matching how Linux powers off — it just needed the
  DT node **and** an explicit driver probe. All the EC/PMIC exploration was a
  detour.

---

## Rebuilding after the DTS change

The DTS edit is in U-Boot's tree, so U-Boot must be cleaned and rebuilt, and the
staged ELF cleared so coreboot re-pulls it:

```sh
cd $LB                                   # libreboot source dir
./mk -c u-boot gru_kevin
./mk -b u-boot gru_kevin
rm -f elf/u-boot/default/gru_kevin/default/*
./mk -b coreboot gru_kevin
# flash bin/gru_kevin/uboot_gru_kevin_libgfxinit_corebootfb.rom
```

---

## References

- `cmd_tw.c` — `tw_poweroff()`: save, probe PSCI, `invoke_psci_fn(SYSTEM_OFF)`.
- `arch/arm/dts/rk3399-gru-u-boot.dtsi` — the added `/psci` node.
- `drivers/firmware/psci.c` — `psci_probe()` (sets the SMC/HVC conduit from
  `method`), `invoke_psci_fn()`, and U-Boot's own `do_poweroff`.
- Linux `drivers/firmware/psci/psci.c` — `pm_power_off = psci_sys_poweroff`,
  the same `SYSTEM_OFF` call.
