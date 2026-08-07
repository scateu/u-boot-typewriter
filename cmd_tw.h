/* cmd_tw.h - shared definitions for the `typewriter` command.
 *
 * A GNU Nano-style, modeless, full-screen text editor that runs on U-Boot's
 * video framebuffer (not a serial terminal), with a built-in Wubi 86 Chinese
 * input method. Chinese glyphs are drawn from an embedded GNU Unifont subset
 * (font_data.c); the wubi table is embedded too (wubi_embed.c) and looked up
 * via the ported engine (ime_table.c).
 *
 * The document is stored as lines of Unicode codepoints (not bytes): each cell
 * is one codepoint, which occupies one editor column and 1 (ASCII) or 2 (CJK)
 * screen columns. This keeps cursor motion and per-line editing exact while
 * still rendering wide hanzi. Files are read/written as UTF-8.
 */
#ifndef __CMD_TW_H
#define __CMD_TW_H

#include <linux/types.h>
#include "ime_table.h"

/* ---- limits (all state is static in BSS; no heap) ---------------------- */
#define TW_MAX_LINES     2048
#define TW_MAX_COLS      512      /* codepoints per line (+1 guard) */
#define TW_MAX_FILENAME  256
#define TW_CMD_BUF       128      /* prompt answer buffer (filename / search) */
#define TW_TABW          8        /* Tab expands to spaces to the next stop */

/* Font cell geometry (GNU Unifont): 16px tall, 8px per narrow column, scaled
 * up by TW_SCALE so text is legible on a hi-res panel. A narrow cell is
 * (TW_CELL_W * TW_SCALE) wide and (TW_GLYPH_H * TW_SCALE) tall; a wide (CJK)
 * glyph is two narrow cells. Scaling also means fewer cells per screen, so
 * fewer glyph blits per frame. */
#define TW_GLYPH_H       16
#define TW_CELL_W        8
#define TW_SCALE         2
#define TW_CELL_PX       (TW_CELL_W * TW_SCALE)   /* narrow cell width, px */
#define TW_ROW_PX        (TW_GLYPH_H * TW_SCALE)  /* cell height, px */

/* Panel backlight (see cmd_tw_video.c). It comes up full-bright on gru/kevin,
 * which dominates idle power; default lower and step it with ^- / ^=. */
#define TW_BACKLIGHT_DEFAULT 40      /* percent set on startup */
#define TW_BACKLIGHT_STEP    20      /* percent per ^- / ^= press */
#define TW_BACKLIGHT_MIN     20      /* never dim below this - 0/OFF freezes it */

/* File I/O scratch is malloc()'d at runtime (see cmd_tw_fs.c) - NOT staged at
 * a fixed CONFIG_SYS_LOAD_ADDR, which on this board overlaps U-Boot's own DRAM
 * image + heap and corrupted the FAT partition.
 *
 * Size for the true worst case: utf8_encode emits up to 4 bytes per codepoint
 * (non-BMP), plus one newline per line, plus a slack page. The previous budget
 * of 3 bytes/cp with zero slack could overflow the buffer - and overflowing a
 * malloc chunk during fs_write scribbles over the FAT driver's own heap
 * allocations (directory iterator / LFN scratch), corrupting the on-disk
 * directory. The save loop also bounds-checks every write against this size. */
#define TW_FILE_BUF_SIZE (TW_MAX_LINES * (TW_MAX_COLS * 4 + 1) + 4096)

/* ---- cooked key codes (returned by tw_read_key) ------------------------ */
#define KEY_ESC       0x1B
#define KEY_ENTER     0x0D
#define KEY_LF        0x0A
#define KEY_BACKSPACE 0x7F
#define KEY_BS        0x08
#define KEY_TAB       0x09

/* readline-style Ctrl chords */
#define KEY_CTRL_A    0x01       /* beginning of line */
#define KEY_CTRL_B    0x02       /* back one char */
#define KEY_CTRL_D    0x04       /* delete char under cursor */
#define KEY_CTRL_E    0x05       /* end of line */
#define KEY_CTRL_F    0x06       /* forward one char */
#define KEY_CTRL_G    0x07       /* help */
#define KEY_CTRL_K    0x0B       /* kill to end of line */
#define KEY_CTRL_N    0x0E       /* next line */
#define KEY_CTRL_P    0x10       /* previous line */
#define KEY_CTRL_Q    0x11       /* power off (save+sync+EC hibernate), confirmed */
#define KEY_CTRL_S    0x13       /* save (one-handed; replaced ^O write-out) */
#define KEY_CTRL_R    0x12       /* open file (read into buffer) */
#define KEY_CTRL_W    0x17       /* delete word backward */
#define KEY_CTRL_X    0x18       /* exit */
#define KEY_CTRL_Y    0x19       /* yank (paste kill buffer) */
#define KEY_CTRL_SPACE 0x00      /* Ctrl-Space (a.k.a. Ctrl-@): toggle Wubi/En */

