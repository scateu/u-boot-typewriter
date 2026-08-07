#ifndef _STUB_ASM_IO_H
#define _STUB_ASM_IO_H
/* Host-test stub for <asm/io.h>. The real readl/writel are only used in the
 * arm64-only WFI block of cmd_tw.c (guarded by CONFIG_ARM64 && __aarch64__),
 * so these are here just to satisfy the unconditional #include. */
static inline unsigned int readl(const volatile void *a){ (void)a; return 0; }
static inline void writel(unsigned int v, volatile void *a){ (void)v; (void)a; }
#endif
