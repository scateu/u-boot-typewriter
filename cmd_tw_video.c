// SPDX-License-Identifier: GPL-2.0+
/*
 * cmd_tw_video.c - framebuffer glyph renderer + Nano chrome for `typewriter`.
 *
 * Draws directly into U-Boot's video framebuffer (struct video_priv) rather
 * than the text console, because U-Boot's built-in console font is ASCII-only
 * and cannot show hanzi. Glyphs (ASCII and CJK) come from the embedded GNU
 * Unifont subset in font_data.c, scaled up by TW_SCALE for legibility.
 *
 * Rendering is black & white only and, critically, *incremental*: a full-screen
 * repaint per keystroke is far too slow on a hi-res panel, so the renderer
 * repaints only the text rows / bars flagged dirty since the last frame and
 * reports just that damaged rectangle to video_sync(). Pixel writes use
 * bpp-specialized row fills (clip once, native stores) instead of a
 * bounds-checked per-byte put_pixel in the hot path.
 */
#include <video.h>
#include <video_console.h>
#include <dm.h>
#include <stdio.h>
#include <linux/string.h>
#include "cmd_tw.h"
#include "font_data.h"

/* Black & white only: two packed native pixels. */
static u32 C_FG, C_BG;

static struct udevice    *g_vdev;
static struct video_priv *g_vp;
static u8                *g_fb;
static int                g_bpp;      /* bytes per pixel */
static int                g_stride;   /* bytes per line */

/* Damaged-rectangle accumulator for this frame (in pixels). Reset each render;
 * reported to video_damage()/video_sync() so only changed area is flushed. */
static int d_x0, d_y0, d_x1, d_y1, d_any;

static void damage(int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	if (!d_any) {
		d_x0 = x; d_y0 = y; d_x1 = x + w; d_y1 = y + h; d_any = 1;
		return;
	}
	if (x < d_x0) d_x0 = x;
	if (y < d_y0) d_y0 = y;
	if (x + w > d_x1) d_x1 = x + w;
	if (y + h > d_y1) d_y1 = y + h;
}

/* Pack black/white into the framebuffer's native pixel (bpp/format correct). */
static u32 tw_pack_bw(int white)
{
	u8 v = white ? 0xff : 0x00;

	switch (g_vp->bpix) {
	case VIDEO_BPP16:
		return white ? 0xffff : 0x0000;
	case VIDEO_BPP32:
		if (g_vp->format == VIDEO_RGBA8888)
			return white ? 0xffffffffu : 0x000000ffu;
		return white ? 0x00ffffffu : 0x00000000u;
	default:
		return (u32)v;
	}
}

/* Fill a pixel rectangle with a native color. Clips once to the screen, then
 * writes native-width stores row by row - no per-pixel function call. */
static void fill_rect(int x0, int y0, int w, int h, u32 color)
{
	int x1 = x0 + w, y1 = y0 + h;
	int y, x;

	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > g_vp->xsize) x1 = g_vp->xsize;
	if (y1 > g_vp->ysize) y1 = g_vp->ysize;
	if (x1 <= x0 || y1 <= y0)
		return;

	for (y = y0; y < y1; y++) {
		u8 *row = g_fb + (size_t)y * g_stride;

		switch (g_bpp) {
		case 4: {
			u32 *p = (u32 *)row + x0;

			for (x = x0; x < x1; x++)
				*p++ = color;
			break;
		}
		case 2: {
			u16 *p = (u16 *)row + x0;
			u16 c = (u16)color;

			for (x = x0; x < x1; x++)
				*p++ = c;
			break;
		}
		default: {
			u8 *p = row + (size_t)x0 * g_bpp;
			int i;

			for (x = x0; x < x1; x++)
				for (i = 0; i < g_bpp; i++)
					*p++ = (u8)(color >> (i * 8));
			break;
		}
		}
	}
	damage(x0, y0, x1 - x0, y1 - y0);
}

/* -------------------------------------------------------------- glyphs --- */
static const struct tw_glyph *find_glyph(u32 cp)
{
	unsigned lo = 0, hi = tw_glyphs_count;

	while (lo < hi) {
		unsigned mid = lo + (hi - lo) / 2;
		u32 c = tw_glyphs[mid].cp;

		if (c < cp)
			lo = mid + 1;
		else if (c > cp)
			hi = mid;
		else
			return &tw_glyphs[mid];
	}
	return NULL;
}

