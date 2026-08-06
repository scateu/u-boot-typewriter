#ifndef _STUB_ASM_SYSTEM_H
#define _STUB_ASM_SYSTEM_H
/* In the host functional test, tw_poweroff ends in `for(;;) wfi();`, which
 * would hang the test. If the test defines TW_TEST_WFI_ESCAPE, wfi() calls it
 * (a longjmp back into the test). Otherwise it's a no-op. */
#ifdef TW_TEST_WFI_ESCAPE
void TW_TEST_WFI_ESCAPE(void);
#define wfi() TW_TEST_WFI_ESCAPE()
#else
#define wfi() do { } while (0)
#endif
#endif
