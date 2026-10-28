/*
 * Copyright (C) 2012--2016 Spreadtrum Communications Inc.
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
#include <linux/cdev.h>
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/of_device.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include "sprd_iommu.h"
#include "sprd_ion.h"
#include <linux/uaccess.h>
#include "vsp_common.h"

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "sprd-vsp: " fmt

unsigned int codec_instance_count[VSP_CODEC_INSTANCE_COUNT_MAX];
struct register_gpr regs[ARRAY_SIZE(tb_name)];

struct clk *vsp_get_clk_src_name(struct clock_name_map_t clock_name_map[],
				unsigned int freq_level,
				unsigned int max_freq_level)
{
	if (freq_level >= max_freq_level) {
		pr_info("set freq_level to 0\n");
		freq_level = 0;
	}

	pr_debug(" freq_level %d %s\n", freq_level,
		 clock_name_map[freq_level].name);
	return clock_name_map[freq_level].clk_parent;
}

int find_vsp_freq_level(struct clock_name_map_t clock_name_map[],
			unsigned long freq,
			unsigned int max_freq_level)
{
	int level = 0;
	int i;

	for (i = 0; i < max_freq_level; i++) {
		if (clock_name_map[i].freq == freq) {
			level = i;
			break;
		}
	}
	return level;
}

/*
 * compat_vsp_ioctl() (32-bit compat ioctl bridge for VSP_GET_IOVA /
 * VSP_FREE_IOVA) relied on compat_alloc_user_space(), which has been
 * removed from the kernel entirely (no replacement API). Confirmed
 * with the maintainer that no 32-bit/compat userspace process ever
 * calls into this driver on this target, so this compat path (and its
 * .compat_ioctl registration in sprd_vsp.c) has been dropped rather
 * than reimplemented. If 32-bit compat support is ever needed again,
 * this needs a proper from-scratch redesign (e.g. translating the
 * compat struct entirely in kernel space and calling vsp_get_iova()/
 * vsp_free_iova() directly instead of bouncing through a fake
 * user-space scratch buffer).
 */

int vsp_get_iova(struct vsp_dev_t *vsp_hw_dev,
		 struct vsp_iommu_map_data *mapdata, void __user *arg)
{
	int ret = 0;
	struct sprd_iommu_map_data iommu_map_data;
	u32 power_state1, vsp_eb_reg;

	vsp_clk_enable(vsp_hw_dev);
	sprd_iommu_resume(vsp_hw_dev->vsp_dev);
	ret = sprd_ion_get_buffer(mapdata->fd, NULL,
					&(iommu_map_data.buf),
					&iommu_map_data.iova_size);
	if (ret) {
		pr_err("get_sg_table failed, ret %d\n", ret);
		sprd_iommu_suspend(vsp_hw_dev->vsp_dev);
		vsp_clk_disable(vsp_hw_dev);
		return ret;
	}
	if (vsp_hw_dev->version == SHARKL3) {
		regmap_read(regs[PMU_PWR_STATUS].gpr, regs[PMU_PWR_STATUS].reg,
			&power_state1);
		regmap_read(regs[VSP_DOMAIN_EB].gpr, regs[VSP_DOMAIN_EB].reg,
			&vsp_eb_reg);
		pr_debug("reg 0x%x, vsp_power 0x%x, reg 0x%x, vsp_eb 0x%x\n",
			regs[PMU_PWR_STATUS].reg, power_state1, regs[VSP_DOMAIN_EB].reg,
			vsp_eb_reg);
	}
	iommu_map_data.ch_type = SPRD_IOMMU_FM_CH_RW;
	ret = sprd_iommu_map(vsp_hw_dev->vsp_dev, &iommu_map_data);
	if (!ret) {
		mapdata->iova_addr = iommu_map_data.iova_addr;
		mapdata->size = iommu_map_data.iova_size;
		pr_debug("vsp iommu map success iova addr 0x%lx size 0x%zx\n",
			mapdata->iova_addr, mapdata->size);
		ret = copy_to_user((void __user *)arg, (void *)mapdata,
					sizeof(struct vsp_iommu_map_data));
		if (ret) {
			pr_err("copy_to_user failed, ret %d\n", ret);
			sprd_iommu_suspend(vsp_hw_dev->vsp_dev);
			vsp_clk_disable(vsp_hw_dev);
			return ret;
		}
	} else
		pr_err("vsp iommu map failed, ret %d, map size 0x%zx\n",
			ret, iommu_map_data.iova_size);
	sprd_iommu_suspend(vsp_hw_dev->vsp_dev);
	vsp_clk_disable(vsp_hw_dev);
	return ret;
}
int vsp_free_iova(struct vsp_dev_t *vsp_hw_dev,
		  struct vsp_iommu_map_data *ummapdata)
{
	int ret = 0;
	struct sprd_iommu_unmap_data iommu_ummap_data;

