/*
 * arch/arm/mach-tegra/board-olympus-panel.c
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/resource.h>
#include <linux/platform_device.h>
#include <asm/mach-types.h>
#include <mach/irqs.h>
#include <mach/iomap.h>
#include <mach/tegra_fb.h>

/* Framebuffer */
static struct resource fb_resource[] = {
	[0] = {
		.start  = INT_DISPLAY_GENERAL,
		.end    = INT_DISPLAY_GENERAL,
		.flags  = IORESOURCE_IRQ,
	},
	[1] = {
		.start	= TEGRA_DISPLAY_BASE,
		.end	= TEGRA_DISPLAY_BASE + TEGRA_DISPLAY_SIZE-1,
		.flags	= IORESOURCE_MEM,
	},
	[2] = {
		.start	= 0x1c03a000,
		.end	= 0x1c03a000 + 0x500000 - 1,
		.flags	= IORESOURCE_MEM,
	},
};

static struct tegra_fb_lcd_data tegra_fb_lcd_platform_data = {
	.lcd_xres	= 480,
	.lcd_yres	= 854,
	.fb_xres	= 480,
	.fb_yres	= 854,
	.bits_per_pixel	= 16,
};

static struct platform_device tegra_fb_device = {
	.name 		= "tegrafb",
	.id		= 0,
	.resource	= fb_resource,
	.num_resources 	= ARRAY_SIZE(fb_resource),
	.dev = {
		.platform_data = &tegra_fb_lcd_platform_data,
	},
};

static int __init olympus_init_panel(void) {
	int ret;

	if (!machine_is_olympus())
		return 0;

	ret = platform_device_register(&tegra_fb_device);
	if (ret != 0)
		return ret;

	return 0;
}

device_initcall(olympus_init_panel);

