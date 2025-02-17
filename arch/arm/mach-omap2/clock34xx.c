/*
 * OMAP3-specific clock framework functions
 *
 * Copyright (C) 2007-2008 Texas Instruments, Inc.
 * Copyright (C) 2007-2009 Nokia Corporation
 *
 * Written by Paul Walmsley
 * Testing and integration fixes by Jouni Högander
 *
 * Parts of this code are based on code written by
 * Richard Woodruff, Tony Lindgren, Tuukka Tikkanen, Karthik Dasu
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#undef DEBUG

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/list.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/limits.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/cpufreq.h>

#ifdef CONFIG_OMAP3_OVERCLOCK
#include <linux/proc_fs.h>
#include <asm/uaccess.h>
#include <linux/vmalloc.h>
#include <linux/cpufreq.h>
#endif

#include <plat/cpu.h>
#include <plat/clock.h>
#include <plat/sram.h>
#include <plat/omap-pm.h>

#include <asm/div64.h>
#include <asm/clkdev.h>

#include <plat/sdrc.h>
#include "clock.h"
#include "prm.h"
#include "prm-regbits-34xx.h"
#include "cm.h"
#include "cm-regbits-34xx.h"
#include "pm.h"

static const struct clkops clkops_noncore_dpll_ops;

static void omap3430es2_clk_ssi_find_idlest(struct clk *clk,
					    void __iomem **idlest_reg,
					    u8 *idlest_bit);
static void omap3430es2_clk_hsotgusb_find_idlest(struct clk *clk,
					    void __iomem **idlest_reg,
					    u8 *idlest_bit);
static void omap3430es2_clk_dss_usbhost_find_idlest(struct clk *clk,
						    void __iomem **idlest_reg,
						    u8 *idlest_bit);

static const struct clkops clkops_omap3430es2_ssi_wait = {
	.enable		= omap2_dflt_clk_enable,
	.disable	= omap2_dflt_clk_disable,
	.find_idlest	= omap3430es2_clk_ssi_find_idlest,
	.find_companion = omap2_clk_dflt_find_companion,
};

static const struct clkops clkops_omap3430es2_hsotgusb_wait = {
	.enable		= omap2_dflt_clk_enable,
	.disable	= omap2_dflt_clk_disable,
	.find_idlest	= omap3430es2_clk_hsotgusb_find_idlest,
	.find_companion = omap2_clk_dflt_find_companion,
};

static const struct clkops clkops_omap3430es2_dss_usbhost_wait = {
	.enable		= omap2_dflt_clk_enable,
	.disable	= omap2_dflt_clk_disable,
	.find_idlest	= omap3430es2_clk_dss_usbhost_find_idlest,
	.find_companion = omap2_clk_dflt_find_companion,
};

static const struct clkops clkops_omap3630_pwrdn_bug = {
	.enable		= omap3_pwrdn_bug_clk_enable,
	.disable        = omap2_dflt_clk_disable,
	.find_companion = omap2_clk_dflt_find_companion,
	.find_idlest    = omap2_clk_dflt_find_idlest,
};

#include "clock34xx.h"

struct omap_clk {
	u32		cpu;
	struct clk_lookup lk;
};

#define CLK(dev, con, ck, cp) 		\
	{				\
		 .cpu = cp,		\
		.lk = {			\
			.dev_id = dev,	\
			.con_id = con,	\
			.clk = ck,	\
		},			\
	}

#define CK_343X		(1 << 0)
#define CK_3430ES1	(1 << 1)
#define CK_3430ES2	(1 << 2)
#define CK_363X		(1 << 3)

static struct omap_clk omap34xx_clks[] = {
	CLK(NULL,	"omap_32k_fck",	&omap_32k_fck,	CK_343X),
	CLK(NULL,	"virt_12m_ck",	&virt_12m_ck,	CK_343X),
	CLK(NULL,	"virt_13m_ck",	&virt_13m_ck,	CK_343X),
	CLK(NULL,	"virt_16_8m_ck", &virt_16_8m_ck, CK_3430ES2),
	CLK(NULL,	"virt_19_2m_ck", &virt_19_2m_ck, CK_343X),
	CLK(NULL,	"virt_26m_ck",	&virt_26m_ck,	CK_343X),
	CLK(NULL,	"virt_38_4m_ck", &virt_38_4m_ck, CK_343X),
	CLK(NULL,	"osc_sys_ck",	&osc_sys_ck,	CK_343X),
	CLK(NULL,	"sys_ck",	&sys_ck,	CK_343X),
	CLK(NULL,	"sys_altclk",	&sys_altclk,	CK_343X),
	CLK(NULL,	"mcbsp_clks",	&mcbsp_clks,	CK_343X),
	CLK(NULL,	"sys_clkout1",	&sys_clkout1,	CK_343X),
	CLK(NULL,	"dpll1_ck",	&dpll1_ck,	CK_343X),
	CLK(NULL,	"dpll1_x2_ck",	&dpll1_x2_ck,	CK_343X),
	CLK(NULL,	"dpll1_x2m2_ck", &dpll1_x2m2_ck, CK_343X),
	CLK(NULL,	"dpll2_ck",	&dpll2_ck,	CK_343X),
	CLK(NULL,	"dpll2_m2_ck",	&dpll2_m2_ck,	CK_343X),
	CLK(NULL,	"dpll3_ck",	&dpll3_ck,	CK_343X),
	CLK(NULL,	"core_ck",	&core_ck,	CK_343X),
	CLK(NULL,	"dpll3_x2_ck",	&dpll3_x2_ck,	CK_343X),
	CLK(NULL,	"dpll3_m2_ck",	&dpll3_m2_ck,	CK_343X),
	CLK(NULL,	"dpll3_m2x2_ck", &dpll3_m2x2_ck, CK_343X),
	CLK(NULL,	"dpll3_m3_ck",	&dpll3_m3_ck,	CK_343X),
	CLK(NULL,	"dpll3_m3x2_ck", &dpll3_m3x2_ck, CK_343X),
	CLK(NULL,	"emu_core_alwon_ck", &emu_core_alwon_ck, CK_343X),
	CLK(NULL,	"dpll4_ck",	&dpll4_ck,	CK_343X),
	CLK(NULL,	"dpll4_x2_ck",	&dpll4_x2_ck,	CK_343X),
	CLK(NULL,	"omap_192m_alwon_ck", &omap_192m_alwon_ck, CK_363X),
	CLK(NULL,	"omap_96m_alwon_fck", &omap_96m_alwon_fck, CK_343X),
	CLK(NULL,	"omap_96m_fck",	&omap_96m_fck,	CK_343X),
	CLK(NULL,	"cm_96m_fck",	&cm_96m_fck,	CK_343X),
	CLK(NULL,	"omap_54m_fck",	&omap_54m_fck,	CK_343X),
	CLK(NULL,	"omap_48m_fck",	&omap_48m_fck,	CK_343X),
	CLK(NULL,	"omap_12m_fck",	&omap_12m_fck,	CK_343X),
	CLK(NULL,	"dpll4_m2_ck",	&dpll4_m2_ck,	CK_343X),
	CLK(NULL,	"dpll4_m2x2_ck", &dpll4_m2x2_ck, CK_343X),
	CLK(NULL,	"dpll4_m3_ck",	&dpll4_m3_ck,	CK_343X | CK_363X),
	CLK(NULL,	"dpll4_m3x2_ck", &dpll4_m3x2_ck, CK_343X),
	CLK(NULL,	"dpll4_m4_ck",	&dpll4_m4_ck,	CK_343X | CK_363X),
	CLK(NULL,	"dpll4_m4x2_ck", &dpll4_m4x2_ck, CK_343X),
	CLK(NULL,	"dpll4_m5_ck",	&dpll4_m5_ck,	CK_343X | CK_363X),
	CLK(NULL,	"dpll4_m5x2_ck", &dpll4_m5x2_ck, CK_343X),
	CLK(NULL,	"dpll4_m6_ck",	&dpll4_m6_ck,	CK_343X | CK_363X),
	CLK(NULL,	"dpll4_m6x2_ck", &dpll4_m6x2_ck, CK_343X),
	CLK(NULL,	"emu_per_alwon_ck", &emu_per_alwon_ck, CK_343X),
	CLK(NULL,	"dpll5_ck",	&dpll5_ck,	CK_3430ES2),
	CLK(NULL,	"dpll5_m2_ck",	&dpll5_m2_ck,	CK_3430ES2),
	CLK(NULL,	"clkout2_src_ck", &clkout2_src_ck, CK_343X),
	CLK(NULL,	"sys_clkout2",	&sys_clkout2,	CK_343X),
	CLK(NULL,	"corex2_fck",	&corex2_fck,	CK_343X),
	CLK(NULL,	"dpll1_fck",	&dpll1_fck,	CK_343X),
	CLK(NULL,	"mpu_ck",	&mpu_ck,	CK_343X),
	CLK(NULL,	"arm_fck",	&arm_fck,	CK_343X),
	CLK(NULL,	"emu_mpu_alwon_ck", &emu_mpu_alwon_ck, CK_343X),
	CLK(NULL,	"dpll2_fck",	&dpll2_fck,	CK_343X),
	CLK(NULL,	"iva2_ck",	&iva2_ck,	CK_343X),
	CLK(NULL,	"l3_ick",	&l3_ick,	CK_343X),
	CLK(NULL,	"l4_ick",	&l4_ick,	CK_343X),
	CLK(NULL,	"rm_ick",	&rm_ick,	CK_343X),
	CLK(NULL,	"gfx_l3_ck",	&gfx_l3_ck,	CK_3430ES1),
	CLK(NULL,	"gfx_l3_fck",	&gfx_l3_fck,	CK_3430ES1),
	CLK(NULL,	"gfx_l3_ick",	&gfx_l3_ick,	CK_3430ES1),
	CLK(NULL,	"gfx_cg1_ck",	&gfx_cg1_ck,	CK_3430ES1),
	CLK(NULL,	"gfx_cg2_ck",	&gfx_cg2_ck,	CK_3430ES1),
	CLK(NULL,	"sgx_fck",	&sgx_fck,	CK_3430ES2),
	CLK(NULL,	"sgx_ick",	&sgx_ick,	CK_3430ES2),
	CLK(NULL,	"d2d_26m_fck",	&d2d_26m_fck,	CK_3430ES1),
	CLK(NULL,	"modem_fck",	&modem_fck,	CK_343X),
	CLK(NULL,	"sad2d_ick",	&sad2d_ick,	CK_343X),
	CLK(NULL,	"mad2d_ick",	&mad2d_ick,	CK_343X),
	CLK(NULL,	"gpt10_fck",	&gpt10_fck,	CK_343X),
	CLK(NULL,	"gpt11_fck",	&gpt11_fck,	CK_343X),
	CLK(NULL,	"cpefuse_fck",	&cpefuse_fck,	CK_3430ES2),
	CLK(NULL,	"ts_fck",	&ts_fck,	CK_3430ES2),
	CLK(NULL,	"usbtll_fck",	&usbtll_fck,	CK_3430ES2),
	CLK(NULL,	"core_96m_fck",	&core_96m_fck,	CK_343X),
	CLK("mmci-omap-hs.2",	"fck",	&mmchs3_fck,	CK_3430ES2),
	CLK("mmci-omap-hs.1",	"fck",	&mmchs2_fck,	CK_343X),
	CLK(NULL,	"mspro_fck",	&mspro_fck,	CK_343X),
	CLK("mmci-omap-hs.0",	"fck",	&mmchs1_fck,	CK_343X),
	CLK("i2c_omap.3", "fck",	&i2c3_fck,	CK_343X),
	CLK("i2c_omap.2", "fck",	&i2c2_fck,	CK_343X),
	CLK("i2c_omap.1", "fck",	&i2c1_fck,	CK_343X),
	CLK("omap-mcbsp.5", "fck",	&mcbsp5_fck,	CK_343X),
	CLK("omap-mcbsp.1", "fck",	&mcbsp1_fck,	CK_343X),
	CLK(NULL,	"core_48m_fck",	&core_48m_fck,	CK_343X),
	CLK("omap2_mcspi.4", "fck",	&mcspi4_fck,	CK_343X),
	CLK("omap2_mcspi.3", "fck",	&mcspi3_fck,	CK_343X),
	CLK("omap2_mcspi.2", "fck",	&mcspi2_fck,	CK_343X),
	CLK("omap2_mcspi.1", "fck",	&mcspi1_fck,	CK_343X),
	CLK(NULL,	"uart2_fck",	&uart2_fck,	CK_343X),
	CLK(NULL,	"uart1_fck",	&uart1_fck,	CK_343X),
	CLK(NULL,	"fshostusb_fck", &fshostusb_fck, CK_3430ES1),
	CLK(NULL,	"core_12m_fck",	&core_12m_fck,	CK_343X),
	CLK("omap_hdq.0", "fck",	&hdq_fck,	CK_343X),
	CLK(NULL,	"ssi_ssr_fck",	&ssi_ssr_fck_3430es1,	CK_3430ES1),
	CLK(NULL,	"ssi_ssr_fck",	&ssi_ssr_fck_3430es2,	CK_3430ES2),
	CLK(NULL,	"ssi_sst_fck",	&ssi_sst_fck_3430es1,	CK_3430ES1),
	CLK(NULL,	"ssi_sst_fck",	&ssi_sst_fck_3430es2,	CK_3430ES2),
	CLK(NULL,	"core_l3_ick",	&core_l3_ick,	CK_343X),
	CLK("musb_hdrc",	"ick",	&hsotgusb_ick_3430es1,	CK_3430ES1),
	CLK("musb_hdrc",	"ick",	&hsotgusb_ick_3430es2,	CK_3430ES2),
	CLK(NULL,	"sdrc_ick",	&sdrc_ick,	CK_343X),
	CLK(NULL,	"gpmc_fck",	&gpmc_fck,	CK_343X),
	CLK(NULL,	"security_l3_ick", &security_l3_ick, CK_343X),
	CLK(NULL,	"pka_ick",	&pka_ick,	CK_343X),
	CLK(NULL,	"core_l4_ick",	&core_l4_ick,	CK_343X),
	CLK(NULL,	"usbtll_ick",	&usbtll_ick,	CK_3430ES2),
	CLK("mmci-omap-hs.2",	"ick",	&mmchs3_ick,	CK_3430ES2),
	CLK(NULL,	"icr_ick",	&icr_ick,	CK_343X),
	CLK(NULL,	"aes2_ick",	&aes2_ick,	CK_343X),
	CLK(NULL,	"sha12_ick",	&sha12_ick,	CK_343X),
	CLK(NULL,	"des2_ick",	&des2_ick,	CK_343X),
	CLK("mmci-omap-hs.1",	"ick",	&mmchs2_ick,	CK_343X),
	CLK("mmci-omap-hs.0",	"ick",	&mmchs1_ick,	CK_343X),
	CLK(NULL,	"mspro_ick",	&mspro_ick,	CK_343X),
	CLK("omap_hdq.0", "ick",	&hdq_ick,	CK_343X),
	CLK("omap2_mcspi.4", "ick",	&mcspi4_ick,	CK_343X),
	CLK("omap2_mcspi.3", "ick",	&mcspi3_ick,	CK_343X),
	CLK("omap2_mcspi.2", "ick",	&mcspi2_ick,	CK_343X),
	CLK("omap2_mcspi.1", "ick",	&mcspi1_ick,	CK_343X),
	CLK("i2c_omap.3", "ick",	&i2c3_ick,	CK_343X),
	CLK("i2c_omap.2", "ick",	&i2c2_ick,	CK_343X),
	CLK("i2c_omap.1", "ick",	&i2c1_ick,	CK_343X),
	CLK(NULL,	"uart2_ick",	&uart2_ick,	CK_343X),
	CLK(NULL,	"uart1_ick",	&uart1_ick,	CK_343X),
	CLK(NULL,	"gpt11_ick",	&gpt11_ick,	CK_343X),
	CLK(NULL,	"gpt10_ick",	&gpt10_ick,	CK_343X),
	CLK("omap-mcbsp.5", "ick",	&mcbsp5_ick,	CK_343X),
	CLK("omap-mcbsp.1", "ick",	&mcbsp1_ick,	CK_343X),
	CLK(NULL,	"fac_ick",	&fac_ick,	CK_3430ES1),
	CLK(NULL,	"mailboxes_ick", &mailboxes_ick, CK_343X),
	CLK(NULL,	"omapctrl_ick",	&omapctrl_ick,	CK_343X),
	CLK(NULL,	"ssi_l4_ick",	&ssi_l4_ick,	CK_343X),
	CLK(NULL,	"ssi_ick",	&ssi_ick_3430es1,	CK_3430ES1),
	CLK(NULL,	"ssi_ick",	&ssi_ick_3430es2,	CK_3430ES2),
	CLK(NULL,	"usb_l4_ick",	&usb_l4_ick,	CK_3430ES1),
	CLK(NULL,	"security_l4_ick2", &security_l4_ick2, CK_343X),
	CLK(NULL,	"aes1_ick",	&aes1_ick,	CK_343X),
	CLK(NULL,       "rng_ick",      &rng_ick,       CK_343X),
	CLK(NULL,	"sha11_ick",	&sha11_ick,	CK_343X),
	CLK(NULL,	"des1_ick",	&des1_ick,	CK_343X),
	CLK("omapdss",	"dss1_fck",	&dss1_alwon_fck_3430es1, CK_3430ES1),
	CLK("omapdss",	"dss1_fck",	&dss1_alwon_fck_3430es2, CK_3430ES2),
	CLK("omapdss",	"tv_fck",	&dss_tv_fck,	CK_343X),
	CLK("omapdss",	"video_fck",	&dss_96m_fck,	CK_343X),
	CLK("omapdss",	"dss2_fck",	&dss2_alwon_fck, CK_343X),
	CLK("omapdss",	"ick",		&dss_ick_3430es1,	CK_3430ES1),
	CLK("omapdss",	"ick",		&dss_ick_3430es2,	CK_3430ES2),
	CLK(NULL,	"cam_mclk",	&cam_mclk,	CK_343X),
	CLK(NULL,	"cam_ick",	&cam_ick,	CK_343X),
	CLK(NULL,	"csi2_96m_fck",	&csi2_96m_fck,	CK_343X),
	CLK(NULL,	"usbhost_120m_fck", &usbhost_120m_fck, CK_3430ES2),
	CLK(NULL,	"usbhost_48m_fck", &usbhost_48m_fck, CK_3430ES2),
	CLK(NULL,	"usbhost_ick",	&usbhost_ick,	CK_3430ES2),
	CLK(NULL,	"usim_fck",	&usim_fck,	CK_3430ES2),
	CLK(NULL,	"gpt1_fck",	&gpt1_fck,	CK_343X),
	CLK(NULL,	"wkup_32k_fck",	&wkup_32k_fck,	CK_343X),
	CLK(NULL,	"gpio1_dbck",	&gpio1_dbck,	CK_343X),
	CLK("omap_wdt",	"fck",		&wdt2_fck,	CK_343X),
	CLK(NULL,	"wkup_l4_ick",	&wkup_l4_ick,	CK_343X),
	CLK(NULL,	"usim_ick",	&usim_ick,	CK_3430ES2),
	CLK("omap_wdt",	"ick",		&wdt2_ick,	CK_343X),
	CLK(NULL,	"wdt1_ick",	&wdt1_ick,	CK_343X),
	CLK(NULL,	"gpio1_ick",	&gpio1_ick,	CK_343X),
	CLK(NULL,	"omap_32ksync_ick", &omap_32ksync_ick, CK_343X),
	CLK(NULL,	"gpt12_ick",	&gpt12_ick,	CK_343X),
	CLK(NULL,	"gpt1_ick",	&gpt1_ick,	CK_343X),
	CLK(NULL,	"per_96m_fck",	&per_96m_fck,	CK_343X),
	CLK(NULL,	"per_48m_fck",	&per_48m_fck,	CK_343X),
	CLK(NULL,	"uart3_fck",	&uart3_fck,	CK_343X),
	CLK(NULL,	"gpt2_fck",	&gpt2_fck,	CK_343X),
	CLK(NULL,	"gpt3_fck",	&gpt3_fck,	CK_343X),
	CLK(NULL,	"gpt4_fck",	&gpt4_fck,	CK_343X),
	CLK(NULL,	"gpt5_fck",	&gpt5_fck,	CK_343X),
	CLK(NULL,	"gpt6_fck",	&gpt6_fck,	CK_343X),
	CLK(NULL,	"gpt7_fck",	&gpt7_fck,	CK_343X),
	CLK(NULL,	"gpt8_fck",	&gpt8_fck,	CK_343X),
	CLK(NULL,	"gpt9_fck",	&gpt9_fck,	CK_343X),
	CLK(NULL,	"per_32k_alwon_fck", &per_32k_alwon_fck, CK_343X),
	CLK(NULL,	"gpio6_dbck",	&gpio6_dbck,	CK_343X),
	CLK(NULL,	"gpio5_dbck",	&gpio5_dbck,	CK_343X),
	CLK(NULL,	"gpio4_dbck",	&gpio4_dbck,	CK_343X),
	CLK(NULL,	"gpio3_dbck",	&gpio3_dbck,	CK_343X),
	CLK(NULL,	"gpio2_dbck",	&gpio2_dbck,	CK_343X),
	CLK(NULL,	"wdt3_fck",	&wdt3_fck,	CK_343X),
	CLK(NULL,	"per_l4_ick",	&per_l4_ick,	CK_343X),
	CLK(NULL,	"gpio6_ick",	&gpio6_ick,	CK_343X),
	CLK(NULL,	"gpio5_ick",	&gpio5_ick,	CK_343X),
	CLK(NULL,	"gpio4_ick",	&gpio4_ick,	CK_343X),
	CLK(NULL,	"gpio3_ick",	&gpio3_ick,	CK_343X),
	CLK(NULL,	"gpio2_ick",	&gpio2_ick,	CK_343X),
	CLK(NULL,	"wdt3_ick",	&wdt3_ick,	CK_343X),
	CLK(NULL,	"uart3_ick",	&uart3_ick,	CK_343X),
	CLK(NULL,	"gpt9_ick",	&gpt9_ick,	CK_343X),
	CLK(NULL,	"gpt8_ick",	&gpt8_ick,	CK_343X),
	CLK(NULL,	"gpt7_ick",	&gpt7_ick,	CK_343X),
	CLK(NULL,	"gpt6_ick",	&gpt6_ick,	CK_343X),
	CLK(NULL,	"gpt5_ick",	&gpt5_ick,	CK_343X),
	CLK(NULL,	"gpt4_ick",	&gpt4_ick,	CK_343X),
	CLK(NULL,	"gpt3_ick",	&gpt3_ick,	CK_343X),
	CLK(NULL,	"gpt2_ick",	&gpt2_ick,	CK_343X),
	CLK("omap-mcbsp.2", "ick",	&mcbsp2_ick,	CK_343X),
	CLK("omap-mcbsp.3", "ick",	&mcbsp3_ick,	CK_343X),
	CLK("omap-mcbsp.4", "ick",	&mcbsp4_ick,	CK_343X),
	CLK("omap-mcbsp.2", "fck",	&mcbsp2_fck,	CK_343X),
	CLK("omap-mcbsp.3", "fck",	&mcbsp3_fck,	CK_343X),
	CLK("omap-mcbsp.4", "fck",	&mcbsp4_fck,	CK_343X),
	CLK(NULL,	"emu_src_ck",	&emu_src_ck,	CK_343X),
	CLK(NULL,	"pclk_fck",	&pclk_fck,	CK_343X),
	CLK(NULL,	"pclkx2_fck",	&pclkx2_fck,	CK_343X),
	CLK(NULL,	"atclk_fck",	&atclk_fck,	CK_343X),
	CLK(NULL,	"traceclk_src_fck", &traceclk_src_fck, CK_343X),
	CLK(NULL,	"traceclk_fck",	&traceclk_fck,	CK_343X),
	CLK(NULL,	"sr1_fck",	&sr1_fck,	CK_343X),
	CLK(NULL,	"sr2_fck",	&sr2_fck,	CK_343X),
	CLK(NULL,	"sr_l4_ick",	&sr_l4_ick,	CK_343X),
	CLK(NULL,	"secure_32k_fck", &secure_32k_fck, CK_343X),
	CLK(NULL,	"gpt12_fck",	&gpt12_fck,	CK_343X),
	CLK(NULL,	"wdt1_fck",	&wdt1_fck,	CK_343X),
};

/* CM_AUTOIDLE_PLL*.AUTO_* bit values */
#define DPLL_AUTOIDLE_DISABLE			0x0
#define DPLL_AUTOIDLE_LOW_POWER_STOP		0x1