	vsp_clk_enable(vsp_hw_dev);
	sprd_iommu_resume(vsp_hw_dev->vsp_dev);
	iommu_ummap_data.iova_addr = ummapdata->iova_addr;
	iommu_ummap_data.iova_size = ummapdata->size;
	iommu_ummap_data.ch_type = SPRD_IOMMU_FM_CH_RW;
	iommu_ummap_data.buf = NULL;

	ret = sprd_iommu_unmap(vsp_hw_dev->vsp_dev,
					&iommu_ummap_data);

	if (ret)
		pr_err("vsp iommu-unmap failed ret %d addr&size 0x%lx 0x%zx\n",
			ret, ummapdata->iova_addr, ummapdata->size);
	else
		pr_debug("vsp iommu-unmap success iova addr 0x%lx size 0x%zx\n",
			ummapdata->iova_addr, ummapdata->size);
	sprd_iommu_suspend(vsp_hw_dev->vsp_dev);
	vsp_clk_disable(vsp_hw_dev);

	return ret;
}

int vsp_get_mm_clk(struct vsp_dev_t *vsp_hw_dev)
{
	int ret = 0;
	struct clk *clk_mm_eb;
	struct clk *clk_vsp_mq_ahb_eb;
	struct clk *clk_vsp_ahb_mmu_eb;
	struct clk *clk_axi_gate_vsp;
	struct clk *clk_ahb_gate_vsp_eb;
#if !IS_ENABLED(CONFIG_SPRD_APSYS_DVFS_DEVFREQ)
	struct clk *clk_vsp;
#endif
	struct clk *clk_ahb_vsp;
	struct clk *clk_emc_vsp;
	struct clk *clk_parent;

	if (vsp_hw_dev->version == SHARKL3  || vsp_hw_dev->version == PIKE2) {
		clk_mm_eb = devm_clk_get(vsp_hw_dev->vsp_dev, "clk_mm_eb");

		if (IS_ERR_OR_NULL(clk_mm_eb)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
				"clk_mm_eb", clk_mm_eb);
			vsp_hw_dev->clk_mm_eb = NULL;
			ret = -EINVAL;
			goto errout;
		} else
			vsp_hw_dev->clk_mm_eb = clk_mm_eb;

		clk_axi_gate_vsp = devm_clk_get(vsp_hw_dev->vsp_dev,
			"clk_axi_gate_vsp");
		if (IS_ERR_OR_NULL(clk_axi_gate_vsp)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
				"clk_axi_gate_vsp",
			clk_axi_gate_vsp);
			vsp_hw_dev->clk_axi_gate_vsp = NULL;
			vsp_hw_dev->light_sleep_en = false;
			goto errout;
		} else {
			vsp_hw_dev->clk_axi_gate_vsp = clk_axi_gate_vsp;
			vsp_hw_dev->light_sleep_en = false;
		}
	}

	clk_ahb_gate_vsp_eb =
	    devm_clk_get(vsp_hw_dev->vsp_dev, "clk_ahb_gate_vsp_eb");

	if (IS_ERR_OR_NULL(clk_ahb_gate_vsp_eb)) {
		pr_err("Failed: Can't get clock [%s]! %p\n",
		       "clk_ahb_gate_vsp_eb", clk_ahb_gate_vsp_eb);
		ret = -EINVAL;
		vsp_hw_dev->clk_ahb_gate_vsp_eb = NULL;
		goto errout;
	} else
		vsp_hw_dev->clk_ahb_gate_vsp_eb = clk_ahb_gate_vsp_eb;

#if !IS_ENABLED(CONFIG_SPRD_APSYS_DVFS_DEVFREQ)
	clk_vsp = devm_clk_get(vsp_hw_dev->vsp_dev, "clk_vsp");

	if (IS_ERR_OR_NULL(clk_vsp)) {
		pr_err("Failed: Can't get clock [%s]! %p\n", "clk_vsp",
		       clk_vsp);
		ret = -EINVAL;
		vsp_hw_dev->vsp_clk = NULL;
		goto errout;
	} else
		vsp_hw_dev->vsp_clk = clk_vsp;
