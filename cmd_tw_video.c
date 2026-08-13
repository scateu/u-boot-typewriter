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
#include <backlight.h>
#include <panel.h>
#include <dm.h>
#include <stdio.h>
#include <linux/string.h>
#include "cmd_tw.h"
#include "font_data.h"

/* Precomputed native-pixel colors. Text area is B/W (max contrast for reading).
 * The title/candidate bar uses a muted gray so it doesn't grab attention; the
 * bottom hint bar uses plain black (transparent) so it recedes fully. Text on
 * both bars is light gray. All packed once at init - a colored bar costs the
 * same as a white one. */
static u32 C_FG, C_BG;      /* text area: white on black */
static u32 C_BAR, C_BARTX;  /* title/candidate bar: light-gray text on gray */
static u32 C_HINT;          /* bottom hint bar bg: plain black (transparent) */

static struct udevice    *g_vdev;
static struct video_priv *g_vp;
static u8                *g_fb;
static int                g_bpp;      /* bytes per pixel */
static int                g_stride;   /* bytes per line */

/*
 * Panel backlight. On gru/kevin the backlight is driven by the ChromeOS EC PWM
 * (CONFIG_PWM_CROS_EC); it comes up at full brightness, which is a big chunk of
 * this board's idle draw. We default it lower and let ^- / ^= step it.
 *
 * There are two ways the brightness control is reachable, and which one exists
 * depends on the DT/driver wiring:
 *   - a standalone UCLASS_PANEL_BACKLIGHT device (backlight_set_brightness), or
 *   - via the UCLASS_PANEL device that OWNS the backlight (panel_set_backlight),
 *     which is how the rockchip eDP simple-panel exposes it on gru.
 * We prefer the direct backlight device and fall back to the panel. Both are
 * NULL if neither exists, in which case the setter is a silent no-op.
 */
static struct udevice    *g_backlight;   /* UCLASS_PANEL_BACKLIGHT, or NULL */
static struct udevice    *g_panel;       /* UCLASS_PANEL owning it, or NULL */
static int                g_brightness = TW_BACKLIGHT_DEFAULT;

/* Defined later in this file; used by tw_video_init() to set the startup
 * brightness before the definition appears. */
int tw_backlight_set(int pct);

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
/* Pack an 8-bit-per-channel RGB triple into the framebuffer's native pixel.
 * Handles RGB565 (BPP16) and the two BPP32 orders; falls back to luma for odd
 * formats. Precomputed once into the C_* colors, so this never runs in the hot
 * path - a gray bar costs exactly the same as a white one. */
static u32 tw_pack_rgb(u8 r, u8 g, u8 b)
{
	switch (g_vp->bpix) {
	case VIDEO_BPP16:   /* RGB565 */
		return ((u32)(r & 0xf8) << 8) |
		       ((u32)(g & 0xfc) << 3) |
		       ((u32)(b) >> 3);
	case VIDEO_BPP32:
		if (g_vp->format == VIDEO_RGBA8888)
			return ((u32)r << 24) | ((u32)g << 16) |
			       ((u32)b << 8) | 0xff;
		return ((u32)r << 16) | ((u32)g << 8) | (u32)b;   /* xRGB8888 */
	default:
		return (r + g + b) / 3;   /* luma-ish fallback */
	}
}