#define MAX_DPLL_WAIT_TRIES		1000000

#define MIN_SDRC_DLL_LOCK_FREQ		83000000

#define CYCLES_PER_MHZ			1000000

/* Scale factor for fixed-point arith in omap3_core_dpll_m2_set_rate() */
#define SDRC_MPURATE_SCALE		8

/* 2^SDRC_MPURATE_BASE_SHIFT: MPU MHz that SDRC_MPURATE_LOOPS is defined for */
#define SDRC_MPURATE_BASE_SHIFT		9

/*
 * SDRC_MPURATE_LOOPS: Number of MPU loops to execute at
 * 2^MPURATE_BASE_SHIFT MHz for SDRC to stabilize
 */
#define SDRC_MPURATE_LOOPS		24

/*
 * DPLL5_FREQ_FOR_USBHOST: USBHOST and USBTLL are the only clocks
 * that are sourced by DPLL5, and both of these require this clock
 * to be at 120 MHz for proper operation.
 */
#define DPLL5_FREQ_FOR_USBHOST		120000000

unsigned long long cpu_hz;
EXPORT_SYMBOL(cpu_hz);
EXPORT_SYMBOL(mpu_opps);

/**
 * omap3430es2_clk_ssi_find_idlest - return CM_IDLEST info for SSI
 * @clk: struct clk * being enabled
 * @idlest_reg: void __iomem ** to store CM_IDLEST reg address into
 * @idlest_bit: pointer to a u8 to store the CM_IDLEST bit shift into
 *
 * The OMAP3430ES2 SSI target CM_IDLEST bit is at a different shift
 * from the CM_{I,F}CLKEN bit.  Pass back the correct info via
 * @idlest_reg and @idlest_bit.  No return value.
 */