#endif

	if (vsp_hw_dev->version == SHARKL3) {
		clk_ahb_vsp =
			devm_clk_get(vsp_hw_dev->vsp_dev, "clk_ahb_vsp");

		if (IS_ERR_OR_NULL(clk_ahb_vsp)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
				   "clk_ahb_vsp", clk_ahb_vsp);
			ret = -EINVAL;
			goto errout;
		} else
			vsp_hw_dev->clk_ahb_vsp = clk_ahb_vsp;

		clk_parent = devm_clk_get(vsp_hw_dev->vsp_dev,
				"clk_ahb_vsp_parent");
		if (IS_ERR_OR_NULL(clk_parent)) {
			pr_err("clock[%s]: failed to get parent in probe!\n",
				 "clk_ahb_vsp_parent");
			ret = -EINVAL;
			goto errout;
		} else
			vsp_hw_dev->ahb_parent_clk = clk_parent;

		clk_emc_vsp =
			devm_clk_get(vsp_hw_dev->vsp_dev, "clk_emc_vsp");

		if (IS_ERR_OR_NULL(clk_emc_vsp)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
				   "clk_emc_vsp", clk_emc_vsp);
			ret = -EINVAL;
			goto errout;
		} else
			vsp_hw_dev->clk_emc_vsp = clk_emc_vsp;

		clk_parent = devm_clk_get(vsp_hw_dev->vsp_dev,
				"clk_emc_vsp_parent");
		if (IS_ERR_OR_NULL(clk_parent)) {
			pr_err("clock[%s]: failed to get parent in probe!\n",
				 "clk_emc_vsp_parent");
			ret = -EINVAL;
			goto errout;
		} else
			vsp_hw_dev->emc_parent_clk = clk_parent;

		clk_vsp_ahb_mmu_eb =
			devm_clk_get(vsp_hw_dev->vsp_dev,
				"clk_vsp_ahb_mmu_eb");

		if (IS_ERR_OR_NULL(clk_vsp_ahb_mmu_eb)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
				   "clk_vsp_ahb_mmu_eb", clk_vsp_ahb_mmu_eb);
			ret = -EINVAL;
			vsp_hw_dev->clk_vsp_ahb_mmu_eb = NULL;
			goto errout;
		} else
			vsp_hw_dev->clk_vsp_ahb_mmu_eb = clk_vsp_ahb_mmu_eb;
	}

	if (vsp_hw_dev->version == PIKE2) {
		clk_vsp_mq_ahb_eb =
		    devm_clk_get(vsp_hw_dev->vsp_dev, "clk_vsp_mq_ahb_eb");

		if (IS_ERR(clk_vsp_mq_ahb_eb)) {
			pr_err("Failed: Can't get clock [%s]! %p\n",
			       "clk_vsp_mq_ahb_eb", clk_vsp_mq_ahb_eb);
			ret = PTR_ERR(clk_vsp_mq_ahb_eb);
			vsp_hw_dev->clk_vsp_mq_ahb_eb = NULL;
			goto errout;
		} else {
			vsp_hw_dev->clk_vsp_mq_ahb_eb = clk_vsp_mq_ahb_eb;
		}
	}

errout:
	return ret;
}

