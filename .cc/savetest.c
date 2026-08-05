/* Capture the exact bytes tw_file_save hands to fs_write, for a small buffer.
 * Proves our serialization is clean (right length, right UTF-8, no overflow),
 * isolating our code from U-Boot's FAT layer. */
#include <stdio.h>
#include <string.h>
#include "cmd_tw.h"
#include "wubi_embed.h"
int tw_cp_cols(u32 cp){return cp>=0x80?2:1;}
void tw_render(struct tw_state*s){(void)s;}
int tw_video_init(struct tw_state*s){s->fb_w=640;s->fb_h=480;s->text_cols=78;s->text_rows=26;return 0;}
void schedule(void){} int getchar(void){return 0;} int tstc(void){return 1;}

/* capture fs_write */
static unsigned char cap[64]; static int caplen=-1; static char capname[64];
int fs_set_blk_dev(const char*a,const char*b,int c){(void)a;(void)b;(void)c;return 0;}
int fs_size(const char*f,long long*s){(void)f;if(s)*s=0;return 0;}
int fs_read(const char*f,unsigned long a,long long o,long long l,long long*r){(void)f;(void)a;(void)o;(void)l;if(r)*r=0;return 0;}
int fs_write(const char*f,unsigned long a,long long o,long long l,long long*w){
    (void)o; strncpy(capname,f,63);
    caplen=(int)l; if(l<=64) memcpy(cap,(void*)(size_t)a,l);
    if(w)*w=l; return 0;
}
#define U_BOOT_CMD(...) struct _u;
#include "cmd_tw.c"

int main(void){
    struct tw_state*s=&g_tw; int fails=0;
    memset(s,0,sizeof(*s)); tw_video_init(s);
    strncpy(s->filename,"a.txt",TW_MAX_FILENAME-1);
    s->writable=1; s->fs.valid=1;
    /* line 0: 你好 (U+4F60 U+597D), line 1: abc */
    s->num_lines=2;
    s->lines[0][0]=0x4F60; s->lines[0][1]=0x597D; s->line_len[0]=2;
    s->lines[1][0]='a'; s->lines[1][1]='b'; s->lines[1][2]='c'; s->line_len[1]=3;
    tw_do_save(s);
    /* expected: e4 bd a0 e5 a5 bd 0a 61 62 63 0a  (你好\nabc\n) = 11 bytes */
    unsigned char want[]={0xe4,0xbd,0xa0,0xe5,0xa5,0xbd,0x0a,'a','b','c',0x0a};
    int ok = caplen==(int)sizeof(want) && memcmp(cap,want,sizeof(want))==0
             && strcmp(capname,"a.txt")==0;
    printf("name='%s' size=%d (want %zu)\n",capname,caplen,sizeof(want));
    printf("bytes:"); for(int i=0;i<caplen&&i<20;i++)printf(" %02x",cap[i]); printf("\n");
    printf("status: %s\n", s->status_msg);
    printf("%s\n", ok?"PASS serialization exact":"FAIL");
    if(!ok)fails++;
    return fails?1:0;
}
