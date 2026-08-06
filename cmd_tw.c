// SPDX-License-Identifier: GPL-2.0+
/*
 * cmd_tw.c - the `typewriter` command: a Nano-style, modeless, full-screen
 * editor on U-Boot's video framebuffer, with a built-in Wubi 86 IME.
 *
 * This file owns the U_BOOT_CMD entry point, the key reader (cooked ASCII plus
 * arrow/nav escape sequences), the editing primitives over the codepoint line
 * model, the Ctrl-key command dispatch, the Wubi composition state machine, and
 * the bottom-line prompts (^O write-out, ^W where-is, ^X exit-if-modified).
 *
 * Rendering, framebuffer setup, and the embedded glyph/table data live in the
 * sibling files (cmd_tw_video.c, cmd_tw_fs.c, ime_table.c, font_data.c,
 * wubi_embed.c).
 */
#include <command.h>
#include <stdio.h>
#include <vsprintf.h>
#include <u-boot/schedule.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <dm.h>
#include <cros_ec.h>
#include "cmd_tw.h"
#include "wubi_embed.h"

/* --- from cmd_tw_fs.c --- */
int tw_fs_probe(struct tw_fs *fs, const char *iftype, const char *dev_part,
		const char *fstype);
int tw_file_load(struct tw_state *s, const char *path);
int tw_file_save(struct tw_state *s);
int tw_list_files(struct tw_state *s);

/* --- from cmd_tw_video.c --- */
int  tw_video_init(struct tw_state *s);
void tw_render(struct tw_state *s);
int  tw_cp_cols(u32 cp);
int  tw_line_rows(struct tw_state *s, int fr);

static struct tw_state g_tw;

/* ----------------------------------------------------------- key input --- */
/* Read one logical keypress: cooked ASCII (0x00-0xFF) or an extended KEY_*
 * constant (> 0xFF) parsed from an ANSI arrow/nav escape sequence. Mirrors the
 * ved reference's parser; input comes from U-Boot's console (serial or USB kbd
 * routed to stdin). */
/*
 * Read the next byte of an escape sequence, waiting up to ~30 ms for it. The
 * bytes of an ESC-sequence / Meta chord arrive right after the ESC but not in
 * the same instant, so a single tstc() races the UART and drops the follow-up
 * (this was why M-w / M-0..9 "didn't work"). Returns -1 if nothing arrives
 * (i.e. it really was a bare ESC).
 */
static int tw_getch_timeout(void)
{
	int spins = 30;         /* ~30 ms at 1 ms/spin */

	while (!tstc() && spins-- > 0)
		udelay(1000);
	if (!tstc())
		return -1;
	return getchar();
}

static int tw_read_key(void)
{
	int c;

	/*
	 * Wait for a key. U-Boot's console has no blocking "sleep until a key"
	 * primitive, so we poll - but with a 10 ms delay between polls instead
	 * of a tight spin, so an idle editor doesn't peg the CPU at 100%. The
	 * delay is far below human typing latency, so input still feels instant.
	 */
	while (!tstc()) {
		schedule();          /* keep the watchdog fed */
		udelay(10000);       /* 10 ms: ~100 polls/sec, negligible CPU */
	}
	c = getchar();

	if (c != KEY_ESC)
		return c;

	/* ESC alone, or the start of a CSI/SS3 sequence (arrows/nav) or a Meta
	 * chord (M-x arrives as ESC then x). */
	c = tw_getch_timeout();
	if (c < 0)
		return KEY_ESC;         /* bare ESC */

	if (c == '[' || c == 'O') {
		c = tw_getch_timeout();
		switch (c) {
		case 'A': return KEY_ARROW_UP;
		case 'B': return KEY_ARROW_DOWN;
		case 'C': return KEY_ARROW_RIGHT;
		case 'D': return KEY_ARROW_LEFT;
		case 'H': return KEY_HOME_SEQ;
		case 'F': return KEY_END_SEQ;
		case '1': case '2': case '3':
		case '4': case '5': case '6': {
			int sub = c;
			int c4 = tw_getch_timeout();

			if (c4 == '~') {
				if (sub == '5') return KEY_PAGE_UP;
				if (sub == '6') return KEY_PAGE_DOWN;
				if (sub == '3') return KEY_DELETE;
				if (sub == '1') return KEY_HOME_SEQ;
				if (sub == '4') return KEY_END_SEQ;
			}
			return KEY_ESC;
		}
		default:
			return KEY_ESC;
		}
	}

	/* Meta (Alt) chord: ESC followed by a letter. NOTE: Meta keys don't work
	 * on the target hardware/terminal, so these are effectively dormant - kept
	 * wired (word motion / find) in case a future terminal delivers them. The
	 * M-0..9 file slots were removed; use the ^R picker to switch files. */
	switch (c) {
	case 'f': case 'F': return KEY_META_F;
	case 'b': case 'B': return KEY_META_B;
	case 'd': case 'D': return KEY_META_D;
	case 'w': case 'W': return KEY_META_W;
	default:            return KEY_ESC;
	}
}

static void tw_status(struct tw_state *s, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(s->status_msg, sizeof(s->status_msg), fmt, ap);
	va_end(ap);
}

static void tw_clear_status(struct tw_state *s)
{
	s->status_msg[0] = '\0';
}