static void omap3430es2_clk_ssi_find_idlest(struct clk *clk,
					    void __iomem **idlest_reg,
					    u8 *idlest_bit)
{
	u32 r;

	r = (((__force u32)clk->enable_reg & ~0xf0) | 0x20);
	*idlest_reg = (__force void __iomem *)r;
	*idlest_bit = OMAP3430ES2_ST_SSI_IDLE_SHIFT;
}

/**
 * omap3430es2_clk_dss_usbhost_find_idlest - CM_IDLEST info for DSS, USBHOST
 * @clk: struct clk * being enabled
 * @idlest_reg: void __iomem ** to store CM_IDLEST reg address into
 * @idlest_bit: pointer to a u8 to store the CM_IDLEST bit shift into
 *
 * Some OMAP modules on OMAP3 ES2+ chips have both initiator and
 * target IDLEST bits.  For our purposes, we are concerned with the
 * target IDLEST bits, which exist at a different bit position than
 * the *CLKEN bit position for these modules (DSS and USBHOST) (The
 * default find_idlest code assumes that they are at the same
 * position.)  No return value.
 */
static void omap3430es2_clk_dss_usbhost_find_idlest(struct clk *clk,
						    void __iomem **idlest_reg,
						    u8 *idlest_bit)
{
	u32 r;

	r = (((__force u32)clk->enable_reg & ~0xf0) | 0x20);
	*idlest_reg = (__force void __iomem *)r;
	/* USBHOST_IDLE has same shift */
	*idlest_bit = OMAP3430ES2_ST_DSS_IDLE_SHIFT;
}

