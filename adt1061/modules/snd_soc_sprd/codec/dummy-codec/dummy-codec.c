/*
 * sound/soc/sprd/codec/dummy-codec/dummy-codec.c
 *
 * DUMMY-CODEC -- SpreadTrum just for codec code.
 *
 * Copyright (C) 2015 SpreadTrum Ltd.
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

#include "sprd-asoc-debug.h"
#define pr_fmt(fmt) pr_sprd_fmt("DMYCD")""fmt

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/sysfs.h>

#include <sound/core.h>
#include <sound/soc.h>

#include "sprd-asoc-common.h"

#define STUB_RATES	SNDRV_PCM_RATE_8000_192000
#define STUB_FORMATS	(SNDRV_PCM_FMTBIT_S8 | \
			SNDRV_PCM_FMTBIT_U8 | \
			SNDRV_PCM_FMTBIT_S16_LE | \
			SNDRV_PCM_FMTBIT_U16_LE | \
			SNDRV_PCM_FMTBIT_S24_LE | \
			SNDRV_PCM_FMTBIT_U24_LE | \
			SNDRV_PCM_FMTBIT_S32_LE | \
			SNDRV_PCM_FMTBIT_U32_LE | \
			SNDRV_PCM_FMTBIT_IEC958_SUBFRAME_LE)

struct snd_soc_dai_driver dummy_playback_dai[] = {
	{
	 .name = "sprd-dummy-playback-dai",
	 .playback = {
		      .stream_name = "Playback",
		      .channels_min = 1,
		      .channels_max = 384,
		      .rates = STUB_RATES,
		      .formats = STUB_FORMATS,
		      },
	},
};

/*
 * struct snd_soc_codec_driver / snd_soc_register_codec() /
 * snd_soc_unregister_codec() were removed upstream: codec and platform
 * drivers were merged into the single component model. This driver set
 * no codec_driver fields at all, so it converts directly to an empty
 * struct snd_soc_component_driver registered (with its DAI) via
 * devm_snd_soc_register_component(), which also unregisters itself
 * automatically on device removal.
 */
static const struct snd_soc_component_driver dummy_playback_component;

static int sprd_dummy_playback_codec_probe(struct platform_device *pdev)
{
	return devm_snd_soc_register_component(&pdev->dev,
				     &dummy_playback_component,
				     dummy_playback_dai, 1);
}

static void sprd_dummy_playback_codec_remove(struct platform_device *pdev)
{
}

static struct platform_driver sprd_dummy_driver = {
	.driver = {
		.name = "sprd-dummy-playback",
	},
	.probe = sprd_dummy_playback_codec_probe,
	.remove = sprd_dummy_playback_codec_remove,
};

static struct platform_device *sprd_dummy_dev;

int __init sprd_dummy_playback_codec_init(void)
{
	int ret;

	sprd_dummy_dev = platform_device_register_simple("sprd-dummy-playback",
							 -1, NULL, 0);
	if (IS_ERR(sprd_dummy_dev))
		return PTR_ERR(sprd_dummy_dev);

	ret = platform_driver_register(&sprd_dummy_driver);
	if (ret != 0)
		platform_device_unregister(sprd_dummy_dev);

	return ret;
}

void __exit sprd_dummy_playback_codec_exit(void)
{
	platform_device_unregister(sprd_dummy_dev);
	platform_driver_unregister(&sprd_dummy_driver);
}

/*
 * NOT module_init()/module_exit() here: this file is compiled into
 * the same snd_soc_sprd.ko as several other files that each used to
 * register their own entry point. All of them are consolidated into
 * a single module_init/module_exit in sprd-asoc-common.c, which
 * calls these two functions directly (still declared int/void, not
 * static, so that file can see them).
 */

MODULE_DESCRIPTION("Dummy Playback ALSA SoC Codec Driver");
MODULE_AUTHOR("Peng Lee <peng.lee@spreadtrum.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("codec:dummy-playback");
