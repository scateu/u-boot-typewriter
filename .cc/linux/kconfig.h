#ifndef _STUB_KCONFIG_H
#define _STUB_KCONFIG_H
/* test shim: CONFIG_IS_ENABLED(x) -> defined(CONFIG_x) */
#define CONFIG_IS_ENABLED(x) defined(CONFIG_ ## x)
#endif
