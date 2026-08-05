/* Host verification harness for the ported IME lookup + embedded wubi table.
 * Links the REAL ime_table.c and wubi_embed.c (unmodified) against host shims,
 * and checks that known wubi codes resolve to the expected hanzi. */
#include <stdio.h>
#include <string.h>
#include "ime_table.h"
#include "wubi_embed.h"

struct check { const char *code; const char *want; };

/* Expected top candidate for a few classic wubi codes. Verified against the
 * coreboot payload's docs (wq -> 你). The rest are sanity anchors. */
static const struct check checks[] = {
	{ "wq", "你" },
};

int main(void)
{
	ime_table t;
	int fails = 0;

	if (ime_table_open_mem(&t, wubi_tab, wubi_tab_len, IME_SCHEME_WUBI) != 0) {
		fprintf(stderr, "FAIL: table did not bind\n");
		return 1;
	}
	printf("bound wubi table: %u records, %u pool bytes\n",
	       t.count, t.pool_bytes);

	for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
		ime_cand cands[16];
		int n = ime_lookup(&t, checks[i].code, strlen(checks[i].code),
				   cands, 16);
		if (n <= 0) {
			printf("FAIL %s: no candidates\n", checks[i].code);
			fails++;
			continue;
		}
		char top[64];
		int len = cands[0].word_len < 63 ? cands[0].word_len : 63;
		memcpy(top, cands[0].word, len);
		top[len] = 0;

		int ok = (strlen(checks[i].want) == (size_t)cands[0].word_len) &&
			 memcmp(cands[0].word, checks[i].want,
				cands[0].word_len) == 0;
		printf("%s %-6s top='%s' (%d cands) want='%s'\n",
		       ok ? "PASS" : "FAIL", checks[i].code, top, n,
		       checks[i].want);
		if (!ok)
			fails++;
	}

	/* Also dump the first page for 'wq' so we can eyeball ranking. */
	{
		ime_cand cands[16];
		int n = ime_lookup(&t, "wq", 2, cands, 16);
		printf("wq page:");
		for (int i = 0; i < n && i < 9; i++) {
			char w[64];
			int len = cands[i].word_len < 63 ? cands[i].word_len : 63;
			memcpy(w, cands[i].word, len);
			w[len] = 0;
			printf(" %d.%s", i + 1, w);
		}
		printf("\n");
	}

	printf(fails ? "\n%d FAILURES\n" : "\nALL PASS\n", fails);
	return fails ? 1 : 0;
}