/**
 * omap3430es2_clk_hsotgusb_find_idlest - return CM_IDLEST info for HSOTGUSB
 * @clk: struct clk * being enabled
 * @idlest_reg: void __iomem ** to store CM_IDLEST reg address into
 * @idlest_bit: pointer to a u8 to store the CM_IDLEST bit shift into
 *
 * The OMAP3430ES2 HSOTGUSB target CM_IDLEST bit is at a different
 * shift from the CM_{I,F}CLKEN bit.  Pass back the correct info via
 * @idlest_reg and @idlest_bit.  No return value.
 */
static void omap3430es2_clk_hsotgusb_find_idlest(struct clk *clk,
						 void __iomem **idlest_reg,
						 u8 *idlest_bit)
{
	u32 r;

	r = (((__force u32)clk->enable_reg & ~0xf0) | 0x20);
	*idlest_reg = (__force void __iomem *)r;
	*idlest_bit = OMAP3430ES2_ST_HSOTGUSB_IDLE_SHIFT;
}

/**
 * omap3_dpll_recalc - recalculate DPLL rate
 * @clk: DPLL struct clk
 *
 * Recalculate and propagate the DPLL rate.
 */
static unsigned long omap3_dpll_recalc(struct clk *clk)
{
	return omap2_get_dpll_rate(clk);
}

/* _omap3_dpll_write_clken - write clken_bits arg to a DPLL's enable bits */
static void _omap3_dpll_write_clken(struct clk *clk, u8 clken_bits)
{
	const struct dpll_data *dd;
	u32 v;

	dd = clk->dpll_data;

	v = __raw_readl(dd->control_reg);
	v &= ~dd->enable_mask;
	v |= clken_bits << __ffs(dd->enable_mask);
	__raw_writel(v, dd->control_reg);
}

/* _omap3_wait_dpll_status: wait for a DPLL to enter a specific state */
static int _omap3_wait_dpll_status(struct clk *clk, u8 state)
{
	const struct dpll_data *dd;
	int i = 0;
	int ret = -EINVAL;

	dd = clk->dpll_data;

	state <<= __ffs(dd->idlest_mask);

	while (((__raw_readl(dd->idlest_reg) & dd->idlest_mask) != state) &&
	       i < MAX_DPLL_WAIT_TRIES) {
		i++;
		udelay(1);
	}

	if (i == MAX_DPLL_WAIT_TRIES) {
		printk(KERN_ERR "clock: %s failed transition to '%s'\n",
		       clk->name, (state) ? "locked" : "bypassed");
	} else {
		pr_debug("clock: %s transition to '%s' in %d loops\n",
			 clk->name, (state) ? "locked" : "bypassed", i);

		ret = 0;
	}

	return ret;
}

/* From 3430 TRM ES2 4.7.6.2 */
static u16 _omap3_dpll_compute_freqsel(struct clk *clk, u8 n)
{
	unsigned long fint;
	u16 f = 0;

	fint = clk->dpll_data->clk_ref->rate / n;

	pr_debug("clock: fint is %lu\n", fint);

	if (fint >= 750000 && fint <= 1000000)
		f = 0x3;
	else if (fint > 1000000 && fint <= 1250000)
		f = 0x4;
	else if (fint > 1250000 && fint <= 1500000)
		f = 0x5;
	else if (fint > 1500000 && fint <= 1750000)
		f = 0x6;
	else if (fint > 1750000 && fint <= 2100000)
		f = 0x7;
	else if (fint > 7500000 && fint <= 10000000)
		f = 0xB;
	else if (fint > 10000000 && fint <= 12500000)
		f = 0xC;
	else if (fint > 12500000 && fint <= 15000000)
		f = 0xD;
	else if (fint > 15000000 && fint <= 17500000)
		f = 0xE;
	else if (fint > 17500000 && fint <= 21000000)
		f = 0xF;
	else
		pr_debug("clock: unknown freqsel setting for %d\n", n);

	return f;
}

/* Non-CORE DPLL (e.g., DPLLs that do not control SDRC) clock functions */

