/* font_data.h - interface to the embedded Unifont bitmap subset.
 * The glyph array is defined in the auto-generated font_data.c (see gen.sh). */
#ifndef __TW_FONT_DATA_H
#define __TW_FONT_DATA_H

#include <linux/types.h>

/* One glyph: 16 rows. `width` is 8 (narrow/ASCII) or 16 (wide/CJK) pixels.
 * rows[] is 16 rows x 2 bytes; a narrow glyph uses only rows[r*2+0] (MSB-first,
 * 8 pixels), a wide glyph uses rows[r*2+0] (left 8px) and rows[r*2+1] (right
 * 8px). Sorted ascending by cp so the renderer can binary-search. */
struct tw_glyph {
	u32 cp;
	u8  width;
	u8  rows[32];
};

extern const struct tw_glyph tw_glyphs[];
extern const unsigned tw_glyphs_count;

#endif /* __TW_FONT_DATA_H */
