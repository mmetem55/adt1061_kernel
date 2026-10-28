
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

#ifndef _DISP_LIB_H_
#define _DISP_LIB_H_

#include <drm/drm_device.h>
#include <drm/drm_print.h>
#include <linux/list.h>

#ifdef pr_fmt
#undef pr_fmt
#define pr_fmt(__fmt) "[drm][%20s] "__fmt, __func__
#endif

struct ops_entry {
	const char *ver;
	void *ops;
};

struct ops_list {
	struct list_head head;
	struct ops_entry *entry;
};

int str_to_u32_array(const char *p, u32 base, u32 array[]);
int str_to_u8_array(const char *p, u32 base, u8 array[]);
int dump_bmp32(const char *p, u32 width, u32 height,
		bool bgra, const char *filename);
/*
 * 2026 GKI 6.12 port: this now loads via request_firmware(), not a raw
 * file path - see disp_lib.c. 'name' is a firmware name looked up under
 * the standard firmware search paths (e.g. /vendor/firmware/<name>),
 * and 'dev' is the requesting device (used for firmware_class logging
 * and any /lib/firmware/<dev-specific> search order).
 */
int load_dtb_to_mem(struct device *dev, const char *name, void **blob);

void *disp_ops_attach(const char *str, struct list_head *head);
int disp_ops_register(struct ops_entry *entry, struct list_head *head);

struct device *sprd_disp_pipe_get_by_port(struct device *dev, int port);
struct device *sprd_disp_pipe_get_input(struct device *dev);
struct device *sprd_disp_pipe_get_output(struct device *dev);

#endif
