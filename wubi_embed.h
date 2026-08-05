/* wubi_embed.h - the Wubi 86 `.tab` image compiled into the binary.
 * The array itself is in the generated wubi_embed.c (see gen.sh). */
#ifndef __WUBI_EMBED_H
#define __WUBI_EMBED_H

#include <linux/types.h>

extern const unsigned char wubi_tab[];
extern const unsigned long wubi_tab_len;

#endif /* __WUBI_EMBED_H */
