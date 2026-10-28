/*
 * sound/soc/sprd/codec/sprd/sc2721/sprd-audio-power.h
 *
 * SPRD-AUDIO-POWER -- SpreadTrum intergrated audio power supply.
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

#ifndef __SPRD_AUDIO_POWER_H
#define __SPRD_AUDIO_POWER_H

#include "sprd-codec.h"

/*
 * sprd-audio-power.c and sprd-codec.c are both globbed into the same
 * snd_soc_sprd_codec_sc2730.ko. Each used to register its own
 * module_init()/module_exit() (or late_initcall()), but those all expand
 * to the exact same init_module/cleanup_module symbols for a single
 * loadable module, which is a duplicate-symbol link error once both
 * object files land in the same .ko. Only sprd-codec.c now owns the
 * module's single late_initcall()/module_exit(); it calls these two to
 * also bring up/tear down the audio-power regulator driver.
 */
int sprd_audio_power_init(void);
void sprd_audio_power_exit(void);

#define SPRD_AUDIO_POWER_VREG		1
#define SPRD_AUDIO_POWER_VB		2
#define SPRD_AUDIO_POWER_BG		3
#define SPRD_AUDIO_POWER_BIAS		4
#define SPRD_AUDIO_POWER_MICBIAS1	5
#define SPRD_AUDIO_POWER_MICBIAS2	6
#define SPRD_AUDIO_POWER_HEADMICBIAS	7
#define SPRD_AUDIO_POWER_DCL			8
#define SPRD_AUDIO_POWER_DIG_CLK_INTC	9
#define SPRD_AUDIO_POWER_DIG_CLK_HID	10
#define SPRD_AUDIO_POWER_CLK_DCL_32K	11
#endif
