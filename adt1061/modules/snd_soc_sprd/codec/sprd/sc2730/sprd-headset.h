/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
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

#ifndef __HEADSET_SPRD_H__
#define __HEADSET_SPRD_H__

/*
 * CONFIG_SND_SOC_SPRD_CODEC_SC2730/SC2721 are old Kconfig symbols; this
 * Kleaf DDK build has no Kconfig, so neither was ever defined and this
 * header used to include nothing at all, leaving sprd-headset-sc2730.c
 * without struct hdst_eic_type/TO_STRING/etc. We pass
 * -DCONFIG_SND_SOC_SPRD_CODEC_SC2730 in this module's copts (BUILD.bazel)
 * to take the sc2730 branch.
 *
 * The original "./sc2730/..." relative path also assumed a nested
 * vendor layout (sprd-headset.h one directory above a sc2730/
 * subdirectory). In this adt1061 tree the two files sit flat in the
 * same directory, so the path is fixed to a plain quote-include
 * (Kleaf hdrs/quote-includes only resolve against files physically in
 * the including file's own directory).
 */
#if defined(CONFIG_SND_SOC_SPRD_CODEC_SC2730)
#include "sprd-headset-2730.h"
#elif defined(CONFIG_SND_SOC_SPRD_CODEC_SC2721)
#include "sprd-headset-2721.h"
#endif

#endif
