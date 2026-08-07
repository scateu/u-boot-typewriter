# typewriter — a Nano-like editor with Wubi Chinese input for U-Boot

 - Youtube Demo: [1](https://youtu.be/vDTKMEDWPtY) [2](https://youtube.com/shorts/Up-6oE29sNA)

`typewriter` adds a full-screen, GNU **Nano**-style text editor to the U-Boot
shell that runs **on the video framebuffer** and lets you type Chinese with a
built-in **Wubi 86 (五笔)** input method. It reads and writes UTF-8 files on a
FAT or ext4 partition, exactly like the `ved` editor it descends from — but
instead of driving a serial terminal with ANSI escapes, it draws every glyph
itself (ASCII and hanzi) from an embedded **GNU Unifont** subset, so Chinese
renders correctly on the device's own screen.

Target tree: **U-Boot 2026.07**.

```
+------------------------------------------------------------------------+
| typewriter    /notes.txt    Modified                    BAT: 68% [En]  |  title bar (gray)
|                                                                        |
|  你好，世界                                                            |  text area (B/W)
|  boot config: setenv bootargs ...                                      |  (hanzi = 2 cols)
|                                                                        |
| [五] wq  1.你 2.低 3.父荫 4.仰卧 …                              (1/2)  |  candidate bar (gray)
| ^S Save         ^A/^E BOL/EOL   ^B/^F ^P/^N mv  ^K Kill  ^W DelWord  … |  hints, aligned
| ^X Exit         ^R Open         ^Y Yank         ^D Del  ^-/^] Bright  … |  columns (gray)
+------------------------------------------------------------------------+
```

## Why a framebuffer (and not the serial console)

U-Boot's built-in console fonts (`VIDEO_FONT_8X16`, etc.) cover ASCII only, so
a hanzi codepoint sent to the normal console shows as blanks or mojibake. To put
Chinese on the device's display, `typewriter` bypasses the text console and
writes pixels straight into the framebuffer exposed by `struct video_priv`
(`fb`, `xsize`, `ysize`, `line_length`, `bpix`, `format`), drawing each glyph
from an embedded 16×16 Unifont bitmap. This is the same approach the sibling
`coreboot-typewriter` payload uses; the glyph renderer and Unifont subsetting
were adapted from it.

## Requirements

- `CONFIG_VIDEO=y` and a working display driver for your board (the command
  refuses to run if no `UCLASS_VIDEO` device with a framebuffer is present).
- `CONFIG_FS_FAT=y` and/or `CONFIG_FS_EXT4=y` (with `CONFIG_EXT4_WRITE=y` for
  ext4 saves). `CONFIG_CMD_FS_GENERIC=y` is normally already on.
- Input reaches the editor through U-Boot's stdin (a USB keyboard via
  `CONFIG_USB_KEYBOARD`, or a serial console — arrow keys are parsed from their
  ANSI escape sequences either way).

## Files

Hand-written:

| File | Purpose |
|---|---|
| `cmd_tw.h` | limits, key codes, `struct tw_state` (editor + IME + geometry) |
| `cmd_tw.c` | `U_BOOT_CMD` entry, key reader, editing primitives, Ctrl-key dispatch, Wubi composer, prompts |
| `cmd_tw_fs.c` | FS probe / load / save; UTF-8 ⇄ codepoint-line conversion |
| `cmd_tw_video.c` | framebuffer glyph renderer + Nano chrome (title, text, candidate bar, hints) |
| `ime_table.h` / `ime_table.c` | the `.tab` lookup engine (in-memory only), ported from wubi-fep |
| `font_data.h` | interface to the embedded glyph subset |
| `wubi_embed.h` | interface to the embedded Wubi table |
| `Makefile`, `Kconfig` | build glue and the `CONFIG_CMD_TYPEWRITER` option |

Data + generators:

| File | Purpose |
|---|---|
| `gen.sh` | regenerates both embedded C files (run once before building) |
| `gen_font.py` | `unifont.hex` + `wubi.tab` → `font_data.c` (only the needed glyphs) |
| `wubi.tab` | the compiled Wubi 86 table (from the wubi-fep repo) |
| `unifont.hex` | GNU Unifont source (fetched separately; see below) |

Generated (git-ignore these; they are large):

| File | Size | Content |
|---|---|---|
| `wubi_embed.c` | ~7 MB source → ~2.4 MB rodata | the Wubi table baked in |
| `font_data.c` | ~2.4 MB source → ~0.7 MB rodata | ~21k Unifont glyphs (ASCII + every hanzi in the table) |

## Generating the embedded data

You need two inputs next to these sources:

- `wubi.tab` — already checked in here (copied from the wubi-fep repo, produced
  by its `gen_table.py`).
- `unifont.hex` — the GNU Unifont glyph source. Fetch it once:

  ```sh
  curl -fL -o unifont.hex \
    https://unifoundry.com/pub/unifont/unifont-15.1.05/font-builds/unifont-15.1.05.hex
  # or on Debian/Ubuntu: apt install unifont; cp /usr/share/unifont/unifont.hex .
  ```