/*
 * _omap3_noncore_dpll_lock - instruct a DPLL to lock and wait for readiness
 * @clk: pointer to a DPLL struct clk
 *
 * Instructs a non-CORE DPLL to lock.  Waits for the DPLL to report
 * readiness before returning.  Will save and restore the DPLL's
 * autoidle state across the enable, per the CDP code.  If the DPLL
 * locked successfully, return 0; if the DPLL did not lock in the time
 * allotted, or DPLL3 was passed in, return -EINVAL.
 */
static int _omap3_noncore_dpll_lock(struct clk *clk)
{
	u8 ai;
	int r;

	if (clk == &dpll3_ck)
		return -EINVAL;

	pr_debug("clock: locking DPLL %s\n", clk->name);

	ai = omap3_dpll_autoidle_read(clk);

	omap3_dpll_deny_idle(clk);

	_omap3_dpll_write_clken(clk, DPLL_LOCKED);

	r = _omap3_wait_dpll_status(clk, 1);

	if (ai)
		omap3_dpll_allow_idle(clk);

	return r;
}

/*
 * _omap3_noncore_dpll_bypass - instruct a DPLL to bypass and wait for readiness
 * @clk: pointer to a DPLL struct clk
 *
 * Instructs a non-CORE DPLL to enter low-power bypass mode.  In
 * bypass mode, the DPLL's rate is set equal to its parent clock's
 * rate.  Waits for the DPLL to report readiness before returning.
 * Will save and restore the DPLL's autoidle state across the enable,
 * per the CDP code.  If the DPLL entered bypass mode successfully,
 * return 0; if the DPLL did not enter bypass in the time allotted, or
 * DPLL3 was passed in, or the DPLL does not support low-power bypass,
 * return -EINVAL.
 */
static int _omap3_noncore_dpll_bypass(struct clk *clk)
{
	int r;
	u8 ai;

	if (clk == &dpll3_ck)
		return -EINVAL;

	if (!(clk->dpll_data->modes & (1 << DPLL_LOW_POWER_BYPASS)))
		return -EINVAL;

	pr_debug("clock: configuring DPLL %s for low-power bypass\n",
		 clk->name);

	ai = omap3_dpll_autoidle_read(clk);

	_omap3_dpll_write_clken(clk, DPLL_LOW_POWER_BYPASS);

	r = _omap3_wait_dpll_status(clk, 0);

	if (ai)
		omap3_dpll_allow_idle(clk);
	else
		omap3_dpll_deny_idle(clk);

	return r;
}

/*
 * _omap3_noncore_dpll_stop - instruct a DPLL to stop
 * @clk: pointer to a DPLL struct clk
 *
 * Instructs a non-CORE DPLL to enter low-power stop. Will save and
 * restore the DPLL's autoidle state across the stop, per the CDP
 * code.  If DPLL3 was passed in, or the DPLL does not support
 * low-power stop, return -EINVAL; otherwise, return 0.
 */
static int _omap3_noncore_dpll_stop(struct clk *clk)
{
	u8 ai;

	if (clk == &dpll3_ck)
		return -EINVAL;

	if (!(clk->dpll_data->modes & (1 << DPLL_LOW_POWER_STOP)))
		return -EINVAL;

	pr_debug("clock: stopping DPLL %s\n", clk->name);

	ai = omap3_dpll_autoidle_read(clk);

	_omap3_dpll_write_clken(clk, DPLL_LOW_POWER_STOP);

	if (ai)
		omap3_dpll_allow_idle(clk);
	else
		omap3_dpll_deny_idle(clk);

	return 0;
}

/**
 * omap3_noncore_dpll_enable - instruct a DPLL to enter bypass or lock mode
 * @clk: pointer to a DPLL struct clk
 *
 * Instructs a non-CORE DPLL to enable, e.g., to enter bypass or lock.
 * The choice of modes depends on the DPLL's programmed rate: if it is
 * the same as the DPLL's parent clock, it will enter bypass;
 * otherwise, it will enter lock.  This code will wait for the DPLL to
 * indicate readiness before returning, unless the DPLL takes too long
 * to enter the target state.  Intended to be used as the struct clk's
 * enable function.  If DPLL3 was passed in, or the DPLL does not
 * support low-power stop, or if the DPLL took too long to enter
 * bypass or lock, return -EINVAL; otherwise, return 0.
 */
static int omap3_noncore_dpll_enable(struct clk *clk)
{
	int r;
	struct dpll_data *dd;

	if (clk == &dpll3_ck)
		return -EINVAL;

	dd = clk->dpll_data;
	if (!dd)
		return -EINVAL;
	/*
	 * Ensure M/N register is confgured before
	 * Enable it, otherwise DPLL will fail to lock
	 */
	if (clk->rate == 0) {
		WARN_ON(dd->default_rate == 0);
		r = omap3_noncore_dpll_set_rate(clk, dd->default_rate);
		if (r)
			return r;
	}

	if (clk->rate == dd->clk_bypass->rate) {
		WARN_ON(clk->parent != dd->clk_bypass);
		r = _omap3_noncore_dpll_bypass(clk);
	} else {
		WARN_ON(clk->parent != dd->clk_ref);
		r = _omap3_noncore_dpll_lock(clk);
	}
	/* FIXME: this is dubious - if clk->rate has changed, what about propagating? */
	if (!r)
		clk->rate = omap2_get_dpll_rate(clk);

	return r;
}

/**
 * omap3_noncore_dpll_disable - instruct a DPLL to enter bypass or lock mode
 * @clk: pointer to a DPLL struct clk
 *
 * Instructs a non-CORE DPLL to enable, e.g., to enter bypass or lock.
 * The choice of modes depends on the DPLL's programmed rate: if it is
 * the same as the DPLL's parent clock, it will enter bypass;
 * otherwise, it will enter lock.  This code will wait for the DPLL to
 * indicate readiness before returning, unless the DPLL takes too long
 * to enter the target state.  Intended to be used as the struct clk's
 * enable function.  If DPLL3 was passed in, or the DPLL does not
 * support low-power stop, or if the DPLL took too long to enter
 * bypass or lock, return -EINVAL; otherwise, return 0.
 */
static void omap3_noncore_dpll_disable(struct clk *clk)
{
	if (clk == &dpll3_ck)
		return;

	_omap3_noncore_dpll_stop(clk);
}

/* OMAP3630 j-type DPLL4 compensation variables */
void lookup_dco_sddiv(struct clk *clk, u8 *dco, u8 *sd_div, u16 m, u8 n)
{
	unsigned long fint, clkinp, sd; /* watch out for overflow */
	int mod1, mod2;

	++n; /* always n+1 below */
	clkinp = clk->parent->rate;
	fint = (clkinp / n) * m;

	if (fint < 1000000000)
		*dco = 2;
	else
		*dco = 4;
	/*
	* target sigma-delta to near 250MHz
	* sd = ceil[(m/(n+1)) * (clkinp_MHz / 250)]
	*/
	clkinp /= 100000; /* shift from MHz to 10*Hz for 38.4 and 19.2*/
	mod1 = (clkinp * m) % (250 * n);
	sd = (clkinp * m) / (250 * n);
	mod2 = sd % 10;
	sd /= 10;

	if (mod1 + mod2)
		++sd;
	*sd_div = sd;
}

/*
 * omap3_noncore_dpll_program - set non-core DPLL M,N values directly
 * @clk: struct clk * of DPLL to set
 * @m: DPLL multiplier to set
 * @n: DPLL divider to set
 * @freqsel: FREQSEL value to set
 *
 * Program the DPLL with the supplied M, N values, and wait for the DPLL to
 * lock..  Returns -EINVAL upon error, or 0 upon success.
 */
