# Known Issues

## 1. FAT write corrupts the SD/eMMC directory (U-Boot dwmmc, NOT typewriter)

**Symptom.** After saving a file, `fatls mmc 0:1` shows garbage: filenames with
every character doubled and the extension mangled (`a.txt` → `AA`, `AA.TTT/`),
directories where files should be, and sizes of `4294967295` (`0xFFFFFFFF`).
Existing files (e.g. `extlinux.conf`, `vmlinuz`) can disappear. The file may be
unreadable from Linux.

**It is NOT the typewriter command.** This reproduces with U-Boot's own
`fatwrite`, no editor involved. On a freshly `mkfs.vfat`'d card:

```
=> mw.b 0x02000000 41 100
=> fatwrite mmc 0:1 0x02000000 ctrl.txt 0x100
=> fatls mmc 0:1
4294967295  AA
CCRR.TTT/
```

`ctrl.txt` comes back as `AA` / `CCRR.TTT/` — doubled bytes — with plain
`fatwrite`. The corruption is below U-Boot's FAT filesystem layer, in the block
transfer.

**Root cause (under investigation).** The board (RK3399 gru/kevin) uses the
DesignWare MMC controller (`CONFIG_MMC_DW` / `CONFIG_MMC_DW_ROCKCHIP`) with
IDMAC DMA. The every-other-byte doubling is characteristic of a block-transfer
defect — a DMA/cache-coherency or block-size/stride problem in the write path —
not the FAT software (which was audited: given a clean `"a.txt"`, the LFN slot
builder and 8.3 short-name generator produce correct entries; the directory
slot buffer is stack-allocated and zeroed). `CONFIG_BOUNCE_BUFFER=y` is set, so
that safety net is present. The exact defect (suspected block-size related) is
still being pinned down.

**Workarounds.**
- **Use typewriter read-only** (the default) or the no-arg scratch buffer for
  on-device use; do actual file writes from Linux. typewriter refuses to save
  unless invoked with a trailing `rw`, precisely so an accidental run can't
  trigger this.
- Forcing PIO/FIFO mode on the dw_mmc controller (device-tree `fifo-mode;` on
  the `&sdmmc` node, or the driver equivalent) bypasses the DMA path and is
  expected to write correctly (slower). Not applied here per project preference
  to leave U-Boot's `mmc/` and the board DTS untouched.

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