/* ---------------------------------------------------------- scrolling ---- */
/*
 * Keep the cursor's logical line visible, counting SCREEN rows (soft-wrapped),
 * not logical lines - a wrapped long line can occupy several rows, so a
 * screenful holds fewer lines than text_rows. If the cursor line is above the
 * viewport, snap scroll_top to it; if the rows from scroll_top through the
 * cursor line exceed the screen, advance scroll_top until they fit.
 */
static void tw_scroll_adjust(struct tw_state *s)
{
	if (s->cur_row < s->scroll_top)
		s->scroll_top = s->cur_row;
	if (s->scroll_top < 0)
		s->scroll_top = 0;

	for (;;) {
		int rows = 0, fr;

		for (fr = s->scroll_top; fr <= s->cur_row; fr++)
			rows += tw_line_rows(s, fr);
		if (rows <= s->text_rows || s->scroll_top >= s->cur_row)
			break;
		s->scroll_top++;        /* cursor line overflows: scroll down */
	}
}

static void tw_clamp_col(struct tw_state *s)
{
	if (s->cur_col > s->line_len[s->cur_row])
		s->cur_col = s->line_len[s->cur_row];
	if (s->cur_col < 0)
		s->cur_col = 0;
}

/* ------------------------------------------------------ editing prims ---- */
static void tw_insert_cp(struct tw_state *s, u32 cp)
{
	int row = s->cur_row;
	int i;

	if (s->line_len[row] >= TW_MAX_COLS - 1)
		return;                         /* line full */
	for (i = s->line_len[row]; i > s->cur_col; i--)
		s->lines[row][i] = s->lines[row][i - 1];
	s->lines[row][s->cur_col] = cp;
	s->line_len[row]++;
	s->cur_col++;
	s->dirty = 1;
}

static void tw_insert_newline(struct tw_state *s)
{
	int row = s->cur_row;
	int rest, i;

	if (s->num_lines >= TW_MAX_LINES)
		return;

	/* shift lines below down by one */
	for (i = s->num_lines; i > row + 1; i--) {
		memcpy(s->lines[i], s->lines[i - 1],
		       sizeof(u32) * TW_MAX_COLS);
		s->line_len[i] = s->line_len[i - 1];
	}
	s->num_lines++;

	/* move the tail of the current line onto the new line */
	rest = s->line_len[row] - s->cur_col;
	for (i = 0; i < rest; i++)
		s->lines[row + 1][i] = s->lines[row][s->cur_col + i];
	s->line_len[row + 1] = rest;
	s->line_len[row] = s->cur_col;

	s->cur_row++;
	s->cur_col = 0;
	s->dirty = 1;
}

static void tw_backspace(struct tw_state *s)
{
	int row = s->cur_row;
	int i, prev_len;

	if (s->cur_col > 0) {
		for (i = s->cur_col - 1; i < s->line_len[row] - 1; i++)
			s->lines[row][i] = s->lines[row][i + 1];
		s->line_len[row]--;
		s->cur_col--;
		s->dirty = 1;
		return;
	}
	if (row == 0)
		return;                         /* top of file */

	/* merge this line onto the end of the previous one */
	prev_len = s->line_len[row - 1];
	for (i = 0; i < s->line_len[row] &&
		    prev_len + i < TW_MAX_COLS - 1; i++)
		s->lines[row - 1][prev_len + i] = s->lines[row][i];
	s->line_len[row - 1] = prev_len + i;

	for (i = row; i < s->num_lines - 1; i++) {
		memcpy(s->lines[i], s->lines[i + 1],
		       sizeof(u32) * TW_MAX_COLS);
		s->line_len[i] = s->line_len[i + 1];
	}
	s->num_lines--;
	s->cur_row--;
	s->cur_col = prev_len;
	s->dirty = 1;
}

static void tw_delete_cp(struct tw_state *s)
{
	int row = s->cur_row;
	int i;

	if (s->cur_col >= s->line_len[row]) {
		/* at EOL: pull the next line up (join), like Nano's Del */
		if (row + 1 < s->num_lines) {
			s->cur_row++;
			s->cur_col = 0;
			tw_backspace(s);
		}
		return;
	}
	for (i = s->cur_col; i < s->line_len[row] - 1; i++)
		s->lines[row][i] = s->lines[row][i + 1];
	s->line_len[row]--;
	s->dirty = 1;
}

/*
 * readline C-k: kill from the cursor to end of line into the kill buffer.
 * When there's nothing to the right of the cursor (empty line, or cursor
 * already at EOL), C-k instead deletes the line break - joining the next line
 * up - so repeated C-k on an empty line eats lines rather than stalling.
 */
static void tw_kill_to_eol(struct tw_state *s)
{
	int row = s->cur_row, i, n;

	n = s->line_len[row] - s->cur_col;
	if (n <= 0) {
		/* nothing to kill on this line: pull the next line up (like Del
		 * at EOL). Does nothing on the very last line. */
		if (row + 1 < s->num_lines)
			tw_delete_cp(s);
		return;
	}
	for (i = 0; i < n; i++)
		s->cut[i] = s->lines[row][s->cur_col + i];
	s->cut_len = n;
	s->cut_valid = 1;
	s->line_len[row] = s->cur_col;    /* truncate at the cursor */
	s->dirty = 1;
}

