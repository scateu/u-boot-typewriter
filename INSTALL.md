# Installing typewriter on a libreboot Chromebook (RK3399 gru/kevin)

This is the end-to-end recipe for building the `typewriter` payload into
**libreboot's U-Boot** for the **Samsung Chromebook Plus (gru/kevin, RK3399)**,
booting straight into it, and getting file saving working. It is the concrete,
board-specific companion to the generic integration notes in
[README.md](README.md).

> The generic "copy these files into `cmd/`, add the Makefile/Kconfig lines"
> steps are in the README. This document assumes you've done that once, and
> focuses on the **libreboot build tree, boot configuration, flashing, and the
> eMMC-vs-microSD storage story.**

---

## 0. TL;DR

```sh
# in the libreboot source tree:
LB=~/repo/libreboot/libreboot-26.01rev1_src

# 1. put the typewriter sources in U-Boot's cmd/ (once)
cp *.c *.h $LB/src/u-boot/default/cmd/
#   + add the obj- line to cmd/Makefile and the config block to cmd/Kconfig
#     (see README "Integration"), and enable CMD_TYPEWRITER + VIDEO + FS_FAT.

# 2. set boot config (see sections 2-4): BOOTCOMMAND, autoboot delay, preboot

# 3. build U-Boot then coreboot
cd $LB
./mk -c u-boot gru_kevin          # clean, so the config change is picked up
./mk -b u-boot gru_kevin
rm -f elf/u-boot/default/gru_kevin/default/*   # force coreboot to re-pull u-boot
./mk -b coreboot gru_kevin

# 4. flash bin/gru_kevin/uboot_gru_kevin_libgfxinit_corebootfb.rom
```

Then on the device: it boots into `typewriter`, editing `a.txt` on the
**microSD** (`mmc 1:1`). Type, `^S` to save. Save to the **microSD, not the
eMMC** (see section 6).

---

## 1. Prerequisites

