// SPDX-License-Identifier: GPL-2.0+
/*
 * cmd_tw_fs.c - filesystem load/save for the typewriter command.
 *
 * Adapted from the vim-for-Uboot `ved` reference, updated to the U-Boot 2026.07
 * header set (no <common.h>) and to the codepoint line model: files are UTF-8
 * on disk, decoded into u32 codepoint lines on load and re-encoded on save.
 *
 * The I/O scratch buffer is malloc()'d from U-Boot's heap, NOT staged at a
 * fixed CONFIG_SYS_LOAD_ADDR like the ved reference. On this board (RK3399)
 * CONFIG_SYS_LOAD_ADDR is 0x800800, but U-Boot proper runs from DRAM at
 * 0x00800000 with its heap right above - so a multi-MB buffer there overwrote
 * U-Boot's own image and the FAT driver's directory/table buffers, which were
 * then flushed to the card and CORRUPTED THE PARTITION. malloc() hands out a
 * region distinct from both, so there is no overlap. fs_set_blk_dev() is called
 * before every op because U-Boot's FS layer doesn't retain the active device.
 */
#include <command.h>
#include <fs.h>
#include <mapmem.h>
#include <malloc.h>
#include <linux/string.h>
#include <linux/errno.h>
#include "cmd_tw.h"

int tw_fs_str_to_type(const char *s)
{
	if (!strcmp(s, "fat") || !strcmp(s, "vfat"))
		return FS_TYPE_FAT;
	if (!strcmp(s, "ext4"))
		return FS_TYPE_EXT;
	if (!strcmp(s, "squashfs"))
		return FS_TYPE_SQUASHFS;
	return FS_TYPE_ANY;
}

int tw_fs_probe(struct tw_fs *fs, const char *iftype,
		const char *dev_part, const char *fstype)
{
	int fs_type;

	if (!iftype || !dev_part)
		return -1;

	strncpy(fs->iftype, iftype, sizeof(fs->iftype) - 1);
	strncpy(fs->dev_part, dev_part, sizeof(fs->dev_part) - 1);
	fs->iftype[sizeof(fs->iftype) - 1] = '\0';
	fs->dev_part[sizeof(fs->dev_part) - 1] = '\0';

	if (fstype) {
		strncpy(fs->fstype, fstype, sizeof(fs->fstype) - 1);
		fs->fstype[sizeof(fs->fstype) - 1] = '\0';
		fs_type = tw_fs_str_to_type(fstype);
	} else {
		fs->fstype[0] = '\0';
		fs_type = FS_TYPE_ANY;
	}

	if (fs_set_blk_dev(fs->iftype, fs->dev_part, fs_type) != 0) {
		fs->valid = 0;
		return -1;
	}

	fs->valid = 1;
	return 0;
}

static int tw_fs_set(struct tw_fs *fs)
{
	int fs_type;

	if (!fs->valid)
		return -1;

	fs_type = fs->fstype[0] ? tw_fs_str_to_type(fs->fstype) : FS_TYPE_ANY;

	return fs_set_blk_dev(fs->iftype, fs->dev_part, fs_type);
}

/* Decode one UTF-8 codepoint from s (len bytes available). Returns bytes
 * consumed and sets *cp. Malformed input consumes 1 byte and yields U+FFFD. */
static int utf8_decode(const char *s, int len, u32 *cp)
{
	unsigned char c = (unsigned char)s[0];

	if (c < 0x80) {
		*cp = c;
		return 1;
	}
	if ((c & 0xE0) == 0xC0 && len >= 2) {
		*cp = ((u32)(c & 0x1F) << 6) | ((unsigned char)s[1] & 0x3F);
		return 2;
	}
	if ((c & 0xF0) == 0xE0 && len >= 3) {
		*cp = ((u32)(c & 0x0F) << 12) |
		      (((unsigned char)s[1] & 0x3F) << 6) |
		      ((unsigned char)s[2] & 0x3F);
		return 3;
	}
	if ((c & 0xF8) == 0xF0 && len >= 4) {
		*cp = ((u32)(c & 0x07) << 18) |
		      (((unsigned char)s[1] & 0x3F) << 12) |
		      (((unsigned char)s[2] & 0x3F) << 6) |
		      ((unsigned char)s[3] & 0x3F);
		return 4;
	}
	*cp = 0xFFFD;
	return 1;
}