Then generate both C files:

```sh
./gen.sh
# paths are overridable: TAB=… UNIFONT_HEX=… OUT=… FONT_OUT=… ./gen.sh
```

`gen.sh` prints how many bytes it embedded and how many glyphs it kept (it
should report `0 wanted codepoints had no unifont glyph`).

## Integration into U-Boot

> Building for a **libreboot RK3399 Chromebook (gru/kevin)**? See
> **[INSTALL.md](INSTALL.md)** for the full board-specific tutorial: building
> from the libreboot tree, booting straight into typewriter (BOOTCOMMAND),
> autoboot/preboot tuning, flashing, and the eMMC-vs-microSD storage story. The
> section below is the generic U-Boot integration it builds on.
>
> Board-specific companion docs: **[POWEROFF.md](POWEROFF.md)** (how `^Q`
> powers off / boots the OS via PSCI), **[POWERSAVE.md](POWERSAVE.md)** (cutting
> idle power/heat via the event-stream `udelay` + 408 MHz), and
> **[KNOWN_ISSUES.md](KNOWN_ISSUES.md)** (the eMMC FAT-write defect).

There are three edits to the tree, then you enable the option. **Steps 2 and 3
edit different files with different syntax — don't mix them up** (a `Kconfig`
*definition* is not the same as a `.config` *assignment*; see the warning
below).

### 1. Copy the sources into `cmd/`

```sh
cp cmd_tw.c cmd_tw_fs.c cmd_tw_video.c cmd_tw.h \
   ime_table.c ime_table.h font_data.h font_data.c \
   wubi_embed.h wubi_embed.c \
   /path/to/u-boot-2026.07/cmd/
```

(`font_data.c` and `wubi_embed.c` are generated — run `./gen.sh` first.)

### 2. Add the build rule to `cmd/Makefile`

Append this one line:

```makefile
obj-$(CONFIG_CMD_TYPEWRITER) += cmd_tw.o cmd_tw_fs.o cmd_tw_video.o \
				ime_table.o wubi_embed.o font_data.o
# optional WFI-idle debug command (temporary; see POWERSAVE.md):
obj-$(CONFIG_CMD_TYPEWRITER) += cmd_twwfi.o
```

### 3. Add the option *definition* to `cmd/Kconfig`

Paste this **`config` block inside the `if CMDLINE ... endif`** region — i.e.
*before* the final `endif` in `cmd/Kconfig`, alongside the other `config CMD_*`
entries (the provided `Kconfig` file in this repo is exactly this block):

```kconfig
config CMD_TYPEWRITER
	bool "typewriter - Nano-like editor with Wubi Chinese input"
	depends on VIDEO
	depends on FS_FAT || FS_EXT4
	help
	  A full-screen, GNU Nano-style text editor on the video framebuffer,
	  with a built-in Wubi 86 (五笔) input method. See the README.
```

> ⚠️ **Do not paste `CONFIG_CMD_TYPEWRITER=y` into `cmd/Kconfig`.** That is
> `.config`/defconfig syntax, not Kconfig syntax — it is not a valid Kconfig
> statement, so the option will silently never appear in `menuconfig` and the
> command won't build. `cmd/Kconfig` gets the `config CMD_TYPEWRITER` *block*
> above; the `CONFIG_*=y` *assignments* go in your board config (step 4).

### 4. Enable it (and its dependencies) in your board config

`CMD_TYPEWRITER` **`depends on VIDEO` and `FS_FAT || FS_EXT4`**, so those must be
on or the option stays hidden in `menuconfig`. Enable them in your `.config`:

```sh
make <yourboard>_defconfig
./scripts/config -e VIDEO -e FS_FAT -e CMD_TYPEWRITER
# ext4 instead of/as well as FAT? add: -e FS_EXT4 -e EXT4_WRITE
# physical keyboard on the framebuffer? add: -e USB_KEYBOARD
make olddefconfig
```

or interactively via `make menuconfig` (enable **Video** first, then find
**typewriter** under *Command line interface*). Confirm it took:

```sh
grep CMD_TYPEWRITER .config          # -> CONFIG_CMD_TYPEWRITER=y
```

The resulting build registers a `typewriter` command in the U-Boot shell (check
with `help typewriter`). If it doesn't appear, it's almost always because
`VIDEO` (or a filesystem) isn't enabled — the dependency keeps the option
hidden.

## Usage

```
=> typewriter <iftype> <dev:part> <filename> [fstype]
=> typewriter mmc 0:1 /notes.txt
=> typewriter usb 0:1 /boot/uEnv.txt fat
```

Opens an empty buffer if the file does not exist; `^S` creates it.

## Keys

Modeless, like Nano — typing inserts text; Ctrl chords are commands.

