/* fs + printf stubs for the functional test (console/schedule live in functest.c) */
#include "fs.h"
int fs_set_blk_dev(const char *a,const char *b,int c){ (void)a;(void)b;(void)c; return 0; }
int fs_read(const char *f,unsigned long a,long long o,long long l,long long *r){ (void)f;(void)a;(void)o;(void)l; if(r)*r=0; return 0; }
int fs_write(const char *f,unsigned long a,long long o,long long l,long long *w){ (void)f;(void)a;(void)o;(void)l; if(w)*w=l; return 0; }
int fs_size(const char*f,long long*s){(void)f;if(s)*s=0;return 0;}
void udelay(unsigned long u){(void)u;}