/* Encode codepoint cp into buf (>= 4 bytes). Returns bytes written. */
static int utf8_encode(u32 cp, char *buf)
{
	if (cp < 0x80) {
		buf[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		buf[0] = (char)(0xC0 | (cp >> 6));
		buf[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		buf[0] = (char)(0xE0 | (cp >> 12));
		buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		buf[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	buf[0] = (char)(0xF0 | (cp >> 18));
	buf[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	buf[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	buf[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

static void tw_empty_buffer(struct tw_state *s)
{
	s->num_lines = 1;
	s->line_len[0] = 0;
}

/* Append codepoint cp to the current last line, splitting lines on '\n'. */
static void tw_load_cp(struct tw_state *s, u32 cp)
{
	int row = s->num_lines - 1;

	if (cp == '\n') {
		if (s->num_lines < TW_MAX_LINES) {
			s->line_len[s->num_lines] = 0;
			s->num_lines++;
		}
		return;
	}
	if (cp == '\r')
		return;                 /* drop CR of CRLF */
	if (s->line_len[row] < TW_MAX_COLS - 1)
		s->lines[row][s->line_len[row]++] = cp;
	/* else: line full, silently truncate the rest of this line */
}

int tw_file_load(struct tw_state *s, const char *path)
{
	loff_t len_read = 0, fsize = 0;
	size_t cap;
	char  *buf;
	ulong  addr;
	int    ret, off;

	/* Query the size first and allocate exactly that (+1 guard), rather than
	 * a fixed multi-MB buffer held across fs_read() - same heap-hygiene
	 * reason as tw_file_save(): keep the pool clean for the FS layer's own
	 * allocations. A missing file -> empty buffer. */
	if (tw_fs_set(&s->fs) != 0)
		return -1;
	ret = fs_size(path, &fsize);
	if (ret < 0 || fsize <= 0) {
		tw_empty_buffer(s);              /* new/empty file */
		return 0;
	}
	if (fsize > TW_FILE_BUF_SIZE)        /* clamp pathologically large files */
		fsize = TW_FILE_BUF_SIZE;

	cap = (size_t)fsize + 1;
	buf = malloc(cap);
	if (!buf)
		return -1;
	addr = map_to_sysmem(buf);       /* physical addr for fs_read() */

	/* fs_size() cleared the active device; re-assert before fs_read(). */
	if (tw_fs_set(&s->fs) != 0) {
		free(buf);
		return -1;
	}

	ret = fs_read(path, addr, 0, (loff_t)fsize, &len_read);
	if (ret < 0) {
		free(buf);
		if (ret == -ENOENT || len_read == 0) {
			tw_empty_buffer(s);     /* new file: empty buffer */
			return 0;
		}
		return ret;
	}

	tw_empty_buffer(s);
	s->line_len[0] = 0;
	s->num_lines = 1;

	off = 0;
	while (off < (int)len_read) {
		u32 cp;

		off += utf8_decode(buf + off, (int)len_read - off, &cp);
		tw_load_cp(s, cp);
	}

	free(buf);
	return 0;
}

/* Compute the exact UTF-8 byte length of the whole buffer (content + one
 * newline per line), so we can allocate precisely that much. */
static size_t tw_serialized_len(struct tw_state *s)
{
	size_t n = 0;
	int i, j;
	char tmp[4];

	for (i = 0; i < s->num_lines; i++) {
		for (j = 0; j < s->line_len[i]; j++)
			n += utf8_encode(s->lines[i][j], tmp);
		n += 1;                          /* newline */
	}
	return n;
}

int tw_file_save(struct tw_state *s)
{
	size_t need = tw_serialized_len(s);
	size_t cap  = need + 8;              /* small slack; bounds-checked below */
	char  *buf, *p;
	ulong  addr;
	loff_t written = 0;
	int    ret, i, j;

	/*
	 * Allocate EXACTLY what this file needs, not a fixed multi-MB buffer.
	 * The previous fixed 3-4 MB allocation was held across fs_write() while
	 * U-Boot's FAT writer did its own malloc()/strdup()/malloc_cache_aligned
	 * for the directory iterator and LFN scratch; a large outstanding
	 * allocation can fragment/stress the pool so those land on bad memory,
	 * corrupting the on-disk directory. A tight buffer keeps the heap clean.
	 */
	buf = malloc(cap);
	if (!buf)
		return -1;
	addr = map_to_sysmem(buf);           /* physical addr for fs_write() */

	p = buf;
	for (i = 0; i < s->num_lines; i++) {
		for (j = 0; j < s->line_len[i]; j++) {
			if ((size_t)(p - buf) + 4 > cap)   /* hard overflow guard */
				goto overflow;
			p += utf8_encode(s->lines[i][j], p);
		}
		if ((size_t)(p - buf) + 1 > cap)
			goto overflow;
		*p++ = '\n';
	}

	if (tw_fs_set(&s->fs) != 0) {
		free(buf);
		return -1;
	}

	{
		loff_t total = (loff_t)(p - buf);

		ret = fs_write(s->filename, addr, 0, total, &written);
		free(buf);
		if (ret < 0 || written != total)
			return -1;
		s->last_write_bytes = (int)total;  /* on-screen diagnostic */
	}
	s->dirty = 0;
	return 0;

overflow:
	free(buf);
	return -1;
}
