/* SPDX-License-Identifier: GPL-2.0 */
/*
 * GKI does not export a few DRM helpers panfrost uses. Local equivalents
 * (copied from drivers/gpu/drm/drm_gem.c and drm_gem_shmem_helper.c).
 */
#ifndef __PANFROST_COMPAT_H__
#define __PANFROST_COMPAT_H__

#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>

static inline int pf_gem_objects_lookup(struct drm_file *filp,
					void __user *bo_handles, int count,
					struct drm_gem_object ***objs_out)
{
	struct drm_gem_object **objs;
	u32 *handles;
	int ret = 0, i;

	if (!count)
		return 0;

	objs = kvmalloc_array(count, sizeof(*objs), GFP_KERNEL | __GFP_ZERO);
	if (!objs)
		return -ENOMEM;
	*objs_out = objs;

	handles = kvmalloc_array(count, sizeof(u32), GFP_KERNEL);
	if (!handles) {
		return -ENOMEM;
	}
	if (copy_from_user(handles, bo_handles, count * sizeof(u32))) {
		ret = -EFAULT;
		goto out;
	}
	for (i = 0; i < count; i++) {
		objs[i] = drm_gem_object_lookup(filp, handles[i]);
		if (!objs[i]) {
			ret = -ENOENT;
			break;
		}
	}
out:
	kvfree(handles);
	return ret;
}

static inline int pf_gem_shmem_madvise(struct drm_gem_shmem_object *shmem,
				       int madv)
{
	dma_resv_assert_held(shmem->base.resv);
	if (shmem->madv >= 0)
		shmem->madv = madv;
	return shmem->madv >= 0;
}

#endif
