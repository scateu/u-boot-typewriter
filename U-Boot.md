# U-Boot customizations (gru/kevin)

Quick reminder list of everything we changed in the U-Boot tree. Details live in
the linked docs.

## New files (the command) — copy into `cmd/`
- `cmd_tw.c` — editor core, keys, Wubi, poweroff, EC (battery/power-btn/lid)
- `cmd_tw_video.c` — framebuffer glyph renderer + backlight
- `cmd_tw_fs.c` — file load/save (malloc buffers, UTF-8)
- `cmd_tw.h` — limits, key codes, state
- `wubi_embed.c/.h`, `font_data.c/.h` — embedded Wubi table + Unifont subset

## Build glue
- `cmd/Makefile` — add `obj-$(CONFIG_CMD_TYPEWRITER) += cmd_tw*.o ...`
- `cmd/Kconfig` — add `config CMD_TYPEWRITER` block (before final `endif`)

## Config (`.config` / defconfig)
- `CMD_TYPEWRITER=y` — the command
- `VIDEO=y`, `FS_FAT=y` — framebuffer + FAT (deps)
- `PWM_CROS_EC`, `DM_PWM`, `BACKLIGHT_PWM`, `SIMPLE_PANEL` — backlight `^-`/`^]` (POWERSAVE.md)
- `ARMV8_UDELAY_EVENT_STREAM=y` — WFE idle, power (POWERSAVE.md)
- CPU freq 600→408 MHz — power (POWERSAVE.md)
- `SYSRESET_PSCI` — leave **OFF**: breaks SPL/TPL link, unused (POWEROFF.md)

## Device tree
- **No DT patch needed** — `/psci { method="smc" }` already in `rk3399-base.dtsi` (POWEROFF.md)

## No U-Boot core edits
- Battery: EC has a proto-v3 + `cros_ec_read_batt_charge()` bug; we reimplement
  the EC command in `cmd_tw.c` instead of patching `cros_ec.c` (POWEROFF.md)

## Known hardware issues
- eMMC (mmc 0) FAT writes corrupt — use microSD (mmc 1); eMMC forced read-only (KNOWN_ISSUES.md)
