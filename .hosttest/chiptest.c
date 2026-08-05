/* Decode the [五] chip bytes the way draw_word does, and confirm each cp has a
 * glyph (a NULL glyph => blank cell => the "blends into white" bug). */
#include <stdio.h>
#include "font_data.h"
static const struct tw_glyph *find(unsigned cp){
    unsigned lo=0,hi=tw_glyphs_count;
    while(lo<hi){unsigned m=lo+(hi-lo)/2;unsigned c=tw_glyphs[m].cp;
        if(c<cp)lo=m+1;else if(c>cp)hi=m;else return &tw_glyphs[m];}
    return 0;
}
static int utf8_next(const char*w,int wl,int off,unsigned*cp){
    unsigned char c=w[off];
    if(c<0x80){*cp=c;return off+1;}
    if((c&0xE0)==0xC0&&off+1<wl){*cp=((c&0x1F)<<6)|(w[off+1]&0x3F);return off+2;}
    if((c&0xF0)==0xE0&&off+2<wl){*cp=((c&0x0F)<<12)|((w[off+1]&0x3F)<<6)|(w[off+2]&0x3F);return off+3;}
    *cp=0xFFFD;return off+1;
}
int main(void){
    const char tag[]="[\344\272\224]"; int wl=sizeof(tag)-1,off=0,bad=0;
    printf("tag bytes:"); for(int i=0;i<wl;i++)printf(" %02x",(unsigned char)tag[i]); printf("\n");
    while(off<wl){unsigned cp; off=utf8_next(tag,wl,off,&cp);
        const struct tw_glyph*g=find(cp);
        printf("  U+%04X -> %s%s\n",cp,g?"glyph":"MISSING(blank!)",g&&g->width==16?" (wide)":"");
        if(!g)bad++;
    }
    printf(bad?"\nFAIL %d missing\n":"\nALL GLYPHS PRESENT\n",bad);
    return bad?1:0;
}
