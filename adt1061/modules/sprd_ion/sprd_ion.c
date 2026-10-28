/*
 * Copyright (C) 2016 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ---------------------------------------------------------------------
 * NOTE (2026 GKI 5.10 port):
 *
 * drivers/staging/android/ion/ is NOT present in the Android common
 * kernel 5.10 tree (GKI dropped ION in favour of the upstream DMA-BUF
 * Heaps framework, drivers/dma-buf/dma-heap.c + drivers/dma-buf/heaps/).
 * Because of that, every ion_* symbol this driver used to call
 * (ion_device_add_heap, ion_carveout_heap_create, struct ion_buffer, ...)
 * is simply undefined when linking against a 5.10 GKI kernel - it has
 * nothing to do with this driver's own code.
 *
 * This file has been rewritten from scratch as its own, self-contained
 * DMA-BUF exporter: it registers one dma-buf heap (/dev/dma_heap/<name>)
 * per <reg/label/type/memory-region> child node under the device tree
 * node this platform driver binds to - exactly the same device-tree
 * contract the old ION carveout heap used - and backs every heap with a
 * gen_pool carved out of the reserved-memory region named by that node's
 * "memory-region" phandle, exactly like the old ion_carveout_heap did.
 *
 * All sprd_ion_* helper entry points that drv/sprd_iommu_core.c and other
 * out-of-tree SPRD drivers call (sprd_ion_get_sg, sprd_ion_set_dma,
 * sprd_ion_put_dma, sprd_ion_get_phys_addr, sprd_ion_map_kernel, ...) keep
 * their exact original prototypes. They now operate on our own
 * "struct sprd_dmabuf_buffer" (stored in dmabuf->priv) instead of the
 * old "struct ion_buffer" - callers never touched that struct directly,
 * they always went through fd/dma_buf or the opaque void *buf handle, so
 * sprd_iommu_core.c needs NO changes for this part.
 * ---------------------------------------------------------------------
 */

#include <linux/compat.h>
#include <linux/dma-buf.h>
#include <linux/dma-direct.h>
#include <linux/dma-heap.h>
#include <linux/dma-mapping.h>
#include <linux/export.h>
#include <linux/err.h>
#include <linux/genalloc.h>
#include <linux/highmem.h>
#include <linux/io.h>
#include <linux/iosys-map.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "sprd_iommu.h"
#include "sprd_ion.h"

#define MAX_MAP_USER 15

/* mirrors the old struct ion_map_info, only used for local bookkeeping */
struct sprd_dmabuf_map_info {
	pid_t pid;
	char task_name[TASK_COMM_LEN];
	int fd;
	bool valid;
	u64 map_time;
	u64 unmap_time;
};

/* one of these per DT child node -> one /dev/dma_heap/<name> */
struct sprd_carveout_heap {
	struct dma_heap *heap;
	struct gen_pool *pool;
	struct device *dev;	/* the of_platform_device created for this node */
	void *base_vaddr;	/* memremap() of [base, base+size) */
	phys_addr_t base;
	size_t size;
	u32 id;			/* enum sprd_ion_heap_ids value from DT "reg" */
	const char *name;
};

/* replaces "struct ion_buffer" - one per allocated dma_buf */
struct sprd_dmabuf_buffer {
	struct sprd_carveout_heap *heap;
	struct mutex lock;
	struct list_head attachments;
	size_t size;
	phys_addr_t paddr;
	void *vaddr;		/* heap->base_vaddr + (paddr - heap->base) */
	int vmap_cnt;

	/* fields the SPRD iommu/display/camera stack expects to find
	 * through the opaque void *buf handle - kept at the exact same
	 * semantics as the old struct ion_buffer fields of the same name.
	 */
	int iomap_cnt[SPRD_IOMMU_MAX];
	struct sprd_dmabuf_map_info mappers[MAX_MAP_USER];
	bool reserved;		/* true unless this came off the SYSTEM heap id */
};

struct sprd_dmabuf_attachment {
	struct device *dev;
	struct sg_table table;
	struct list_head list;
	bool mapped;
};