/* Screen columns a codepoint occupies (1 narrow, 2 wide). */
int tw_cp_cols(u32 cp)
{
	const struct tw_glyph *g = find_glyph(cp);

	return (g && g->width == 16) ? 2 : 1;
}

/*
 * Draw one glyph at pixel (px,py), scaled by TW_SCALE, fg on bg. Returns the
 * cell width in *unscaled* px (8 or 16) so callers can advance columns. The
 * background is painted first (one fill_rect), then set foreground pixels are
 * written as TW_SCALE x TW_SCALE blocks. Returns the unscaled advance.
 */
static int draw_glyph(int px, int py, u32 cp, u32 fg, u32 bg)
{
	const struct tw_glyph *g = find_glyph(cp);
	int w = g ? g->width : TW_CELL_W;         /* unscaled cell width */
	int row, bit;

	fill_rect(px, py, w * TW_SCALE, TW_GLYPH_H * TW_SCALE, bg);
	if (!g)
		return w;

	for (row = 0; row < TW_GLYPH_H; row++) {
		u8 left  = g->rows[row * 2 + 0];
		u8 right = g->rows[row * 2 + 1];

		for (bit = 0; bit < 8; bit++) {
			if (left & (0x80 >> bit))
				fill_rect(px + bit * TW_SCALE,
					  py + row * TW_SCALE,
					  TW_SCALE, TW_SCALE, fg);
			if (w == 16 && (right & (0x80 >> bit)))
				fill_rect(px + (8 + bit) * TW_SCALE,
					  py + row * TW_SCALE,
					  TW_SCALE, TW_SCALE, fg);
		}
	}
	return w;
}

/* Draw an ASCII C string; returns end pixel-x. */
static int draw_str(int px, int py, const char *s, u32 fg, u32 bg)
{
	for (; *s; s++)
		px += draw_glyph(px, py, (u32)(unsigned char)*s, fg, bg)
		      * TW_SCALE;
	return px;
}

/* Decode one UTF-8 codepoint (used to draw candidate words). */
static int utf8_next(const char *w, int wlen, int off, u32 *cp)
{
	unsigned char c = (unsigned char)w[off];

	if (c < 0x80) {
		*cp = c; return off + 1;
	}
	if ((c & 0xE0) == 0xC0 && off + 1 < wlen) {
		*cp = ((u32)(c & 0x1F) << 6) | (w[off + 1] & 0x3F);
		return off + 2;
	}
	if ((c & 0xF0) == 0xE0 && off + 2 < wlen) {
		*cp = ((u32)(c & 0x0F) << 12) | ((w[off + 1] & 0x3F) << 6) |
		      (w[off + 2] & 0x3F);
		return off + 3;
	}
	*cp = 0xFFFD;
	return off + 1;
}

/* Draw a UTF-8 candidate word (wlen bytes); returns end pixel-x. */
static int draw_word(int px, int py, const char *w, int wlen, u32 fg, u32 bg)
{
	int off = 0;

	while (off < wlen) {
		u32 cp;

		off = utf8_next(w, wlen, off, &cp);
		px += draw_glyph(px, py, cp, fg, bg) * TW_SCALE;
	}
	return px;
}

/* --------------------------------------------------------------- init ---- */
int tw_video_init(struct tw_state *s)
{
	if (uclass_first_device_err(UCLASS_VIDEO, &g_vdev))
		return -1;
	g_vp = dev_get_uclass_priv(g_vdev);
	if (!g_vp || !g_vp->fb)
		return -1;

	g_fb     = g_vp->fb;
	g_bpp    = VNBYTES(g_vp->bpix);
	g_stride = g_vp->line_length;

	s->fb_w = g_vp->xsize;
	s->fb_h = g_vp->ysize;

	C_FG = tw_pack_bw(1);   /* white text */
	C_BG = tw_pack_bw(0);   /* black background */

	/* Geometry (all in scaled px): title (1 row) + text area + candidate
	 * bar (1) + hints (2). Text area fills the screen edge to edge. */
	s->text_x0 = TW_CELL_PX;
	s->text_y0 = TW_ROW_PX;                    /* below the title row */
	s->hint_y  = s->fb_h - 2 * TW_ROW_PX;      /* two hint rows */
	s->bar_y   = s->hint_y - TW_ROW_PX;        /* candidate bar */
	s->text_cols = (s->fb_w - 2 * s->text_x0) / TW_CELL_PX;
	s->text_rows = (s->bar_y - s->text_y0) / TW_ROW_PX;
	if (s->text_cols < 1)
		s->text_cols = 1;
	if (s->text_rows < 1)
		s->text_rows = 1;
	if (s->text_rows > TW_MAX_LINES)
		s->text_rows = TW_MAX_LINES;

	s->first_paint = 1;
	s->dirty_all = 1;
	s->dirty_bar = 1;
	s->dirty_title = 1;
	return 0;
}

