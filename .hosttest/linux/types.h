/* host shim: map U-Boot's linux/types.h to stdint for the lookup harness */
#ifndef __HOSTTEST_LINUX_TYPES_H
#define __HOSTTEST_LINUX_TYPES_H
#include <stdint.h>
#include <stddef.h>
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
#endif
