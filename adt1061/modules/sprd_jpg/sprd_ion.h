/*
 * Copyright (C) 2012 Spreadtrum Communications Inc.
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

#ifndef _UAPI_SPRD_ION_H
#define _UAPI_SPRD_ION_H

#include <linux/dma-buf.h>
#include <linux/scatterlist.h>

enum sprd_ion_heap_ids {
	ION_HEAP_ID_SYSTEM = 0,
	ION_HEAP_ID_MM,
	ION_HEAP_ID_OVERLAY,
	ION_HEAP_ID_FB,
	ION_HEAP_ID_CAM,
	ION_HEAP_ID_VDSP,
};

#define ION_HEAP_ID_MASK_SYSTEM        (1<<ION_HEAP_ID_SYSTEM)
#define ION_HEAP_ID_MASK_MM            (1<<ION_HEAP_ID_MM)
#define ION_HEAP_ID_MASK_OVERLAY       (1<<ION_HEAP_ID_OVERLAY)
#define ION_HEAP_ID_MASK_FB            (1<<ION_HEAP_ID_FB)
#define ION_HEAP_ID_MASK_CAM           (1<<ION_HEAP_ID_CAM)
#define ION_HEAP_ID_MASK_VDSP          (1<<ION_HEAP_ID_VDSP)

#define ION_FLAG_SECURE  (1<<31)
#define ION_FLAG_NO_CLEAR (1 << 16)


enum SPRD_DEVICE_SYNC_TYPE {
	SPRD_DEVICE_PRIMARY_SYNC,
	SPRD_DEVICE_VIRTUAL_SYNC,
};

struct ion_phys_data {
	int fd_buffer;
	unsigned long phys;
	size_t size;
};

struct ion_msync_data {
	int fd_buffer;
	unsigned long vaddr;
	unsigned long paddr;
	size_t size;
};

struct ion_fence_data {
	uint32_t device_type;
	int life_value;
	int release_fence_fd;
	int retired_fence_fd;
};

struct ion_kmap_data {
	int fd_buffer;
	uint64_t kaddr;
	size_t size;
};

struct ion_kunmap_data {
	int fd_buffer;
};

/*
 * sprd_ion_* kernel API, now provided by the DMA-BUF-based sprd_ion
 * driver (converted alongside sprd_iommu/sprd_npu). Signatures are
 * unchanged from the old ION-backed driver, so sprd_jpg_common.c's
 * existing call sites need no changes - this header was just missing
 * them.
 */
int sprd_ion_is_reserved(int fd, struct dma_buf *dmabuf,
			  bool *reserved);

int sprd_ion_get_sg_table(int fd, struct dma_buf *dmabuf,
			   struct sg_table **table, size_t *size);

int sprd_ion_get_buffer(int fd, struct dma_buf *dmabuf,
			 void **buf, size_t *size);

int sprd_ion_get_sg(void *buf, struct sg_table **table);

void sprd_ion_set_dma(void *buf, int id);

void sprd_ion_put_dma(void *buf, int id);

void sprd_ion_unmap_dma(void *buffer);

int sprd_ion_get_phys_addr(int fd, struct dma_buf *dmabuf,
			    unsigned long *phys_addr, size_t *size);

int sprd_ion_get_phys_addr_by_db(struct dma_buf *dmabuf,
				  unsigned long *phys_addr,
				  size_t *size);

void *sprd_ion_map_kernel(struct dma_buf *dmabuf, unsigned long offset);

int sprd_ion_unmap_kernel(struct dma_buf *dmabuf, unsigned long offset);

#endif /* _UAPI_SPRD_ION_H */