/* --------------------------------------------------------------- chrome -- */
/* Reverse video (white bar, black text) for title/bar/hints. */
static void draw_title(struct tw_state *s)
{
	char line[128];
	int x;

	fill_rect(0, 0, s->fb_w, TW_ROW_PX, C_FG);   /* white bar */
	snprintf(line, sizeof(line), " typewriter    %s%s",
		 s->filename[0] ? s->filename : "[ New Buffer ]",
		 s->dirty ? "    Modified" : "");
	draw_str(TW_CELL_PX, 0, line, C_BG, C_FG);
	x = s->fb_w - 7 * TW_CELL_PX;
	draw_str(x, 0, s->ime.mode == TW_IME_WUBI ? "[Wubi]" : "[ En ]",
		 C_BG, C_FG);
}

/* Draw a single text row sr (screen row) from the file line scroll_top+sr. */
static void draw_text_row(struct tw_state *s, int sr)
{
	int fr = s->scroll_top + sr;
	int py = s->text_y0 + sr * TW_ROW_PX;
	int col = 0, i;

	/* clear the whole row band to bg first */
	fill_rect(s->text_x0, py, s->fb_w - 2 * s->text_x0, TW_ROW_PX, C_BG);
	if (fr >= s->num_lines)
		return;
	for (i = 0; i < s->line_len[fr]; i++) {
		u32 cp = s->lines[fr][i];
		int cw = tw_cp_cols(cp);

		if (col + cw > s->text_cols)
			break;                    /* clip long lines (no wrap) */
		draw_glyph(s->text_x0 + col * TW_CELL_PX, py, cp, C_FG, C_BG);
		col += cw;
	}
}

/* Block cursor: draw (white) at the insertion point, or erase (bg) it. */
static void draw_cursor(struct tw_state *s, int row, int col, int on)
{
	int sr = row - s->scroll_top;
	int cx = 0, i, px, py;

	if (sr < 0 || sr >= s->text_rows)
		return;
	for (i = 0; i < col && i < s->line_len[row]; i++)
		cx += tw_cp_cols(s->lines[row][i]);
	px = s->text_x0 + cx * TW_CELL_PX;
	py = s->text_y0 + sr * TW_ROW_PX;
	/* a 2px-wide (scaled) caret */
	fill_rect(px, py, 2 * TW_SCALE, TW_ROW_PX, on ? C_FG : C_BG);
}

static void draw_bar(struct tw_state *s)
{
	struct tw_ime *im = &s->ime;
	int x, shown, i;

	fill_rect(0, s->bar_y, s->fb_w, TW_ROW_PX, C_FG);   /* white bar */

	if (s->prompt != TW_PROMPT_NONE) {
		const char *label =
			s->prompt == TW_PROMPT_SAVE   ? " File Name to Write: " :
			s->prompt == TW_PROMPT_OPEN   ? " File to open: " :
			s->prompt == TW_PROMPT_SEARCH ? " Search: " :
			" Save modified buffer?  Y)es  N)o  C)ancel ";
		x = draw_str(0, s->bar_y, label, C_BG, C_FG);
		x = draw_str(x, s->bar_y, s->prompt_ans, C_BG, C_FG);
		if (s->prompt != TW_PROMPT_EXIT)
			fill_rect(x, s->bar_y, 2 * TW_SCALE, TW_ROW_PX, C_BG);
		return;
	}

	if (s->status_msg[0]) {
		draw_str(TW_CELL_PX, s->bar_y, s->status_msg, C_BG, C_FG);
		return;
	}

	/* Mode chip. The Wubi tag contains the hanzi 五 (U+4E94, UTF-8
	 * e4 ba 94), which is multibyte - draw it with draw_word (UTF-8 aware),
	 * NOT draw_str (byte-per-glyph), or the hanzi renders as blank cells and
	 * vanishes into the white bar. [En] is ASCII so either works. */
	if (im->mode == TW_IME_WUBI) {
		const char tag[] = "[\344\272\224]";   /* [五] */

		x = draw_word(TW_CELL_PX, s->bar_y, tag, sizeof(tag) - 1,
			      C_BG, C_FG);
	} else {
		x = draw_str(TW_CELL_PX, s->bar_y, "[En]", C_BG, C_FG);
	}
	x += TW_CELL_PX;

	if (im->mode != TW_IME_WUBI || im->code_len == 0)
		return;

	x = draw_str(x, s->bar_y, im->code, C_BG, C_FG);
	x += TW_CELL_PX;

	for (shown = 0, i = im->page; i < im->ncand && shown < TW_PAGE;
	     i++, shown++) {
		char num[4];

		num[0] = '1' + shown;
		num[1] = '.';
		num[2] = '\0';
		x = draw_str(x, s->bar_y, num, C_BG, C_FG);
		x = draw_word(x, s->bar_y, im->cand[i].word,
			      im->cand[i].word_len, C_BG, C_FG);
		x += TW_CELL_PX;
		if (x > s->fb_w - 4 * TW_CELL_PX)
			break;
	}

	if (im->ncand > TW_PAGE) {
		char ind[16];
		int pages = (im->ncand + TW_PAGE - 1) / TW_PAGE;

		snprintf(ind, sizeof(ind), "(%d/%d)",
			 im->page / TW_PAGE + 1, pages);
		draw_str(s->fb_w - 8 * TW_CELL_PX, s->bar_y, ind, C_BG, C_FG);
	}
}

