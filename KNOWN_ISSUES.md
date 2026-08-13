# Known Issues

## 1. eMMC FAT writes corrupt the directory (U-Boot rockchip_sdhci, NOT typewriter) — use the microSD instead

**Symptom.** Saving a file to the **eMMC** (`mmc 0`) corrupts the FAT: `fatls`
shows filenames with every character doubled and the extension mangled (`a.txt`
→ `AA`, `AA.TTT/`), directories where files should be, and sizes of
`4294967295` (`0xFFFFFFFF`). Existing files (e.g. `extlinux.conf`, `vmlinuz`)
can disappear.

**It is NOT the typewriter command, and NOT the FAT layer.** It reproduces with
plain `fatwrite`, no editor:

```
=> mw.b 0x02000000 41 100
=> fatwrite mmc 0:1 0x02000000 ctrl.txt 0x100
=> fatls mmc 0:1
4294967295  AA
CCRR.TTT/
```

**Confirmed root cause: the RK3399 eMMC write path (`rockchip_sdhci` / SDHCI
SDMA).** The corruption is specific to the **eMMC controller's write
direction**. Established by elimination:

- Reproduces with plain `fatwrite`/`fatmkdir` → below the FS layer.
- **Reads are fine**; only writes corrupt → controller write path.
- Not a heap/buffer bug: typewriter's serialization is byte-exact and I/O
  buffers are exact-size malloc()'d.
- Not sector-size mismatch: FAT logical sector (512) == card block len (512).
- Not multi-block cluster: reformatting `-s 1` (single-block clusters) still
  corrupts (the doubling only lessens: `CCRR` → `CC`).
- Not data-cache coherency: `dcache off` does not fix it, and the cache is
  write-through anyway.
- The FAT software was audited — given a clean `"a.txt"`, the LFN slot builder
  and 8.3 short-name generator produce correct entries.

**microSD (`mmc 1`) works correctly.** The removable TF/microSD card uses a
different controller and driver (`dw_mmc` / IDMAC), and FAT writes to it are
clean. **This is the working write path — save to the microSD, not the eMMC.**

**Workarounds.**
- **Save to the microSD** (`typewriter mmc 1:1 <file> rw`). Writes work.
- typewriter enforces this automatically: **`mmc 0` (eMMC) is hard-locked
  read-only** — saving is always refused there, regardless of args — while
  other devices (the microSD `mmc 1`, etc.) are writable by default. Pass a
  trailing `ro` to force read-only on a writable device. So the eMMC can never
  be corrupted by typewriter, and the microSD (bare `typewriter` default) just
  works.

**Upstream.** The `rockchip_sdhci` eMMC write defect on RK3399 is worth a U-Boot
bug report. Not fixed here, per project preference to leave U-Boot's `mmc/`
untouched.

**What was verified on typewriter's side.** The editor's serialization is
byte-exact: saving `你好\nabc\n` hands `fs_write` exactly
`e4 bd a0 e5 a5 bd 0a 61 62 63 0a` (11 bytes) with filename `a.txt`. I/O buffers
are malloc()'d at the exact needed size (not a fixed multi-MB buffer at
`CONFIG_SYS_LOAD_ADDR`, which on this board overlaps U-Boot's own DRAM image).
So nothing typewriter passes to `fs_write` is malformed.

---

## 2. Rare hanzi are not typeable / have no glyph (embedded-table trim)

**Symptom.** Some uncommon characters can't be produced by their Wubi code, and
would not render even if entered — e.g. `禊` (U+798A).

**Cause.** To fit the 8 MiB SPI ROM (and its fixed binman offsets), the embedded
Wubi table is trimmed with `gen_table.py --top 20000 --max-phrases 20000`,
keeping the ~20 000 most common hanzi/phrases (~4 869 distinct glyphs). The font
subset only embeds glyphs for codepoints present in the table, so a character
outside the top-20 000 is both un-lookup-able and un-drawable. `禊` is in the
full dictionary and in unifont.hex, but was cut by the trim.

**Workaround / tuning.** Raise `--top` in `gen.sh` (see its header) if you have
ROM headroom — but note distinct glyphs plateau around ~4 900, so gains taper
off. Lower it if you need to save more space. Rebuild the embedded data with
`./gen.sh` after changing the trim.

---

