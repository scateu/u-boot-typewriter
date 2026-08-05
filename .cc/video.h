#ifndef _STUB_VIDEO_H
#define _STUB_VIDEO_H
#include <linux/types.h>
enum video_log2_bpp { VIDEO_BPP1=0,VIDEO_BPP2,VIDEO_BPP4,VIDEO_BPP8,VIDEO_BPP16,VIDEO_BPP32 };
#define VNBYTES(bpix) ((1 << (bpix)) / 8)
enum video_format { VIDEO_UNKNOWN, VIDEO_RGBA8888, VIDEO_X8B8G8R8, VIDEO_X8R8G8B8, VIDEO_X2R10G10B10 };
struct video_priv {
	ushort xsize, ysize, rot;
	enum video_log2_bpp bpix;
	enum video_format format;
	const char *vidconsole_drv_name;
	int font_size;
	void *fb;
	int fb_size;
	void *copy_fb;
	struct { int xstart, ystart, xend, yend; } damage;
	int line_length;
	u32 colour_fg, colour_bg;
	int flush_dcache;
	u8 fg_col_idx, bg_col_idx;
	ulong last_sync;
	int white_on_black;
};
struct udevice;
void video_damage(struct udevice *vid,int x,int y,int w,int h);
int video_sync(struct udevice *vid, int force);
#endif