static int omap3_noncore_dpll_program(struct clk *clk, u16 m, u8 n, u16 freqsel)
{
	struct dpll_data *dd = clk->dpll_data;
	u32 v;

	/* 3430 ES2 TRM: 4.7.6.9 DPLL Programming Sequence */
	_omap3_noncore_dpll_bypass(clk);

	/* Set jitter correction */
	v = __raw_readl(dd->control_reg);
	v &= ~dd->freqsel_mask;
	v |= freqsel << __ffs(dd->freqsel_mask);
	__raw_writel(v, dd->control_reg);

	/* Set DPLL multiplier, divider */
	v = __raw_readl(dd->mult_div1_reg);
	v &= ~(dd->mult_mask | dd->div1_mask);
	v |= m << __ffs(dd->mult_mask);
	v |= (n - 1) << __ffs(dd->div1_mask);
	if (dd->jtype) {
		u8 dco, sd_div;
		lookup_dco_sddiv(clk, &dco, &sd_div, m, n);
		v &= ~(dd->dco_sel_mask | dd->sd_div_mask);
		v |=  dco << __ffs(dd->dco_sel_mask);
		v |=  sd_div << __ffs(dd->sd_div_mask);
	}
	__raw_writel(v, dd->mult_div1_reg);

	/* We let the clock framework set the other output dividers later */

	/* REVISIT: Set ramp-up delay? */

	_omap3_noncore_dpll_lock(clk);

	return 0;
}

/**
 * omap3_noncore_dpll_set_rate - set non-core DPLL rate
 * @clk: struct clk * of DPLL to set
 * @rate: rounded target rate
 *
 * Set the DPLL CLKOUT to the target rate.  If the DPLL can enter
 * low-power bypass, and the target rate is the bypass source clock
 * rate, then configure the DPLL for bypass.  Otherwise, round the
 * target rate if it hasn't been done already, then program and lock
 * the DPLL.  Returns -EINVAL upon error, or 0 upon success.
 */
static int omap3_noncore_dpll_set_rate(struct clk *clk, unsigned long rate)
{
	struct clk *new_parent = NULL;
	u16 freqsel;
	struct dpll_data *dd;
	int ret;

	if (!clk || !rate)
		return -EINVAL;

	dd = clk->dpll_data;
	if (!dd)
		return -EINVAL;

	if (rate == omap2_get_dpll_rate(clk))
		return 0;

	/*
	 * Ensure both the bypass and ref clocks are enabled prior to
	 * doing anything; we need the bypass clock running to reprogram
	 * the DPLL.
	 */
	omap2_clk_enable(dd->clk_bypass);
	omap2_clk_enable(dd->clk_ref);

	if (dd->clk_bypass->rate == rate &&
	    (clk->dpll_data->modes & (1 << DPLL_LOW_POWER_BYPASS))) {
		pr_debug("clock: %s: set rate: entering bypass.\n", clk->name);

		ret = _omap3_noncore_dpll_bypass(clk);
		if (!ret)
			new_parent = dd->clk_bypass;
	} else {
		if (dd->last_rounded_rate != rate)
			omap2_dpll_round_rate(clk, rate);

		if (dd->last_rounded_rate == 0)
			return -EINVAL;

		freqsel = _omap3_dpll_compute_freqsel(clk, dd->last_rounded_n);
		if (!freqsel)
			WARN_ON(1);

		pr_debug("clock: %s: set rate: locking rate to %lu.\n",
			 clk->name, rate);

		ret = omap3_noncore_dpll_program(clk, dd->last_rounded_m,
						 dd->last_rounded_n, freqsel);
		if (!ret)
			new_parent = dd->clk_ref;
	}
	if (!ret) {
		/*
		 * Switch the parent clock in the heirarchy, and make sure
		 * that the new parent's usecount is correct.  Note: we
		 * enable the new parent before disabling the old to avoid
		 * any unnecessary hardware disable->enable transitions.
		 */
		if (clk->usecount) {
			omap2_clk_enable(new_parent);
			omap2_clk_disable(clk->parent);
		}
		clk_reparent(clk, new_parent);
		clk->rate = rate;
	}
	omap2_clk_disable(dd->clk_ref);
	omap2_clk_disable(dd->clk_bypass);

	return 0;
}

static int omap3_dpll4_set_rate(struct clk *clk, unsigned long rate)
{
	/*
	 * According to the 12-5 CDP code from TI, "Limitation 2.5"
	 * on 3430ES1 prevents us from changing DPLL multipliers or dividers
	 * on DPLL4.
	 */
	if (omap_rev() == OMAP3430_REV_ES1_0) {
		printk(KERN_ERR "clock: DPLL4 cannot change rate due to "
		       "silicon 'Limitation 2.5' on 3430ES1.\n");
		return -EINVAL;
	}
	return omap3_noncore_dpll_set_rate(clk, rate);
}


/*
 * CORE DPLL (DPLL3) rate programming functions
 *
 * These call into SRAM code to do the actual CM writes, since the SDRAM
 * is clocked from DPLL3.
 */

/**
 * omap3_core_dpll_m2_set_rate - set CORE DPLL M2 divider
 * @clk: struct clk * of DPLL to set
 * @rate: rounded target rate
 *
 * Program the DPLL M2 divider with the rounded target rate.  Returns
 * -EINVAL upon error, or 0 upon success.
 */
static int omap3_core_dpll_m2_set_rate(struct clk *clk, unsigned long rate)
{
	u32 new_div = 0;
	u32 unlock_dll = 0;
	u32 c;
	unsigned long validrate, sdrcrate, mpurate;
	struct omap_sdrc_params *sdrc_cs0;
	struct omap_sdrc_params *sdrc_cs1;
	int ret;

	if (!clk || !rate)
		return -EINVAL;

	if (clk != &dpll3_m2_ck)
		return -EINVAL;

	validrate = omap2_clksel_round_rate_div(clk, rate, &new_div);
	if (validrate != rate)
		return -EINVAL;

	sdrcrate = sdrc_ick.rate;
	if (rate > clk->rate)
		sdrcrate <<= ((rate / clk->rate) >> 1);
	else
		sdrcrate >>= ((clk->rate / rate) >> 1);

	ret = omap2_sdrc_get_params(sdrcrate, &sdrc_cs0, &sdrc_cs1);
	if (ret)
		return -EINVAL;

	if (sdrcrate < MIN_SDRC_DLL_LOCK_FREQ) {
		pr_debug("clock: will unlock SDRC DLL\n");
		unlock_dll = 1;
	}

	/*
	 * XXX This only needs to be done when the CPU frequency changes
	 */
	mpurate = arm_fck.rate / CYCLES_PER_MHZ;
	c = (mpurate << SDRC_MPURATE_SCALE) >> SDRC_MPURATE_BASE_SHIFT;
	c += 1;  /* for safety */
	c *= SDRC_MPURATE_LOOPS;
	c >>= SDRC_MPURATE_SCALE;
	if (c == 0)
		c = 1;

	pr_debug("clock: changing CORE DPLL rate from %lu to %lu\n", clk->rate,
		 validrate);
	pr_debug("clock: SDRC CS0 timing params used:"
		 " RFR %08x CTRLA %08x CTRLB %08x MR %08x\n",
		 sdrc_cs0->rfr_ctrl, sdrc_cs0->actim_ctrla,
		 sdrc_cs0->actim_ctrlb, sdrc_cs0->mr);
	if (sdrc_cs1)
		pr_debug("clock: SDRC CS1 timing params used: "
		 " RFR %08x CTRLA %08x CTRLB %08x MR %08x\n",
		 sdrc_cs1->rfr_ctrl, sdrc_cs1->actim_ctrla,
		 sdrc_cs1->actim_ctrlb, sdrc_cs1->mr);

	/* Here irq already disabled */
	lock_scratchpad_sem();

	if (sdrc_cs1)
		omap3_configure_core_dpll(
				  new_div, unlock_dll, c, rate > clk->rate,
				  sdrc_cs0->rfr_ctrl, sdrc_cs0->actim_ctrla,
				  sdrc_cs0->actim_ctrlb, sdrc_cs0->mr,
				  sdrc_cs1->rfr_ctrl, sdrc_cs1->actim_ctrla,
				  sdrc_cs1->actim_ctrlb, sdrc_cs1->mr);
	else
		omap3_configure_core_dpll(
				  new_div, unlock_dll, c, rate > clk->rate,
				  sdrc_cs0->rfr_ctrl, sdrc_cs0->actim_ctrla,
				  sdrc_cs0->actim_ctrlb, sdrc_cs0->mr,
				  0, 0, 0, 0);

#ifdef CONFIG_PM
	omap3_save_scratchpad_contents();
#endif
	unlock_scratchpad_sem();

	return 0;
}


