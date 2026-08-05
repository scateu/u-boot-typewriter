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
#include <linux/string.h>
#include "cmd_tw.h"
#include "wubi_embed.h"

/* --- from cmd_tw_fs.c --- */
int tw_fs_probe(struct tw_fs *fs, const char *iftype, const char *dev_part,
		const char *fstype);
int tw_file_load(struct tw_state *s, const char *path);
int tw_file_save(struct tw_state *s);

/* --- from cmd_tw_video.c --- */
int  tw_video_init(struct tw_state *s);
void tw_render(struct tw_state *s);
int  tw_cp_cols(u32 cp);

static struct tw_state g_tw;

/* ----------------------------------------------------------- key input --- */
/* Read one logical keypress: cooked ASCII (0x00-0xFF) or an extended KEY_*
 * constant (> 0xFF) parsed from an ANSI arrow/nav escape sequence. Mirrors the
 * ved reference's parser; input comes from U-Boot's console (serial or USB kbd
 * routed to stdin). */
static int tw_read_key(void)
{
	int c;

	while (!tstc())
		schedule();     /* keep the watchdog fed while blocking */
	c = getchar();

	if (c != KEY_ESC)
		return c;

	if (!tstc())
		return KEY_ESC;
	c = getchar();
	if (c != '[' && c != 'O')
		return KEY_ESC;

	c = getchar();
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
		int c4 = getchar();

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
static void tw_scroll_adjust(struct tw_state *s)
{
	if (s->cur_row < s->scroll_top)
		s->scroll_top = s->cur_row;
	if (s->cur_row >= s->scroll_top + s->text_rows)
		s->scroll_top = s->cur_row - s->text_rows + 1;
	if (s->scroll_top < 0)
		s->scroll_top = 0;
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

/* ^K cut current line into the cut buffer; ^U paste it back above cursor. */
static void tw_cut_line(struct tw_state *s)
{
	int row = s->cur_row, i;

	s->cut_len = s->line_len[row];
	for (i = 0; i < s->cut_len; i++)
		s->cut[i] = s->lines[row][i];
	s->cut_valid = 1;

	if (s->num_lines == 1) {
		s->line_len[0] = 0;             /* clear the only line */
	} else {
		for (i = row; i < s->num_lines - 1; i++) {
			memcpy(s->lines[i], s->lines[i + 1],
			       sizeof(u32) * TW_MAX_COLS);
			s->line_len[i] = s->line_len[i + 1];
		}
		s->num_lines--;
		if (s->cur_row >= s->num_lines)
			s->cur_row = s->num_lines - 1;
	}
	s->cur_col = 0;
	s->dirty = 1;
}

static void tw_paste_line(struct tw_state *s)
{
	int i;

	if (!s->cut_valid || s->num_lines >= TW_MAX_LINES)
		return;
	for (i = s->num_lines; i > s->cur_row; i--) {
		memcpy(s->lines[i], s->lines[i - 1], sizeof(u32) * TW_MAX_COLS);
		s->line_len[i] = s->line_len[i - 1];
	}
	for (i = 0; i < s->cut_len; i++)
		s->lines[s->cur_row][i] = s->cut[i];
	s->line_len[s->cur_row] = s->cut_len;
	s->num_lines++;
	s->cur_col = 0;
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
		/* Read-only guard: saving is refused unless the user explicitly
		 * opened the file read-write (trailing 'rw' arg). This prevents
		 * an accidental run from ever writing to the card. */
		tw_status(s, "[ Read-only - re-run with a trailing 'rw' to save ]");
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
	int prompt, has_status;
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
}

static void tw_mark_dirty(struct tw_state *s, const struct tw_snap *o)
{
	if (s->num_lines != o->num_lines || s->scroll_top != o->scroll_top)
		s->dirty_all = 1;
	if (s->dirty != o->dirty ||
	    (s->filename[0] != '\0') != o->has_name ||
	    s->ime.mode != o->ime_mode)
		s->dirty_title = 1;
	if (s->ime.mode != o->ime_mode || s->ime.code_len != o->code_len ||
	    s->ime.page != o->page || s->ime.ncand != o->ncand ||
	    s->prompt != o->prompt ||
	    (s->status_msg[0] != '\0') != o->has_status ||
	    s->status_msg[0] != '\0')
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
	case KEY_CTRL_X:
		/* Only offer "save modified buffer?" when saving is possible.
		 * Read-only: quit immediately (edits can't be written anyway). */
		if (s->dirty && s->writable)
			tw_prompt_start(s, TW_PROMPT_EXIT);
		else
			s->quit = 1;
		break;
	case KEY_CTRL_W:
		tw_prompt_start(s, TW_PROMPT_SEARCH);
		break;
	case KEY_CTRL_K:
		tw_cut_line(s);
		break;
	case KEY_CTRL_U:
		tw_paste_line(s);
		break;
	case KEY_CTRL_A:
	case KEY_HOME_SEQ:
		s->cur_col = 0;
		break;
	case KEY_CTRL_E:
	case KEY_END_SEQ:
		s->cur_col = s->line_len[s->cur_row];
		break;

	case KEY_ARROW_LEFT:  tw_move_left(s);  break;
	case KEY_ARROW_RIGHT: tw_move_right(s); break;
	case KEY_ARROW_UP:    tw_move_up(s);    break;
	case KEY_ARROW_DOWN:  tw_move_down(s);  break;

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
	case KEY_DELETE:
		tw_delete_cp(s);
		break;

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
	/* Start in Wubi if the table bound, matching the request. */
	s->ime.mode = s->ime.ready ? TW_IME_WUBI : TW_IME_OFF;
	tw_ime_reset(&s->ime);
}

static void tw_print_usage(struct cmd_tbl *cmdtp)
{
	/* Print the same text as `help typewriter`, then return to the shell. */
	if (cmdtp)
		cmd_usage(cmdtp);
}

static int do_typewriter(struct cmd_tbl *cmdtp, int flag,
			 int argc, char *const argv[])
{
	struct tw_state *s = &g_tw;
	const char *fstype = NULL;
	int writable = 0;
	int scratch;
	int i;

	/* `typewriter -h` / `--help`: print usage to the console and return
	 * without launching the editor (which would take over the framebuffer). */
	if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
		tw_print_usage(cmdtp);
		return CMD_RET_SUCCESS;
	}

	/* Argument shapes:
	 *   typewriter                        -> empty scratch buffer (RO)
	 *   typewriter <if> <dev:part> <file> [fstype] [rw]  -> file-backed
	 * Anything with 1-3 args (an incomplete file spec) is a usage error. */
	if (argc == 1) {
		scratch = 1;
	} else if (argc >= 4) {
		scratch = 0;
	} else {
		return CMD_RET_USAGE;
	}

	memset(s, 0, sizeof(*s));

	if (!scratch) {
		/* Trailing optional args, in any order: the literal token "rw"
		 * enables saving (read-only by default, so an accidental run
		 * can't write); any other trailing token is the fs type. */
		for (i = 4; i < argc; i++) {
			if (!strcmp(argv[i], "rw"))
				writable = 1;
			else
				fstype = argv[i];
		}
	}
	s->writable = writable;

	if (tw_video_init(s) != 0) {
		printf("typewriter: no usable video framebuffer.\n");
		printf("  Enable CONFIG_VIDEO and a display driver on this board.\n");
		return CMD_RET_FAILURE;
	}

	if (scratch) {
		/* No file: a blank in-memory buffer. s->fs stays invalid so a
		 * save is impossible (and it's read-only anyway). */
		s->num_lines = 1;
		s->line_len[0] = 0;
		tw_bind_ime(s);
		tw_status(s, "[ New scratch buffer - read-only, ^X to exit ]");
		while (!s->quit) {
			tw_render(s);
			tw_handle_key(s, tw_read_key());
		}
		return CMD_RET_SUCCESS;
	}

	if (tw_fs_probe(&s->fs, argv[1], argv[2], fstype) < 0) {
		printf("typewriter: cannot access %s %s\n", argv[1], argv[2]);
		return CMD_RET_FAILURE;
	}

	strncpy(s->filename, argv[3], TW_MAX_FILENAME - 1);
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
	"                        - open an empty scratch buffer (read-only)\n"
	"typewriter -h                        - show this help\n"
	"typewriter <iftype> <dev:part> <filename> [fstype] [rw]\n"
	"  iftype  : mmc usb virtio nvme sata\n"
	"  dev:part: 0:1  1:2  ...\n"
	"  filename: full path on the filesystem\n"
	"  fstype  : fat ext4  (optional, auto-detect)\n"
	"  rw      : allow saving (READ-ONLY by default)\n"
	"Keys: ^O write  ^X exit  ^K cut  ^U paste  ^W search\n"
	"      ^A home  ^E end  arrows/PgUp/PgDn move\n"
	"      ^Space toggle Wubi/English; in Wubi: a-z code,\n"
	"      1-9/Space commit, =/- page, Esc cancel"
);
