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

## 3. High CPU while the editor is open (fixed)

Earlier the key-wait loop was a tight `while (!tstc()) schedule();` busy-spin,
pegging the CPU at 100 % while idle. It now polls with a 10 ms `udelay` between
checks (`tw_read_key`), cutting idle CPU ~100× with no perceptible typing
latency. If you still see high CPU, it is elsewhere (e.g. the display sync), not
the input loop.