/* readline C-y: yank (insert) the kill buffer at the cursor. */
static void tw_yank(struct tw_state *s)
{
	int row = s->cur_row, i, n;

	if (!s->cut_valid || s->cut_len == 0)
		return;
	n = s->cut_len;
	if (s->line_len[row] + n > TW_MAX_COLS - 1)
		n = TW_MAX_COLS - 1 - s->line_len[row];
	if (n <= 0)
		return;
	/* make room at the cursor */
	for (i = s->line_len[row] - 1; i >= s->cur_col; i--)
		s->lines[row][i + n] = s->lines[row][i];
	for (i = 0; i < n; i++)
		s->lines[row][s->cur_col + i] = s->cut[i];
	s->line_len[row] += n;
	s->cur_col += n;
	s->dirty = 1;
}

/* A "word" character for M-f/M-b/M-d/C-w: ASCII alphanumeric, or any codepoint
 * >= 0x80 (treat CJK/other as word content). */
static int tw_is_word_cp(u32 cp)
{
	if (cp >= 0x80)
		return 1;
	if (cp >= '0' && cp <= '9')
		return 1;
	if (cp >= 'a' && cp <= 'z')
		return 1;
	if (cp >= 'A' && cp <= 'Z')
		return 1;
	return 0;
}

/* Column of the start of the previous word on the current line (readline M-b:
 * skip non-word chars left, then skip the word). */
static int tw_prev_word_col(struct tw_state *s)
{
	int row = s->cur_row, c = s->cur_col;

	while (c > 0 && !tw_is_word_cp(s->lines[row][c - 1]))
		c--;
	while (c > 0 && tw_is_word_cp(s->lines[row][c - 1]))
		c--;
	return c;
}

/* Column just past the end of the next word (readline M-f). */
static int tw_next_word_col(struct tw_state *s)
{
	int row = s->cur_row, c = s->cur_col, len = s->line_len[row];

	while (c < len && !tw_is_word_cp(s->lines[row][c]))
		c++;
	while (c < len && tw_is_word_cp(s->lines[row][c]))
		c++;
	return c;
}

/* Delete the [from,to) range on the current line (from < to), leaving the
 * cursor at `from`. Used by C-w (delete word back) and M-d (kill word fwd). */
static void tw_delete_range(struct tw_state *s, int from, int to)
{
	int row = s->cur_row, i, n = to - from;

	if (n <= 0)
		return;
	for (i = to; i < s->line_len[row]; i++)
		s->lines[row][i - n] = s->lines[row][i];
	s->line_len[row] -= n;
	s->cur_col = from;
	s->dirty = 1;
}

/* --------------------------------------------------------- navigation ---- */
static void tw_move_left(struct tw_state *s)
{
	if (s->cur_col > 0) {
		s->cur_col--;
	} else if (s->cur_row > 0) {
		s->cur_row--;
		s->cur_col = s->line_len[s->cur_row];
	}
}

static void tw_move_right(struct tw_state *s)
{
	if (s->cur_col < s->line_len[s->cur_row]) {
		s->cur_col++;
	} else if (s->cur_row < s->num_lines - 1) {
		s->cur_row++;
		s->cur_col = 0;
	}
}

static void tw_move_up(struct tw_state *s)
{
	if (s->cur_row > 0) {
		s->cur_row--;
		tw_clamp_col(s);
	}
}

static void tw_move_down(struct tw_state *s)
{
	if (s->cur_row < s->num_lines - 1) {
		s->cur_row++;
		tw_clamp_col(s);
	}
}

/* ---------------------------------------------------------- searching ---- */
/* Find `pat` (ASCII, from the search prompt) starting just after the cursor.
 * Matches against the codepoint buffer treating ASCII cp == byte. Returns 1 and
 * moves the cursor on a hit, 0 otherwise. */
static int tw_search(struct tw_state *s, const char *pat)
{
	int plen = strlen(pat);
	int row = s->cur_row, col = s->cur_col + 1;
	int scanned = 0;

	if (plen == 0)
		return 0;

	while (scanned <= s->num_lines) {
		int len = s->line_len[row];
		int i;

		for (i = col; i + plen <= len; i++) {
			int j;

			for (j = 0; j < plen; j++)
				if (s->lines[row][i + j] != (u32)(unsigned char)pat[j])
					break;
			if (j == plen) {
				s->cur_row = row;
				s->cur_col = i;
				tw_scroll_adjust(s);
				return 1;
			}
		}
		row = (row + 1) % s->num_lines;
		col = 0;
		scanned++;
	}
	return 0;
}

/* ---------------------------------------------------------------- IME ---- */
static void tw_ime_reset(struct tw_ime *im)
{
	im->code_len = 0;
	im->code[0] = '\0';
	im->ncand = 0;
	im->page = 0;
}

static void tw_ime_relookup(struct tw_ime *im)
{
	im->ncand = 0;
	im->page = 0;
	if (im->ready && im->code_len)
		im->ncand = ime_lookup(&im->tab, im->code, im->code_len,
				       im->cand, TW_MAX_CANDS);
}

/* Commit candidate `idx` into the document: decode its UTF-8 to codepoints and
 * insert each through the normal edit path. */