static const struct clkops clkops_noncore_dpll_ops = {
	.enable		= &omap3_noncore_dpll_enable,
	.disable	= &omap3_noncore_dpll_disable,
};

/* DPLL autoidle read/set code */


/**
 * omap3_dpll_autoidle_read - read a DPLL's autoidle bits
 * @clk: struct clk * of the DPLL to read
 *
 * Return the DPLL's autoidle bits, shifted down to bit 0.  Returns
 * -EINVAL if passed a null pointer or if the struct clk does not
 * appear to refer to a DPLL.
 */
static u32 omap3_dpll_autoidle_read(struct clk *clk)
{
	const struct dpll_data *dd;
	u32 v;

	if (!clk || !clk->dpll_data)
		return -EINVAL;

	dd = clk->dpll_data;

	v = __raw_readl(dd->autoidle_reg);
	v &= dd->autoidle_mask;
	v >>= __ffs(dd->autoidle_mask);

	return v;
}

/**
 * omap3_dpll_allow_idle - enable DPLL autoidle bits
 * @clk: struct clk * of the DPLL to operate on
 *
 * Enable DPLL automatic idle control.  This automatic idle mode
 * switching takes effect only when the DPLL is locked, at least on
 * OMAP3430.  The DPLL will enter low-power stop when its downstream
 * clocks are gated.  No return value.
 */
static void omap3_dpll_allow_idle(struct clk *clk)
{
	const struct dpll_data *dd;
	u32 v;

	if (!clk || !clk->dpll_data)
		return;

	dd = clk->dpll_data;

	/*
	 * REVISIT: CORE DPLL can optionally enter low-power bypass
	 * by writing 0x5 instead of 0x1.  Add some mechanism to
	 * optionally enter this mode.
	 */
	v = __raw_readl(dd->autoidle_reg);
	v &= ~dd->autoidle_mask;
	v |= DPLL_AUTOIDLE_LOW_POWER_STOP << __ffs(dd->autoidle_mask);
	__raw_writel(v, dd->autoidle_reg);
}

/**
 * omap3_dpll_deny_idle - prevent DPLL from automatically idling
 * @clk: struct clk * of the DPLL to operate on
 *
 * Disable DPLL automatic idle control.  No return value.
 */
static void omap3_dpll_deny_idle(struct clk *clk)
{
	const struct dpll_data *dd;
	u32 v;

	if (!clk || !clk->dpll_data)
		return;

	dd = clk->dpll_data;

	v = __raw_readl(dd->autoidle_reg);
	v &= ~dd->autoidle_mask;
	v |= DPLL_AUTOIDLE_DISABLE << __ffs(dd->autoidle_mask);
	__raw_writel(v, dd->autoidle_reg);
}

/* Clock control for DPLL outputs */

/**
 * omap3_clkoutx2_recalc - recalculate DPLL X2 output virtual clock rate
 * @clk: DPLL output struct clk
 *
 * Using parent clock DPLL data, look up DPLL state.  If locked, set our
 * rate to the dpll_clk * 2; otherwise, just use dpll_clk.
 */
static unsigned long omap3_clkoutx2_recalc(struct clk *clk)
{
	const struct dpll_data *dd;
	unsigned long rate;
	u32 v;
	struct clk *pclk;

	/* Walk up the parents of clk, looking for a DPLL */
	pclk = clk->parent;
	while (pclk && !pclk->dpll_data)
		pclk = pclk->parent;

	/* clk does not have a DPLL as a parent? */
	WARN_ON(!pclk);

	dd = pclk->dpll_data;

	WARN_ON(!dd->enable_mask);

	v = __raw_readl(dd->control_reg) & dd->enable_mask;
	v >>= __ffs(dd->enable_mask);
	if (v != OMAP3XXX_EN_DPLL_LOCKED || dd->jtype)
		rate = clk->parent->rate;
	else
		rate = clk->parent->rate * 2;
	return rate;
}

/* Common clock code */

/*
 * As it is structured now, this will prevent an OMAP2/3 multiboot
 * kernel from compiling.  This will need further attention.
 */
#if defined(CONFIG_ARCH_OMAP3)

#ifdef CONFIG_CPU_FREQ
static struct cpufreq_frequency_table freq_table[VDD1_OPP6+1];

#ifdef CONFIG_OMAP3_OVERCLOCK

static int proc_freq_table_read(char *buffer, char **buffer_location,
																off_t offset, int count, int *eof, void *data)
{
	int i, ret = 0;

	if (offset > 0)
		ret = 0;
	else
		for(i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++)
		{
			if(ret >= count)
				break;

			ret += scnprintf(buffer+ret, count-ret, "freq_table[%d] index=%u frequency=%u\n", i, freq_table[i].index, freq_table[i].frequency);
		}

		return ret;
}

static int proc_mpu_opps_read(char *buffer, char **buffer_location,
															off_t offset, int count, int *eof, void *data)
{
	int i, ret = 0;

	if (offset > 0)
		ret = 0;
	else
		for(i = MAX_VDD1_OPP; mpu_opps[i].rate; i--)
		{
			if(ret >= count)
				break;

			ret += scnprintf(buffer+ret, count-ret, "mpu_opps[%d] rate=%lu opp_id=%u vsel=%u sr_adjust_vsel=%u\n", i,
											 mpu_opps[i].rate, mpu_opps[i].opp_id, mpu_opps[i].vsel, mpu_opps[i].sr_adjust_vsel);
		}

		return ret;
}

#ifdef CONFIG_CPU_FREQ_STAT
extern void cpufreq_stats_freqtable_update(unsigned int cpu, unsigned int index, unsigned int freq);
#endif

static int proc_mpu_opps_write(struct file *filp, const char __user *buffer,
															 unsigned long len, void *data)
{
	uint index, rate, vsel;
	struct cpufreq_policy *policy;

	if (!mpu_opps)
		return -EFAULT;

	if(!len)
		return -ENOSPC;

	if(sscanf(buffer, "%d %d %d", &index, &rate, &vsel) == 3)
	{
		if (index < 1 || index > MAX_VDD1_OPP)
		{
			printk(KERN_INFO "overclock: invalid index\n");
			return -EFAULT;
		}

		//update mpu_opps
		mpu_opps[index].rate = rate;
		mpu_opps[index].vsel = vsel;
		mpu_opps[index].sr_adjust_vsel = 0;

		//update frequency table (MAX_VDD1_OPP - index)
		freq_table[MAX_VDD1_OPP - index].frequency = rate / 1000;
#ifdef CONFIG_CPU_FREQ_STAT
		cpufreq_stats_freqtable_update(0, MAX_VDD1_OPP - index, rate / 1000);
#endif

		//in case of MAX_VDD1_OPP update max policy
		//and in case of one update min policy
		if (index == MAX_VDD1_OPP)
		{
			policy = cpufreq_cpu_get(0);
			policy->max = policy->cpuinfo.max_freq =
			policy->user_policy.max = rate / 1000;
		}
		else if (index == 1)
		{
			policy = cpufreq_cpu_get(0);
			policy->min = policy->cpuinfo.min_freq =
			policy->user_policy.min = rate / 1000;
		}
	}
	else
		printk(KERN_INFO "overclock: insufficient parameters for mpu_opps\n");

	return len;
}

static int overclock_init(void)
{
	struct proc_dir_entry *proc_entry;

	printk(KERN_INFO "%s: Loading OMAP3 overclock\n", __func__);

	proc_mkdir("overclock", NULL);
	proc_entry = create_proc_read_entry("overclock/freq_table", 0444, NULL, proc_freq_table_read, NULL);
	proc_entry = create_proc_read_entry("overclock/mpu_opps", 0644, NULL, proc_mpu_opps_read, NULL);
	proc_entry->write_proc = proc_mpu_opps_write;

	return 0;
}
#endif

