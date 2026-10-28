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
#define pr_fmt(__fmt) "[drm][%20s] "__fmt, __func__

#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/libfdt.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include "disp_lib.h"

/*
 * 2026 GKI 6.12 port note
 * -----------------------
 * filp_open()/kernel_read()/kernel_write() are on Android GKI's
 * "protected exports" list: direct kernel-space file I/O from an
 * out-of-tree/vendor module is intentionally blocked (this is the exact
 * boundary that produced the "Protected symbol: filp_open/kernel_read/
 * kernel_write (err -13)" lines at module load). It is not a bug and
 * cannot be worked around from the vendor module side - GKI simply does
 * not let a vendor .ko touch the filesystem from ring 0, to keep vendor
 * code inside normal Android sandboxing/SELinux instead of bypassing it.
 *
 * Both call sites below were debug/override hooks, never part of the
 * normal probe/display path (load_lcddtb_store()/wb_debug_store() in
 * sysfs_panel.c/sysfs_dpu.c, triggered only by a developer writing to a
 * sysfs file) - but the "err -13" warning is emitted at module *load*
 * time for every protected symbol the module merely references, whether
 * or not that code path ever runs, so the references had to go either
 * way. They're replaced with GKI-legal mechanisms below:
 *
 *   - load_dtb_to_mem(): now uses request_firmware(), the sanctioned way
 *     for a vendor driver to pull a named blob into kernel memory. The
 *     override DTB must be pushed to a firmware search path instead of
 *     an arbitrary file path, e.g.:
 *         adb push lcd.dtb /vendor/firmware/lcd.dtb
 *     (was: adb push lcd.dtb /data/lcd.dtb)
 *
 *   - dump_bmp32(): there is no vendor-module-callable kernel-space
 *     file-write API left under GKI, full stop - so the dump can no
 *     longer be written to a file from kernel space at all. Instead the
 *     assembled .bmp buffer is published through a debugfs blob; a
 *     developer reads it out with plain *userspace* file I/O, which has
 *     no GKI restriction:
 *         cat /sys/kernel/debug/sprd_drm/wb_dump.bmp > wb_dump.bmp
 */

struct bmp_header {
	u16 magic;
	u32 size;
	u32 unused;
	u32 start;
} __attribute__((__packed__));

struct dib_header {
	u32 size;
	u32 width;
	u32 height;
	u16 planes;
	u16 bpp;
	u32 compression;
	u32 data_size;
	u32 h_res;
	u32 v_res;
	u32 colours;
	u32 important_colours;
	u32 red_mask;
	u32 green_mask;
	u32 blue_mask;
	u32 alpha_mask;
	u32 colour_space;
	u32 unused[12];
} __attribute__((__packed__));

int str_to_u32_array(const char *p, u32 base, u32 array[])
{
	const char *start = p;
	char str[12];
	int length = 0;
	int i, ret;

	pr_info("input: %s", p);

	for (i = 0 ; i < 255; i++) {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		start = p;

		while ((*p != ' ') && (*p != '\0'))
			p++;

		if ((p - start) >= sizeof(str))
			break;

		memset(str, 0, sizeof(str));
		memcpy(str, start, p - start);

		ret = kstrtou32(str, base, &array[i]);
		if (ret) {
			DRM_ERROR("input format error\n");
			break;
		}

		length++;
	}

	return length;
}
EXPORT_SYMBOL_GPL(str_to_u32_array);

int load_dtb_to_mem(struct device *dev, const char *name, void **blob)
{
	const struct firmware *fw;
	const struct fdt_header *dtbhead;
	u32 count;
	int ret;

	ret = request_firmware(&fw, name, dev);
	if (ret < 0) {
		DRM_ERROR("%s: request_firmware(\"%s\") failed: %d "
			  "(push the override dtb to /vendor/firmware/%s)\n",
			  __func__, name, ret, name);
		return ret;
	}

	if (fw->size < sizeof(*dtbhead)) {
		DRM_ERROR("%s: firmware \"%s\" is smaller than an fdt header\n",
			  __func__, name);
		release_firmware(fw);
		return -EINVAL;
	}

	dtbhead = (const struct fdt_header *)fw->data;
	count = ntohl(dtbhead->totalsize);
	if (count == 0 || count > fw->size) {
		DRM_ERROR("%s: bad fdt totalsize %u (firmware size %zu)\n",
			  __func__, count, fw->size);
		release_firmware(fw);
		return -EINVAL;
	}

	*blob = kzalloc(count, GFP_KERNEL);
	if (*blob == NULL) {
		release_firmware(fw);
		return -ENOMEM;
	}

	memcpy(*blob, fw->data, count);
	release_firmware(fw);

	return 0;
}
EXPORT_SYMBOL_GPL(load_dtb_to_mem);

int str_to_u8_array(const char *p, u32 base, u8 array[])
{
	const char *start = p;
	char str[12];
	int length = 0;
	int i, ret;

	pr_info("input: %s", p);

	for (i = 0 ; i < 255; i++) {
		while (*p == ' ')
			p++;
		if (*p == '\0')
			break;
		start = p;

		while ((*p != ' ') && (*p != '\0'))
			p++;

		if ((p - start) >= sizeof(str))
			break;

		memset(str, 0, sizeof(str));
		memcpy(str, start, p - start);

		ret = kstrtou8(str, base, &array[i]);
		if (ret) {
			DRM_ERROR("input format error\n");
			break;
		}

		length++;
	}

	return length;
}
EXPORT_SYMBOL_GPL(str_to_u8_array);

