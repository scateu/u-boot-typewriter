#ifndef _STUB_FS_H
#define _STUB_FS_H
#include <linux/types.h>
#define FS_TYPE_ANY 0
#define FS_TYPE_FAT 1
#define FS_TYPE_EXT 2
#define FS_TYPE_SQUASHFS 6
int fs_set_blk_dev(const char *ifname, const char *dev_part_str, int fstype);
int fs_size(const char *filename, loff_t *size);
int fs_read(const char *filename, ulong addr, loff_t offset, loff_t len, loff_t *actread);
int fs_write(const char *filename, ulong addr, loff_t offset, loff_t len, loff_t *actwrite);
#endif
