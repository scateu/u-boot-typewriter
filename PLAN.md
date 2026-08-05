# Plan: `typewriter` — a Nano-like editor with Wubi input for U-Boot 2026.07

## Goal
Add a `typewriter` command to U-Boot that clears the screen and runs a GNU
Nano-style, modeless full-screen text editor, with a built-in Wubi 86 Chinese
input method. File-backed (loads/saves a file on a real filesystem, exactly like
the `ved` reference). Target tree: `u-boot-2026.07`.

## Key facts established from exploration
- `ved` (vim-for-Uboot) gives the proven skeleton: static `struct` in BSS, ANSI
  render-every-frame, `ved_read_key()` escape parser, `fs_set_blk_dev` +
  `fs_read`/`fs_write` via `map_sysmem(CONFIG_SYS_LOAD_ADDR)`.
- `wubi-fep` gives the IME: the `.tab` binary format + `ime_lookup()` in
  `table.c` (prefix index + binary search) and the composition state machine in
  `handle_cn_byte()`. `table.c`'s host-only bits are just `ime_table_open()`
  (mmap) and `fprintf(stderr,...)` — I will drop the mmap path and keep only the
  in-memory `ime_table_open_mem()`, which is already pure.
- **`include/common.h` was removed in 2026.07.** The `ved` sources `#include
  <common.h>` and will NOT compile as-is. New code must use the modern header
  set (as `cmd/2048.c` does): `command.h`, `stdio.h`, `vsprintf.h`,
  `linux/string.h`, `linux/ctype.h`, `fs.h`, `mapmem.h`.
- Wubi table: `wubi.tab`, 2,406,559 bytes, scheme 0, codelen 16, 86,791 records.
  Baked in as a `static const unsigned char[]` (rodata) — comparable to ved's
  ~2 MB BSS, acceptable.
- Console primitives available: `getchar()`, `tstc()`, `putc()`, `puts()`,
  `printf()`, `snprintf()`, `vsnprintf()`.

## Files to create in `u-boot-typewriter/` (then copied into `cmd/`)
| File | Purpose |
|---|---|
| `cmd_tw.h` | limits, key constants, ANSI macros, `struct tw_state` (incl. IME sub-state) |
| `cmd_tw.c` | `U_BOOT_CMD(typewriter,...)`, key reader, Nano modeless input loop, Ctrl-key dispatch, IME composition, ex-less command prompts (search / save-as) |
| `cmd_tw_fs.c` | FS probe / load / save — adapted from `cmd_ved_fs.c`, modern headers |
| `cmd_tw_render.c` | Nano UI: top title bar, text area, status line, 2-row shortcut hint bar, Wubi candidate bar overlay |
| `ime_table.h` | trimmed copy of wubi-fep `table.h` (no mmap fields needed but harmless) |
| `ime_table.c` | `ime_table_open_mem()` + `ime_lookup()` only; `fprintf` → return -1 / silent |
| `wubi_embed.h` | declares the embedded table symbol/len |
| `wubi_embed.c` | **generated** by reused `gen_embed.py` from `wubi.tab` (huge, ~13 MB of C) |
| `Makefile` | `obj-$(CONFIG_CMD_TYPEWRITER) += ...` |
| `Kconfig` | `CONFIG_CMD_TYPEWRITER`, depends on `FS_FAT || FS_EXT4` |
| `README.md` | usage, keys, Wubi notes, integration steps |

## UI layout (Nano-authentic), 24×80 default
```
 row 1            title bar (reverse):  " U-Boot typewriter   <filename>   Modified"
 rows 2..T-4      text area (no line-number gutter — Nano has none)
 row T-3          status/message line (e.g. "[ Read 12 lines ]", search prompt)
 row T-2          Wubi candidate bar: "五 wq  1你 2怎 …"  (blank/[En] chip when idle)
 rows T-1, T      two-row shortcut hint bar (reverse), Nano style:
                  ^O Write Out  ^X Exit  ^K Cut  ^U Paste  ^W Where Is
                  ^\ Wubi/En    ^G Help  arrows move   Bksp/Del edit
```
When a search / write-out prompt is active it takes over the status line (row
T-3) like Nano's answer prompt.

