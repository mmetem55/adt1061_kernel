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

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "sprd_dvfs_apsys.h"

/*
 * sprd_apsys_dvfs.ko is built from many source files - this one, the
 * per-board apsys ops variants under sys/, the dpu/vdsp/vsp top-level
 * drivers (sprd_dvfs_dpu.c, sprd_dvfs_vdsp.c, sprd_dvfs_vsp.c) and
 * their own per-board ops variants under dpu/, vdsp/, vsp/. Each used
 * to register itself with its own initcall macro (module_init,
 * subsys_initcall, ...), but every initcall level collapses to
 * module_init() when MODULE is defined, so building them all into one
 * .ko produced many conflicting global init_module/cleanup_module
 * symbols ("ld.lld: error: duplicate symbol: init_module"). All of
 * those macros were removed and the functions below made callable
 * from here, the one surviving initcall/exit for the whole module.
 */
int apsys_dvfs_register_roc1(void);
int apsys_dvfs_register_sharkl5(void);
int apsys_dvfs_register_sharkl5pro(void);

int dpu_dvfs_register_roc1(void);
int dpu_dvfs_register_sharkl5(void);
int dpu_dvfs_register_sharkl5pro(void);

int vdsp_dvfs_register_roc1(void);
int vdsp_dvfs_register_sharkl5pro(void);

int vsp_dvfs_register_roc1(void);
int vsp_dvfs_register_sharkl5(void);
int vsp_dvfs_register_sharkl5pro(void);

int dpu_dvfs_init(void);
void dpu_dvfs_exit(void);
int vdsp_dvfs_init(void);
void vdsp_dvfs_exit(void);
int vsp_dvfs_init(void);
void vsp_dvfs_exit(void);

#define to_apsys(DEV)	container_of((DEV), struct apsys_dev, dev)

LIST_HEAD(apsys_dvfs_head);
DEFINE_MUTEX(apsys_glb_reg_lock);

struct class *dvfs_class;
struct apsys_regmap regmap_ctx;

void *dvfs_ops_attach(const char *str, struct list_head *head)
{
	struct dvfs_ops_list *list;
	const char *ver;

	list_for_each_entry(list, head, head) {
		ver = list->entry->ver;
		if (!strcmp(str, ver))
			return list->entry->ops;
	}

	pr_err("attach dvfs ops %s failed\n", str);

	return NULL;
}
EXPORT_SYMBOL_GPL(dvfs_ops_attach);

int dvfs_ops_register(struct dvfs_ops_entry *entry, struct list_head *head)
{
	struct dvfs_ops_list *list;

	list = kzalloc(sizeof(struct dvfs_ops_list), GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	list->entry = entry;
	list_add(&list->head, head);

	return 0;
}
EXPORT_SYMBOL_GPL(dvfs_ops_register);

static ssize_t top_cur_volt_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct apsys_dev *apsys = to_apsys(dev);
	int ret, cur_volt;

	if (apsys->dvfs_ops && apsys->dvfs_ops->top_cur_volt) {
		cur_volt = apsys->dvfs_ops->top_cur_volt();
	} else {
		pr_info("%s: apsys ops null\n", __func__);
		cur_volt = -EINVAL;
	}

	if (cur_volt == 0)
		ret = sprintf(buf, "0.7v\n");
	else if (cur_volt == 1)
		ret = sprintf(buf, "0.75v\n");
	else if (cur_volt == 2)
		ret = sprintf(buf, "0.8v\n");
	else
		ret = sprintf(buf, "undefined\n");

	return ret;
}

static ssize_t apsys_hold_en_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct apsys_dev *apsys = to_apsys(dev);
	int ret, hold_en;

	ret = sscanf(buf, "%d\n", &hold_en);
	if (ret == 0)
		return -EINVAL;

	if (apsys->dvfs_ops && apsys->dvfs_ops->apsys_hold_en)
		apsys->dvfs_ops->apsys_hold_en(hold_en);
	else
		pr_info("%s: apsys ops null\n", __func__);

	return count;
}

static ssize_t apsys_force_en_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct apsys_dev *apsys = to_apsys(dev);
	int ret, force_en;

	ret = sscanf(buf, "%d\n", &force_en);
	if (ret == 0)
		return -EINVAL;

	if (apsys->dvfs_ops && apsys->dvfs_ops->apsys_force_en)
		apsys->dvfs_ops->apsys_force_en(force_en);
	else
		pr_info("%s: apsys ops null\n", __func__);

	return count;
}