static void tw_ime_commit(struct tw_state *s, int idx)
{
	const ime_cand *c;
	int off;

	if (idx < 0 || idx >= s->ime.ncand)
		return;
	c = &s->ime.cand[idx];
	off = 0;
	while (off < c->word_len) {
		u32 cp;
		unsigned char b = (unsigned char)c->word[off];

		if (b < 0x80) {
			cp = b; off += 1;
		} else if ((b & 0xE0) == 0xC0 && off + 1 < c->word_len) {
			cp = ((u32)(b & 0x1F) << 6) | (c->word[off + 1] & 0x3F);
			off += 2;
		} else if ((b & 0xF0) == 0xE0 && off + 2 < c->word_len) {
			cp = ((u32)(b & 0x0F) << 12) |
			     ((c->word[off + 1] & 0x3F) << 6) |
			     (c->word[off + 2] & 0x3F);
			off += 3;
		} else {
			cp = 0xFFFD; off += 1;
		}
		tw_insert_cp(s, cp);
	}
	tw_ime_reset(&s->ime);
}

/*
 * Feed one key to the Wubi composer. Returns 1 if consumed by the IME, 0 if the
 * caller should handle it as a normal editor key. Mirrors wubi-fep's
 * handle_cn_byte(), but commits into the document instead of a pty.
 */
static int tw_ime_key(struct tw_state *s, int key)
{
	struct tw_ime *im = &s->ime;

	if (im->mode != TW_IME_WUBI)
		return 0;

	if (key >= 'a' && key <= 'z') {
		if (im->code_len < IME_CODE_LEN) {
			im->code[im->code_len++] = (char)key;
			im->code[im->code_len] = '\0';
			tw_ime_relookup(im);
		}
		return 1;
	}

	if (key >= '1' && key <= '9' && im->code_len > 0) {
		int abs = im->page + (key - '1');

		if (abs < im->ncand && (key - '1') < TW_PAGE)
			tw_ime_commit(s, abs);
		return 1;
	}

	if (key == ' ') {
		if (im->code_len > 0 && im->ncand > 0) {
			tw_ime_commit(s, im->page);
			return 1;
		}
		return 0;               /* nothing pending: literal space */
	}

	if (key == KEY_BACKSPACE || key == KEY_BS) {
		if (im->code_len > 0) {
			im->code[--im->code_len] = '\0';
			tw_ime_relookup(im);
			return 1;
		}
		return 0;               /* nothing pending: edit document */
	}

	if ((key == '=' || key == '.') && im->code_len > 0) {
		if (im->page + TW_PAGE < im->ncand)
			im->page += TW_PAGE;
		return 1;
	}
	if ((key == '-' || key == ',') && im->code_len > 0) {
		if (im->page >= TW_PAGE)
			im->page -= TW_PAGE;
		return 1;
	}

	if (key == KEY_ESC) {
		if (im->code_len > 0) {
			tw_ime_reset(im);
			return 1;       /* swallow: "discard candidates" */
		}
		return 0;
	}

	if (key == KEY_ENTER || key == KEY_LF) {
		if (im->code_len > 0) {
			/* emit the raw typed letters, swallow the newline */
			int i;

			for (i = 0; i < im->code_len; i++)
				tw_insert_cp(s, (u32)(unsigned char)im->code[i]);
			tw_ime_reset(im);
			return 1;
		}
		return 0;
	}

	/* other printable ASCII with a pending code: commit first, then let the
	 * punctuation fall through to the editor. */
	if (key >= 0x20 && key < 0x7f && im->code_len > 0) {
		if (im->ncand > 0)
			tw_ime_commit(s, im->page);
		else
			tw_ime_reset(im);
		return 0;               /* punctuation still handled below */
	}

	return 0;
}

/* -------------------------------------------------------------- prompts -- */
static void tw_do_save(struct tw_state *s)
{
	if (!s->writable) {
		/* Read-only: either the user passed 'ro', or this is the eMMC
		 * (mmc 0), which is hard-locked because its FAT writes corrupt
		 * the card on this board. Use the microSD (mmc 1) to save. */
		tw_status(s, "[ Read-only (eMMC is locked; save on mmc 1) ]");
		return;
	}
	if (!s->filename[0]) {
		tw_status(s, "[ No file name ]");
		return;
	}
	if (tw_file_save(s) == 0)
		/* Show name + byte count so a corrupt/short write is visible
		 * on the framebuffer status line (no serial needed to debug). */
		tw_status(s, "[ Wrote %d line%s, %d bytes -> %s ]",
			  s->num_lines, s->num_lines == 1 ? "" : "s",
			  s->last_write_bytes, s->filename);
	else
		tw_status(s, "[ Error writing %s ]", s->filename);
}

/*
 * Save (if writable & dirty), let the write settle, then power the board off
 * via the ChromeOS EC. gru/kevin has no PMIC - the EC controls power - so a
 * software power-off means asking the EC to hibernate. Returns only if it
 * fails (no EC, or the EC refused); on success the board is off.
 */
