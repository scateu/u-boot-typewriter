/* ime_table.h - in-memory lookup for a wubi-fep `.tab` image.
 *
 * Ported from wubi-fep's table.h, trimmed to the in-memory (embedded) path:
 * no mmap, no file I/O. The table image is compiled into the U-Boot binary
 * (see wubi_embed.c) and bound with ime_table_open_mem(). See gen_table.py in
 * the wubi-fep repo for the on-disk format this reads.
 */
#ifndef __IME_TABLE_H
#define __IME_TABLE_H

#include <linux/types.h>

#define IME_CODE_LEN 16   /* fits multi-syllable pinyin phrases; wubi uses <=4 */
#define IME_INDEX_ENTRIES (26 * 26 + 1)   /* 677 */

#define IME_SCHEME_WUBI   0
#define IME_SCHEME_PINYIN 1

/* One on-disk record: fixed (IME_CODE_LEN + 6) bytes, little-endian, sorted by
 * (code, rank). Must stay byte-identical to the generator's layout. */
typedef struct {
	char     code[IME_CODE_LEN]; /* ASCII a-z, NUL-padded */
	u32      pool_off;           /* byte offset into the string pool */
	u8       word_len;           /* UTF-8 byte length of the word (1..255) */
	u8       rank;               /* 0 = best (most common) for this code */
} __attribute__((packed)) ime_record;

typedef struct {
	const u8         *base;       /* table image base */
	size_t            size;       /* image size */
	int               scheme;     /* IME_SCHEME_* */
	u32               count;      /* number of records */
	u32               pool_bytes;
	const u32        *index;      /* IME_INDEX_ENTRIES prefix lower-bounds */
	const ime_record *records;    /* count records */
	const char       *pool;       /* pool_bytes UTF-8 bytes */
} ime_table;

/* A single candidate returned by a lookup. `word`/`word_len` point into the
 * table's pool (not NUL-terminated). `exact` is 1 when the record's code
 * equals the query exactly, 0 when it is only a prefix extension. */
typedef struct {
	const char *word;
	u8          word_len;
	u8          rank;
	int         exact;
} ime_cand;

/* Bind `t` to an in-memory table image (compiled into the binary). The buffer
 * must outlive `t`. Returns 0 on success, -1 on a malformed image. */
int ime_table_open_mem(ime_table *t, const void *buf, size_t size,
		       int expect_scheme);

/* Fill `out` (capacity `max`) with candidates whose code starts with `query`
 * (lowercase a-z, length 1..IME_CODE_LEN). Exact matches first (by rank), then
 * prefix extensions (by code then rank). Returns the number written. */
int ime_lookup(const ime_table *t, const char *query, size_t qlen,
	       ime_cand *out, int max);

#endif /* __IME_TABLE_H */