/* Brightness. Ctrl chording punctuation is keymap-dependent. On this board's
 * cros_ec keyboard, VERIFIED: Ctrl-'-' emits 0x1F (= Ctrl-'_') and Ctrl-']'
 * emits 0x1D. Ctrl-'=' does NOT emit a control byte (it types a literal '='),
 * so the "brighten" key is Ctrl-']', not Ctrl-'='. */
#define KEY_CTRL_MINUS 0x1F      /* Ctrl-'-' : brightness DOWN one step */
#define KEY_CTRL_EQUAL 0x1D      /* Ctrl-']' : brightness UP one step */

/* Extended keys (> 0xFF), parsed from ANSI arrow/nav escape sequences and from
 * Meta (Alt) chords, which arrive as ESC followed by the key. */
#define KEY_ARROW_UP    0x141
#define KEY_ARROW_DOWN  0x142
#define KEY_ARROW_RIGHT 0x143
#define KEY_ARROW_LEFT  0x144
#define KEY_HOME_SEQ    0x148
#define KEY_END_SEQ     0x146
#define KEY_PAGE_UP     0x200
#define KEY_PAGE_DOWN   0x201
#define KEY_DELETE      0x202
/* Meta (Alt) chords. NOTE: Meta keys do not work on the target hardware; these
 * are kept wired but dormant. File switching is via the ^R picker instead. */
/* Synthetic key: an EC power-off trigger fired - the power button was pressed
 * OR the lid was closed (detected via host-event flags in the key-wait loop,
 * not a real console byte). Dispatched like ^Q -> Y: save + power off. */
#define KEY_POWER_BTN   0x220

/* Synthetic key: the periodic battery-poll timer elapsed. Not a real byte;
 * the handler re-reads the gauge and repaints the title bar if the % changed. */
#define KEY_REFRESH     0x221
#define TW_BATT_POLL_MS 30000    /* battery re-read interval (ms) */

/* Synthetic keys for power-saving mode (see cmd_tw.c). KEY_PWRSAVE fires when
 * the idle timer trips (dim + go lazy); KEY_PWRSAVE_WAKE is the swallowed key
 * that woke us (restore brightness + speed, but don't type it). */
#define KEY_PWRSAVE      0x222
#define KEY_PWRSAVE_WAKE 0x223

/* Idle-loop tuning (power). TW_KEY_NAP_MS is one WFE nap = worst-case keystroke
 * latency (imperceptible). TW_EC_POLL_MS throttles the EC host-event SPI poll
 * (power button / lid) - human reaction time is ~150 ms, so 200 ms is plenty
 * and cuts EC bus traffic vs. polling every nap. See POWERSAVE.md. */
#define TW_KEY_NAP_MS   25       /* WFE nap length per idle iteration (ms) */
#define TW_EC_POLL_MS   200      /* EC power-button/lid poll interval (ms) */

/* Power-saving mode: after TW_IDLE_SAVE_MS of no keystroke, dim the backlight,
 * show [power saving] in the title, and switch to lazy polling (TW_SAVE_NAP_MS)
 * so the core/EC are nearly idle. Any key wakes (the wake key is swallowed).
 * Sacrifices only the first keystroke's latency for a big idle-power drop. */
#define TW_IDLE_SAVE_MS 60000    /* idle time before entering power-save (ms) */
#define TW_SAVE_NAP_MS  500      /* WFE nap / poll cadence while saving (ms) */
#define TW_BATT_SAVE_MS 60000    /* battery re-read while saving (still visible) */

#define KEY_META_F      0x210    /* M-f: forward one word */
#define KEY_META_B      0x211    /* M-b: backward one word */
#define KEY_META_D      0x212    /* M-d: kill word forward */
#define KEY_META_W      0x213    /* M-w: search */

/* ---- IME (Wubi) input state -------------------------------------------- */
#define TW_IME_OFF    0          /* English: bytes typed literally */
#define TW_IME_WUBI   1

#define TW_PAGE       9          /* candidates shown per page */
#define TW_MAX_CANDS  64         /* fetched per lookup */

