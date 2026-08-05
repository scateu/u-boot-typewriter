#include <stdio.h>
#include <string.h>
#include "cmd_tw.h"
#include "wubi_embed.h"
int tw_cp_cols(u32 cp){ return cp>=0x80?2:1; }
void tw_render(struct tw_state *s){ (void)s; }
int tw_video_init(struct tw_state *s){ s->fb_w=640;s->fb_h=480;s->text_x0=8;s->text_y0=16;s->text_cols=78;s->text_rows=26;s->bar_y=432;s->hint_y=448;return 0; }
void schedule(void){} int getchar(void){return 0;} int tstc(void){return 1;}
/* count fs_write calls to prove read-only never writes */
int g_writes=0;
#define U_BOOT_CMD(...) struct _u;
#include "cmd_tw.c"
int main(void){
    struct tw_state *s=&g_tw; int fails=0;
    /* read-only: save must be refused, status says read-only, dirty stays */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1;
    strncpy(s->filename,"a.txt",TW_MAX_FILENAME-1);
    s->writable=0; s->dirty=1; g_writes=0;
    tw_do_save(s);
    int ro_ok = (g_writes==0) && (s->dirty==1) && strstr(s->status_msg,"Read-only");
    printf("%s read-only refuses save: writes=%d dirty=%d msg='%s'\n",
        ro_ok?"PASS":"FAIL", g_writes, s->dirty, s->status_msg);
    if(!ro_ok) fails++;
    /* writable: save proceeds (fs_write called once), dirty cleared */
    s->writable=1; s->dirty=1; g_writes=0; s->fs.valid=1;
    tw_do_save(s);
    int rw_ok = (g_writes==1) && (s->dirty==0);
    printf("%s rw saves: writes=%d dirty=%d msg='%s'\n",
        rw_ok?"PASS":"FAIL", g_writes, s->dirty, s->status_msg);
    if(!rw_ok) fails++;
    printf(fails?"\n%d FAIL\n":"\nALL PASS\n",fails);
    return fails?1:0;
}
