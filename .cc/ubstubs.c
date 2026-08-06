/* Minimal implementations of the U-Boot externs, so the four command sources
 * (plus generated data) link cleanly - proving all cross-file signatures and
 * data symbols resolve. Not meant to run; just to link. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "video.h"
#include "dm.h"
#include "fs.h"

int getchar(void){ return 0; }
int tstc(void){ return 0; }
void putc(const char c){ (void)c; }
void puts(const char *s){ (void)s; }
void schedule(void){}
int video_sync(struct udevice *v, int f){ (void)v;(void)f; return 0; }
void video_damage(struct udevice *v,int x,int y,int w,int h){(void)v;(void)x;(void)y;(void)w;(void)h;}
int uclass_first_device_err(enum uclass_id id, struct udevice **d){ (void)id;(void)d; return 0; }
void *dev_get_uclass_priv(const struct udevice *d){ (void)d; return 0; }
int fs_set_blk_dev(const char *a,const char *b,int c){ (void)a;(void)b;(void)c; return 0; }
int fs_size(const char *f,long long *sz){ (void)f; if(sz)*sz=0; return 0; }
int fs_read(const char *f,unsigned long a,long long o,long long l,long long *r){ (void)f;(void)a;(void)o;(void)l; if(r)*r=0; return 0; }
int fs_write(const char *f,unsigned long a,long long o,long long l,long long *w){ (void)f;(void)a;(void)o;(void)l; if(w)*w=l; return 0; }
/* printf/snprintf/vsnprintf: forward to libc (names match) */

int main(void){ return 0; }
int cmd_usage(const struct cmd_tbl *c){(void)c;return 0;}
void udelay(unsigned long u){(void)u;}
struct fs_dir_stream *fs_opendir(const char *f){ (void)f; return 0; }
struct fs_dirent *fs_readdir(struct fs_dir_stream *d){ (void)d; return 0; }
void fs_closedir(struct fs_dir_stream *d){ (void)d; }
int cros_ec_reboot(struct udevice *d, int c, unsigned char f){(void)d;(void)c;(void)f;return 0;}
int cros_ec_battery_cutoff(struct udevice *d, unsigned char f){(void)d;(void)f;return 0;}
unsigned long invoke_psci_fn(unsigned long a,unsigned long b,unsigned long c,unsigned long d){(void)a;(void)b;(void)c;(void)d;return 0;}
void enable_interrupts(void){}
int disable_interrupts(void){return 0;}
int uclass_get_device_by_name(enum uclass_id id,const char*n,struct udevice**d){(void)id;(void)n;if(d)*d=0;return 0;}