struct tw_ime {
	int         mode;                    /* TW_IME_OFF / TW_IME_WUBI */
	int         ready;                   /* wubi table bound ok */
	ime_table   tab;
	char        code[IME_CODE_LEN + 1];  /* pending a-z code */
	int         code_len;
	ime_cand    cand[TW_MAX_CANDS];
	int         ncand;
	int         page;                    /* first cand index of current page */
};

/* ---- filesystem descriptor (same shape as the ved reference) ----------- */
struct tw_fs {
	char iftype[16];
	char dev_part[16];
	char fstype[8];
	int  valid;
};

/* ---- prompt (Nano's bottom-line answer line: ^O filename, ^W search) --- */
#define TW_PROMPT_NONE   0
#define TW_PROMPT_SAVE   1       /* "File Name to Write: " */
#define TW_PROMPT_SEARCH 2       /* "Search: " */
#define TW_PROMPT_EXIT   3       /* "Save modified buffer? (Y/N/C)" */
#define TW_PROMPT_OPEN   4       /* "File to open: " (^R) - text entry */
#define TW_PROMPT_PICK   5       /* ^R file picker: arrow-select from fatls */
#define TW_PROMPT_POWEROFF 6     /* ^Q: "Save & power off? (Y/N)" */

/* Sentinel for tw_read_battery(): distinct from a real negative errno so the
 * popup can say "no EC" vs. "EC command failed with code N". */
#define TW_BATT_NO_EC    (-1000)

/* File picker (^R): a scrollable list of the files on the current device. */
#define TW_PICK_MAX      128     /* max files listed */
#define TW_PICK_NAMELEN  64      /* max filename shown */

/* ---- editor state ------------------------------------------------------ */
struct tw_state {
	u32   lines[TW_MAX_LINES][TW_MAX_COLS]; /* codepoints per line */
	int   line_len[TW_MAX_LINES];           /* codepoints in each line */
	int   num_lines;

	int   cur_row;      /* cursor line (0-based, into lines[]) */
	int   cur_col;      /* cursor column in codepoints */
	int   scroll_top;   /* first visible file line */

	int   dirty;
	int   quit;
	int   writable;    /* 0 = read-only (default); saving is refused */
	int   last_write_bytes;  /* bytes written by the last successful save */

	char  filename[TW_MAX_FILENAME];
	char  status_msg[128];

	u32   cut[TW_MAX_COLS];   /* ^K line cut buffer (codepoints) */
	int   cut_len;
	int   cut_valid;

	struct tw_fs  fs;
	struct tw_ime ime;

	/* prompt / answer line */
	int   prompt;
	char  prompt_ans[TW_CMD_BUF];
	int   prompt_len;
	char  search_last[TW_CMD_BUF];

	/* ^R file picker (TW_PROMPT_PICK) */
	char  pick_name[TW_PICK_MAX][TW_PICK_NAMELEN];
	int   pick_count;       /* number of files listed */
	int   pick_sel;         /* selected index */
	int   pick_top;         /* first visible row (scroll) */

	/* Battery %, shown in the title bar. Refreshed by the periodic poll
	 * (KEY_REFRESH, every TW_BATT_POLL_MS). >= 0 is a percentage (drawn);
	 * negative means unavailable (no EC / read failed) and is hidden. */
	int   batt_pct;

	/* Power-saving mode: 1 while dimmed/idle (title shows [power saving]). */
	int   power_saving;

	/* framebuffer-derived geometry (pixels + derived cols/rows) */
	int   fb_w, fb_h;       /* screen pixels */
	int   text_x0, text_y0; /* top-left px of the text area */
	int   text_cols;        /* usable screen columns (of TW_CELL_PX px) */
	int   text_rows;        /* usable text rows (of TW_ROW_PX px) */
	int   bar_y;            /* top px of the candidate bar */
	int   hint_y;           /* top px of the shortcut-hint bar */

	/* Dirty tracking: the renderer repaints only what changed since the
	 * last frame, which is the key to typing latency (a full-screen repaint
	 * per keystroke is far too slow on a hi-res panel). Editing marks the
	 * touched text row(s) + the bar; scrolling marks dirty_all. */
	u8    dirty_row[TW_MAX_LINES];  /* per screen-row (0..text_rows-1) */
	int   dirty_all;                /* repaint entire text area */
	int   dirty_bar;                /* candidate/status bar changed */
	int   dirty_title;              /* title (filename/modified/mode) changed */
	int   first_paint;              /* draw static chrome once */
	int   last_cur_row, last_cur_col; /* to erase the old cursor cell */
	int   last_scroll_top;
};

#endif /* __CMD_TW_H */
