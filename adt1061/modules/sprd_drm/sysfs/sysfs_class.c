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

#include <linux/kernel.h>
#include <linux/module.h>

#include "sysfs_display.h"

struct class *display_class;

static int __init display_class_init(void)
{
	pr_info("[drm] display class register\n");

	/*
	 * 2026 GKI 6.12 port note: class_create() dropped its owner-module
	 * argument upstream (the core tracks the owning module itself now).
	 */
	display_class = class_create("display");
	if (IS_ERR(display_class)) {
		pr_err("[drm] Unable to create display class\n");
		return PTR_ERR(display_class);
	}

	return 0;
}

/*
 * GKI 6.12 port note: this used to be its own loadable module, so
 * postcore_initcall(display_class_init) (which expands to module_init() when
 * built as a module) was fine. Now that this whole driver tree is
 * compiled into a single combined sprd_drm.ko, every file's own
 * module_init/module_exit-family macro would collide at link time
 * (duplicate init_module/cleanup_module). Instead, this exported
 * wrapper is called explicitly from sprd_drm.c's own module_init.
 */
int __init sprd_display_class_register(void)
{
	return display_class_init();
}

MODULE_AUTHOR("Leon He <leon.he@unisoc.com>");
MODULE_DESCRIPTION("Provide display class for hardware driver");
MODULE_LICENSE("GPL v2");