static int num_heaps;
static struct sprd_carveout_heap *carveout_heaps;

/* ---------------------------------------------------------------------
 * dma_buf_ops
 * ---------------------------------------------------------------------
 */

static int sprd_heap_attach(struct dma_buf *dmabuf,
			     struct dma_buf_attachment *attachment)
{
	struct sprd_dmabuf_buffer *buffer = dmabuf->priv;
	struct sprd_dmabuf_attachment *a;
	int ret;

	a = kzalloc(sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	ret = sg_alloc_table(&a->table, 1, GFP_KERNEL);
	if (ret) {
		kfree(a);
		return ret;
	}
	sg_set_page(a->table.sgl, phys_to_page(buffer->paddr), buffer->size, 0);

	a->dev = attachment->dev;
	a->mapped = false;
	INIT_LIST_HEAD(&a->list);
	attachment->priv = a;

	mutex_lock(&buffer->lock);
	list_add(&a->list, &buffer->attachments);
	mutex_unlock(&buffer->lock);

	return 0;
}

static void sprd_heap_detach(struct dma_buf *dmabuf,
			      struct dma_buf_attachment *attachment)
{
	struct sprd_dmabuf_buffer *buffer = dmabuf->priv;
	struct sprd_dmabuf_attachment *a = attachment->priv;

	mutex_lock(&buffer->lock);
	list_del(&a->list);
	mutex_unlock(&buffer->lock);

	sg_free_table(&a->table);
	kfree(a);
}

static struct sg_table *sprd_heap_map_dma_buf(struct dma_buf_attachment *attachment,
					       enum dma_data_direction direction)
{
	struct sprd_dmabuf_attachment *a = attachment->priv;
	int ret;

	ret = dma_map_sgtable(attachment->dev, &a->table, direction, 0);
	if (ret)
		return ERR_PTR(ret);

	a->mapped = true;
	return &a->table;
}

static void sprd_heap_unmap_dma_buf(struct dma_buf_attachment *attachment,
				     struct sg_table *table,
				     enum dma_data_direction direction)
{
	struct sprd_dmabuf_attachment *a = attachment->priv;

	a->mapped = false;
	dma_unmap_sgtable(attachment->dev, table, direction, 0);
}

static int sprd_heap_dma_buf_begin_cpu_access(struct dma_buf *dmabuf,
					       enum dma_data_direction direction)
{
	struct sprd_dmabuf_buffer *buffer = dmabuf->priv;
	struct sprd_dmabuf_attachment *a;

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->attachments, list) {
		if (a->mapped)
			dma_sync_sgtable_for_cpu(a->dev, &a->table, direction);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int sprd_heap_dma_buf_end_cpu_access(struct dma_buf *dmabuf,
					     enum dma_data_direction direction)
{
	struct sprd_dmabuf_buffer *buffer = dmabuf->priv;
	struct sprd_dmabuf_attachment *a;

	mutex_lock(&buffer->lock);
	list_for_each_entry(a, &buffer->attachments, list) {
		if (a->mapped)
			dma_sync_sgtable_for_device(a->dev, &a->table, direction);
	}
	mutex_unlock(&buffer->lock);

	return 0;
}

static int sprd_heap_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	struct sprd_dmabuf_buffer *buffer = dmabuf->priv;

	return remap_pfn_range(vma, vma->vm_start,
				PFN_DOWN(buffer->paddr) + vma->vm_pgoff,
				vma->vm_end - vma->vm_start, vma->vm_page_prot);
}

/*
 * dma_buf_ops.vmap/vunmap on 6.12 (GKI android16-6.12) take a
 * "struct iosys_map *" (struct dma_buf_map was renamed to iosys_map
 * upstream around v5.19; the plain-void* form only existed pre-5.12).
 * dma_buf_vmap()/dma_buf_vunmap() below use the matching convention.
 */
static int sprd_heap_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct sprd_dmabuf_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	buffer->vmap_cnt++;
	mutex_unlock(&buffer->lock);

	iosys_map_set_vaddr(map, buffer->vaddr);
	return 0;
}

