#include <stdio.h>
#include <string.h>
#include "cmd_tw.h"
#include "wubi_embed.h"

/* Stub the video side so cmd_tw.c's tw_render() call is a no-op and geometry
 * is fixed. These satisfy the externs cmd_tw.c expects from cmd_tw_video.c. */
int tw_cp_cols(u32 cp){ return cp >= 0x80 ? 2 : 1; }
void tw_render(struct tw_state *s){ (void)s; }
int tw_video_init(struct tw_state *s){
    s->fb_w=640; s->fb_h=480; s->text_x0=8; s->text_y0=16;
    s->text_cols=78; s->text_rows=26; s->bar_y=432; s->hint_y=448; return 0;
}
/* U-Boot externs */
void schedule(void){}
int getchar(void){ return 0; }
int tstc(void){ return 1; }

/* Pull in the editor logic; we need its static functions, so #include the .c.
 * Redefine do_typewriter's registration away by neutralizing U_BOOT_CMD. */
#define U_BOOT_CMD(...) struct _tw_unused_reg;
#include "cmd_tw.c"

/* Reach the statics we test by declaring thin wrappers here isn't possible for
 * statics; instead re-expose via the fact that this TU now owns them. */
static void feed(struct tw_state *s, const char *keys){
    for (const char *p=keys; *p; p++) tw_handle_key(s, (unsigned char)*p);
}

static void dump_line0(struct tw_state *s, char *out){
    int n=0;
    for (int i=0;i<s->line_len[0];i++){
        u32 cp=s->lines[0][i];
        if (cp<0x80) out[n++]=(char)cp;
        else if (cp<0x800){ out[n++]=0xC0|(cp>>6); out[n++]=0x80|(cp&0x3F);}
        else { out[n++]=0xE0|(cp>>12); out[n++]=0x80|((cp>>6)&0x3F); out[n++]=0x80|(cp&0x3F);}
    }
    out[n]=0;
}

int main(void){
    struct tw_state *s=&g_tw;
    int fails=0;
    memset(s,0,sizeof(*s));
    tw_video_init(s);
    s->num_lines=1; s->line_len[0]=0;
    /* bind IME */
    tw_bind_ime(s);
    if(!s->ime.ready){ printf("FAIL: ime not ready\n"); return 1; }

    /* Test 1: English typing */
    s->ime.mode=TW_IME_OFF;
    feed(s,"hi");
    char buf[256]; dump_line0(s,buf);
    printf("%s english: '%s'\n", strcmp(buf,"hi")==0?"PASS":"FAIL", buf);
    if(strcmp(buf,"hi")) fails++;

    /* Test 2: Wubi wq -> commit first candidate (你) via Space */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_WUBI;
    feed(s,"wq ");  /* w q then Space commits candidate 0 */
    dump_line0(s,buf);
    printf("%s wubi wq+Space: '%s' (want 你)\n", strcmp(buf,"你")==0?"PASS":"FAIL", buf);
    if(strcmp(buf,"你")) fails++;

    /* Test 3: Wubi wq then digit '1' commits candidate 0 too */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_WUBI;
    feed(s,"wq1");
    dump_line0(s,buf);
    printf("%s wubi wq+1: '%s' (want 你)\n", strcmp(buf,"你")==0?"PASS":"FAIL", buf);
    if(strcmp(buf,"你")) fails++;

    /* Test 4: newline splits line */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    feed(s,"ab"); tw_handle_key(s, KEY_ENTER); feed(s,"cd");
    printf("%s newline: lines=%d len0=%d len1=%d\n",
        (s->num_lines==2 && s->line_len[0]==2 && s->line_len[1]==2)?"PASS":"FAIL",
        s->num_lines, s->line_len[0], s->line_len[1]);
    if(!(s->num_lines==2 && s->line_len[0]==2 && s->line_len[1]==2)) fails++;

    /* Test 5: backspace merges lines */
    s->cur_row=1; s->cur_col=0; tw_handle_key(s, KEY_BS);
    printf("%s bs-merge: lines=%d len0=%d\n",
        (s->num_lines==1 && s->line_len[0]==4)?"PASS":"FAIL",
        s->num_lines, s->line_len[0]);
    if(!(s->num_lines==1 && s->line_len[0]==4)) fails++;

    /* Test 6: ^K cut then ^U paste restores a line */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    feed(s,"xy"); s->cur_col=0;
    tw_handle_key(s, KEY_CTRL_K);  /* cut -> empties the only line */
    tw_handle_key(s, KEY_CTRL_U);  /* paste it back */
    dump_line0(s,buf);
    printf("%s cut/paste: '%s' (want xy)\n", strcmp(buf,"xy")==0?"PASS":"FAIL", buf);
    if(strcmp(buf,"xy")) fails++;

    printf(fails?"\n%d FAIL\n":"\nALL PASS\n", fails);
    return fails?1:0;
}