static u32 tw_pack_bw(int white)
{
	return white ? tw_pack_rgb(0xff, 0xff, 0xff) : tw_pack_rgb(0, 0, 0);
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
 * Number of screen rows a logical line occupies when soft-wrapped to text_cols
 * columns. A codepoint that wouldn't fit in the remaining columns wraps to the
 * next row *before* being placed - so a 2-column hanzi is never split across
 * the wrap boundary. An empty line still takes one row.
 */
int tw_line_rows(struct tw_state *s, int fr)
{
	int i, col = 0, rows = 1;

	if (fr < 0 || fr >= s->num_lines)
		return 1;
	for (i = 0; i < s->line_len[fr]; i++) {
		int cw = tw_cp_cols(s->lines[fr][i]);

		if (col + cw > s->text_cols) {
			rows++;
			col = 0;
		}
		col += cw;
	}
	return rows;
}

/* Screen (row offset within the line, column) of codepoint index `cc` on
 * logical line `fr`, using the same wrap-before-wide rule. */
static void tw_wrap_pos(struct tw_state *s, int fr, int cc, int *out_row,
			int *out_col)
{
	int i, col = 0, row = 0;

	for (i = 0; i < cc && i < s->line_len[fr]; i++) {
		int cw = tw_cp_cols(s->lines[fr][i]);

		if (col + cw > s->text_cols) {
			row++;
			col = 0;
		}
		col += cw;
	}
	/* if the cursor sits exactly at a wrap boundary, it's on the next row */
	if (cc <= s->line_len[fr] && col > s->text_cols) {
		row++;
		col = 0;
	}
	*out_row = row;
	*out_col = col;
}

/*
 * Draw one glyph at pixel (px,py), scaled by TW_SCALE, fg on bg. Returns the
 * cell width in *unscaled* px (8 or 16) so callers can advance columns. The
 * background is painted first (one fill_rect), then the set foreground pixels.
 *
 * Speed: rather than one fill_rect per pixel (up to 256 tiny calls for a 16x16
 * hanzi - the source of candidate-bar lag), we coalesce consecutive set bits in
 * each row into a single wide fill_rect. A solid N-pixel stroke becomes one call
 * instead of N. The whole row's bits (8 or 16 wide) go into one mask so runs
 * that cross the left/right byte boundary of a wide glyph merge too.
 */
static int draw_glyph(int px, int py, u32 cp, u32 fg, u32 bg)
{
	const struct tw_glyph *g = find_glyph(cp);
	int w = g ? g->width : TW_CELL_W;         /* unscaled cell width */
	int row, col;

	fill_rect(px, py, w * TW_SCALE, TW_GLYPH_H * TW_SCALE, bg);
	if (!g)
		return w;

	for (row = 0; row < TW_GLYPH_H; row++) {
		/* Row bits, bit 15 (0x8000) = leftmost pixel. Wide glyphs use
		 * both bytes (cols 0..15); narrow only the left byte (cols 0..7,
		 * and the loop stops at w=8 so the low byte is never examined). */
		unsigned int bits = (unsigned int)g->rows[row * 2 + 0] << 8;

		if (w == 16)
			bits |= g->rows[row * 2 + 1];

		/* Emit one fill_rect per horizontal run of set bits. */
		col = 0;
		while (col < w) {
			int start;

			if (!(bits & (0x8000 >> col))) {   /* pixel clear */
				col++;
				continue;
			}
			start = col;
			while (col < w && (bits & (0x8000 >> col)))
				col++;                     /* extend the run */
			fill_rect(px + start * TW_SCALE, py + row * TW_SCALE,
				  (col - start) * TW_SCALE, TW_SCALE, fg);
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

	C_FG    = tw_pack_bw(1);                  /* white text */
	C_BG    = tw_pack_bw(0);                  /* black background */
	C_BAR   = tw_pack_rgb(0x60, 0x60, 0x60);  /* title/candidate bar: gray */
	C_BARTX = tw_pack_rgb(0xd0, 0xd0, 0xd0);  /* light-gray text on bars */
	C_HINT  = tw_pack_bw(0);                  /* hint bar bg: plain black */

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

	/* Bring the backlight to our (dimmer) default. Best-effort: prefer a
	 * standalone backlight device, else the panel that owns it. A board with
	 * neither just keeps whatever brightness it booted with. */
	if (uclass_first_device_err(UCLASS_PANEL_BACKLIGHT, &g_backlight))
		g_backlight = NULL;
	if (!g_backlight &&
	    uclass_first_device_err(UCLASS_PANEL, &g_panel))
		g_panel = NULL;
	tw_backlight_set(TW_BACKLIGHT_DEFAULT);
	return 0;
}

/*
 * Set panel brightness to a percentage and return the value actually applied.
 *
 * IMPORTANT: we clamp to a NONZERO minimum (TW_BACKLIGHT_MIN) and NEVER use
 * BACKLIGHT_OFF. On this board, driving the backlight to 0 / OFF powers the PWM
 * (and its regulator) fully down, which froze the board hard - the display went
 * black and even ^Q was dead. So dimming bottoms out at a dim-but-alive level
 * and always keeps the panel enabled. No-op if there's no controllable
 * backlight device.
 */
/* Write the panel brightness (clamped) WITHOUT changing the remembered user
 * level g_brightness. Used both by the normal setter and by power-save dimming
 * (which must not clobber the level to restore). */
static void tw_backlight_apply(int pct)
{
	if (pct < TW_BACKLIGHT_MIN)
		pct = TW_BACKLIGHT_MIN;
	if (pct > 100)
		pct = 100;
	if (g_backlight)
		backlight_set_brightness(g_backlight, pct);
	else if (g_panel)
		panel_set_backlight(g_panel, pct);
}

int tw_backlight_set(int pct)
{
	if (pct < TW_BACKLIGHT_MIN)
		pct = TW_BACKLIGHT_MIN;
	if (pct > 100)
		pct = 100;
	g_brightness = pct;
	tw_backlight_apply(pct);
	return g_brightness;
}

/* Step brightness by delta (typically +/-20) and return the applied value. */
int tw_backlight_step(int delta)
{
	return tw_backlight_set(g_brightness + delta);
}

/* --------------------------------------------------------------- chrome -- */
/* Muted-gray chrome bars (title/candidate/hints): light text on dark gray, so
 * they recede vs. the black-on-white text area. */
static void draw_title(struct tw_state *s)
{
	char line[128];
	int x;

	fill_rect(0, 0, s->fb_w, TW_ROW_PX, C_BAR);   /* gray bar */
	snprintf(line, sizeof(line), " typewriter    %s%s",
		 s->filename[0] ? s->filename : "[ New Buffer ]",
		 s->dirty ? "    Modified" : "");
	draw_str(TW_CELL_PX, 0, line, C_BARTX, C_BAR);

	/* Right side: battery % (if known) then the IME chip. Battery is read on
	 * demand by Ctrl-T (see cmd_tw.c) into s->batt_pct; >= 0 is a percentage,
	 * negative means unavailable / not yet read (hidden). */
	x = s->fb_w - 7 * TW_CELL_PX;
	draw_str(x, 0, s->ime.mode == TW_IME_WUBI ? "[Wubi]" : "[ En ]",
		 C_BARTX, C_BAR);
	if (s->batt_pct >= 0) {
		snprintf(line, sizeof(line), "BAT: %d%%", s->batt_pct);
		draw_str(x - (int)(strlen(line) + 1) * TW_CELL_PX, 0,
			 line, C_BARTX, C_BAR);
	}
}

/* Clear one screen row's band to background. */
static void clear_row(struct tw_state *s, int sr)
{
	fill_rect(s->text_x0, s->text_y0 + sr * TW_ROW_PX,
		  s->fb_w - 2 * s->text_x0, TW_ROW_PX, C_BG);
}

/*
 * Draw the wrapped text starting at screen row `sr0` from logical line `fr0`,
 * down to the bottom of the text area. Each screen row is cleared to bg just
 * before it's drawn (a thin per-row clear, NOT an area-wide clear - that
 * area-wide clear was the source of the per-keystroke black flash). Rows past
 * the last content line are cleared too. Rows above sr0 are left untouched, so
 * a typed character only repaints its own line (and, on reflow, the lines
 * below it) - no flicker.
 */
static void draw_text_from(struct tw_state *s, int sr0, int fr0)
{
	int sr = sr0;
	int fr = fr0;

	while (sr < s->text_rows && fr < s->num_lines) {
		int col = 0, i;
		int py = s->text_y0 + sr * TW_ROW_PX;

		clear_row(s, sr);
		for (i = 0; i < s->line_len[fr]; i++) {
			u32 cp = s->lines[fr][i];
			int cw = tw_cp_cols(cp);

			if (col + cw > s->text_cols) {   /* wrap to next row */
				sr++;
				if (sr >= s->text_rows)
					break;
				col = 0;
				py = s->text_y0 + sr * TW_ROW_PX;
				clear_row(s, sr);
			}
			draw_glyph(s->text_x0 + col * TW_CELL_PX, py, cp,
				   C_FG, C_BG);
			col += cw;
		}
		sr++;                   /* next logical line starts a new row */
		fr++;
	}
	/* clear any leftover rows below the last content line */
	while (sr < s->text_rows)
		clear_row(s, sr++);
}

/* Repaint the whole text area (used on scroll / first paint / picker exit). */
static void draw_text(struct tw_state *s)
{
	draw_text_from(s, 0, s->scroll_top);
}

/* Screen row where logical line `fr` starts, relative to scroll_top (wrap-
 * aware). Assumes fr >= scroll_top and on-screen-ish; caller bounds it. */
static int tw_line_screen_row(struct tw_state *s, int fr)
{
	int sr = 0, i;

	for (i = s->scroll_top; i < fr; i++)
		sr += tw_line_rows(s, i);
	return sr;
}

/* Screen pixel (px,py) of logical position (row,col), wrap-aware.
 * Returns 0 and fills px/py if on-screen, -1 if off-screen. */
static int tw_screen_xy(struct tw_state *s, int row, int col, int *px, int *py)
{
	int base = 0, fr, wr, wc, sr;

	if (row < s->scroll_top)
		return -1;
	for (fr = s->scroll_top; fr < row; fr++)
		base += tw_line_rows(s, fr);
	tw_wrap_pos(s, row, col, &wr, &wc);
	sr = base + wr;
	if (sr < 0 || sr >= s->text_rows)
		return -1;
	*px = s->text_x0 + wc * TW_CELL_PX;
	*py = s->text_y0 + sr * TW_ROW_PX;
	return 0;
}

/* Draw (on=1) or erase (on=0) the block caret at logical (row,col). Erasing
 * restores the cell: bg fill, then the glyph living there (if any) so moving
 * the caret doesn't leave a hole or require a full-screen repaint. */
static void draw_cursor(struct tw_state *s, int row, int col, int on)
{
	int px, py;

	if (tw_screen_xy(s, row, col, &px, &py) != 0)
		return;

	if (on) {
		fill_rect(px, py, 2 * TW_SCALE, TW_ROW_PX, C_FG);
		return;
	}

	/* erase: repaint the cell under the caret (bg + its glyph, if present) */
	if (col < s->line_len[row]) {
		u32 cp = s->lines[row][col];

		draw_glyph(px, py, cp, C_FG, C_BG);   /* draw_glyph bg-fills first */
	} else {
		fill_rect(px, py, TW_CELL_PX, TW_ROW_PX, C_BG);
	}
}

/* Draw the Wubi candidate list starting at pixel-x `x` on the bar row: numbered
 * candidates for the current page, plus a (page/total) indicator at the right.
 * Shared by the normal composing bar and the New/Rename filename prompt. */
static void draw_ime_candidates(struct tw_state *s, int x)
{
	struct tw_ime *im = &s->ime;
	int shown, i;

	for (shown = 0, i = im->page; i < im->ncand && shown < TW_PAGE;
	     i++, shown++) {
		char num[4];

		num[0] = '1' + shown;
		num[1] = '.';
		num[2] = '\0';
		x = draw_str(x, s->bar_y, num, C_BARTX, C_BAR);
		x = draw_word(x, s->bar_y, im->cand[i].word,
			      im->cand[i].word_len, C_BARTX, C_BAR);
		x += TW_CELL_PX;
		if (x > s->fb_w - 4 * TW_CELL_PX)
			break;
	}

	if (im->ncand > TW_PAGE) {
		char ind[16];
		int pages = (im->ncand + TW_PAGE - 1) / TW_PAGE;

		snprintf(ind, sizeof(ind), "(%d/%d)",
			 im->page / TW_PAGE + 1, pages);
		draw_str(s->fb_w - 8 * TW_CELL_PX, s->bar_y, ind, C_BARTX, C_BAR);
	}
}

static void draw_bar(struct tw_state *s)
{
	struct tw_ime *im = &s->ime;
	int x;

	fill_rect(0, s->bar_y, s->fb_w, TW_ROW_PX, C_BAR);   /* gray bar */

	if (s->prompt == TW_PROMPT_PICK) {
		char hint[48];
		const char *nm = s->pick_count ? s->pick_name[s->pick_sel] : "";

		snprintf(hint, sizeof(hint), " Files  [%d/%d]  ",
			 s->pick_count ? s->pick_sel + 1 : 0, s->pick_count);
		x = draw_str(0, s->bar_y, hint, C_BARTX, C_BAR);
		/* UTF-8 aware (names are ASCII, but a non-ASCII name created
		 * elsewhere at least renders per-codepoint, not per-byte garbage). */
		draw_word(x, s->bar_y, nm, (int)strlen(nm), C_BARTX, C_BAR);
		return;
	}

	/* Delete confirm names the selected file (destructive - be explicit). */
	if (s->prompt == TW_PROMPT_PICKDEL) {
		const char *nm = s->pick_count ? s->pick_name[s->pick_sel] : "";

		x = draw_str(0, s->bar_y, " Delete \"", C_BARTX, C_BAR);
		x = draw_word(x, s->bar_y, nm, (int)strlen(nm), C_BARTX, C_BAR);
		draw_str(x, s->bar_y, "\"?  y)es  N)o ", C_BARTX, C_BAR);
		return;
	}

	if (s->prompt == TW_PROMPT_POWEROFF) {
		draw_str(0, s->bar_y,
			 " Save &...  Y) power off   B) boot OS   N) cancel ",
			 C_BARTX, C_BAR);
		return;
	}

	if (s->prompt != TW_PROMPT_NONE) {
		const char *label =
			s->prompt == TW_PROMPT_SAVE   ? " File Name to Write: " :
			s->prompt == TW_PROMPT_OPEN   ? " File to open: " :
			s->prompt == TW_PROMPT_SEARCH ? " Search: " :
			s->prompt == TW_PROMPT_SHELL  ? " Run cmd (output inserted): !" :
			s->prompt == TW_PROMPT_NEW    ? " New file name (ASCII): " :
			s->prompt == TW_PROMPT_RENAME ? " Rename to (ASCII): " :
			" Save modified buffer?  Y)es  N)o  C)ancel ";
		x = draw_str(0, s->bar_y, label, C_BARTX, C_BAR);
		x = draw_str(x, s->bar_y, s->prompt_ans, C_BARTX, C_BAR);
		/* caret */
		if (s->prompt != TW_PROMPT_EXIT)
			fill_rect(x, s->bar_y, 2 * TW_SCALE, TW_ROW_PX, C_BARTX);
		return;
	}

	if (s->status_msg[0]) {
		draw_str(TW_CELL_PX, s->bar_y, s->status_msg, C_BARTX, C_BAR);
		return;
	}

	/* Mode chip. The Wubi tag contains the hanzi 五 (U+4E94, UTF-8
	 * e4 ba 94), which is multibyte - draw it with draw_word (UTF-8 aware),
	 * NOT draw_str (byte-per-glyph), or the hanzi renders as blank cells and
	 * vanishes into the bar. [En] is ASCII so either works. */
	if (im->mode == TW_IME_WUBI) {
		const char tag[] = "[\344\272\224]";   /* [五] */

		x = draw_word(TW_CELL_PX, s->bar_y, tag, sizeof(tag) - 1,
			      C_BARTX, C_BAR);
	} else {
		x = draw_str(TW_CELL_PX, s->bar_y, "[En]", C_BARTX, C_BAR);
	}
	x += TW_CELL_PX;

	if (im->mode != TW_IME_WUBI || im->code_len == 0)
		return;

	x = draw_str(x, s->bar_y, im->code, C_BARTX, C_BAR);
	x += TW_CELL_PX;
	draw_ime_candidates(s, x);
}

/*
 * Two-row shortcut hint bar, Nano-style and grid-aligned. Each cell is a
 * {key, desc} pair drawn at a fixed column x, so keys line up in one sub-column
 * and descriptions in another, across both rows. Only the KEY carries a colored
 * (inverse-video) block; the description sits on the plain hint-bar background.
 * The hint bar bg (C_HINT) is a touch darker than the title/candidate bar.
 */
#define TW_HINT_COLS   7
#define TW_HINT_COL    16    /* full cell width, in character cells */
#define TW_HINT_KEYW   6     /* key sub-column width (chars) before the desc */
struct hint { const char *key, *desc; };

/* Render two rows of key/desc cells into the bottom hint bar. Common to the
 * editing bar and the context-aware picker bar. */
static void draw_hint_rows(struct tw_state *s, const struct hint *row1,
			   const struct hint *row2)
{
	const struct hint *rows[2] = { row1, row2 };
	int r, c;

	fill_rect(0, s->hint_y, s->fb_w, 2 * TW_ROW_PX, C_HINT);
	for (r = 0; r < 2; r++) {
		int py = s->hint_y + r * TW_ROW_PX;

		for (c = 0; c < TW_HINT_COLS; c++) {
			const struct hint *h = &rows[r][c];
			int kx = TW_CELL_PX + c * TW_HINT_COL * TW_CELL_PX;
			int dx = kx + TW_HINT_KEYW * TW_CELL_PX;

			if (!h->key)
				continue;              /* empty cell */
			if (kx > s->fb_w - TW_CELL_PX)
				break;                 /* off the right edge */
			/* Key: inverse video (light block, dark text) so it pops. */
			draw_str(kx, py, h->key, C_HINT, C_BARTX);
			/* Description: normal text on the plain bar bg. */
			draw_str(dx, py, h->desc, C_BARTX, C_HINT);
		}
	}
}

/*
 * Context-aware bottom hint bar. In the ^R file picker (and its New/Rename/
 * Delete sub-prompts) the editing chords (Save/Wubi/Run cmd/...) don't apply, so
 * we swap the whole bar for a picker-specific set, Pine/alpine style. Otherwise
 * the normal editing bar. Redrawn whenever s->dirty_hints is set (picker enter/
 * exit); the static editing bar is otherwise painted once on first_paint.
 */
static void draw_hints(struct tw_state *s)
{
	static const struct hint edit1[TW_HINT_COLS] = {
		{"^S", "Save"},  {"^A/^E", "BOL/EOL"}, {"^B/^F", "back/fwd"},
		{"^K", "Kill"},  {"^W", "DelWord"},    {"^Spc", "Wubi"},
		{"^T", "Battery"},
	};
	static const struct hint edit2[TW_HINT_COLS] = {
		{"^X", "Exit"},  {"^R", "Open"},       {"^P/^N", "prev/next"},
		{"^D", "Del"},   {"^-/^]", "Bright"},  {"^Q", "PowerOff"},
		{"^V", "Run cmd"},  /* 7th slot: directly below ^T Battery */
	};
	/* Picker bar: only what works in the file browser. */
	static const struct hint pick1[TW_HINT_COLS] = {
		{"Enter", "Open"}, {"n", "New"},    {"r", "Rename"},
		{"d", "Delete"},   {NULL, NULL},    {NULL, NULL}, {NULL, NULL},
	};
	static const struct hint pick2[TW_HINT_COLS] = {
		{"Arrow", "Move"}, {"^P/^N", "Move"}, {"Esc", "Cancel"},
		{NULL, NULL}, {NULL, NULL}, {NULL, NULL}, {NULL, NULL},
	};
	int in_picker = (s->prompt == TW_PROMPT_PICK ||
			 s->prompt == TW_PROMPT_NEW ||
			 s->prompt == TW_PROMPT_RENAME ||
			 s->prompt == TW_PROMPT_PICKDEL);

	if (in_picker)
		draw_hint_rows(s, pick1, pick2);
	else
		draw_hint_rows(s, edit1, edit2);
}

/*
 * ^R file picker: a bordered, scrollable list overlaying the text area.
 * The selected row is drawn in reverse (white bar, black text). Keeps
 * pick_top in sync so pick_sel is always visible.
 */
static void draw_picker(struct tw_state *s)
{
	int rows = s->text_rows;         /* list rows = text-area rows */
	int r;

	if (rows < 1)
		rows = 1;

	/* scroll so the selection is visible */
	if (s->pick_sel < s->pick_top)
		s->pick_top = s->pick_sel;
	if (s->pick_sel >= s->pick_top + rows)
		s->pick_top = s->pick_sel - rows + 1;

	/* clear the text area, draw a header line, then the list */
	fill_rect(s->text_x0, s->text_y0,
		  s->fb_w - 2 * s->text_x0, s->bar_y - s->text_y0, C_BG);

	for (r = 0; r < rows; r++) {
		int idx = s->pick_top + r;
		int py = s->text_y0 + r * TW_ROW_PX;
		u32 fg = C_FG, bg = C_BG;

		if (idx >= s->pick_count)
			break;
		if (idx == s->pick_sel) {        /* highlight selection */
			fg = C_BG; bg = C_FG;
			fill_rect(s->text_x0, py,
				  s->fb_w - 2 * s->text_x0, TW_ROW_PX, C_FG);
		}
		/* UTF-8 aware: filenames may contain hanzi (draw_str is byte-wise
		 * and would render each UTF-8 byte as a garbage glyph, e.g. a stray
		 * 'K'). draw_word decodes codepoints. */
		draw_word(s->text_x0 + TW_CELL_PX, py, s->pick_name[idx],
			  (int)strlen(s->pick_name[idx]), fg, bg);
	}
}

/*
 * Render a frame with targeted repaint - the fix for the per-keystroke flash.
 * An EDIT repaints only from the cursor's logical line downward (wrap reflow
 * safe); a pure CURSOR MOVE just moves the caret; a SCROLL (rare) repaints the
 * area. No path does an area-wide clear-to-black, so typing doesn't flicker.
 * Title/bar repaint only when their dirty flag is set; only the damaged
 * rectangle is synced.
 */
void tw_render(struct tw_state *s)
{
	int first = s->first_paint;

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

	/* Context-aware hint bar: swap edit<->picker set on prompt transitions. */
	if (s->dirty_hints) {
		draw_hints(s);
		s->dirty_hints = 0;
	}

	/* The ^R picker takes over the text area with its file list. Its New/
	 * Rename/Delete sub-prompts keep the list visible underneath (the prompt
	 * shows in the bar), so draw the picker for the whole family. */
	if (s->prompt == TW_PROMPT_PICK || s->prompt == TW_PROMPT_NEW ||
	    s->prompt == TW_PROMPT_RENAME || s->prompt == TW_PROMPT_PICKDEL) {
		draw_picker(s);
		s->dirty_all = 1;    /* force a text repaint when the picker exits */
	} else {
		int scrolled = (s->scroll_top != s->last_scroll_top);

		if (scrolled || first) {
			/* whole area (rare): scroll or first paint */
			draw_text(s);
		} else if (s->dirty_all) {
			/*
			 * An edit. Repaint from the FIRST logical line the edit
			 * could have touched, downward - covering a reflow of the
			 * lines below (wrap) while leaving rows ABOVE untouched.
			 * No area-wide clear => no per-keystroke flash.
			 *
			 * The start line is min(last_cur_row, cur_row): a plain
			 * in-line edit keeps cur_row == last_cur_row (one row);
			 * but a newline SPLIT moves cur_row DOWN to the new line
			 * while the line that was split (last_cur_row, above) also
			 * changed - starting at cur_row would leave that split
			 * line showing stale text (e.g. "abc" duplicated onto both
			 * halves). A backspace-JOIN moves cur_row UP, so cur_row is
			 * already the earlier one. Take the min to cover both.
			 */
			int fr0 = s->cur_row < s->last_cur_row
				  ? s->cur_row : s->last_cur_row;
			int sr;

			if (fr0 < s->scroll_top)
				fr0 = s->scroll_top;   /* clamp above the viewport */
			sr = tw_line_screen_row(s, fr0);
			if (sr < 0)
				sr = 0;
			draw_text_from(s, sr, fr0);
		} else if (s->last_cur_row != s->cur_row ||
			   s->last_cur_col != s->cur_col) {
			/* pure cursor move: erase old caret, no clear */
			draw_cursor(s, s->last_cur_row, s->last_cur_col, 0);
		}

		if (s->prompt == TW_PROMPT_NONE && !s->status_msg[0])
			draw_cursor(s, s->cur_row, s->cur_col, 1);

		s->dirty_all = 0;
	}

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