static void tw_poweroff(struct tw_state *s)
{
	/* 1. flush the document to disk if there's anything to save. */
	if (s->writable && s->dirty && s->filename[0]) {
		if (tw_file_save(s) != 0) {
			tw_status(s, "[ Save failed - NOT powering off ]");
			return;
		}
	}

	/* 2. U-Boot's FAT/block writes are synchronous (write-through), so once
	 * tw_file_save() returned the data is on the card. Small settle delay as
	 * belt-and-braces before we cut power. */
	mdelay(200);

#if CONFIG_IS_ENABLED(CROS_EC)
	/*
	 * 3. gru/kevin has no PMIC - the ChromeOS EC controls power. The EC's
	 * reboot/hibernate commands did NOT power the AP off from U-Boot (the EC
	 * has no 'pmu' feature and hibernate expects the OS AP-shutdown
	 * handshake). The real hardware power-off is BATTERY CUTOFF ("ship
	 * mode"): it tells the battery to stop supplying power, fully powering
	 * the device down. flags = 0 means cut off NOW (not "at shutdown").
	 *
	 * NOTE: after a battery cutoff the device typically powers back on only
	 * when the charger/AC is plugged in (not necessarily the power button).
	 */
	{
		struct udevice *ec;
		int r_h6;

		if (uclass_first_device_err(UCLASS_CROS_EC, &ec)) {
			tw_status(s, "[ No EC - power off with the button ]");
			return;
		}
		tw_render(s);         /* show the status before we go dark */

		/*
		 * ISOLATION TEST (on BATTERY): try ONLY hibernate (6), immediate.
		 * Battery cutoff works but wakes only on AC (ship mode). A true EC
		 * hibernate is normally POWER-BUTTON-wakeable, which is what we
		 * want. Earlier hibernate "rebooted" - but that was on AC, and the
		 * EC behaves differently on battery, so re-test on battery:
		 *   - powers off, wakes on power button -> this is the one we want;
		 *   - reboots -> hibernate resets on this firmware;
		 *   - nothing (status shows code, after ~3s hello-poll) -> inert.
		 */
		r_h6 = cros_ec_reboot(ec, EC_REBOOT_HIBERNATE, 0);

		tw_status(s, "[ hib6 returned %d - no off; hold power btn ]", r_h6);
		return;
	}
#else
	tw_status(s, "[ Saved. No EC power-off on this board - use button ]");
#endif
}

/*
 * Switch the buffer to `name` on the current device (used by ^R open and the
 * M-0..M-9 slot keys). AlphaSmart-style: if the current buffer is writable and
 * modified, auto-save it first, then load the new file. Cursor/scroll reset.
 * The device (s->fs) is unchanged - only the filename changes.
 */
static void tw_switch_file(struct tw_state *s, const char *name)
{
	int saved = 0;

	if (!name || !name[0])
		return;

	/* auto-save the outgoing buffer if we can and it changed */
	if (s->writable && s->dirty && s->filename[0]) {
		if (tw_file_save(s) != 0) {
			tw_status(s, "[ Save of %s failed - not switching ]",
				  s->filename);
			return;                 /* don't lose edits on a bad save */
		}
		saved = 1;
	}

	strncpy(s->filename, name, TW_MAX_FILENAME - 1);
	s->filename[TW_MAX_FILENAME - 1] = '\0';

	if (tw_file_load(s, s->filename) < 0) {
		tw_status(s, "[ Cannot open %s ]", s->filename);
		return;
	}
	if (s->num_lines == 0) {
		s->num_lines = 1;
		s->line_len[0] = 0;
	}
	s->cur_row = 0;
	s->cur_col = 0;
	s->scroll_top = 0;
	s->dirty = 0;
	s->first_paint = 1;             /* full repaint for the new content */

	tw_status(s, "[ %s%s - %d line%s ]", saved ? "Saved & opened " : "Opened ",
		  s->filename, s->num_lines, s->num_lines == 1 ? "" : "s");
}

/* ^R: list files on the current device and enter the arrow-select picker. If
 * the directory is empty or unreadable, fall back to a typed "File to open:". */
static void tw_picker_open(struct tw_state *s)
{
	if (tw_list_files(s) > 0) {
		s->prompt = TW_PROMPT_PICK;
		/* pre-select the current file if it's in the list */
		for (int i = 0; i < s->pick_count; i++)
			if (!strcmp(s->pick_name[i], s->filename)) {
				s->pick_sel = i;
				break;
			}
	} else {
		s->prompt = TW_PROMPT_OPEN;     /* nothing to pick: type a name */
		s->prompt_ans[0] = '\0';
		s->prompt_len = 0;
		tw_status(s, "[ No files - type a name ]");
	}
}

static void tw_prompt_start(struct tw_state *s, int which)
{
	s->prompt = which;
	s->prompt_len = 0;
	s->prompt_ans[0] = '\0';
	if (which == TW_PROMPT_SAVE && s->filename[0]) {
		strncpy(s->prompt_ans, s->filename, TW_CMD_BUF - 1);
		s->prompt_ans[TW_CMD_BUF - 1] = '\0';
		s->prompt_len = strlen(s->prompt_ans);
	}
}

