#ifndef _STUB_TIME_H
#define _STUB_TIME_H
/* Host-test stub for U-Boot's <time.h>. get_timer() returns a monotonic-ish
 * millisecond count; the functest just needs it to link and advance. */
unsigned long get_timer(unsigned long base);
#endif