- A libreboot **source** release, e.g.:
  ```sh
  wget https://rsync.libreboot.org/stable/26.01rev1/libreboot-26.01rev1_src.tar.xz
  tar xf libreboot-26.01rev1_src.tar.xz
  ```
  (Build docs: <https://libreboot.org/docs/build/#next-build-rom-images>.)
- A way to flash the RK3399 SPI: either **internal `flashrom`** on the running
  Chromebook, or an external **CH341A** SPI programmer (driver:
  <https://github.com/dimich-dmb/spi-ch341-usb>).
- A **microSD (TF) card**, FAT32-formatted — this is where files are saved
  (the eMMC's U-Boot write path is broken; section 6).

Throughout, `LB` is the libreboot source dir:
```sh
LB=~/repo/libreboot/libreboot-26.01rev1_src
```

---

## 2. Add the typewriter sources (once)

Copy the payload sources into libreboot's bundled U-Boot:

```sh
cp *.c *.h $LB/src/u-boot/default/cmd/
```

`*.c *.h` = `cmd_tw.c cmd_tw.h cmd_tw_fs.c cmd_tw_video.c ime_table.c
ime_table.h font_data.c font_data.h wubi_embed.c wubi_embed.h`. `font_data.c`
and `wubi_embed.c` are generated — run `./gen.sh` first if they're missing (see
README).

Then wire the build, per the README "Integration" section:
- append the `obj-$(CONFIG_CMD_TYPEWRITER) += ...` line to
  `$LB/src/u-boot/default/cmd/Makefile`;
- add the `config CMD_TYPEWRITER` block to
  `$LB/src/u-boot/default/cmd/Kconfig` (inside `if CMDLINE ... endif`).

**Enable the option and its dependencies.** libreboot regenerates U-Boot's
`.config` from a tracked defconfig, so the durable place to enable things is:

```
$LB/config/u-boot/gru_kevin/config/default
```

Add (or confirm) these lines there:
```
CONFIG_CMD_TYPEWRITER=y
CONFIG_VIDEO=y
CONFIG_FS_FAT=y
```
(`VIDEO` is needed because typewriter draws to the framebuffer — U-Boot's
console font is ASCII-only and can't show hanzi. `FS_FAT` for the FAT card.)

For a quick one-off test you can instead edit the live config after a
configure step:
```sh
cd $LB/src/u-boot/default
./scripts/config -e CMD_TYPEWRITER -e VIDEO -e FS_FAT
make olddefconfig
grep CMD_TYPEWRITER .config      # -> CONFIG_CMD_TYPEWRITER=y
```
…but a live `.config` edit is wiped when libreboot re-runs `./mk -m u-boot`, so
prefer the `config/u-boot/.../default` file for anything permanent.

---

## 3. Boot straight into typewriter (BOOTCOMMAND)

Set U-Boot's boot command so the board comes up directly in the editor instead
of scanning for an OS. In `$LB/config/u-boot/gru_kevin/config/default` (or via
`menuconfig` → *Boot options*), change:

```
CONFIG_BOOTCOMMAND="typewriter mmc 1:1 a.txt"
```

- `mmc 1:1` = the **microSD**, first partition — the writable card.
- Bare `typewriter` (no args) already defaults to `mmc 1:1 a.txt`, so
  `CONFIG_BOOTCOMMAND="typewriter"` is equivalent.
- typewriter opens **writable** on the microSD; type and `^S` to save. On the
  eMMC it would open read-only (section 6).

For reference, libreboot's stock boot commands look like one of:
```
# old (2024) style, uses extlinux.conf on partition 1:
bootcmd=bootflow scan -lb
# newer style, menu-driven:
bootcmd=bootflow scan -l; if bootflow menu; then cls; bootflow boot; fi
```
Replacing `CONFIG_BOOTCOMMAND` with `typewriter ...` overrides that so the
machine is a typewriter on power-up.

(A previous "aggressive" direct-boot command that loads a kernel from
partition 2 is kept here only as a reference for restoring normal booting:
```
CONFIG_BOOTCOMMAND="load mmc 0:2 $fdt_addr_r boot/rk3399-gru-kevin.dtb;load mmc 0:2 $kernel_addr_r boot/vmlinuz;load mmc 0:2 $ramdisk_addr_r boot/initrd.img;setenv bootargs root=/dev/mmcblk1p2 rw quiet;booti $kernel_addr_r $ramdisk_addr_r:$filesize $fdt_addr_r"
```
addresses: fdt `0x12000000`, kernel `0x02000000`, ramdisk `0x12180000`.)

---

## 4. Faster startup: autoboot delay, preboot, bootflow delay

Out of the box the board waits several seconds and runs a slow `usb start`
preboot. To make it drop into typewriter quickly:

**a) Autoboot delay → 1 second.** `menuconfig` → *Boot options* → "delay in
seconds before automatically booting", set to `1`. (defconfig:
`CONFIG_BOOTDELAY=1`.)

**b) Disable preboot** (the default preboot is `usb start`, which is slow).
`menuconfig` → *Boot options* → turn **off** "Enable preboot".
(defconfig: `# CONFIG_PREBOOT is not set`, or clear
`CONFIG_USE_PREBOOT`/`CONFIG_PREBOOT`.)

**c) Bootflow scan delay → 1 second.** This one is **not in the menu** (editing
it in menuconfig errors), so set it directly in the U-Boot `.config`:
```sh
vi $LB/src/u-boot/default/.config
# set:
CONFIG_CMD_BOOTFLOW_BOOTDELAY=1
```
(Your note had `=8`; that is the slow default — use `1` for a quick boot. As
with any live `.config` edit, put it in `config/u-boot/gru_kevin/config/default`
to make it survive a re-`mk`.)

Since typewriter itself is now the boot command (section 3), b) and c) mostly
just remove pre-editor stalls.

---

## 5. Build and flash

libreboot won't rebuild U-Boot if it thinks it's already built, so after any
U-Boot config or source change you must clean it and clear the staged ELF:

```sh
cd $LB
export XBMK_THREADS=40                 # parallelism to taste

./mk -c u-boot gru_kevin               # clean u-boot (picks up config changes)
./mk -b u-boot gru_kevin               # build u-boot
rm -f elf/u-boot/default/gru_kevin/default/*   # force coreboot to re-pull u-boot
./mk -b coreboot gru_kevin             # build the coreboot ROM (embeds u-boot)
```

The ROM is:
```
bin/gru_kevin/uboot_gru_kevin_libgfxinit_corebootfb.rom
```

Flash it one of two ways:

**Internal (from the running Chromebook):**
```sh
scp bin/gru_kevin/uboot_gru_kevin_libgfxinit_corebootfb.rom kk@<chromebook-ip>:
ssh kk@<chromebook-ip>
sudo flashrom -p internal -w uboot_gru_kevin_libgfxinit_corebootfb.rom
```

