# Oversized files — limits, the danger, and the guard

How big a file can `typewriter` open, what happens when a file is too big, and
why the guard is shaped the way it is.

## The limits (smallest wins)

There are several independent caps. The **editor buffer** is the binding one.

| Cap | Value | Where |
|-----|-------|-------|
| **Editor buffer (binding)** | 2048 lines × 511 usable codepoints/line ≈ **1.05 M codepoints** | `TW_MAX_LINES` / `TW_MAX_COLS` in `cmd_tw.h` |
| In bytes on disk | **~1 MB ASCII** (1 B/cp) … **~3 MB Chinese** (3 B/cp) | UTF-8 width of the content |
| Load scratch / clamp | `TW_FILE_BUF_SIZE` ≈ **4.2 MB** | `cmd_tw_fs.c` (`fs_size` clamp + `malloc`) |
| Static memory (BSS) | **~4 MiB** always reserved | `u32 lines[2048][512]` in `struct tw_state` |
| FAT32 file size | 4 GiB − 1 (irrelevant) | filesystem |

The buffer is a **fixed 2-D codepoint array** (`u32 lines[2048][512]`) living in
**BSS** as `static struct tw_state g_tw` — so the ~4 MiB is reserved whether the
file is empty or full; it is not stack or heap. The load scratch buffer (~4.2 MB
worst case) is `malloc`'d exactly to the file size and freed after decode; it is
larger than the buffer can hold, so the buffer cap always bites first — a huge
file never over-allocates.

## The danger this guards against

The real hazard is **not** running out of memory (BSS/heap are bounded and DRAM
is 4 GB). It is **silent data loss on a round-trip**:

1. Open a file bigger than the buffer.
2. It loads *partially* — extra lines past 2048, or the tail of any line past
   511 cols, or everything past the ~4 MB byte clamp, are dropped.
3. You edit (or just switch files, triggering an auto-save), and the editor
   writes the buffer back **under the same name**.
4. The original file on disk is now **replaced by the truncated version** —
   the dropped content is gone.

Before the guard, steps 2–4 were completely silent. That is the classic "editor
ate my file" catastrophe, and for a *typewriter* (a writing tool people trust
with their only copy) it is the worst possible failure.

## The guard (what actually happens now)

Principle: **a file the editor cannot fully hold must never be writable.** If we
can't represent the whole file, we must not let a save overwrite it with less.

### Load side — detect and flag
`tw_file_load` clears `s->load_truncated` at the top, then sets it if any data
is dropped:
- byte clamp: `fsize > TW_FILE_BUF_SIZE` → read only the first ~4 MB, flag set;
- `tw_load_cp`: out of lines (`num_lines == TW_MAX_LINES`) or a line hits
  `TW_MAX_COLS-1` → flag set, rest dropped.

### Consequence — read-only + no save
When `s->load_truncated` is set:
- **startup** forces `s->writable = 0` (belt-and-suspenders);
- **`tw_do_save` refuses** with `[ NOT saved: file was truncated on load - would
  lose data ]`;
- **every auto-save path** (file-switch in `tw_switch_file`, `New`, and the
  poweroff/boot save) has `&& !s->load_truncated` added to its condition, so a
  partial buffer is never written implicitly;
- the status line shows `TRUNCATED ... READ-ONLY` on open, so the user knows the
  view is partial.

There is deliberately **no override** — the buffer *cannot* hold the whole file,
so any save is inherently lossy. The correct fix for the user is to raise the
buffer size (below) and reopen, or split the file elsewhere.

### Keyboard side — already safe, unchanged
Insert / newline / yank / paste already bound-check `TW_MAX_LINES` and
`TW_MAX_COLS` (`tw_insert_cp`, `tw_insert_newline`, `tw_yank`, paste). So you
cannot grow the buffer past the cap by typing — input at the boundary is simply
ignored. No guard needed here (this is why the ask was "though not easily
exceeded by keyboard").

### Rename side — refuse rather than truncate
`tw_fs_rename_name` → `tw_fs_copy_name` does a raw byte copy (this U-Boot has no
`fs_rename`). A copy that clamped at `TW_FILE_BUF_SIZE` would silently truncate a
large file during a *rename*, which must be lossless. So `tw_fs_copy_name`
**returns -2** for a source larger than the cap, and the picker reports
`[ Too big to rename (> 4 MB) ]` instead of copying a truncated file.

## Design rationale (why these choices)

- **Fail safe, not silent.** Every truncation now has a visible consequence
  (read-only + message) instead of quiet data loss. The single most important
  property is that the *original file is never overwritten with less than it
  contained*.
- **Read-only is the right lock**, not a scary confirm dialog. A writer who
  opened a too-big file can still *read* all of what fitted; they just can't
  clobber the source. No modal to dismiss, no way to fat-finger past it.
- **Keep the fixed-array buffer.** A growable/rope buffer would raise the limit
  but adds allocation complexity and heap pressure right next to U-Boot's FAT
  writer — which, on this board, is exactly the fragility that corrupts the card
  (see `KNOWN_ISSUES.md` §1). A fixed BSS array is predictable and keeps the heap
  clean for the FS layer. The cap is a deliberate trade, not an oversight.
- **~1–3 MB is plenty for the use case.** 2048 lines is roughly a 150–350 page
  manuscript; a typewriter is for prose, not logs or dumps. The guard exists for
  the rare accident (opening a big file by mistake), not the normal path.

## Raising the limit

Bump `TW_MAX_LINES` and/or `TW_MAX_COLS` in `cmd_tw.h`. Cost is direct and
always-on:

```
BSS growth = Δlines × cols × 4 bytes   (the u32 lines[][] array)
           + Δlines × (4 + 1) bytes    (line_len[] + dirty_row[])
```

e.g. doubling `TW_MAX_LINES` to 4096 adds ~4 MiB of permanently-reserved BSS to
the U-Boot image. The SPI ROM is 8 MiB with fixed binman offsets, so there is not
unlimited headroom — raise only as far as the target actually needs, and rebuild.
`TW_FILE_BUF_SIZE` is derived from these, so the load clamp scales automatically.

## Related

- `KNOWN_ISSUES.md` §4 — the same limits, summarized.
- `KNOWN_ISSUES.md` §1 — the eMMC FAT-write corruption (why heap hygiene around
  `fs_write` matters, and why saves go to the microSD).