## 3. Filenames are ASCII-only — the FAT driver can't round-trip Chinese (U-Boot FAT, NOT typewriter)

**Symptom.** A file whose name contains hanzi does not survive a round-trip:
- A Chinese name **created in typewriter** (or via `fatwrite`) reads back as
  garble — each hanzi becomes several Latin-1 chars (e.g. `文` → `æ–‡`-style).
- A Chinese name **created in Linux** shows in the `^R` picker as a single wrong
  glyph (e.g. a stray `K`).
File **content** is unaffected — hanzi *inside* a file save and load correctly
(the body is opaque UTF-8 bytes). This is purely about the **filename**.

**Root cause: U-Boot's FAT long-name codec is Latin-1/UCS-2, not UTF-8.** In
`fs/fat/` the LFN converters move one byte per UTF-16 code unit:
- write (`str2slot`, fat_write.c): `slot->name[j] = name[i]`, high byte left 0 —
  so a UTF-8 byte becomes a UTF-16 unit; the 3 bytes of a hanzi become 3 junk
  chars.
- read (`slot2str`, fat.c): `l_name[i] = slot->name[j]` (low byte only) — so a
  real UTF-16 hanzi (`U+6587`) is truncated to its low byte (`0x87`) → one wrong
  char.
There is no `utf8_to_utf16`/`utf16_to_utf8` in the driver and no config to enable
one. So CJK filenames fundamentally cannot round-trip on this U-Boot, regardless
of what the editor does.

**Decision.** typewriter keeps **filenames ASCII-only**: the `^R` picker's New
and Rename prompts do NOT route through the Wubi IME (they accept ASCII only, and
the prompt label says "(ASCII)"). Chinese in file *bodies* works as always. A
Wubi-capable filename would require patching U-Boot's `fs/fat/` LFN codec to do
real UTF-8↔UTF-16 — deliberately not done, per project preference to leave
U-Boot's `fs/`/`mmc/` untouched.

---

## 4. Maximum file size — buffer geometry, and the truncation guard

**Limits (smallest wins):**
- **Editor buffer (binding):** `TW_MAX_LINES` 2048 × `TW_MAX_COLS` 511 usable
  codepoints/line ≈ 1.05 M codepoints. In bytes that's **~1 MB ASCII** (1 B/cp)
  up to **~3 MB Chinese** (3 B/cp). The buffer is a static `u32
  lines[2048][512]` (~4 MiB in BSS, always reserved).
- **Load scratch / clamp:** `TW_FILE_BUF_SIZE` ≈ 4.2 MB; a load never allocates
  or reads more. Larger than the buffer can hold, so the buffer cap bites first.
- **FAT:** FAT32's 4 GiB file limit is irrelevant here.

**Guard behavior (added; silent truncation is prevented):**
- **Load:** if a file exceeds the buffer (too many lines, an over-long line, or
  bigger than the ~4 MB clamp), `s->load_truncated` is set. The editor then:
  (a) shows `TRUNCATED ... too big; READ-ONLY` on the status line, (b) forces the
  buffer **read-only**, and (c) **refuses to save** (`tw_do_save`) and **skips all
  auto-saves** (file-switch / New / poweroff). This makes it impossible to
  overwrite the original file with the partial view.
- **Keyboard:** insert / newline / yank / paste all bound-check
  `TW_MAX_LINES`/`TW_MAX_COLS`, so you cannot grow the buffer past the limit by
  typing (extra input is simply ignored at the boundary).
- **Rename (copy):** `tw_fs_copy_name` **refuses** a source larger than
  `TW_FILE_BUF_SIZE` (returns -2 → "Too big to rename") rather than copy a
  truncated file.

**To raise the limit:** bump `TW_MAX_LINES` / `TW_MAX_COLS` in `cmd_tw.h`. Each
increase grows the always-on BSS array by `Δ × 4 bytes/codepoint`, trading RAM/ROM
footprint directly — so raise only as far as needed.

---

## 5. High CPU while the editor is open (fixed)

Earlier the key-wait loop was a tight `while (!tstc()) schedule();` busy-spin,
pegging the CPU at 100 % while idle. It now polls with a 10 ms `udelay` between
checks (`tw_read_key`), cutting idle CPU ~100× with no perceptible typing
latency. If you still see high CPU, it is elsewhere (e.g. the display sync), not
the input loop.