static void sprd_heap_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	struct sprd_dmabuf_buffer *buffer = dmabuf->priv;

	mutex_lock(&buffer->lock);
	if (buffer->vmap_cnt > 0)
		buffer->vmap_cnt--;
	mutex_unlock(&buffer->lock);

	iosys_map_clear(map);
}

static void sprd_heap_dma_buf_release(struct dma_buf *dmabuf)
{
	struct sprd_dmabuf_buffer *buffer = dmabuf->priv;

	gen_pool_free(buffer->heap->pool, buffer->paddr, buffer->size);
	kfree(buffer);
}

static const struct dma_buf_ops sprd_heap_buf_ops = {
	.attach = sprd_heap_attach,
	.detach = sprd_heap_detach,
	.map_dma_buf = sprd_heap_map_dma_buf,
	.unmap_dma_buf = sprd_heap_unmap_dma_buf,
	.begin_cpu_access = sprd_heap_dma_buf_begin_cpu_access,
	.end_cpu_access = sprd_heap_dma_buf_end_cpu_access,
	.mmap = sprd_heap_mmap,
	.vmap = sprd_heap_vmap,
	.vunmap = sprd_heap_vunmap,
	.release = sprd_heap_dma_buf_release,
};

/* ---------------------------------------------------------------------
 * dma_heap_ops.allocate
 * ---------------------------------------------------------------------
 */

static struct dma_buf *sprd_carveout_heap_allocate(struct dma_heap *heap,
						   unsigned long len,
						   u32 fd_flags,
						   u64 heap_flags)
{
	struct sprd_carveout_heap *carveout = dma_heap_get_drvdata(heap);
	struct sprd_dmabuf_buffer *buffer;
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct dma_buf *dmabuf;
	unsigned long aligned_len = PAGE_ALIGN(len);
	phys_addr_t paddr;
	int ret;

	buffer = kzalloc(sizeof(*buffer), GFP_KERNEL);
	if (!buffer)
		return ERR_PTR(-ENOMEM);

	mutex_init(&buffer->lock);
	INIT_LIST_HEAD(&buffer->attachments);
	buffer->heap = carveout;
	buffer->size = aligned_len;
	buffer->reserved = (carveout->id != ION_HEAP_ID_SYSTEM);

	paddr = gen_pool_alloc(carveout->pool, aligned_len);
	if (!paddr) {
		ret = -ENOMEM;
		goto err_free_buffer;
	}
	buffer->paddr = paddr;
	buffer->vaddr = carveout->base_vaddr + (paddr - carveout->base);

	/* zero the buffer before handing it to userspace, same as ION did,
	 * then push it out of cache so stale data is never visible to a
	 * non-coherent device that reads physical memory directly.
	 */
	memset(buffer->vaddr, 0, aligned_len);
	dma_sync_single_for_device(dma_heap_get_dev(heap),
				    phys_to_dma(dma_heap_get_dev(heap), paddr),
				    aligned_len, DMA_TO_DEVICE);

	exp_info.exp_name = "sprd_dmabuf";
	exp_info.ops = &sprd_heap_buf_ops;
	exp_info.size = buffer->size;
	exp_info.flags = fd_flags;
	exp_info.priv = buffer;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		ret = PTR_ERR(dmabuf);
		goto err_free_pool;
	}

	return dmabuf;

err_free_pool:
	gen_pool_free(carveout->pool, paddr, aligned_len);
err_free_buffer:
	kfree(buffer);
	return ERR_PTR(ret);
}

static const struct dma_heap_ops sprd_carveout_heap_ops = {
	.allocate = sprd_carveout_heap_allocate,
};

/* ---------------------------------------------------------------------
 * sprd_ion_* compatibility API - unchanged prototypes, same semantics,
 * now operating on struct sprd_dmabuf_buffer instead of struct ion_buffer.
 * ---------------------------------------------------------------------
 */

static struct sprd_dmabuf_buffer *get_sprd_buffer(int fd, struct dma_buf *dmabuf)
{
	struct sprd_dmabuf_buffer *buffer;

