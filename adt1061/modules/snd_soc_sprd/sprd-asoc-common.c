/*
 * sound/soc/sprd/sprd-asoc-common.c
 *
 * SPRD ASoC Common implement -- SpreadTrum ASOC Common.
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
#define pr_fmt(fmt) pr_sprd_fmt(" COM ")""fmt

#include <linux/atomic.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <sound/soc.h>
#include <sound/info.h>

#include "sprd-asoc-common.h"

/* spreadtrum audio debug */
static int sp_audio_debug_flag = SP_AUDIO_DEBUG_DEFAULT;

inline int get_sp_audio_debug_flag(void)
{
	return sp_audio_debug_flag;
}
EXPORT_SYMBOL(get_sp_audio_debug_flag);

static void snd_pcm_sprd_debug_read(struct snd_info_entry *entry,
				    struct snd_info_buffer *buffer)
{
	int *p_sp_audio_debug_flag = entry->private_data;

	snd_iprintf(buffer, "0x%08x\n", *p_sp_audio_debug_flag);
}

static void snd_pcm_sprd_debug_write(struct snd_info_entry *entry,
				     struct snd_info_buffer *buffer)
{
	int *p_sp_audio_debug_flag = entry->private_data;
	char line[64];
	unsigned long long flag;

	if (!snd_info_get_line(buffer, line, sizeof(line))) {
		if (kstrtoull(line, 16, &flag) != 0) {
			pr_err("ERR: %s kstrtoull failed!\n", __func__);
			return;
		}
		*p_sp_audio_debug_flag = (int)flag;
	}
}

int sprd_audio_debug_init(struct snd_card *card)
{
	struct snd_info_entry *entry;

	entry = snd_info_create_card_entry(card, "asoc-sprd-debug",
							card->proc_root);
	if (entry != NULL) {
		entry->c.text.read = snd_pcm_sprd_debug_read;
		entry->c.text.write = snd_pcm_sprd_debug_write;
		entry->mode |= 0200;
		entry->private_data = &sp_audio_debug_flag;
		if (snd_info_register(entry) < 0) {
			snd_info_free_entry(entry);
			entry = NULL;
		}
	}

	return 0;
}
EXPORT_SYMBOL(sprd_audio_debug_init);

/*
 * snd_soc_sprd.ko is assembled (via BUILD.bazel) from several files
 * that each used to be its own module with its own module_init /
 * module_exit (directly, or through module_platform_driver()). Now
 * that they all link into one .ko, every one of those macros expands
 * to an alias for the same init_module/cleanup_module symbols, which
 * is a hard modpost "redefinition" error. This file has no entry
 * point of its own, so it is the single place that owns the real
 * module_init/module_exit for the whole module; each other file was
 * changed to expose a plain (non-static) init/exit function instead
 * of registering one itself, and those are called from here.
 */
extern int sprd_pcm_driver_init(void);
extern void sprd_pcm_driver_exit(void);
extern int sprd_compr_driver_init(void);
extern void sprd_compr_driver_exit(void);
extern int sprd_pcm_routing_driver_init(void);
extern void sprd_pcm_routing_driver_exit(void);
extern int sprd_dummy_playback_codec_init(void);
extern void sprd_dummy_playback_codec_exit(void);

static int __init snd_soc_sprd_init(void)
{
	int ret;

	ret = sprd_pcm_driver_init();
	if (ret)
		return ret;

	ret = sprd_compr_driver_init();
	if (ret)
		goto err_compr;

	ret = sprd_pcm_routing_driver_init();
	if (ret)
		goto err_routing;

	ret = sprd_dummy_playback_codec_init();
	if (ret)
		goto err_dummy;

	return 0;

err_dummy:
	sprd_pcm_routing_driver_exit();
err_routing:
	sprd_compr_driver_exit();
err_compr:
	sprd_pcm_driver_exit();
	return ret;
}

static void __exit snd_soc_sprd_exit(void)
{
	sprd_dummy_playback_codec_exit();
	sprd_pcm_routing_driver_exit();
	sprd_compr_driver_exit();
	sprd_pcm_driver_exit();
}

module_init(snd_soc_sprd_init);
module_exit(snd_soc_sprd_exit);

MODULE_DESCRIPTION("Spreadtrum ASoC sound driver");
MODULE_LICENSE("GPL");