void omap2_clk_init_cpufreq_table(struct cpufreq_frequency_table **table)
{
	struct omap_opp *prcm;
	int i = 0;

	if (!mpu_opps)
		return;

	prcm = mpu_opps + MAX_VDD1_OPP;
	printk("%s: mpu_opps %ld\n prcm->rate %ld\n",__func__, mpu_opps->rate,  prcm->rate);
	for (; prcm->rate; prcm--) {
		freq_table[i].index = i;
		freq_table[i].frequency = prcm->rate / 1000;

		i++;
	}

	if (i == 0) {
		printk(KERN_WARNING "%s: failed to initialize frequency \
								table\n",
								__func__);
		return;
	}

	freq_table[i].index = i;
	freq_table[i].frequency = CPUFREQ_TABLE_END;

	*table = &freq_table[0];
#ifdef CONFIG_OMAP3_OVERCLOCK
	overclock_init();
#endif
}
#endif

static struct clk_functions omap2_clk_functions = {
	.clk_enable		= omap2_clk_enable,
	.clk_disable		= omap2_clk_disable,
	.clk_round_rate		= omap2_clk_round_rate,
	.clk_set_rate		= omap2_clk_set_rate,
	.clk_set_parent		= omap2_clk_set_parent,
	.clk_disable_unused	= omap2_clk_disable_unused,
#ifdef CONFIG_CPU_FREQ
	.clk_init_cpufreq_table = omap2_clk_init_cpufreq_table,
#endif
};

/*
 * Set clocks for bypass mode for reboot to work.
 */
void omap2_clk_prepare_for_reboot(void)
{
	/* REVISIT: Not ready for 343x */
#if 0
	u32 rate;

	if (vclk == NULL || sclk == NULL)
		return;

	rate = clk_get_rate(sclk);
	clk_set_rate(vclk, rate);
#endif
}

static void omap3_clk_lock_dpll5(void)
{
	struct clk *dpll5_clk;
	struct clk *dpll5_m2_clk;

	dpll5_clk = clk_get(NULL, "dpll5_ck");
	clk_set_rate(dpll5_clk, DPLL5_FREQ_FOR_USBHOST);
	clk_enable(dpll5_clk);

	/* Enable autoidle to allow it to enter low power bypass */
	omap3_dpll_allow_idle(dpll5_clk);

	/* Program dpll5_m2_clk divider for no division */
	dpll5_m2_clk = clk_get(NULL, "dpll5_m2_ck");
	clk_enable(dpll5_m2_clk);
	clk_set_rate(dpll5_m2_clk, DPLL5_FREQ_FOR_USBHOST);

	clk_disable(dpll5_m2_clk);
	clk_disable(dpll5_clk);
	return;
}

/* REVISIT: Move this init stuff out into clock.c */

/*
 * Switch the MPU rate if specified on cmdline.
 * We cannot do this early until cmdline is parsed.
 */
static int __init omap2_clk_arch_init(void)
{
	if (!mpurate)
		return -EINVAL;

	/* REVISIT: not yet ready for 343x */
	if (clk_set_rate(&dpll1_ck, mpurate))
		printk(KERN_ERR "*** Unable to set MPU rate\n");

	recalculate_root_clocks();

	printk(KERN_INFO "Switched to new clocking rate (Crystal/Core/MPU): "
	       "%ld.%01ld/%ld/%ld MHz\n",
	       (osc_sys_ck.rate / 1000000), ((osc_sys_ck.rate / 100000) % 10),
	       (core_ck.rate / 1000000), (arm_fck.rate / 1000000)) ;

	calibrate_delay();

	return 0;
}
arch_initcall(omap2_clk_arch_init);

int __init omap2_clk_init(void)
{
	/* struct prcm_config *prcm; */
	struct omap_clk *c;
	/* u32 clkrate; */
	u32 cpu_clkflg;

	/* Get omap3630 revision id from devtree for sw tiering*/
	if (cpu_is_omap3630())
		get_omap3630_revision_id();

	if (cpu_is_omap34xx()) {
		cpu_mask = RATE_IN_343X;
		cpu_clkflg = CK_343X;

		/*
		 * Update this if there are further clock changes between ES2
		 * and production parts
		 */
		if (omap_rev() == OMAP3430_REV_ES1_0) {
			/* No 3430ES1-only rates exist, so no RATE_IN_3430ES1 */
			cpu_clkflg |= CK_3430ES1;
		} else {
			cpu_mask |= RATE_IN_3430ES2;
			cpu_clkflg |= CK_3430ES2;
		}
		if (cpu_is_omap3630()) {
			dpll4_ck.dpll_data->jtype = 1;
			dpll4_dd.mult_mask =
				OMAP3430_PERIPH_DPLL_36XX_MULT_MASK;

			dpll3_m3x2_ck.ops = &clkops_omap3630_pwrdn_bug;
			dpll4_m2x2_ck.ops = &clkops_omap3630_pwrdn_bug;
			dpll4_m3x2_ck.ops = &clkops_omap3630_pwrdn_bug;
			dpll4_m4x2_ck.ops = &clkops_omap3630_pwrdn_bug;
			dpll4_m5x2_ck.ops = &clkops_omap3630_pwrdn_bug;
			dpll4_m6x2_ck.ops = &clkops_omap3630_pwrdn_bug;

			cpu_clkflg |= CK_363X;
			cpu_mask |= RATE_IN_363X;

			omap_96m_alwon_fck.parent = &omap_192m_alwon_ck;
			omap_96m_alwon_fck.init = &omap2_init_clksel_parent;
			omap_96m_alwon_fck.clksel_reg =
				OMAP_CM_REGADDR(CORE_MOD, CM_CLKSEL);
			omap_96m_alwon_fck.clksel_mask =
				OMAP3630_CLKSEL_96M_MASK;
			omap_96m_alwon_fck.clksel = omap_96m_alwon_fck_clksel;
			omap_96m_alwon_fck.recalc = &omap2_clksel_recalc;
		}
	}

	clk_init(&omap2_clk_functions);

	for (c = omap34xx_clks; c < omap34xx_clks + ARRAY_SIZE(omap34xx_clks); c++)
		clk_preinit(c->lk.clk);

	for (c = omap34xx_clks; c < omap34xx_clks + ARRAY_SIZE(omap34xx_clks); c++) {
		if (cpu_is_omap3630()) {
			if (c->lk.clk->flags & CK_363X)
				c->lk.clk->clksel_mask = c->lk.clk->clksel_mask2;
		}
		if (c->cpu & cpu_clkflg) {
			clkdev_add(&c->lk);
			clk_register(c->lk.clk);
			omap2_init_clk_clkdm(c->lk.clk);
		}
	}

	/* REVISIT: Not yet ready for OMAP3 */
#if 0
	/* Check the MPU rate set by bootloader */
	clkrate = omap2_get_dpll_rate_24xx(&dpll_ck);
	for (prcm = rate_table; prcm->mpu_speed; prcm++) {
		if (!(prcm->flags & cpu_mask))
			continue;
		if (prcm->xtal_speed != sys_ck.rate)
			continue;
		if (prcm->dpll_speed <= clkrate)
			 break;
	}
	curr_prcm_set = prcm;
#endif

	recalculate_root_clocks();

	printk(KERN_INFO "Clocking rate (Crystal/Core/MPU): "
	       "%ld.%01ld/%ld/%ld MHz\n",
	       (osc_sys_ck.rate / 1000000), (osc_sys_ck.rate / 100000) % 10,
	       (core_ck.rate / 1000000), (arm_fck.rate / 1000000));

	cpu_hz = arm_fck.rate;

	/*
	 * Only enable those clocks we will need, let the drivers
	 * enable other clocks as necessary
	 */
	clk_enable_init_clocks();

	/*
	 * Lock DPLL5 and put it in autoidle.
	 */
	if (omap_rev() >= OMAP3430_REV_ES2_0)
		omap3_clk_lock_dpll5();

	/* Avoid sleeping during omap2_clk_prepare_for_reboot() */
	/* REVISIT: not yet ready for 343x */
#if 0
	vclk = clk_get(NULL, "virt_prcm_set");
	sclk = clk_get(NULL, "sys_ck");
#endif
	return 0;
}

#endif /* CONFIG_ARCH_OMAP3 */