/* Handle a key while a bottom-line prompt owns input. */
static void tw_prompt_key(struct tw_state *s, int key)
{
	/* ^R file picker: arrow / ^P / ^N to move, Enter opens, Esc cancels. */
	if (s->prompt == TW_PROMPT_PICK) {
		switch (key) {
		case KEY_ARROW_UP:
		case KEY_CTRL_P:
			if (s->pick_sel > 0)
				s->pick_sel--;
			break;
		case KEY_ARROW_DOWN:
		case KEY_CTRL_N:
			if (s->pick_sel < s->pick_count - 1)
				s->pick_sel++;
			break;
		case KEY_PAGE_UP:
			s->pick_sel -= 8;
			if (s->pick_sel < 0)
				s->pick_sel = 0;
			break;
		case KEY_PAGE_DOWN:
			s->pick_sel += 8;
			if (s->pick_sel > s->pick_count - 1)
				s->pick_sel = s->pick_count - 1;
			break;
		case KEY_ENTER:
		case KEY_LF: {
			char name[TW_PICK_NAMELEN];

			strncpy(name, s->pick_name[s->pick_sel], sizeof(name) - 1);
			name[sizeof(name) - 1] = '\0';
			s->prompt = TW_PROMPT_NONE;
			tw_switch_file(s, name);
			return;
		}
		case KEY_ESC:
		case KEY_CTRL_X:
			s->prompt = TW_PROMPT_NONE;
			tw_status(s, "[ Cancelled ]");
			break;
		}
		return;
	}

	if (s->prompt == TW_PROMPT_EXIT) {
		if (key == 'y' || key == 'Y') {
			tw_do_save(s);
			if (!s->dirty)
				s->quit = 1;
			s->prompt = TW_PROMPT_NONE;
		} else if (key == 'n' || key == 'N') {
			s->quit = 1;
			s->prompt = TW_PROMPT_NONE;
		} else if (key == 'c' || key == 'C' || key == KEY_ESC) {
			s->prompt = TW_PROMPT_NONE;
		}
		return;
	}

	if (s->prompt == TW_PROMPT_POWEROFF) {
		if (key == 'y' || key == 'Y') {
			s->prompt = TW_PROMPT_NONE;
			tw_poweroff(s);        /* saves, syncs, powers off (or reports) */
		} else if (key == 'n' || key == 'N' || key == KEY_ESC) {
			s->prompt = TW_PROMPT_NONE;
			tw_status(s, "[ Cancelled ]");
		}
		return;
	}

	if (key == KEY_ENTER || key == KEY_LF) {
		int which = s->prompt;

		s->prompt = TW_PROMPT_NONE;
		if (which == TW_PROMPT_SAVE) {
			if (s->prompt_ans[0]) {
				strncpy(s->filename, s->prompt_ans,
					TW_MAX_FILENAME - 1);
				s->filename[TW_MAX_FILENAME - 1] = '\0';
			}
			tw_do_save(s);
		} else if (which == TW_PROMPT_SEARCH) {
			strncpy(s->search_last, s->prompt_ans, TW_CMD_BUF - 1);
			s->search_last[TW_CMD_BUF - 1] = '\0';
			if (!tw_search(s, s->search_last))
				tw_status(s, "[ \"%s\" not found ]",
					  s->search_last);
		} else if (which == TW_PROMPT_OPEN) {
			if (s->prompt_ans[0])
				tw_switch_file(s, s->prompt_ans);
		}
		return;
	}
	if (key == KEY_ESC) {
		s->prompt = TW_PROMPT_NONE;
		tw_status(s, "[ Cancelled ]");
		return;
	}
	if ((key == KEY_BACKSPACE || key == KEY_BS) && s->prompt_len > 0) {
		s->prompt_ans[--s->prompt_len] = '\0';
		return;
	}
	if (key >= 0x20 && key < 0x7f && s->prompt_len < TW_CMD_BUF - 1) {
		s->prompt_ans[s->prompt_len++] = (char)key;
		s->prompt_ans[s->prompt_len] = '\0';
	}
}

/*
 * Set the renderer's dirty flags by diffing state around a key dispatch. This
 * keeps the fast incremental renderer correct without threading dirty-marking
 * through every editing primitive: if a change affects the whole text area
 * (line count changed, or the buffer scrolled) we repaint it all; the title
 * bar repaints when the filename/modified/mode indicator changes; the bottom
 * bar repaints when the IME composition, prompt, or status message changes.
 * The current + previous cursor rows are always repainted by tw_render itself.
 */
struct tw_snap {
	int num_lines, scroll_top, dirty, has_name;
	int ime_mode, code_len, page, ncand;
	int prompt, has_status, pick_sel;
	int cur_row, cur_len;   /* to detect an in-line content edit */
};

static void tw_snapshot(struct tw_state *s, struct tw_snap *o)
{
	o->num_lines = s->num_lines;
	o->scroll_top = s->scroll_top;
	o->dirty = s->dirty;
	o->has_name = s->filename[0] != '\0';
	o->ime_mode = s->ime.mode;
	o->code_len = s->ime.code_len;
	o->page = s->ime.page;
	o->ncand = s->ime.ncand;
	o->prompt = s->prompt;
	o->has_status = s->status_msg[0] != '\0';
	o->pick_sel = s->pick_sel;
	o->cur_row = s->cur_row;
	o->cur_len = s->line_len[s->cur_row];
}

static void tw_mark_dirty(struct tw_state *s, const struct tw_snap *o)
{
	/* Repaint the whole text area on any content change: line count or
	 * scroll changed (join/split/scroll), or - while staying on the same
	 * line - that line's length changed (insert/delete/tab/kill/yank). A
	 * pure cursor move (incl. vertical, which changes cur_row but no
	 * content) leaves these equal, so the renderer only moves the caret and
	 * doesn't flicker. */
	if (s->num_lines != o->num_lines || s->scroll_top != o->scroll_top ||
	    (s->cur_row == o->cur_row &&
	     s->line_len[s->cur_row] != o->cur_len))
		s->dirty_all = 1;
	if (s->dirty != o->dirty ||
	    (s->filename[0] != '\0') != o->has_name ||
	    s->ime.mode != o->ime_mode)
		s->dirty_title = 1;
	if (s->ime.mode != o->ime_mode || s->ime.code_len != o->code_len ||
	    s->ime.page != o->page || s->ime.ncand != o->ncand ||
	    s->prompt != o->prompt ||
	    (s->status_msg[0] != '\0') != o->has_status ||
	    s->status_msg[0] != '\0' ||
	    s->pick_sel != o->pick_sel)         /* picker selection moved */
		s->dirty_bar = 1;
}