Modeless, readline-style — typing inserts text; Ctrl/Meta chords are commands.

| Key | Action |
|---|---|
| printable | insert character (English mode) |
| `^B` / `^F`, `←` / `→` | back / forward one char |
| `^P` / `^N`, `↑` / `↓` | previous / next line |
| `^A` / `Home`, `^E` / `End` | start / end of line |
| `PgUp` / `PgDn` | move a screenful |
| `Enter` | split line |
| `Tab` | insert spaces to the next 8-column stop |
| `Backspace` | delete left (merges lines at column 0) |
| `^D` / `Delete` | delete char under cursor |
| `^W` | delete word backward |
| `^K` | kill from cursor to end of line |
| `^Y` | yank (paste the kill buffer) |
| `^S` | save — prompts for filename |
| `^R` | open a file — **picker**: lists files (`fatls`-style), `↑`/`↓` select, `Enter` open, `Esc` cancel (auto-saves current) |
| `^X` | exit — if modified & writable, asks Y/N/C |
| `^Q` | save + sync, then **Y)** power off (PSCI), **B)** boot the OS, **N)** cancel — see [POWEROFF.md](POWEROFF.md) |
| `Ctrl-Space` | toggle **Wubi ⇄ English** input |

Opening a file via `^R` **auto-saves** the current buffer first (if it's
writable and modified) before loading the new one; on a read-only buffer (the
eMMC, or `ro`) nothing is written and outgoing edits are dropped on switch.

> **Meta (Alt) keys** (`M-b`/`M-f` word motion, `M-d` kill-word, `M-w` find)
> are wired in the code but **do not work on the target Chromebook's console**,
> so they're omitted above. Char/line motion and `^W` cover the same ground.
> They may work on other terminals that deliver Alt as an ESC prefix.

### Wubi input (`Ctrl-Space` to turn on)

| Key | Action |
|---|---|
| `a`–`z` | append to the Wubi code (shown in the candidate bar) |
| `1`–`9` | commit the numbered candidate on the current page |
| `Space` | commit the first candidate |
| `=` / `.` | next candidate page |
| `-` / `,` | previous candidate page |
| `Backspace` | delete the last code letter (while composing) |
| `Esc` | cancel the pending code/candidates |
| `Enter` (while composing) | emit the raw code letters literally |

Example: with Wubi on, type `w q` then `1` (or `Space`) → 你.

## How it works

- **Framebuffer.** `tw_video_init()` grabs the first `UCLASS_VIDEO` device and
  its `video_priv`, then draws with `put_pixel`/`fill_rect`. `tw_pack_rgb()`
  mirrors the video uclass's own `video_index_to_colour()` packing so the custom
  palette is correct for 16- and 32-bpp formats. `video_sync(dev, true)` flushes
  after each frame.
- **Glyphs.** Each Unifont glyph is 16 rows; narrow (ASCII) are 8 px wide, wide
  (CJK) 16 px. `find_glyph()` binary-searches the codepoint-sorted subset;
  `draw_glyph()` blits it row-by-row.
- **Document model.** Lines of Unicode **codepoints** (`u32 lines[][]`), so the
  cursor column is exact and a wide hanzi is one edit column but two screen
  columns (`tw_cp_cols()`). Files are UTF-8, decoded on load and re-encoded on
  save.
- **IME.** The Wubi composer (`tw_ime_key`) sits in front of character
  insertion: `a`–`z` build a code, `ime_lookup()` fetches candidates from the
  embedded table, and committing decodes the candidate's UTF-8 and inserts each
  codepoint through the normal edit path. Logic mirrors the wubi-fep FEP.

## Constraints and limits

| Limit | Value | Where |
|---|---|---|
| Max lines | 2048 | `TW_MAX_LINES` |
| Max codepoints per line | 511 | `TW_MAX_COLS` |
| Wubi code length | 16 | `IME_CODE_LEN` |
| Candidates fetched / page | 64 / 9 | `TW_MAX_CANDS` / `TW_PAGE` |

The line buffer is `TW_MAX_LINES × TW_MAX_COLS × 4` bytes of BSS (~4 MB at the
defaults). Halve either constant to shrink it on memory-tight boards. Long
lines **soft-wrap** to the next screen row (never splitting a wide hanzi across
the wrap). `Tab` expands to spaces at `TW_TABW` (8) stops. Search is ASCII-only
(you can't search for hanzi yet). There is no undo. These are deliberate v1
simplifications for a bootloader-resident editor.

## Provenance

- Editor skeleton, FS layer, and key-escape parser: adapted from the
  `vim-for-Uboot` (`ved`) reference, updated to the 2026.07 header set (U-Boot
  removed `<common.h>`).
- `.tab` format, `ime_table.c` lookup, and the Wubi composition state machine:
  from the userspace `wubi-fep` front-end processor.
- Framebuffer glyph renderer and Unifont subsetting: from the
  `coreboot-typewriter` payload.
