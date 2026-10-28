// SPDX-License-Identifier: GPL-2.0
//
// Spreadtrum composite clock driver
//
// Copyright (C) 2017 Spreadtrum, Inc.
// Author: Chunyan Zhang <chunyan.zhang@spreadtrum.com>

#include <linux/clk-provider.h>

#include "composite.h"

static long sprd_comp_round_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long *parent_rate)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_div_helper_round_rate(&cc->common, &cc->div,
					 rate, parent_rate);
}

static unsigned long sprd_comp_recalc_rate(struct clk_hw *hw,
					  unsigned long parent_rate)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_div_helper_recalc_rate(&cc->common, &cc->div, parent_rate);
}

static int sprd_comp_set_rate(struct clk_hw *hw, unsigned long rate,
			     unsigned long parent_rate)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_div_helper_set_rate(&cc->common, &cc->div,
				       rate, parent_rate);
}

static u8 sprd_comp_get_parent(struct clk_hw *hw)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_mux_helper_get_parent(&cc->common, &cc->mux);
}

static int sprd_comp_set_parent(struct clk_hw *hw, u8 index)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_mux_helper_set_parent(&cc->common, &cc->mux, index);
}

/*
 * GKI 6.12 port note: clk core now refuses to register any clk_hw whose
 * ops implement .set_parent (this one does, for the mux half of the
 * composite) without also implementing .determine_rate - the old
 * .round_rate-only form isn't enough any more, because the core can't
 * jointly decide "which parent + what rate" from .round_rate alone. The
 * registration itself doesn't fail loudly - clk_hw_register() silently
 * errors out and the driver's own error path then derefs a pointer the
 * core already cleared, which is what was actually panicking. The real
 * fix is this .determine_rate, built on the exact same divider math the
 * existing .round_rate already uses, so parent selection stays untouched
 * (sprd_comp_get_parent/set_parent) and only the rate side gets the
 * callback shape the current core requires.
 */
static int sprd_comp_determine_rate(struct clk_hw *hw,
				     struct clk_rate_request *req)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);
	long rate;

	rate = sprd_div_helper_round_rate(&cc->common, &cc->div,
					   req->rate, &req->best_parent_rate);
	if (rate < 0)
		return rate;

	req->rate = rate;

	return 0;
}

const struct clk_ops sprd_comp_ops = {
	.get_parent	= sprd_comp_get_parent,
	.set_parent	= sprd_comp_set_parent,

	.round_rate	= sprd_comp_round_rate,
	.determine_rate	= sprd_comp_determine_rate,
	.recalc_rate	= sprd_comp_recalc_rate,
	.set_rate	= sprd_comp_set_rate,
};
EXPORT_SYMBOL_GPL(sprd_comp_ops);

static unsigned long sprd_comp_recalc_rate_sec(struct clk_hw *hw,
					       unsigned long parent_rate)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_div_helper_recalc_rate_sec(&cc->common, &cc->div,
					       parent_rate);
}

static int sprd_comp_set_rate_sec(struct clk_hw *hw, unsigned long rate,
				  unsigned long parent_rate)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_div_helper_set_rate_sec(&cc->common, &cc->div, rate,
					    parent_rate);
}

static u8 sprd_comp_get_parent_sec(struct clk_hw *hw)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_mux_helper_get_parent_sec(&cc->common, &cc->mux);
}

static int sprd_comp_set_parent_sec(struct clk_hw *hw, u8 index)
{
	struct sprd_comp *cc = hw_to_sprd_comp(hw);

	return sprd_mux_helper_set_parent_sec(&cc->common, &cc->mux, index);
}

const struct clk_ops sprd_comp_ops_sec = {
	.get_parent	= sprd_comp_get_parent_sec,
	.set_parent	= sprd_comp_set_parent_sec,

	.round_rate	= sprd_comp_round_rate,
	.determine_rate	= sprd_comp_determine_rate,
	.recalc_rate	= sprd_comp_recalc_rate_sec,
	.set_rate	= sprd_comp_set_rate_sec,
};
EXPORT_SYMBOL_GPL(sprd_comp_ops_sec);