static ssize_t apsys_auto_gate_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct apsys_dev *apsys = to_apsys(dev);
	int ret, gate_sel;

	ret = sscanf(buf, "%d\n", &gate_sel);
	if (ret == 0)
		return -EINVAL;

	if (apsys->dvfs_ops && apsys->dvfs_ops->apsys_auto_gate)
		apsys->dvfs_ops->apsys_auto_gate(gate_sel);
	else
		pr_info("%s: apsys ops null\n", __func__);

	return count;
}

static ssize_t apsys_wait_window_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct apsys_dev *apsys = to_apsys(dev);
	int ret, wait_window;

	ret = sscanf(buf, "%d\n", &wait_window);
	if (ret == 0)
		return -EINVAL;

	if (apsys->dvfs_ops && apsys->dvfs_ops->apsys_wait_window)
		apsys->dvfs_ops->apsys_wait_window(wait_window);
	else
		pr_info("%s: apsys ops null\n", __func__);

	return count;
}

static ssize_t apsys_min_volt_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t count)
{
	struct apsys_dev *apsys = to_apsys(dev);
	int ret, min_volt;

	ret = sscanf(buf, "%d\n", &min_volt);
	if (ret == 0)
		return -EINVAL;

	if (apsys->dvfs_ops && apsys->dvfs_ops->apsys_min_volt)
		apsys->dvfs_ops->apsys_min_volt(min_volt);
	else
		pr_info("%s: apsys ops null\n", __func__);

	return count;
}

static DEVICE_ATTR(cur_volt, 0444, top_cur_volt_show, NULL);
static DEVICE_ATTR(hold_en, 0200, NULL, apsys_hold_en_store);
static DEVICE_ATTR(force_en, 0200, NULL, apsys_force_en_store);
static DEVICE_ATTR(auto_gate, 0200, NULL, apsys_auto_gate_store);
static DEVICE_ATTR(wait_window, 0200, NULL, apsys_wait_window_store);
static DEVICE_ATTR(min_volt, 0200, NULL, apsys_min_volt_store);

static struct attribute *apsys_attrs[] = {
	&dev_attr_cur_volt.attr,
	&dev_attr_hold_en.attr,
	&dev_attr_force_en.attr,
	&dev_attr_auto_gate.attr,
	&dev_attr_wait_window.attr,
	&dev_attr_min_volt.attr,
	NULL,
};

static const struct attribute_group apsys_group = {
	.attrs = apsys_attrs,
};

static __maybe_unused int apsys_dvfs_suspend(struct device *dev)
{
	pr_info("%s()\n", __func__);

	return 0;
}

static __maybe_unused int apsys_dvfs_resume(struct device *dev)
{
	struct apsys_dev *apsys = dev_get_drvdata(dev);

	pr_info("%s()\n", __func__);

	if (apsys->dvfs_ops && apsys->dvfs_ops->dvfs_init)
		apsys->dvfs_ops->dvfs_init(apsys);

	return 0;
}

static SIMPLE_DEV_PM_OPS(apsys_dvfs_pm, apsys_dvfs_suspend,
			 apsys_dvfs_resume);

static int apsys_dvfs_class_init(void)
{
	pr_info("apsys dvfs class init\n");

	dvfs_class = class_create("dvfs");
	if (IS_ERR(dvfs_class)) {
		pr_err("Unable to create apsys dvfs class\n");
		return PTR_ERR(dvfs_class);
	}

	return 0;
}

static int apsys_dvfs_device_create(struct apsys_dev *apsys,
				struct device *parent)
{
	int ret;

	apsys->dev.class = dvfs_class;
	apsys->dev.parent = parent;
	apsys->dev.of_node = parent->of_node;
	dev_set_name(&apsys->dev, "apsys");
	dev_set_drvdata(&apsys->dev, apsys);

	ret = device_register(&apsys->dev);
	if (ret)
		pr_err("apsys dvfs device register failed\n");

	return ret;
}

