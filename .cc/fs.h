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
#define FS_DT_DIR 4
#define FS_DT_REG 8
#define FS_DIRENT_NAME_LEN 256
struct fs_dirent { unsigned int type; long long size; char name[FS_DIRENT_NAME_LEN]; };
struct fs_dir_stream { int _x; };
struct fs_dir_stream *fs_opendir(const char *filename);
struct fs_dirent *fs_readdir(struct fs_dir_stream *dirs);
void fs_closedir(struct fs_dir_stream *dirs);
#endif
