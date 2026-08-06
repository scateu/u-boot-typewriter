#include <stdio.h>
#include <string.h>
#include "cmd_tw.h"
#include "wubi_embed.h"

/* Stub the video side so cmd_tw.c's tw_render() call is a no-op and geometry
 * is fixed. These satisfy the externs cmd_tw.c expects from cmd_tw_video.c. */
int tw_cp_cols(u32 cp){ return cp >= 0x80 ? 2 : 1; }
/* mirror of cmd_tw_video.c's wrap-row counter, so scroll_adjust + wrap tests
 * work in the harness (same wrap-before-wide rule). */
int tw_line_rows(struct tw_state *s, int fr){
    int i, col=0, rows=1;
    if (fr < 0 || fr >= s->num_lines) return 1;
    for (i=0;i<s->line_len[fr];i++){
        int cw = tw_cp_cols(s->lines[fr][i]);
        if (col+cw > s->text_cols){ rows++; col=0; }
        col += cw;
    }
    return rows;
}
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

    /* Test 6: readline ^K kill-to-EOL then ^Y yank restores the text */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    feed(s,"hello"); s->cur_col=2;    /* cursor after 'he' */
    tw_handle_key(s, KEY_CTRL_K);     /* kill "llo" -> line = "he" */
    dump_line0(s,buf);
    int k_ok = strcmp(buf,"he")==0;
    tw_handle_key(s, KEY_CTRL_Y);     /* yank "llo" back at cursor */
    dump_line0(s,buf);
    printf("%s ^K/^Y: after kill='he'?%d final='%s' (want hello)\n",
        (k_ok && strcmp(buf,"hello")==0)?"PASS":"FAIL", k_ok, buf);
    if(!(k_ok && strcmp(buf,"hello")==0)) fails++;

    /* Test 7: ^W delete-word-backward */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    feed(s,"foo bar");               /* cursor at end (col 7) */
    tw_handle_key(s, KEY_CTRL_W);    /* delete "bar" -> "foo " */
    dump_line0(s,buf);
    printf("%s ^W del-word: '%s' (want 'foo ')\n", strcmp(buf,"foo ")==0?"PASS":"FAIL", buf);
    if(strcmp(buf,"foo ")) fails++;

    /* Test 8: M-b / M-f word motion columns */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    feed(s,"foo bar");               /* col 7 */
    tw_handle_key(s, KEY_META_B);    /* -> start of "bar" = col 4 */
    int mb = s->cur_col;
    tw_handle_key(s, KEY_META_B);    /* -> start of "foo" = col 0 */
    int mb2 = s->cur_col;
    tw_handle_key(s, KEY_META_F);    /* -> end of "foo" = col 3 */
    int mf = s->cur_col;
    printf("%s M-b/M-f: %d %d %d (want 4 0 3)\n",
        (mb==4&&mb2==0&&mf==3)?"PASS":"FAIL", mb, mb2, mf);
    if(!(mb==4&&mb2==0&&mf==3)) fails++;

    /* Test 9: eMMC lock predicate (mmc 0 is read-only; mmc 1, usb 0 are not) */
    struct { const char*i; const char*d; int want; } E[] = {
        {"mmc","0",1}, {"mmc","0:1",1}, {"mmc","1:1",0}, {"mmc","10:1",0},
        {"usb","0:1",0}, {"mmc","0:3",1},
    };
    int e_ok=1;
    for (unsigned e=0;e<sizeof(E)/sizeof(E[0]);e++)
        if (tw_is_emmc(E[e].i,E[e].d)!=E[e].want){ e_ok=0;
            printf("  eMMC-lock FAIL: %s %s -> %d (want %d)\n",
                E[e].i,E[e].d,tw_is_emmc(E[e].i,E[e].d),E[e].want); }
    printf("%s eMMC lock predicate\n", e_ok?"PASS":"FAIL");
    if(!e_ok) fails++;

    /* Test 10: tw_switch_file auto-saves current then loads target, and
     * round-trips (this is what ^R picker Enter uses). */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    s->writable=1; s->fs.valid=1;
    strncpy(s->filename,"1.txt",TW_MAX_FILENAME-1);
    feed(s,"one");                       /* 1.txt = "one" */
    tw_switch_file(s, "2.txt");          /* saves 1.txt, opens 2.txt */
    int on2 = (strcmp(s->filename,"2.txt")==0);
    feed(s,"two");                       /* 2.txt = "two" */
    tw_switch_file(s, "1.txt");          /* saves 2.txt, back to 1.txt */
    dump_line0(s,buf);
    int back1 = (strcmp(s->filename,"1.txt")==0) && strcmp(buf,"one")==0;
    tw_switch_file(s, "2.txt");
    dump_line0(s,buf);
    int back2 = (strcmp(s->filename,"2.txt")==0) && strcmp(buf,"two")==0;
    printf("%s switch_file: on2=%d 1.txt='one'?%d 2.txt='two'?%d\n",
        (on2&&back1&&back2)?"PASS":"FAIL", on2, back1, back2);
    if(!(on2&&back1&&back2)) fails++;

    /* Test 11: ^R picker navigation - up/down move selection, Enter opens.
     * Seed a fake file list + backing files, then drive the picker keys. */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->num_lines=1; s->line_len[0]=0;
    tw_bind_ime(s); s->ime.mode=TW_IME_OFF; s->writable=1; s->fs.valid=1;
    /* create three files via save so the picker's switch can load one */
    strncpy(s->filename,"a.txt",TW_MAX_FILENAME-1); feed(s,"AA"); tw_do_save(s);
    strncpy(s->filename,"b.txt",TW_MAX_FILENAME-1); s->line_len[0]=0; s->cur_col=0;
        feed(s,"BB"); tw_do_save(s);
    strncpy(s->filename,"c.txt",TW_MAX_FILENAME-1); s->line_len[0]=0; s->cur_col=0;
        feed(s,"CC"); tw_do_save(s);
    /* enter picker mode with a known list */
    s->prompt=TW_PROMPT_PICK; s->pick_count=3; s->pick_sel=0; s->pick_top=0;
    strcpy(s->pick_name[0],"a.txt");
    strcpy(s->pick_name[1],"b.txt");
    strcpy(s->pick_name[2],"c.txt");
    tw_handle_key(s, KEY_ARROW_DOWN);   /* sel 0 -> 1 */
    int sel1 = s->pick_sel;
    tw_handle_key(s, KEY_ARROW_DOWN);   /* sel 1 -> 2 */
    tw_handle_key(s, KEY_ARROW_UP);     /* sel 2 -> 1 (b.txt) */
    int sel2 = s->pick_sel;
    tw_handle_key(s, KEY_ENTER);        /* open b.txt */
    dump_line0(s,buf);
    int ok11 = (sel1==1)&&(sel2==1)&&(strcmp(s->filename,"b.txt")==0)
               &&(strcmp(buf,"BB")==0)&&(s->prompt==TW_PROMPT_NONE);
    printf("%s ^R picker: sel1=%d sel2=%d opened='%s' content='%s'\n",
        ok11?"PASS":"FAIL", sel1, sel2, s->filename, buf);
    if(!ok11) fails++;

    /* Test 12: soft-wrap row counting (tw_line_rows), narrow + wide + boundary */
    memset(s,0,sizeof(*s)); tw_video_init(s); s->text_cols=10; /* small for test */
    s->num_lines=4;
    s->line_len[0]=0;                              /* empty -> 1 row */
    s->line_len[1]=10; for(int i=0;i<10;i++) s->lines[1][i]='a'; /* exactly 10 -> 1 */
    s->line_len[2]=11; for(int i=0;i<11;i++) s->lines[2][i]='a'; /* 11 -> 2 rows */
    s->line_len[3]=6;  for(int i=0;i<6;i++)  s->lines[3][i]=0x4F60; /* 6 wide=12 cols -> 2 rows, wrap-before-wide at col10 */
    int r0=tw_line_rows(s,0), r1=tw_line_rows(s,1),
        r2=tw_line_rows(s,2), r3=tw_line_rows(s,3);
    int okw = (r0==1)&&(r1==1)&&(r2==2)&&(r3==2);
    printf("%s wrap rows: empty=%d 10-narrow=%d 11-narrow=%d 6-wide=%d (want 1 1 2 2)\n",
        okw?"PASS":"FAIL", r0, r1, r2, r3);
    if(!okw) fails++;

    /* Test 13: C-k on empty/at-EOL line deletes the line (joins next). */
    memset(s,0,sizeof(*s)); tw_video_init(s); tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    s->num_lines=2; s->line_len[0]=0; s->line_len[1]=2;
    s->lines[1][0]='x'; s->lines[1][1]='y';
    s->cur_row=0; s->cur_col=0;
    tw_handle_key(s, KEY_CTRL_K);   /* empty line 0: should join line 1 up */
    dump_line0(s,buf);
    int okk = (s->num_lines==1) && strcmp(buf,"xy")==0;
    printf("%s C-k join: lines=%d line0='%s' (want 1 'xy')\n",
        okk?"PASS":"FAIL", s->num_lines, buf);
    if(!okk) fails++;

    /* Test 14: Tab expands to spaces up to the next TW_TABW stop. */
    memset(s,0,sizeof(*s)); tw_video_init(s); tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    s->num_lines=1; s->line_len[0]=0; s->cur_row=0; s->cur_col=0;
    tw_handle_key(s, KEY_TAB);              /* at col 0 -> 8 spaces */
    int tab1 = s->line_len[0];
    feed(s,"ab"); tw_handle_key(s, KEY_TAB);/* at col 10 -> to col 16 = 6 spaces */
    int tab2 = s->line_len[0];
    printf("%s Tab: after1=%d after'ab'+tab=%d (want 8 16)\n",
        (tab1==8&&tab2==16)?"PASS":"FAIL", tab1, tab2);
    if(!(tab1==8&&tab2==16)) fails++;

    /* Test 15: ^Q power-off saves the dirty buffer FIRST (data safety). */
    extern long df_find_len(const char *f);
    memset(s,0,sizeof(*s)); tw_video_init(s); tw_bind_ime(s); s->ime.mode=TW_IME_OFF;
    s->writable=1; s->fs.valid=1; s->num_lines=1; s->line_len[0]=0;
    strncpy(s->filename,"po.txt",TW_MAX_FILENAME-1);
    feed(s,"save me");                 /* dirty, unsaved */
    tw_handle_key(s, KEY_CTRL_Q);      /* -> poweroff confirm prompt */
    int prompted = (s->prompt==TW_PROMPT_POWEROFF);
    tw_handle_key(s, 'y');             /* confirm: save+sync+(stub)poweroff */
    long saved_len = df_find_len("po.txt");   /* "save me" = 7 + newline = 8 */
    int okpo = prompted && (saved_len==8) && (s->dirty==0);
    printf("%s ^Q poweroff: prompted=%d saved_len=%ld dirty=%d (want 1 8 0)\n",
        okpo?"PASS":"FAIL", prompted, saved_len, s->dirty);
    if(!okpo) fails++;

    printf(fails?"\n%d FAIL\n":"\nALL PASS\n", fails);
    return fails?1:0;
}
