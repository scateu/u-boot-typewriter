#ifndef _STUB_MAPMEM_H
#define _STUB_MAPMEM_H
#include <linux/types.h>
static inline void *map_sysmem(phys_addr_t paddr, unsigned long len){ (void)len; return (void*)(uintptr_t)paddr; }
static inline void unmap_sysmem(const void *vaddr){ (void)vaddr; }
#endif
