/* Render glyphs as ASCII art to verify the narrow/wide row layout: a narrow
 * 'a' must show all 16 rows (top-half bug); 你 must still be correct. */
#include <stdio.h>
#include "font_data.h"
static const struct tw_glyph *find(unsigned cp){
    unsigned lo=0,hi=tw_glyphs_count;
    while(lo<hi){unsigned m=lo+(hi-lo)/2;unsigned c=tw_glyphs[m].cp;
        if(c<cp)lo=m+1;else if(c>cp)hi=m;else return &tw_glyphs[m];}
    return 0;
}
static void show(unsigned cp){
    const struct tw_glyph *g=find(cp);
    if(!g){printf("U+%04X: missing\n",cp);return;}
    printf("U+%04X width=%u\n",cp,g->width);
    for(int row=0;row<16;row++){
        unsigned char left=g->rows[row*2],right=g->rows[row*2+1];
        for(int b=0;b<8;b++)putchar((left&(0x80>>b))?'#':'.');
        if(g->width==16)for(int b=0;b<8;b++)putchar((right&(0x80>>b))?'#':'.');
        putchar('\n');
    }
    putchar('\n');
}
int main(void){ show('a'); show('1'); show(0x4F60); return 0; }
