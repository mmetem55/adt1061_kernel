/*
 * Copyright (c) 2017 Spreadtrum
 *
 * WCN firmware location.
 *
 * The downstream driver walked /fstab.* with filp_open()/kernel_read() to find
 * the block device of the "wcnmodem" partition. GKI modules may not open files
 * (the symbols are not in the KMI), and the images are now fetched through the
 * firmware loader (wcnmodem.bin / gnssmodem.bin), so there is no partition
 * path to discover. Only a logical name is reported for logging.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/module.h>
#include <linux/string.h>

#include "../include/wcn_dbg.h"
#include "wcn_parn_parser.h"

int parse_firmware_path(char *firmware_path)
{
	strscpy(firmware_path, "wcnmodem", 255);
	WCN_INFO("%s: firmware is loaded by name through the firmware loader\n",
		 __func__);

	return 0;
}
