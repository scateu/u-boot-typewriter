#ifndef _STUB_DELAY_H
#define _STUB_DELAY_H
void udelay(unsigned long usec);
#endif
#define mdelay(n) udelay((n)*1000)