/* --------------------------------------------------------- key dispatch -- */
static void tw_handle_key(struct tw_state *s, int key)
{
	struct tw_snap snap;

	tw_snapshot(s, &snap);

	if (s->prompt != TW_PROMPT_NONE) {
		tw_prompt_key(s, key);
		tw_mark_dirty(s, &snap);
		return;
	}

	tw_clear_status(s);

	/* Ctrl-Space toggles the IME in any state. */
	if (key == KEY_CTRL_SPACE) {
		s->ime.mode = (s->ime.mode == TW_IME_WUBI && s->ime.ready)
			      ? TW_IME_OFF
			      : (s->ime.ready ? TW_IME_WUBI : TW_IME_OFF);
		tw_ime_reset(&s->ime);
		tw_mark_dirty(s, &snap);
		return;
	}

	/* Let the Wubi composer consume the key first (a-z, digits, space,
	 * paging, backspace-while-composing, ...). */
	if (tw_ime_key(s, key)) {
		tw_mark_dirty(s, &snap);
		return;
	}

	switch (key) {
	case KEY_CTRL_O:
		tw_prompt_start(s, TW_PROMPT_SAVE);
		break;
	case KEY_CTRL_R:            /* open a file: arrow-select picker */
		tw_picker_open(s);
		break;
	case KEY_CTRL_Q:            /* power off (save + sync + EC hibernate) */
		tw_prompt_start(s, TW_PROMPT_POWEROFF);
		break;
	case KEY_CTRL_X:
		/* Only offer "save modified buffer?" when saving is possible.
		 * Read-only: quit immediately (edits can't be written anyway). */
		if (s->dirty && s->writable)
			tw_prompt_start(s, TW_PROMPT_EXIT);
		else
			s->quit = 1;
		break;
	/* readline kill / yank */
	case KEY_CTRL_K:
		tw_kill_to_eol(s);
		break;
	case KEY_CTRL_Y:
		tw_yank(s);
		break;
	case KEY_CTRL_W: {          /* delete word backward */
		int from = tw_prev_word_col(s);

		tw_delete_range(s, from, s->cur_col);
		break;
	}
	case KEY_META_D: {          /* kill word forward */
		int to = tw_next_word_col(s);

		tw_delete_range(s, s->cur_col, to);
		break;
	}
	case KEY_META_W:            /* search (moved off C-w) */
		tw_prompt_start(s, TW_PROMPT_SEARCH);
		break;

	/* readline cursor motion */
	case KEY_CTRL_A:
	case KEY_HOME_SEQ:
		s->cur_col = 0;
		break;
	case KEY_CTRL_E:
	case KEY_END_SEQ:
		s->cur_col = s->line_len[s->cur_row];
		break;
	case KEY_CTRL_B:
	case KEY_ARROW_LEFT:  tw_move_left(s);  break;
	case KEY_CTRL_F:
	case KEY_ARROW_RIGHT: tw_move_right(s); break;
	case KEY_CTRL_P:
	case KEY_ARROW_UP:    tw_move_up(s);    break;
	case KEY_CTRL_N:
	case KEY_ARROW_DOWN:  tw_move_down(s);  break;
	case KEY_META_B:      s->cur_col = tw_prev_word_col(s); break;
	case KEY_META_F:      s->cur_col = tw_next_word_col(s); break;

	case KEY_PAGE_UP: {
		int i;

		for (i = 0; i < s->text_rows; i++)
			tw_move_up(s);
		break;
	}
	case KEY_PAGE_DOWN: {
		int i;

		for (i = 0; i < s->text_rows; i++)
			tw_move_down(s);
		break;
	}

	case KEY_ENTER:
	case KEY_LF:
		tw_insert_newline(s);
		break;
	case KEY_BACKSPACE:
	case KEY_BS:
		tw_backspace(s);
		break;
	case KEY_CTRL_D:
	case KEY_DELETE:
		tw_delete_cp(s);
		break;

	case KEY_TAB: {
		/* Expand Tab to spaces up to the next tab stop (every TW_TABW
		 * columns). Spaces render cleanly in the fixed font and keep the
		 * column model simple; a literal tab codepoint would not. */
		int n = TW_TABW - (s->cur_col % TW_TABW);

		while (n-- > 0)
			tw_insert_cp(s, (u32)' ');
		break;
	}

	default:
		/* printable ASCII typed literally (English mode, or punctuation
		 * that fell through the IME after a commit) */
		if (key >= 0x20 && key < 0x7f)
			tw_insert_cp(s, (u32)key);
		break;
	}

	tw_clamp_col(s);
	tw_scroll_adjust(s);
	tw_mark_dirty(s, &snap);
}

/* --------------------------------------------------------------- run ----- */
static void tw_bind_ime(struct tw_state *s)
{
	s->ime.ready = 0;
	if (ime_table_open_mem(&s->ime.tab, wubi_tab, wubi_tab_len,
			       IME_SCHEME_WUBI) == 0)
		s->ime.ready = 1;
	/* Start in English; Ctrl-Space toggles to Wubi when the table bound. */
	s->ime.mode = TW_IME_OFF;
	tw_ime_reset(&s->ime);
}

