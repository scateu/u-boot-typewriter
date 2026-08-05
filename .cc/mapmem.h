#ifndef _STUB_MAPMEM_H
#include <stdint.h>
#define _STUB_MAPMEM_H
#include <linux/types.h>
static inline void *map_sysmem(phys_addr_t paddr, unsigned long len){ (void)len; return (void*)(uintptr_t)paddr; }
static inline void unmap_sysmem(const void *vaddr){ (void)vaddr; }
#endif
static inline unsigned long map_to_sysmem(const void *ptr){ return (unsigned long)(uintptr_t)ptr; }
