/*
 * arch/arm/mach-tegra/board.h
 *
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *	Erik Gilling <konkers@google.com>
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

#ifndef __MACH_TEGRA_BOARD_H
#define __MACH_TEGRA_BOARD_H

#include <linux/types.h>

void tegra_assert_system_reset(char mode, const char *cmd);

void __init tegra_init_early(void);
void __init tegra_map_common_io(void);
void __init tegra_init_irq(void);
void __init tegra_init_clock(void);
void __init tegra_reserve(unsigned long carveout_size, unsigned long fb_size,
	unsigned long fb2_size);
int __init tegra_pcie_init(bool init_port0, bool init_port1);
void tegra_init_cache(void);
void __init tegra_protected_aperture_init(unsigned long aperture);
void tegra_move_framebuffer(unsigned long to, unsigned long from,
	unsigned long size);

extern unsigned long tegra_bootloader_fb_start;
extern unsigned long tegra_bootloader_fb_size;
extern unsigned long tegra_fb_start;
extern unsigned long tegra_fb_size;
extern unsigned long tegra_fb2_start;
extern unsigned long tegra_fb2_size;
extern unsigned long tegra_carveout_start;
extern unsigned long tegra_carveout_size;
extern unsigned long tegra_lp0_vec_start;
extern unsigned long tegra_lp0_vec_size;
extern unsigned long tegra_grhost_aperture;

extern struct sys_timer tegra_timer;
#endif
