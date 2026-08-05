/* ime_table.c - in-memory `.tab` lookup, ported from wubi-fep's table.c.
 *
 * The host-only bits of the original (mmap/open/fstat, fprintf) are gone: this
 * only binds an already-in-memory image and does the prefix-window binary
 * search. Validation failures return -1 (the caller reports to the user).
 */
#include "ime_table.h"
#include <linux/string.h>

static const char MAGIC[4] = { 'I', 'M', 'E', 'T' };

/* Header layout (see gen_table.py):
 *   magic[4] version[1] scheme[1] codeLen[2] count[4] poolBytes[4]  = 16
 *   index[677 * u32]
 *   records[count * (codeLen + 6)]   code[codeLen] off[4] wordLen[1] rank[1]
 *   pool[poolBytes]
 */
#define HDR_SIZE 16
#define IDX_SIZE (IME_INDEX_ENTRIES * 4)

int ime_table_open_mem(ime_table *t, const void *buf, size_t size,
		       int expect_scheme)
{
	const u8 *b = (const u8 *)buf;
	u16 code_len;
	u32 count, pool_bytes;
	size_t need;
	u8 scheme;

	memset(t, 0, sizeof(*t));

	if (size < (size_t)HDR_SIZE + IDX_SIZE)
		return -1;
	if (memcmp(b, MAGIC, 4) != 0)
		return -1;

	scheme = b[5];
	/* Header bytes 6-7 hold the code width the table was built with; it must
	 * match IME_CODE_LEN or the fixed-width records would be misaligned. */
	memcpy(&code_len, b + 6, 2);
	if (code_len != IME_CODE_LEN)
		return -1;

	memcpy(&count, b + 8, 4);
	memcpy(&pool_bytes, b + 12, 4);

	need = (size_t)HDR_SIZE + IDX_SIZE
	     + (size_t)count * sizeof(ime_record) + pool_bytes;
	if (need > size)
		return -1;
	if (expect_scheme >= 0 && scheme != expect_scheme)
		return -1;

	t->base       = b;
	t->size       = size;
	t->scheme     = scheme;
	t->count      = count;
	t->pool_bytes = pool_bytes;
	t->index      = (const u32 *)(b + HDR_SIZE);
	t->records    = (const ime_record *)(b + HDR_SIZE + IDX_SIZE);
	t->pool       = (const char *)(b + HDR_SIZE + IDX_SIZE
				       + (size_t)count * sizeof(ime_record));
	return 0;
}

/* Compare a record's fixed-width code against a query. Returns <0/0/>0 like
 * strcmp over the first min(code_len, qlen) chars, treating NUL-padding as
 * end-of-code. 0 means the code prefix-matches the query for its full length. */
static int code_cmp(const char *code, const char *q, size_t qlen)
{
	size_t i;

	for (i = 0; i < IME_CODE_LEN && i < qlen; i++) {
		unsigned char rc = (unsigned char)code[i];
		unsigned char qc = (unsigned char)q[i];

		if (rc == 0)               /* code shorter than query */
			return -1;
		if (rc != qc)
			return (rc < qc) ? -1 : 1;
	}
	return 0;
}

/* Binary-search [lo,hi) for the first record whose code is a
 * prefix-match-or-greater than the query. */
static u32 lower_bound(const ime_table *t, u32 lo, u32 hi,
		       const char *q, size_t qlen)
{
	while (lo < hi) {
		u32 mid = lo + (hi - lo) / 2;

		if (code_cmp(t->records[mid].code, q, qlen) < 0)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

int ime_lookup(const ime_table *t, const char *query, size_t qlen,
	       ime_cand *out, int max)
{
	u32 win_lo, win_hi, lo, i;
	int c0, c1, n;

	if (qlen == 0 || qlen > IME_CODE_LEN || max <= 0)
		return 0;

	c0 = query[0] - 'a';
	c1 = (qlen >= 2) ? (query[1] - 'a') : 0;
	if (c0 < 0 || c0 > 25 || c1 < 0 || c1 > 25)
		return 0;

	/* Prefix index gives a coarse window. For a 1-letter query, the whole
	 * first-letter block spans index[c0*26] .. index[(c0+1)*26]. For >=2
	 * letters, the two-letter cell index[c0*26+c1] .. next cell. */
	if (qlen == 1) {
		win_lo = t->index[c0 * 26];
		win_hi = (c0 + 1 <= 25) ? t->index[(c0 + 1) * 26] : t->count;
	} else {
		int k = c0 * 26 + c1;

		win_lo = t->index[k];
		win_hi = (k + 1 < IME_INDEX_ENTRIES) ? t->index[k + 1] : t->count;
	}
	if (win_lo > t->count)
		win_lo = t->count;
	if (win_hi > t->count)
		win_hi = t->count;

	/* Narrow to records that actually prefix-match the query. */
	lo = lower_bound(t, win_lo, win_hi, query, qlen);

	/* First pass: exact-code matches (code length == qlen). Then prefix
	 * extensions. Both passes preserve the on-disk (code, rank) order. */
	n = 0;
	for (i = lo; i < win_hi && n < max; i++) {
		const ime_record *r = &t->records[i];
		int exact;

		if (code_cmp(r->code, query, qlen) != 0)
			break; /* window is sorted; no more prefix hits */
		exact = (r->code[qlen] == '\0' || qlen == IME_CODE_LEN) ? 1 : 0;
		if (!exact)
			continue;
		out[n].word     = t->pool + r->pool_off;
		out[n].word_len = r->word_len;
		out[n].rank     = r->rank;
		out[n].exact    = 1;
		n++;
	}
	for (i = lo; i < win_hi && n < max; i++) {
		const ime_record *r = &t->records[i];
		int exact;

		if (code_cmp(r->code, query, qlen) != 0)
			break;
		exact = (r->code[qlen] == '\0' || qlen == IME_CODE_LEN) ? 1 : 0;
		if (exact)
			continue;
		out[n].word     = t->pool + r->pool_off;
		out[n].word_len = r->word_len;
		out[n].rank     = r->rank;
		out[n].exact    = 0;
		n++;
	}
	return n;
}