static struct {
	struct mutex lock;
	struct dentry *dir;
	void *buf;
	struct debugfs_blob_wrapper blob;
} g_bmp_dump = {
	.lock = __MUTEX_INITIALIZER(g_bmp_dump.lock),
};

int dump_bmp32(const char *p, u32 width, u32 height,
		bool noflip, const char *filename)
{
	void *buf, *old_buf;
	u8 *dst;
	size_t pixels_size = (size_t)4 * width * height;
	size_t total_size = sizeof(struct bmp_header) +
			     sizeof(struct dib_header) + pixels_size;
	struct dib_header dib_header = {
		.size = sizeof(dib_header),
		.width = width,
		.height = noflip ? -height : height,
		.planes = 1,
		.bpp = 32,
		.compression = 3,
		.data_size = 4 * width * height,
		.h_res = 0xB13,
		.v_res = 0xB13,
		.colours = 0,
		.important_colours = 0,
		.red_mask = 0x000000FF,
		.green_mask = 0x0000FF00,
		.blue_mask = 0x00FF0000,
		.alpha_mask = 0xFF000000,
		.colour_space = 0x57696E20,
	};
	struct bmp_header bmp_header = {
		.magic = 0x4d42,
		.size = total_size,
		.start = sizeof(bmp_header) + sizeof(dib_header),
	};

	buf = kmalloc(total_size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	dst = buf;
	memcpy(dst, &bmp_header, sizeof(bmp_header));
	dst += sizeof(bmp_header);
	memcpy(dst, &dib_header, sizeof(dib_header));
	dst += sizeof(dib_header);
	memcpy(dst, p, pixels_size);

	mutex_lock(&g_bmp_dump.lock);

	if (!g_bmp_dump.dir)
		g_bmp_dump.dir = debugfs_create_dir("sprd_drm", NULL);

	/* Drop the previous blob file before swapping the backing buffer,
	 * since debugfs_create_blob() keeps no reference-counted copy of
	 * the data it points at. debugfs_lookup_and_remove() (6.12 has it)
	 * does the lookup+remove+dput in one call so no dentry ref leaks.
	 */
	debugfs_lookup_and_remove("wb_dump.bmp", g_bmp_dump.dir);

	old_buf = g_bmp_dump.buf;
	g_bmp_dump.buf = buf;
	g_bmp_dump.blob.data = buf;
	g_bmp_dump.blob.size = total_size;

	debugfs_create_blob("wb_dump.bmp", 0440, g_bmp_dump.dir,
			     &g_bmp_dump.blob);

	mutex_unlock(&g_bmp_dump.lock);

	kfree(old_buf);

	DRM_INFO("%s: wrote dump (%ux%u) to debugfs, read it with "
		 "`cat /sys/kernel/debug/sprd_drm/wb_dump.bmp > %s`\n",
		 __func__, width, height, filename);

	return 0;
}
EXPORT_SYMBOL_GPL(dump_bmp32);

void *disp_ops_attach(const char *str, struct list_head *head)
{
	struct ops_list *list;
	const char *ver;

	list_for_each_entry(list, head, head) {
		ver = list->entry->ver;
		if (!strcmp(str, ver))
			return list->entry->ops;
	}

	DRM_ERROR("attach disp ops %s failed\n", str);
	return NULL;
}
EXPORT_SYMBOL_GPL(disp_ops_attach);

int disp_ops_register(struct ops_entry *entry, struct list_head *head)
{
	struct ops_list *list;

	list = kzalloc(sizeof(struct ops_list), GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	list->entry = entry;
	list_add(&list->head, head);

	return 0;
}
EXPORT_SYMBOL_GPL(disp_ops_register);

struct device *sprd_disp_pipe_get_by_port(struct device *dev, int port)
{
	struct device_node *np = dev->of_node;
	struct device_node *endpoint;
	struct device_node *remote_node;
	struct platform_device *remote_pdev;

	endpoint = of_graph_get_endpoint_by_regs(np, port, 0);
	if (!endpoint) {
		DRM_ERROR("%pOF/port%d/endpoint0 was not found\n",
			  np, port);
		return NULL;
	}

	remote_node = of_graph_get_remote_port_parent(endpoint);
	if (!remote_node) {
		DRM_ERROR("device node was not found by endpoint0\n");
		return NULL;
	}

	remote_pdev = of_find_device_by_node(remote_node);
	if (remote_pdev == NULL) {
		DRM_ERROR("find %pOF platform device failed\n",
			  remote_node);
		return NULL;
	}

	return &remote_pdev->dev;
}
EXPORT_SYMBOL_GPL(sprd_disp_pipe_get_by_port);

struct device *sprd_disp_pipe_get_input(struct device *dev)
{
	return sprd_disp_pipe_get_by_port(dev, 1);
}
EXPORT_SYMBOL_GPL(sprd_disp_pipe_get_input);

struct device *sprd_disp_pipe_get_output(struct device *dev)
{
	return sprd_disp_pipe_get_by_port(dev, 0);
}
EXPORT_SYMBOL_GPL(sprd_disp_pipe_get_output);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("leon.he@unisoc.com");
MODULE_DESCRIPTION("display common API library");