	if (fd < 0 && !dmabuf) {
		pr_err("%s, input fd: %d, dmabuf: %p error\n", __func__, fd,
		       dmabuf);
		return ERR_PTR(-EINVAL);
	}

	if (fd >= 0) {
		dmabuf = dma_buf_get(fd);
		if (IS_ERR_OR_NULL(dmabuf)) {
			pr_err("%s, dmabuf=%p dma_buf_get error!\n", __func__,
			       dmabuf);
			return ERR_PTR(-EBADF);
		}
		buffer = dmabuf->priv;
		dma_buf_put(dmabuf);
	} else {
		buffer = dmabuf->priv;
	}

	return buffer;
}

void get_ion_user_info(int fd, bool map)
{
	int i;
	struct sprd_dmabuf_buffer *buffer;
	struct task_struct *task = current->group_leader;
	pid_t pid = task_pid_nr(task);
	struct dma_buf *dmabuf = dma_buf_get(fd);

	if (IS_ERR_OR_NULL(dmabuf))
		return;

	if (dmabuf->ops != &sprd_heap_buf_ops)
		goto out;

	buffer = dmabuf->priv;
	for (i = 0; i < MAX_MAP_USER; i++) {
		if (map) {
			if (!(buffer->mappers[i].fd)) {
				buffer->mappers[i].pid = pid;
				get_task_comm(buffer->mappers[i].task_name, task);
				buffer->mappers[i].fd = fd;
				buffer->mappers[i].valid = true;
				buffer->mappers[i].map_time = ktime_get_ns();
				break;
			}
		} else {
			if (buffer->mappers[i].pid == pid && buffer->mappers[i].fd == fd && buffer->mappers[i].valid) {
				memset((void *)(&buffer->mappers[i]), 0x0, sizeof(buffer->mappers[i]));
				break;
			}
		}
	}
	if (map && i == MAX_MAP_USER)
		pr_err("%s: pid %d fd %d\n", __func__, pid, fd);

out:
	dma_buf_put(dmabuf);
}

int sprd_ion_is_reserved(int fd, struct dma_buf *dmabuf, bool *reserved)
{
	struct sprd_dmabuf_buffer *buffer;

	buffer = get_sprd_buffer(fd, dmabuf);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	*reserved = buffer->reserved;

	return 0;
}
EXPORT_SYMBOL(sprd_ion_is_reserved);

int sprd_ion_get_sg_table(int fd, struct dma_buf *dmabuf,
			  struct sg_table **table, size_t *size)
{
	struct sprd_dmabuf_buffer *buffer;
	struct sprd_dmabuf_attachment *a;

	buffer = get_sprd_buffer(fd, dmabuf);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	if (list_empty(&buffer->attachments))
		return -EINVAL;

	a = list_first_entry(&buffer->attachments, struct sprd_dmabuf_attachment, list);
	*table = &a->table;
	*size = buffer->size;

	return 0;
}
EXPORT_SYMBOL(sprd_ion_get_sg_table);

int sprd_ion_get_buffer(int fd, struct dma_buf *dmabuf,
			void **buf, size_t *size)
{
	struct sprd_dmabuf_buffer *buffer;

	buffer = get_sprd_buffer(fd, dmabuf);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	*buf = (void *)buffer;
	*size = buffer->size;

	return 0;
}
EXPORT_SYMBOL(sprd_ion_get_buffer);

int sprd_ion_get_sg(void *buf, struct sg_table **table)
{
	struct sprd_dmabuf_buffer *buffer;
	struct sprd_dmabuf_attachment *a;

	if (!buf) {
		pr_err("%s, buf==NULL", __func__);
		return -EINVAL;
	}

	buffer = (struct sprd_dmabuf_buffer *)buf;

	if (list_empty(&buffer->attachments))
		return -EINVAL;

	a = list_first_entry(&buffer->attachments, struct sprd_dmabuf_attachment, list);
	*table = &a->table;

	return 0;
}
EXPORT_SYMBOL(sprd_ion_get_sg);