**External CH341A programmer:**
```sh
sudo flashrom -p ch341a_spi --noverify -w \
  bin/gru_kevin/uboot_gru_kevin_libgfxinit_corebootfb.rom
```

Iterating on just the payload C code: re-copy `*.c *.h` into
`src/u-boot/default/cmd/`, then rerun the four build commands above (clean +
build u-boot, clear the staged ELF, build coreboot).

---

## 6. Storage: the eMMC is read-only, save to the microSD

**The eMMC's U-Boot FAT-write path is broken on this board.** Writing a file to
the eMMC (`mmc 0`) corrupts the FAT directory — `fatls` shows doubled/garbled
names (`a.txt` → `AA`, `AA.TTT/`), `0xFFFFFFFF` sizes, and can wipe existing
files.

This is **not** a typewriter bug and **not** a filesystem bug: it reproduces
with plain U-Boot `fatwrite`/`fatmkdir`, and was traced (by elimination) to the
**RK3399 eMMC controller's write path (`rockchip_sdhci`)**. Reads are fine; only
writes corrupt. It is **not** cache-coherency (`dcache off` doesn't help,
write-through cache), **not** a sector-size mismatch (512 == 512), and **not**
cluster size (`mkfs -s 1` still corrupts). See
[KNOWN_ISSUES.md](KNOWN_ISSUES.md) for the full investigation.

> Note: an early hypothesis blamed `CONFIG_BOUNCE_BUFFER`. That turned out to be
> **wrong** — bounce buffer is enabled and the real defect is the eMMC sdhci
> write path. Don't chase the bounce-buffer angle.

**The microSD (`mmc 1`) uses a different controller (`dw_mmc`) and writes
correctly.** So:

- **Save to the microSD.** Boot command uses `mmc 1:1` (section 3); bare
  `typewriter` defaults there.
- typewriter **enforces** this: `mmc 0` (eMMC) is **hard-locked read-only** — it
  refuses to save there no matter what — while the microSD is writable by
  default. So an accidental eMMC session can't corrupt anything.

### Preparing the microSD

FAT32-format the card's first partition (from a Linux box, card = `mmcblk1p1`
here — check yours):
```sh
sudo mkfs.vfat -F 32 /dev/mmcblk1p1
sudo fsck.vfat -v /dev/mmcblk1p1        # sanity check
```
Cluster size doesn't matter for correctness (the eMMC bug is independent of it);
Linux defaults are fine. U-Boot reports the card block length with
`mmc dev 1; mmc info` (`Rd Block Len: 512`).

---

## 7. Using it

On boot you get a full-screen editor. Default input is **English `[En]`**; press
**Ctrl-Space** to toggle Wubi (`[五]`), type a wubi code (`a`–`z`), pick a
candidate with `1`–`9`/Space.

File keys:
- `^S` — save to the current file (prompts for filename).
- `^R` — open a file: shows a list of files on the current device; `↑`/`↓` to
  select, `Enter` to open, `Esc` to cancel. Auto-saves the current buffer first.
- `^X` — exit.
- `^Q` — save, sync, then power the board off via the EC (Y/N confirm).

Editing is readline-style: `^B/^F/^P/^N` and arrows move, `^A/^E` line ends,
`^K` kill-to-EOL, `^Y` yank, `^W` delete-word-back. Full list in the README and
via `help typewriter` at a U-Boot prompt. (Meta/Alt chords are wired but don't
work on this Chromebook's console.)

---

## 8. Restoring normal booting

typewriter is just the boot command; to go back to booting an OS, set
`CONFIG_BOOTCOMMAND` back to libreboot's default (`bootcmd=bootflow scan -lb`,
or the kernel-load form in section 3), rebuild, and reflash. Nothing else
changes — the typewriter command still exists and can be run by hand from the
U-Boot shell (`typewriter mmc 1:1 a.txt`).

---

## Appendix: upstream config locations

| What | Where |
|---|---|
| Payload sources | `$LB/src/u-boot/default/cmd/cmd_tw*.{c,h}`, `ime_table.*`, `font_data.*`, `wubi_embed.*` |
| U-Boot defconfig (durable) | `$LB/config/u-boot/gru_kevin/config/default` |
| Live U-Boot `.config` (transient) | `$LB/src/u-boot/default/.config` |
| Built ROM | `$LB/bin/gru_kevin/uboot_gru_kevin_libgfxinit_corebootfb.rom` |
| Staged U-Boot ELF (clear to force rebuild) | `$LB/elf/u-boot/default/gru_kevin/default/` |