static int apsys_dvfs_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node;
	struct apsys_dev *apsys;
	const char *str = NULL;
	void __iomem *base;
	struct resource r;
	int ret;

	apsys = devm_kzalloc(dev, sizeof(*apsys), GFP_KERNEL);
	if (!apsys)
		return -ENOMEM;

	str = (char *)of_device_get_match_data(dev);

	apsys->dvfs_ops = apsys_dvfs_ops_attach(str);
	if (!apsys->dvfs_ops) {
		pr_err("attach apsys dvfs ops %s failed\n", str);
		return -EINVAL;
	}

	if (of_address_to_resource(np, 0, &r)) {
		pr_err("parse apsys base address failed\n");
		return -ENODEV;
	}

	base = ioremap(r.start, resource_size(&r));
	if (IS_ERR(base)) {
		pr_err("ioremap apsys dvfs address failed\n");
		return -EFAULT;
	}
	regmap_ctx.apsys_base = (unsigned long)base;

	apsys_dvfs_class_init();
	apsys_dvfs_device_create(apsys, dev);

	ret = sysfs_create_group(&(apsys->dev.kobj), &apsys_group);
	if (ret) {
		dev_err(dev, "apsys create sysfs class failed, ret=%d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, apsys);

	if (apsys->dvfs_ops && apsys->dvfs_ops->parse_dt)
		apsys->dvfs_ops->parse_dt(apsys, np);

	if (apsys->dvfs_ops && apsys->dvfs_ops->dvfs_init)
		apsys->dvfs_ops->dvfs_init(apsys);

	if (apsys->dvfs_ops && apsys->dvfs_ops->top_dvfs_init)
		apsys->dvfs_ops->top_dvfs_init();
	pr_info("apsys module registered\n");

	return 0;
}
static void apsys_dvfs_remove(struct platform_device *pdev)
{
}

static const struct of_device_id apsys_dvfs_of_match[] = {
	{ .compatible = "sprd,hwdvfs-apsys-sharkl5",
	  .data = "sharkl5" },
	{ .compatible = "sprd,hwdvfs-apsys-roc1",
	  .data = "roc1" },
	{ .compatible = "sprd,hwdvfs-apsys-sharkl5pro",
	  .data = "sharkl5pro" },
	{ },
};

MODULE_DEVICE_TABLE(of, apsys_dvfs_of_match);

static struct platform_driver apsys_dvfs_driver = {
	.probe	= apsys_dvfs_probe,
	.remove	= apsys_dvfs_remove,
	.driver = {
		.name = "apsys-dvfs",
		.pm	= &apsys_dvfs_pm,
		.of_match_table = apsys_dvfs_of_match,
	},
};

static int __init apsys_dvfs_register(void)
{
	int ret;

	/* Register every board variant's ops table for each layer. */
	apsys_dvfs_register_roc1();
	apsys_dvfs_register_sharkl5();
	apsys_dvfs_register_sharkl5pro();

	dpu_dvfs_register_roc1();
	dpu_dvfs_register_sharkl5();
	dpu_dvfs_register_sharkl5pro();

	vdsp_dvfs_register_roc1();
	vdsp_dvfs_register_sharkl5pro();

	vsp_dvfs_register_roc1();
	vsp_dvfs_register_sharkl5();
	vsp_dvfs_register_sharkl5pro();

	/*
	 * GKI 6.12 port note: apsys_dvfs_driver's own probe() is what
	 * ioremaps the apsys register block and stores it in
	 * regmap_ctx.apsys_base/top_base - every dpu/vdsp/vsp *_dvfs_sharkl5*
	 * /roc1 ops file dereferences that same regmap_ctx. If apsys_base is
	 * still NULL (its default, zero-initialized value) when dpu/vdsp/vsp's
	 * own platform_driver_register() runs below and that triggers a
	 * synchronous probe() (because the matching DT node is already
	 * present), each driver's dvfs_init() callback writes through a NULL
	 * regmap_ctx.apsys_base pointer - exactly the NULL-deref panic seen in
	 * dpu_dvfs_probe() -> dpu->dvfs_ops->dvfs_init(). So apsys_dvfs_driver
	 * must be registered (and thus probed) *first*, not last.
	 */
	ret = platform_driver_register(&apsys_dvfs_driver);
	if (ret) {
		pr_err("apsys_dvfs_driver register failed: %d\n", ret);
		return ret;
	}

	/* Then bring up each layer's own top-level platform driver. */
	ret = dpu_dvfs_init();
	if (ret)
		pr_err("dpu_dvfs_init failed: %d\n", ret);

	ret = vdsp_dvfs_init();
	if (ret)
		pr_err("vdsp_dvfs_init failed: %d\n", ret);

	ret = vsp_dvfs_init();
	if (ret)
		pr_err("vsp_dvfs_init failed: %d\n", ret);

	return 0;
}

static void __exit apsys_dvfs_unregister(void)
{
	vsp_dvfs_exit();
	vdsp_dvfs_exit();
	dpu_dvfs_exit();
	platform_driver_unregister(&apsys_dvfs_driver);
}

subsys_initcall(apsys_dvfs_register);
module_exit(apsys_dvfs_unregister);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("sprd apsys dvfs driver");
MODULE_AUTHOR("Kevin Tang <kevin.tang@unisoc.com>");
