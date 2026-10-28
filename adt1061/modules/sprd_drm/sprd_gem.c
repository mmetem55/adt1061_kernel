/*
 * Copyright (C) 2018 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <drm/drm_prime.h>
#include <linux/io.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <linux/mm.h>
#include <linux/pm_runtime.h>

#include "sprd_drm.h"
#include "sprd_gem.h"
#include "sprd_ion.h"

/*
 * 2026 GKI 6.12 port note: the stock 4.14 driver fell back to the legacy
 * ION allocator (ion_new_alloc() against ION_HEAP_ID_MASK_FB) whenever the
 * CMA allocation above failed. ION is gone on GKI, so the fallback now
 * goes straight through the upstream DMA-BUF Heaps API against the
 * "carveout_fb" heap that sprd_ion registers for heap id ION_HEAP_ID_FB
 * (see stock device tree: /ion/heap@3, label = "carveout_fb").
 */
#define SPRD_GEM_FB_HEAP_NAME "carveout_fb"

static int sprd_gem_cma_object_mmap(struct drm_gem_object *obj,
				     struct vm_area_struct *vma);

/*
 * 2026 GKI 6.12 port note: struct drm_driver no longer carries the legacy
 * .gem_vm_ops / .gem_free_object_unlocked fields (removed along with the
 * old pre-GEM-object-funcs mmap/free dispatch). Every GEM object now gets
 * its per-object behaviour from drm_gem_object.funcs instead, so the vm_ops
 * that used to live in drm_gem_cma_helper.h's exported drm_gem_cma_vm_ops
 * is simply reimplemented here (it only ever needed open/close for
 * refcounting - this driver always pre-maps pages itself via
 * dma_mmap_wc(), never through a fault handler).
 */
static const struct vm_operations_struct sprd_gem_vm_ops = {
	.open = drm_gem_vm_open,
	.close = drm_gem_vm_close,
};

static const struct drm_gem_object_funcs sprd_gem_object_funcs = {
	.free = sprd_gem_free_object,
	.get_sg_table = sprd_gem_cma_prime_get_sg_table,
	.mmap = sprd_gem_cma_object_mmap,
	.vm_ops = &sprd_gem_vm_ops,
};

static void *sprd_gem_map_carveout(struct sprd_gem_obj *sprd_gem,
				   phys_addr_t phys, size_t size)
{
	unsigned int i, n = PAGE_ALIGN(size) >> PAGE_SHIFT;
	struct page **pages;
	void *vaddr;

	pages = kvmalloc_array(n, sizeof(*pages), GFP_KERNEL);
	if (!pages)
		return NULL;
	for (i = 0; i < n; i++)
		pages[i] = pfn_to_page(PHYS_PFN(phys) + i);
	/* the vm_struct points at this array (area->pages): keep it until vunmap() */
	vaddr = vmap(pages, n, VM_MAP, pgprot_writecombine(PAGE_KERNEL));
	if (!vaddr) {
		kvfree(pages);
		return NULL;
	}
	sprd_gem->fb_pages = pages;
	return vaddr;
}

static struct sprd_gem_obj *sprd_gem_obj_create(struct drm_device *drm,
						unsigned long size)
{
	struct sprd_gem_obj *sprd_gem;
	int ret;

	sprd_gem = kzalloc(sizeof(*sprd_gem), GFP_KERNEL);
	if (!sprd_gem)
		return ERR_PTR(-ENOMEM);

	ret = drm_gem_object_init(drm, &sprd_gem->base, size);
	if (ret < 0) {
		DRM_ERROR("failed to initialize gem object\n");
		goto error;
	}

	sprd_gem->base.funcs = &sprd_gem_object_funcs;

	ret = drm_gem_create_mmap_offset(&sprd_gem->base);
	if (ret) {
		drm_gem_object_release(&sprd_gem->base);
		goto error;
	}

	return sprd_gem;

error:
	kfree(sprd_gem);
	return ERR_PTR(ret);
}

void sprd_gem_free_object(struct drm_gem_object *obj)
{
	struct sprd_gem_obj *sprd_gem = to_sprd_gem_obj(obj);

	DRM_DEBUG("gem = %p\n", obj);

	if (sprd_gem->vaddr && !sprd_gem->fb_reserved) {
		dma_free_wc(obj->dev->dev, obj->size,
			sprd_gem->vaddr, sprd_gem->dma_addr);
	} else if (sprd_gem->vaddr && sprd_gem->fb_reserved) {
		vunmap(sprd_gem->vaddr);
		kvfree(sprd_gem->fb_pages);
	} else if (sprd_gem->sgtb)
		drm_prime_gem_destroy(obj, sprd_gem->sgtb);

	drm_gem_object_release(obj);

	kfree(sprd_gem);
}

int sprd_gem_cma_dumb_create(struct drm_file *file_priv, struct drm_device *drm,
			    struct drm_mode_create_dumb *args)
{
	struct sprd_gem_obj *sprd_gem;
	struct dma_buf *dmabuf;
	struct dma_heap *fb_heap;
	int ret;
	unsigned long phyaddr;
	size_t size;

	args->pitch = DIV_ROUND_UP(args->width * args->bpp, 8);
	args->size = round_up(args->pitch * args->height, PAGE_SIZE);

	sprd_gem = sprd_gem_obj_create(drm, args->size);
	if (IS_ERR(sprd_gem))
		return PTR_ERR(sprd_gem);

	sprd_gem->vaddr = dma_alloc_wc(drm->dev, args->size,
			&sprd_gem->dma_addr, GFP_KERNEL | __GFP_NOWARN | GFP_DMA);

