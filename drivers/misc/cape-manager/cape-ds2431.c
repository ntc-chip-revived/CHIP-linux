/*
 * Copyright (C) 2015 - Antoine Tenart <antoine.tenart@free-electrons.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include "../../w1/w1.h"
#include "../../w1/w1_family.h"

#define W1_F2D_READ_EEPROM      0xF0

extern void cape_manager_insert(struct device *dev, u32 id);

struct device *plop;

int cape_ds2431_callback(struct w1_slave *sl)
{
	u8 wrbuf[3];
	u8 buf, cmp;
	int tries = 10, offset = 0;

	do {
		wrbuf[0] = W1_F2D_READ_EEPROM;
		wrbuf[1] = offset & 0xff;
		wrbuf[2] = offset >> 8;

		if (w1_reset_select_slave(sl))
			return -1;

		w1_write_block(sl->master, wrbuf, 3);
		w1_read_block(sl->master, &buf, 1);

		if (w1_reset_select_slave(sl))
			return -1;

		w1_write_block(sl->master, wrbuf, 3);
		w1_read_block(sl->master, &cmp, 1);

		if (!memcmp(&cmp, &buf, 1)) {
			cape_manager_insert(plop, buf);
			return 0;
		}
	} while (tries--);

	return -1;
}

static struct w1_family_ops w1_f2d_fops = {
	.callback = &cape_ds2431_callback,
};

static struct w1_family w1_family_2d = {
	.fid	= W1_EEPROM_DS2431,
	.fops	= &w1_f2d_fops,
};

static int cape_ds2431_probe(struct platform_device *pdev)
{
	plop = &pdev->dev;
	w1_register_family(&w1_family_2d);

	return 0;
}

static int cape_ds2431_remove(struct platform_device *pdev)
{
	w1_unregister_family(&w1_family_2d);

	return 0;
}

static struct platform_driver capemgr_driver = {
	.probe	= cape_ds2431_probe,
	.remove	= cape_ds2431_remove,
	.driver	= {
		.name = "cape-ds2431",
		.owner = THIS_MODULE,
	},
};
module_platform_driver(capemgr_driver);

MODULE_AUTHOR("Antoine Tenart <antoine.tenart@free-electrons.com>");
MODULE_DESCRIPTION("Cape manager ID provider from a DS2431 EEPROM");
MODULE_LICENSE("GPL v2");
