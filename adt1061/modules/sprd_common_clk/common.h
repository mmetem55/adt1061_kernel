// SPDX-License-Identifier: GPL-2.0
//
// Spreadtrum clock infrastructure
//
// Copyright (C) 2017 Spreadtrum, Inc.
// Author: Chunyan Zhang <chunyan.zhang@spreadtrum.com>

#ifndef _SPRD_CLK_COMMON_H_
#define _SPRD_CLK_COMMON_H_

#include <linux/clk-provider.h>
#include <linux/of_platform.h>
#include <linux/regmap.h>
#include "sprd_sip_svc.h"

struct device_node;

struct sprd_clk_common {
	union {
		struct regmap	*regmap;
		struct sprd_sip_svc_handle *svc_handle;
	};
	union {
		u32		reg;
		u32		id;
	};
	struct clk_hw	hw;
	/*
	 * Optional: the provider's "power" (force-shutdown) bit. Set means the
	 * block holding this clock's registers is powered off and must not be
	 * read: the GPU power domain is switched at runtime by sprd_gpu_pd.
	 */
	struct regmap	*pwr_regmap;
	u32		pwr_reg;
	u32		pwr_mask;
};

struct sprd_clk_desc {
	struct sprd_clk_common		**clk_clks;
	unsigned long			num_clk_clks;
	struct clk_hw_onecell_data      *hw_clks;
};

static inline struct sprd_clk_common *
	hw_to_sprd_clk_common(const struct clk_hw *hw)
{
	return container_of(hw, struct sprd_clk_common, hw);
}
int sprd_clk_regmap_init(struct platform_device *pdev,
			 const struct sprd_clk_desc *desc);
int sprd_clk_probe(struct device *dev, struct clk_hw_onecell_data *clkhw);

#endif /* _SPRD_CLK_COMMON_H_ */