## Keybindings (modeless, Nano-style)
- Typing: printable ASCII inserted at cursor (when IME is in English mode).
- Arrows / Home / End / PageUp / PageDown: navigation (reuse ved motions).
- `Enter`: split line. `Backspace`/`Del`: delete (merge lines at col 0).
- `Ctrl-O`: Write Out — prompt shows filename (pre-filled), Enter saves.
- `Ctrl-X`: Exit — if modified, prompt "Save modified buffer? (Y/N/C)".
- `Ctrl-K`: cut current line to cutbuffer. `Ctrl-U`: paste cutbuffer (uncut).
- `Ctrl-W`: Where Is — search prompt; `Ctrl-W` again / Enter repeats.
- `Ctrl-A` / `Ctrl-E`: beginning / end of line (Nano bindings).
- `Ctrl-G`: brief help line.
- **`Ctrl-\`**: toggle Wubi ⇄ English input (the FEP's quick-toggle key). Only
  two modes here (En / 五) since Pinyin is out of scope.

## Wubi input integration (the interesting part)
The IME is layered *in front of* the editor's character insertion, mirroring
`handle_cn_byte()` but committing into the buffer instead of a pty:
- State in `struct tw_state`: `ime_on`, `code[17]`, `code_len`, `page`.
- In the main loop, when `ime_on` and the key is a plain byte, feed it to
  `tw_ime_byte()`:
  - `a`–`z`: append to `code`, look up candidates, redraw candidate bar.
  - `1`–`9`: commit Nth candidate on page → the hanzi UTF-8 bytes are inserted
    into the line buffer via the normal `tw_insert_char` path (byte by byte;
    line buffer is already `char[]`, UTF-8-clean).
  - `Space`: commit first candidate. `Backspace`: pop a code letter.
  - `-`/`,` prev page, `=`/`.` next page.
  - `Enter` with pending code: emit the raw letters (Nano-ish), swallow newline.
  - `Esc` with pending code: cancel composition. No pending: ignored.
  - non-`a-z` printable with pending code: commit first, then insert the punct.
- When `ime_on` is false, bytes go straight to `tw_insert_char`.
- Rendering hanzi: the text area prints raw UTF-8 bytes; a serial terminal with
  a CJK font shows them. Cursor-column math counts *display width* approximately
  by treating bytes ≥ 0x80 lead bytes as width-2 (best-effort, matches ved's
  simple column model closely enough for editing). Documented as a limitation.

## Data model (from ved, unchanged shape)
- `lines[TW_MAX_LINES][TW_MAX_LINE_LEN]`, `line_len[]`, `num_lines`.
- Cursor `cur_row/cur_col/scroll_top`; `dirty`; `quit`; `filename`.
- `cutbuffer[TW_MAX_LINE_LEN]`, `cut_valid` for ^K/^U.
- IME sub-state as above. Everything static in BSS; no heap.
- Defaults `TW_MAX_LINES=4096`, `TW_MAX_LINE_LEN=512` (tunable; note ~2 MB BSS).

## FS layer
Copy `ved_fs_*` almost verbatim into `cmd_tw_fs.c`, swap headers to the 2026.07
set, rename symbols `tw_*`. Same `map_sysmem(CONFIG_SYS_LOAD_ADDR)` I/O buffer
and `fs_set_blk_dev` before each op.

## Usage
```
=> typewriter <iftype> <dev:part> <filename> [fstype]
=> typewriter mmc 0:1 /notes.txt
```
Opens empty buffer if the file doesn't exist (Ctrl-O creates it), same as ved.

## Build / integration
1. `python3 wubi-fep/gen_embed.py --out wubi_embed.c wubi:wubi.tab:0` (reuse the
   existing script; rename the emitted accessor to a `typewriter`-specific
   symbol, or keep `embedded_tables[]` and index [0]). I'll wrap this in a tiny
   `gen.sh` so it's reproducible.
2. Copy `cmd_tw*.c/.h`, `ime_table.*`, `wubi_embed.*` into `u-boot-2026.07/cmd/`.
3. Append one line to `cmd/Makefile`:
   `obj-$(CONFIG_CMD_TYPEWRITER) += cmd_tw.o cmd_tw_fs.o cmd_tw_render.o ime_table.o wubi_embed.o`
4. Append the `CONFIG_CMD_TYPEWRITER` block to `cmd/Kconfig`.
5. `CONFIG_CMD_TYPEWRITER=y` + `CONFIG_FS_FAT=y` (and/or `FS_EXT4`) in defconfig.

## Verification (no target board here)
- Primary: compile-check the sources against the 2026.07 tree headers. If a
  cross toolchain + a defconfig (e.g. `qemu_arm64` / sandbox) is available, do a
  real `make` of just the command objects. I'll confirm which is feasible when I
  reach that step; if no toolchain is installed I'll report that and provide the
  exact build commands instead of claiming a green build.
- Table sanity: a tiny host harness that links `ime_table.c` + `wubi_embed.c`
  and looks up a few known codes (e.g. `wq`→你) to prove the embed + lookup port
  is correct before trusting it in-tree.

## Scope / non-goals
- Wubi only (no Pinyin), per your choice.
- No syntax highlighting, no multi-file, no undo (Nano has undo; out of scope
  for v1 — will note it).
- CJK column width is best-effort; combining chars not handled.

## Open risk I'll surface as I go
- The ~2.4 MB embedded table roughly doubles if I'm not careful with the C
  encoding; `gen_embed.py` emits decimal bytes (~13 MB source) which compiles to
  ~2.4 MB rodata — fine. I'll confirm the object size after building.
