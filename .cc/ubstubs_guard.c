#include "fs.h"
extern int g_writes;
int fs_set_blk_dev(const char*a,const char*b,int c){(void)a;(void)b;(void)c;return 0;}
int fs_read(const char*f,unsigned long a,long long o,long long l,long long*r){(void)f;(void)a;(void)o;(void)l;if(r)*r=0;return 0;}
int fs_write(const char*f,unsigned long a,long long o,long long l,long long*w){(void)f;(void)a;(void)o;g_writes++;if(w)*w=l;return 0;}
int fs_size(const char*f,long long*s){(void)f;if(s)*s=0;return 0;}
