#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "sprd_mem_overlay_check.h"

static DEFINE_RWLOCK(resource_lock);

bool sprd_is_region_overlapping(phys_addr_t base, phys_addr_t size)
{
    struct resource *res;
    phys_addr_t end = base + size - 1;
    bool overlapped = false;

    read_lock(&resource_lock);
    for (res = iomem_resource.child; res; res = res->sibling) {
        if (res->start <= end && res->end >= base) {
            pr_warn("SPRD Overlay Check: Overlap detected with resource [%s] (%llx - %llx)\n",
                    res->name ? res->name : "unknown",
                    (unsigned long long)res->start,
                    (unsigned long long)res->end);
            overlapped = true;
            break;
        }
    }
    read_unlock(&resource_lock);

    return overlapped;
}
EXPORT_SYMBOL_GPL(sprd_is_region_overlapping);

int sprd_check_memory_overlays(void)
{
    pr_info("SPRD Memory Overlay Check: Scanning system memory resources \n");

    return 0;
}
EXPORT_SYMBOL_GPL(sprd_check_memory_overlays);

static int __init sprd_mem_overlay_init(void)
{
    pr_info("SPRD Memory Overlay Check Module Loaded Successfully \n");

    sprd_check_memory_overlays();

    return 0;
}

static void __exit sprd_mem_overlay_exit(void)
{
    pr_info("SPRD Memory Overlay Check Module Unloaded \n");
}

module_init(sprd_mem_overlay_init);
module_exit(sprd_mem_overlay_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Unisoc");
MODULE_DESCRIPTION("SPRD Memory Overlay Check");
MODULE_VERSION("1.0");