void sprd_ion_set_dma(void *buf, int id)
{
	struct sprd_dmabuf_buffer *buffer = (struct sprd_dmabuf_buffer *)buf;

	if (id < 0 || id >= SPRD_IOMMU_MAX) {
		pr_err("%s: iommu id %d out of range!", __func__, id);
		return;
	}

	if (buffer->iomap_cnt[id] == 0x6b6b6b6b) {
		pr_err("%s: iommu id %d memory corruption!", __func__, id);
		dump_stack();
	}
	buffer->iomap_cnt[id]++;
}
EXPORT_SYMBOL(sprd_ion_set_dma);

void sprd_ion_put_dma(void *buf, int id)
{
	struct sprd_dmabuf_buffer *buffer = (struct sprd_dmabuf_buffer *)buf;

	if (id < 0 || id >= SPRD_IOMMU_MAX) {
		pr_err("%s: iommu id %d out of range!", __func__, id);
		return;
	}

	if (buffer->iomap_cnt[id] == 0x6b6b6b6b) {
		pr_err("%s: iommu id %d memory corruption!", __func__, id);
		dump_stack();
	}
	buffer->iomap_cnt[id]--;
}
EXPORT_SYMBOL(sprd_ion_put_dma);

void sprd_ion_unmap_dma(void *buffer)
{
	int i;
	struct sprd_dmabuf_buffer *buf = (struct sprd_dmabuf_buffer *)buffer;
	struct sprd_iommu_unmap_data data;

	for (i = 0; i < SPRD_IOMMU_MAX; i++) {
		if (buf->iomap_cnt[i]) {
			struct sg_table *table;

			buf->iomap_cnt[i] = 0;
			if (sprd_ion_get_sg(buffer, &table))
				continue;
			data.buf = buffer;
			data.table = table;
			data.iova_size = buf->size;
			data.dev_id = i;
			sprd_iommu_unmap_orphaned(&data);
		}
	}
}
EXPORT_SYMBOL(sprd_ion_unmap_dma);

int sprd_ion_get_phys_addr(int fd, struct dma_buf *dmabuf,
			   unsigned long *phys_addr, size_t *size)
{
	struct sprd_dmabuf_buffer *buffer;

	buffer = get_sprd_buffer(fd, dmabuf);
	if (IS_ERR(buffer))
		return PTR_ERR(buffer);

	*phys_addr = buffer->paddr;
	*size = buffer->size;

	return 0;
}
EXPORT_SYMBOL(sprd_ion_get_phys_addr);

int sprd_ion_get_phys_addr_by_db(struct dma_buf *dmabuf,
				 unsigned long *phys_addr,
				 size_t *size)
{
	struct sprd_dmabuf_buffer *buffer;

	if (!dmabuf)
		return -EINVAL;

	buffer = dmabuf->priv;
	if (IS_ERR_OR_NULL(buffer))
		return PTR_ERR(buffer);

	*phys_addr = buffer->paddr;
	*size = buffer->size;

	return 0;
}
EXPORT_SYMBOL(sprd_ion_get_phys_addr_by_db);

void *sprd_ion_map_kernel(struct dma_buf *dmabuf, unsigned long offset)
{
	struct iosys_map map;
	int ret;

	if (!dmabuf)
		return ERR_PTR(-EINVAL);

	ret = dma_buf_begin_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
	if (ret)
		return ERR_PTR(ret);

	ret = dma_buf_vmap(dmabuf, &map);
	if (ret) {
		dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
		return ERR_PTR(ret);
	}
	if (map.is_iomem) {
		dma_buf_vunmap(dmabuf, &map);
		dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);
		return ERR_PTR(-ENOMEM);
	}

	return map.vaddr + offset;
}
EXPORT_SYMBOL(sprd_ion_map_kernel);

int sprd_ion_unmap_kernel(struct dma_buf *dmabuf, unsigned long offset)
{
	struct iosys_map map;
	int ret;

	if (!dmabuf)
		return -EINVAL;

	ret = dma_buf_vmap(dmabuf, &map);
	if (!ret)
		dma_buf_vunmap(dmabuf, &map);

	dma_buf_end_cpu_access(dmabuf, DMA_BIDIRECTIONAL);

	return 0;
}
EXPORT_SYMBOL(sprd_ion_unmap_kernel);

