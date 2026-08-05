#ifndef _STUB_STDIO_H
#define _STUB_STDIO_H
int getchar(void);
int tstc(void);
void putc(const char c);
void puts(const char *s);
int printf(const char *fmt, ...);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);
#endif