	if (!sprd_gem->vaddr) {
		DRM_ERROR("failed to allocate buffer with size %llu, use dma-buf heap\n",
				args->size);

		fb_heap = dma_heap_find(SPRD_GEM_FB_HEAP_NAME);
		if (!fb_heap) {
			DRM_ERROR("%s dma-buf heap not found\n",
				  SPRD_GEM_FB_HEAP_NAME);
			ret = -ENOMEM;
			goto error;
		}

		dmabuf = dma_heap_buffer_alloc(fb_heap, args->size,
						O_RDWR | O_CLOEXEC, 0);
		dma_heap_put(fb_heap);

		if (IS_ERR_OR_NULL(dmabuf)) {
			DRM_ERROR("dma_heap_buffer_alloc dumb buffer fail\n");
			ret = -ENOMEM;
			goto error;
		}

		size = (size_t)args->size;
		sprd_ion_get_phys_addr_by_db(dmabuf, &phyaddr, &size);
		sprd_gem->base.dma_buf = dmabuf;
		sprd_gem->dma_addr = phyaddr;
		/*
		 * The carveout is physically contiguous reserved memory that is
		 * part of the linear map (not "no-map"), so memremap(WC) refuses
		 * it. Map its pages write-combined with vmap(). A kernel
		 * dma_buf_vmap() here was never undone and made dma_buf_release()
		 * BUG() on vmapping_counter once the last handle/fd went away.
		 */
		DRM_INFO("dumb buffer %llu bytes from the %s heap (phys 0x%lx)\n",
			 args->size, SPRD_GEM_FB_HEAP_NAME, phyaddr);
		sprd_gem->vaddr = sprd_gem_map_carveout(sprd_gem, phyaddr, args->size);
		sprd_gem->fb_reserved = true;

		if (!sprd_gem->vaddr) {
			DRM_ERROR("failed to allocate buffer with size %llu\n",
					args->size);
			sprd_gem->base.dma_buf = NULL;
			sprd_gem->fb_reserved = false;
			dma_buf_put(dmabuf);
			ret = -ENOMEM;
			goto error;
		}
	}

	ret = drm_gem_handle_create(file_priv, &sprd_gem->base, &args->handle);
	if (ret)
		goto error;

	drm_gem_object_put(&sprd_gem->base);

	return 0;

error:
	sprd_gem_free_object(&sprd_gem->base);
	return ret;
}

static int sprd_gem_cma_object_mmap(struct drm_gem_object *obj,
				   struct vm_area_struct *vma)

{
	int ret;
	struct sprd_gem_obj *sprd_gem = to_sprd_gem_obj(obj);

	/*
	 * 2026 GKI 6.12 port note: vma->vm_flags is now const - direct
	 * read-modify-write is a compile error. vm_flags_clear() is the
	 * mandated accessor (introduced alongside that change).
	 */
	vm_flags_clear(vma, VM_PFNMAP);
	vma->vm_pgoff = 0;

	ret = dma_mmap_wc(obj->dev->dev, vma,
				    sprd_gem->vaddr, sprd_gem->dma_addr,
				    vma->vm_end - vma->vm_start);
	if (ret)
		drm_gem_vm_close(vma);

	return ret;
}

int sprd_gem_cma_mmap(struct file *filp, struct vm_area_struct *vma)
{
	/*
	 * 2026 GKI 6.12 port note: drm_gem_mmap() now looks up the GEM
	 * object itself and dispatches straight to obj->funcs->mmap (i.e.
	 * sprd_gem_cma_object_mmap(), wired via sprd_gem_object_funcs
	 * above), which already does the dma_mmap_wc() mapping.
	 * Calling sprd_gem_cma_object_mmap() again here would remap the
	 * same vma a second time, so it has been removed.
	 */
	return drm_gem_mmap(filp, vma);
}

int sprd_gem_cma_prime_mmap(struct drm_gem_object *obj,
			    struct vm_area_struct *vma)
{
	/* Same reasoning as sprd_gem_cma_mmap() above: drm_gem_mmap_obj()
	 * already calls obj->funcs->mmap() internally.
	 */
	return drm_gem_mmap_obj(obj, obj->size, vma);
}

struct sg_table *sprd_gem_cma_prime_get_sg_table(struct drm_gem_object *obj)
{
	struct sprd_gem_obj *sprd_gem = to_sprd_gem_obj(obj);
	struct sg_table *sgtb;
	int ret;

	sgtb = kzalloc(sizeof(*sgtb), GFP_KERNEL);
	if (!sgtb)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable(obj->dev->dev, sgtb, sprd_gem->vaddr,
			      sprd_gem->dma_addr, obj->size);
	if (ret) {
		DRM_ERROR("failed to allocate sg_table, %d\n", ret);
		kfree(sgtb);
		return ERR_PTR(ret);
	}

	return sgtb;
}

struct drm_gem_object *sprd_gem_prime_import_sg_table(struct drm_device *drm,
		struct dma_buf_attachment *attach, struct sg_table *sgtb)
{
	struct sprd_gem_obj *sprd_gem;
	bool reserved;

	sprd_gem = sprd_gem_obj_create(drm, attach->dmabuf->size);
	if (IS_ERR(sprd_gem))
		return ERR_CAST(sprd_gem);

	DRM_DEBUG("gem = %p\n", &sprd_gem->base);

	if (sgtb->nents == 1)
		sprd_gem->dma_addr = sg_dma_address(sgtb->sgl);

	sprd_gem->sgtb = sgtb;

	if (sprd_ion_is_reserved(-1, attach->dmabuf, &reserved))
		DRM_ERROR("sprd_ion_is_reserved fail\n");
	else
		sprd_gem->need_iommu = !reserved;

	return &sprd_gem->base;
}