/* ---------------------------------------------------------------------
 * platform driver / device-tree plumbing - same contract as before:
 * one heap per child node, "reg" = heap id, "label" = name,
 * "memory-region" = reserved-memory phandle giving base+size.
 * "type" is parsed for compatibility but every heap is carveout-backed.
 * ---------------------------------------------------------------------
 */

struct sprd_heap_dt_entry {
	u32 id;
	u32 type;
	const char *name;
	phys_addr_t base;
	size_t size;
};

static struct sprd_heap_dt_entry *sprd_ion_parse_dt(struct platform_device *pdev)
{
	int i = 0, ret = 0;
	const struct device_node *parent = pdev->dev.of_node;
	struct device_node *child = NULL;
	struct sprd_heap_dt_entry *entries = NULL;
	u32 val = 0, type = 0;
	const char *name;
	u32 out_values[4];
	struct device_node *np_memory;

	for_each_child_of_node(parent, child)
		num_heaps++;

	pr_info("%s: num_heaps=%d\n", __func__, num_heaps);

	if (!num_heaps)
		return NULL;

	entries = kcalloc(num_heaps, sizeof(*entries), GFP_KERNEL);
	if (!entries)
		return ERR_PTR(-ENOMEM);

	for_each_child_of_node(parent, child) {
		ret = of_property_read_u32(child, "reg", &val);
		if (ret) {
			pr_err("%s: Unable to find reg key, ret=%d", __func__, ret);
			goto out;
		}
		entries[i].id = val;

		ret = of_property_read_string(child, "label", &name);
		if (ret) {
			pr_err("%s: Unable to find label key, ret=%d", __func__, ret);
			goto out;
		}
		entries[i].name = name;

		ret = of_property_read_u32(child, "type", &type);
		if (ret) {
			pr_err("%s: Unable to find type key, ret=%d", __func__, ret);
			goto out;
		}
		entries[i].type = type;

		np_memory = of_parse_phandle(child, "memory-region", 0);
		if (!np_memory) {
			entries[i].base = 0;
			entries[i].size = 0;
		} else {
#ifdef CONFIG_64BIT
			ret = of_property_read_u32_array(np_memory, "reg", out_values, 4);
			if (!ret) {
				entries[i].base = out_values[0];
				entries[i].base = entries[i].base << 32;
				entries[i].base |= out_values[1];

				entries[i].size = out_values[2];
				entries[i].size = entries[i].size << 32;
				entries[i].size |= out_values[3];
			} else {
				entries[i].base = 0;
				entries[i].size = 0;
			}
#else
			ret = of_property_read_u32_array(np_memory, "reg", out_values, 2);
			if (!ret) {
				entries[i].base = out_values[0];
				entries[i].size = out_values[1];
			} else {
				entries[i].base = 0;
				entries[i].size = 0;
			}
#endif
			of_node_put(np_memory);
		}

		pr_info("%s: heaps[%d]: id: %u %s type: %d base: 0x%llx size 0x%zx\n",
			__func__, i, entries[i].id, entries[i].name, entries[i].type,
			(u64)(entries[i].base), entries[i].size);
		++i;
	}
	return entries;
out:
	kfree(entries);
	return ERR_PTR(ret);
}