static void draw_hints(struct tw_state *s)
{
	static const char *row1 =
		" ^O Write  ^R Open  M-0..9 Slot  ^K Kill  ^Y Yank  ^Spc Wubi ";
	static const char *row2 =
		" ^X Exit  ^W DelWord  M-f/b Word  ^A/^E ^B/^F ^P/^N  M-w Find ";

	fill_rect(0, s->hint_y, s->fb_w, 2 * TW_ROW_PX, C_FG);  /* white */
	draw_str(TW_CELL_PX, s->hint_y, row1, C_BG, C_FG);
	draw_str(TW_CELL_PX, s->hint_y + TW_ROW_PX, row2, C_BG, C_FG);
}

/*
 * Incremental render. Repaints only what changed since the last frame:
 *  - first_paint: everything (static hints + title + full text + bar).
 *  - dirty_all / scrolled: whole text area.
 *  - otherwise: the cursor's old + new rows (marked in dirty_row) and, if the
 *    cursor moved off a row, erase the stale caret there.
 * The candidate/status bar and title repaint only when their dirty flag is set.
 * Only the accumulated damage rectangle is handed to video_sync().
 */
void tw_render(struct tw_state *s)
{
	int sr;

	d_any = 0;   /* reset per-frame damage */

	if (s->first_paint) {
		fill_rect(0, 0, s->fb_w, s->fb_h, C_BG);
		draw_hints(s);
		s->first_paint = 0;
		s->dirty_title = 1;
		s->dirty_bar = 1;
		s->dirty_all = 1;
	}

	if (s->dirty_title) {
		draw_title(s);
		s->dirty_title = 0;
	}

	/* Scrolling forces a full text-area repaint. */
	if (s->scroll_top != s->last_scroll_top)
		s->dirty_all = 1;

	if (s->dirty_all) {
		for (sr = 0; sr < s->text_rows; sr++)
			draw_text_row(s, sr);
		s->dirty_all = 0;
		memset(s->dirty_row, 0, (size_t)s->text_rows);
	} else {
		/* erase the old caret's row region if the cursor row changed */
		if (s->last_cur_row != s->cur_row) {
			int osr = s->last_cur_row - s->scroll_top;

			if (osr >= 0 && osr < s->text_rows)
				s->dirty_row[osr] = 1;
		}
		s->dirty_row[s->cur_row - s->scroll_top] = 1;
		for (sr = 0; sr < s->text_rows; sr++) {
			if (s->dirty_row[sr]) {
				draw_text_row(s, sr);
				s->dirty_row[sr] = 0;
			}
		}
	}

	/* caret (only when no prompt/status owns the bar area meaning) */
	if (s->prompt == TW_PROMPT_NONE && !s->status_msg[0])
		draw_cursor(s, s->cur_row, s->cur_col, 1);

	if (s->dirty_bar) {
		draw_bar(s);
		s->dirty_bar = 0;
	}

	s->last_cur_row = s->cur_row;
	s->last_cur_col = s->cur_col;
	s->last_scroll_top = s->scroll_top;

	if (d_any) {
		video_damage(g_vdev, d_x0, d_y0, d_x1 - d_x0, d_y1 - d_y0);
		video_sync(g_vdev, false);
	}
}