static void tw_print_usage(struct cmd_tbl *cmdtp)
{
	/* Print the same text as `help typewriter`, then return to the shell. */
	if (cmdtp)
		cmd_usage(cmdtp);
}

/*
 * Is this the eMMC (mmc device 0)? Its FAT write path corrupts the card on this
 * board, so it is hard-locked read-only regardless of the requested mode.
 * `devpart` is "<dev>[:<part>]"; we match device index 0 exactly (so "0", "0:1"
 * lock, but "10:1" does not).
 */
static int tw_is_emmc(const char *iftype, const char *devpart)
{
	return !strcmp(iftype, "mmc") &&
	       devpart[0] == '0' &&
	       (devpart[1] == ':' || devpart[1] == '\0');
}

static int do_typewriter(struct cmd_tbl *cmdtp, int flag,
			 int argc, char *const argv[])
{
	struct tw_state *s = &g_tw;
	const char *fstype = NULL;
	/* Defaults for a bare `typewriter`: the microSD, which has a working
	 * FAT write path on this board (the eMMC's is broken - see
	 * KNOWN_ISSUES). The microSD is writable by default. */
	const char *iftype  = "mmc";
	const char *devpart = "1:1";
	const char *fname   = "a.txt";
	int want_ro = 0;
	int i;

	/* `typewriter -h` / `--help`: print usage to the console and return
	 * without launching the editor (which would take over the framebuffer). */
	if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		tw_print_usage(cmdtp);
		return CMD_RET_SUCCESS;
	}

	/* Argument shapes:
	 *   typewriter                        -> default: mmc 1:1 a.txt (RW)
	 *   typewriter <if> <dev:part> <file> [fstype] [ro]  -> explicit
	 * Anything with 1-3 args (an incomplete file spec) is a usage error. */
	if (argc >= 4) {
		iftype  = argv[1];
		devpart = argv[2];
		fname   = argv[3];
		/* Trailing optional args, in any order: the literal token "ro"
		 * forces read-only; any other trailing token is the fs type.
		 * (Devices are writable by default; only the eMMC below is
		 * hard-locked read-only regardless.) */
		for (i = 4; i < argc; i++) {
			if (!strcmp(argv[i], "ro"))
				want_ro = 1;
			else
				fstype = argv[i];
		}
	} else if (argc != 1) {
		return CMD_RET_USAGE;
	}

	memset(s, 0, sizeof(*s));

	/*
	 * Writable unless the user forced `ro`, EXCEPT the eMMC (mmc 0:*) is
	 * ALWAYS read-only: its FAT write path corrupts the card on this board
	 * (see KNOWN_ISSUES), so we hard-lock it no matter what was requested.
	 */
	s->writable = !want_ro;
	if (tw_is_emmc(iftype, devpart))
		s->writable = 0;

	if (tw_video_init(s) != 0) {
		printf("typewriter: no usable video framebuffer.\n");
		printf("  Enable CONFIG_VIDEO and a display driver on this board.\n");
		return CMD_RET_FAILURE;
	}

	if (tw_fs_probe(&s->fs, iftype, devpart, fstype) < 0) {
		printf("typewriter: cannot access %s %s\n", iftype, devpart);
		return CMD_RET_FAILURE;
	}

	strncpy(s->filename, fname, TW_MAX_FILENAME - 1);
	s->filename[TW_MAX_FILENAME - 1] = '\0';

	if (tw_file_load(s, s->filename) < 0) {
		printf("typewriter: cannot load %s\n", s->filename);
		return CMD_RET_FAILURE;
	}
	if (s->num_lines == 0) {
		s->num_lines = 1;
		s->line_len[0] = 0;
	}

	tw_bind_ime(s);
	tw_status(s, "[ Read %d line%s%s ]", s->num_lines,
		  s->num_lines == 1 ? "" : "s",
		  s->writable ? "" : " - read-only");

	while (!s->quit) {
		tw_render(s);
		tw_handle_key(s, tw_read_key());
	}

	return CMD_RET_SUCCESS;
}

U_BOOT_CMD(
	typewriter, 6, 0, do_typewriter,
	"Nano-like editor with Wubi Chinese input (framebuffer)",
	"                        - default: open mmc 1:1 a.txt (writable)\n"
	"typewriter -h                        - show this help\n"
	"typewriter <iftype> <dev:part> <filename> [fstype] [ro]\n"
	"  iftype  : mmc usb virtio nvme sata\n"
	"  dev:part: 0:1  1:2  ...\n"
	"  filename: full path on the filesystem\n"
	"  fstype  : fat ext4  (optional, auto-detect)\n"
	"  ro      : open read-only (writable by default)\n"
	"  NOTE: mmc 0 (eMMC) is ALWAYS read-only - its FAT writes\n"
	"        corrupt the card; save on mmc 1 (microSD) instead.\n"
	"Keys (readline-style):\n"
	"  ^O write  ^R open (pick from list)  ^X exit  ^Q power off  ^G help\n"
	"  ^B/^F char  ^P/^N line  ^A/^E bol/eol  arrows/PgUp/PgDn move\n"
	"  ^D del  ^W del-word-back  ^K kill-eol  ^Y yank\n"
	"  ^Space toggle Wubi/English; in Wubi: a-z code,\n"
	"  1-9/Space commit, =/- page, Esc cancel"
);
