/* fs + misc stubs for the functional test. Provides a tiny in-memory "disk"
 * (a few named files) so switch-file save/load can round-trip. Console and
 * schedule live in functest.c. */
#include "fs.h"
#include <string.h>
#include <stdint.h>

int fs_set_blk_dev(const char *a, const char *b, int c)
{ (void)a; (void)b; (void)c; return 0; }

/* up to 4 files, 4 KiB each */
#define NF 4
static char  df_name[NF][64];
static char  df_data[NF][4096];
static long  df_len[NF];
static int   df_count;

static int find(const char *f)
{
	for (int i = 0; i < df_count; i++)
		if (!strcmp(df_name[i], f)) return i;
	return -1;
}

int fs_size(const char *f, long long *s)
{
	int i = find(f);
	if (i < 0) { if (s) *s = 0; return -1; }
	if (s) *s = df_len[i];
	return 0;
}

int fs_read(const char *f, unsigned long a, long long o, long long l, long long *r)
{
	(void)o; (void)l;
	int i = find(f);
	if (i < 0) { if (r) *r = 0; return -1; }
	memcpy((void *)(uintptr_t)a, df_data[i], df_len[i]);
	if (r) *r = df_len[i];
	return 0;
}

int fs_write(const char *f, unsigned long a, long long o, long long l, long long *w)
{
	(void)o;
	int i = find(f);
	if (i < 0) {
		if (df_count >= NF) { if (w) *w = 0; return -1; }
		i = df_count++;
		strncpy(df_name[i], f, 63);
	}
	if (l > 4096) l = 4096;
	memcpy(df_data[i], (void *)(uintptr_t)a, l);
	df_len[i] = l;
	if (w) *w = l;
	return 0;
}

void udelay(unsigned long u) { (void)u; }
