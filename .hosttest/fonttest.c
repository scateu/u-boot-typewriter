/* Render 你 (U+4F60) as ASCII art from the embedded font subset, proving the
 * glyph pipeline: find_glyph + row/bit decode match font_data.h's layout. */
#include <stdio.h>
#include "font_data.h"

static const struct tw_glyph *find(unsigned cp){
    unsigned lo=0,hi=tw_glyphs_count;
    while(lo<hi){unsigned m=lo+(hi-lo)/2;unsigned c=tw_glyphs[m].cp;
        if(c<cp)lo=m+1;else if(c>cp)hi=m;else return &tw_glyphs[m];}
    return 0;
}
int main(void){
    const struct tw_glyph *g=find(0x4F60); /* 你 */
    if(!g){printf("no glyph\n");return 1;}
    printf("cp=U+%04X width=%u\n",g->cp,g->width);
    for(int row=0;row<16;row++){
        unsigned char left=g->rows[row*2],right=g->rows[row*2+1];
        for(int b=0;b<8;b++)putchar((left&(0x80>>b))?'#':'.');
        if(g->width==16)for(int b=0;b<8;b++)putchar((right&(0x80>>b))?'#':'.');
        putchar('\n');
    }
    return 0;
}
