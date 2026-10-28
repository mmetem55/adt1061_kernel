#ifndef __SPRD_MEM_OVERLAY_CHECK_H__
#define __SPRD_MEM_OVERLAY_CHECK_H__

#include <linux/types.h>
#include <linux/kernel.h>

int sprd_check_memory_overlays(void);
bool sprd_is_region_overlapping(phys_addr_t base, phys_addr_t size);

#endif /* __SPRD_MEM_OVERLAY_CHECK_H__ */