int vsp_clk_enable(struct vsp_dev_t *vsp_hw_dev)
{
	int ret = 0;

	if (vsp_hw_dev->version == SHARKL3  || vsp_hw_dev->version == PIKE2) {
		if (vsp_hw_dev->clk_mm_eb) {
			ret = clk_prepare_enable(vsp_hw_dev->clk_mm_eb);
			if (ret) {
				pr_err("vsp clk_mm_eb: clk_enable failed!\n");
				return ret;
			}
			pr_debug("vsp clk_mm_eb: clk_prepare_enable ok.\n");
		}
	}

	if (vsp_hw_dev->clk_ahb_gate_vsp_eb != NULL) {
		ret = clk_prepare_enable(vsp_hw_dev->clk_ahb_gate_vsp_eb);
		if (ret) {
			pr_err("clk_ahb_gate_vsp_eb: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_ahb_gate_vsp_eb: clk_prepare_enable ok.\n");
	}

	if (vsp_hw_dev->version == PIKE2) {
		if (vsp_hw_dev->clk_axi_gate_vsp) {
			ret = clk_prepare_enable(vsp_hw_dev->clk_axi_gate_vsp);
			if (ret) {
				pr_err("clk_axi_gate_vsp: clk_prepare_enable fail!\n");
				return ret;
			}
			pr_debug("clk_axi_gate_vsp: clk_prepare_enable ok.\n");
		}

		if (vsp_hw_dev->clk_vsp_mq_ahb_eb) {
			ret = clk_prepare_enable(vsp_hw_dev->clk_vsp_mq_ahb_eb);
			if (ret) {
				pr_err("clk_vsp_mq_ahb_eb: clk_enable fail!\n");
				return ret;
			}
			pr_debug("clk_vsp_mq_ahb_eb: clk_prepare_enable ok.\n");
		}
	}

#if !IS_ENABLED(CONFIG_SPRD_APSYS_DVFS_DEVFREQ)
	ret = clk_set_parent(vsp_hw_dev->vsp_clk, vsp_hw_dev->vsp_parent_clk);
	if (ret) {
		pr_err("clock[%s]: clk_set_parent() failed!", "clk_vsp");
		return -EINVAL;
	}

	ret = clk_prepare_enable(vsp_hw_dev->vsp_clk);
	if (ret) {
		pr_err("vsp_clk: clk_prepare_enable failed!\n");
		return ret;
	}
	pr_debug("vsp_clk: clk_prepare_enable ok.\n");
#endif

	if (vsp_hw_dev->version == SHARKL3) {
		ret = clk_prepare_enable(vsp_hw_dev->clk_vsp_ahb_mmu_eb);
		if (ret) {
			pr_err("clk_vsp_ahb_mmu_eb: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_vsp_ahb_mmu_eb: clk_prepare_enable ok.\n");

		if (vsp_hw_dev->clk_axi_gate_vsp) {
			ret = clk_prepare_enable(vsp_hw_dev->clk_axi_gate_vsp);
			if (ret) {
				pr_err("clk_axi_gate_vsp: clk_prepare_enable failed!\n");
				return ret;
			}
			pr_debug("clk_axi_gate_vsp: clk_prepare_enable ok.\n");
		}

		ret = clk_set_parent(vsp_hw_dev->clk_ahb_vsp,
				   vsp_hw_dev->ahb_parent_clk);
		if (ret) {
			pr_err("clock[%s]: clk_set_parent() failed!",
				   "ahb_parent_clk");
			return -EINVAL;
		}

		ret = clk_prepare_enable(vsp_hw_dev->clk_ahb_vsp);
		if (ret) {
			pr_err("clk_ahb_vsp: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_ahb_vsp: clk_prepare_enable ok.\n");

		ret = clk_set_parent(vsp_hw_dev->clk_emc_vsp,
				   vsp_hw_dev->emc_parent_clk);
		if (ret) {
			pr_err("clock[%s]: clk_set_parent() failed!",
				   "emc_parent_clk");
			return -EINVAL;
		}

		ret = clk_prepare_enable(vsp_hw_dev->clk_emc_vsp);
		if (ret) {
			pr_err("clk_emc_vsp: clk_prepare_enable failed!\n");
			return ret;
		}
		pr_debug("clk_emc_vsp: clk_prepare_enable ok.\n");
	}

	return ret;
}

void vsp_clk_disable(struct vsp_dev_t *vsp_hw_dev)
{
	if (vsp_hw_dev->version == SHARKL3) {
		clk_disable_unprepare(vsp_hw_dev->clk_emc_vsp);
		clk_disable_unprepare(vsp_hw_dev->clk_ahb_vsp);
		clk_disable_unprepare(vsp_hw_dev->clk_axi_gate_vsp);
		clk_disable_unprepare(vsp_hw_dev->clk_vsp_ahb_mmu_eb);
	}
#if !IS_ENABLED(CONFIG_SPRD_APSYS_DVFS_DEVFREQ)
	clk_disable_unprepare(vsp_hw_dev->vsp_clk);
#endif
	if (vsp_hw_dev->version == PIKE2) {
		clk_disable_unprepare(vsp_hw_dev->clk_axi_gate_vsp);
		clk_disable_unprepare(vsp_hw_dev->clk_vsp_mq_ahb_eb);
	}
	clk_disable_unprepare(vsp_hw_dev->clk_ahb_gate_vsp_eb);
	if (vsp_hw_dev->version == SHARKL3  || vsp_hw_dev->version == PIKE2)
		clk_disable_unprepare(vsp_hw_dev->clk_mm_eb);
}