static int sprd_ion_probe(struct platform_device *pdev)
{
	int i, ret;
	struct sprd_heap_dt_entry *entries;
	struct dma_heap_export_info exp_info;

	entries = sprd_ion_parse_dt(pdev);
	if (IS_ERR(entries)) {
		pr_err("%s: parse dt failed with err %ld\n", __func__, PTR_ERR(entries));
		return PTR_ERR(entries);
	}
	if (!entries)
		return 0;

	carveout_heaps = kcalloc(num_heaps, sizeof(*carveout_heaps), GFP_KERNEL);
	if (!carveout_heaps) {
		ret = -ENOMEM;
		goto err_free_entries;
	}

	for (i = 0; i < num_heaps; i++) {
		struct sprd_carveout_heap *c = &carveout_heaps[i];

		if (!entries[i].base || !entries[i].size) {
			pr_err("%s: heap %s has no usable memory-region, skipping\n",
			       __func__, entries[i].name);
			continue;
		}

		c->base = entries[i].base;
		c->size = entries[i].size;
		c->id = entries[i].id;
		c->name = entries[i].name;

		c->base_vaddr = memremap(c->base, c->size, MEMREMAP_WB);
		if (!c->base_vaddr) {
			pr_err("%s: memremap failed for heap %s\n", __func__, c->name);
			ret = -ENOMEM;
			goto err_destroy_heaps;
		}

		c->pool = gen_pool_create(PAGE_SHIFT, -1);
		if (!c->pool) {
			memunmap(c->base_vaddr);
			ret = -ENOMEM;
			goto err_destroy_heaps;
		}
		/* gen_pool here tracks *physical* addresses directly, same
		 * trick the old ion_carveout_heap used.
		 */
		ret = gen_pool_add(c->pool, c->base, c->size, -1);
		if (ret) {
			gen_pool_destroy(c->pool);
			memunmap(c->base_vaddr);
			goto err_destroy_heaps;
		}

		memset(&exp_info, 0, sizeof(exp_info));
		exp_info.name = c->name;
		exp_info.ops = &sprd_carveout_heap_ops;
		exp_info.priv = c;

		c->heap = dma_heap_add(&exp_info);
		if (IS_ERR(c->heap)) {
			ret = PTR_ERR(c->heap);
			gen_pool_destroy(c->pool);
			memunmap(c->base_vaddr);
			goto err_destroy_heaps;
		}

		pr_info("%s: registered dma-buf heap '%s' (id=%u base=0x%llx size=0x%zx)\n",
			__func__, c->name, c->id, (u64)c->base, c->size);
	}

	kfree(entries);
	return 0;

err_destroy_heaps:
	while (--i >= 0) {
		if (carveout_heaps[i].pool)
			gen_pool_destroy(carveout_heaps[i].pool);
		if (carveout_heaps[i].base_vaddr)
			memunmap(carveout_heaps[i].base_vaddr);
	}
	kfree(carveout_heaps);
	carveout_heaps = NULL;
err_free_entries:
	kfree(entries);
	return ret;
}

/*
 * struct platform_driver.remove returns void as of upstream kernel
 * v6.11 (the old int-returning "remove" callback - "remove_new" during
 * the transition period - was dropped); android16-6.12 is past that
 * point, so this must be void, not int.
 */
static void sprd_ion_remove(struct platform_device *pdev)
{
	int i;

	for (i = 0; i < num_heaps; i++) {
		if (carveout_heaps[i].pool)
			gen_pool_destroy(carveout_heaps[i].pool);
		if (carveout_heaps[i].base_vaddr)
			memunmap(carveout_heaps[i].base_vaddr);
	}
	kfree(carveout_heaps);
	carveout_heaps = NULL;
}

static const struct of_device_id sprd_ion_ids[] = {
	{ .compatible = "sprd,ion"},
	{},
};

static struct platform_driver ion_driver = {
	.probe = sprd_ion_probe,
	.remove = sprd_ion_remove,
	.driver = {
		.name = "ion",
		.of_match_table = of_match_ptr(sprd_ion_ids),
	}
};

static int __init sprd_ion_init(void)
{
	int result = 0;

	result = platform_driver_register(&ion_driver);
	pr_info("%s,result:%d\n", __func__, result);
	return result;
}

static void __exit sprd_ion_exit(void)
{
	platform_driver_unregister(&ion_driver);
}

subsys_initcall(sprd_ion_init);
module_exit(sprd_ion_exit);

MODULE_IMPORT_NS(DMA_BUF);

MODULE_DESCRIPTION("SPRD DMA-BUF Heap Driver (ION-free, GKI 5.10 port)");
MODULE_LICENSE("GPL");
