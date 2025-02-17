/*
 * linux/drivers/video/omap2/dss/dsi.c
 *
 * Copyright (C) 2009 Nokia Corporation
 * Author: Tomi Valkeinen <tomi.valkeinen@nokia.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define DSS_SUBSYS_NAME "DSI"

#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/seq_file.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/kthread.h>
#include <linux/wait.h>

#include <plat/display.h>
#include <plat/clock.h>

#include "dss.h"

#define PRINT_FPS 1
#ifdef PRINT_FPS
static int dsi_fps;
module_param_named(dsi_fps, dsi_fps, bool, S_IRUGO | S_IWUSR | S_IWGRP);
#endif

/*#define VERBOSE_IRQ*/
#define DSI_CATCH_MISSING_TE

#define DSI_CMD_VC   1
#define DSI_VIDEO_VC 0

#define DSI_BASE		0x4804FC00

struct dsi_reg { u16 idx; };

#define DSI_REG(idx)		((const struct dsi_reg) { idx })

#define DSI_SZ_REGS		SZ_1K
/* DSI Protocol Engine */

#define DSI_REVISION			DSI_REG(0x0000)
#define DSI_SYSCONFIG			DSI_REG(0x0010)
#define DSI_SYSSTATUS			DSI_REG(0x0014)
#define DSI_IRQSTATUS			DSI_REG(0x0018)
#define DSI_IRQENABLE			DSI_REG(0x001C)
#define DSI_CTRL			DSI_REG(0x0040)
#define DSI_COMPLEXIO_CFG1		DSI_REG(0x0048)
#define DSI_COMPLEXIO_IRQ_STATUS	DSI_REG(0x004C)
#define DSI_COMPLEXIO_IRQ_ENABLE	DSI_REG(0x0050)
#define DSI_CLK_CTRL			DSI_REG(0x0054)
#define DSI_TIMING1			DSI_REG(0x0058)
#define DSI_TIMING2			DSI_REG(0x005C)
#define DSI_VM_TIMING1			DSI_REG(0x0060)
#define DSI_VM_TIMING2			DSI_REG(0x0064)
#define DSI_VM_TIMING3			DSI_REG(0x0068)
#define DSI_CLK_TIMING			DSI_REG(0x006C)
#define DSI_TX_FIFO_VC_SIZE		DSI_REG(0x0070)
#define DSI_RX_FIFO_VC_SIZE		DSI_REG(0x0074)
#define DSI_COMPLEXIO_CFG2		DSI_REG(0x0078)
#define DSI_RX_FIFO_VC_FULLNESS		DSI_REG(0x007C)
#define DSI_VM_TIMING4			DSI_REG(0x0080)
#define DSI_TX_FIFO_VC_EMPTINESS	DSI_REG(0x0084)
#define DSI_VM_TIMING5			DSI_REG(0x0088)
#define DSI_VM_TIMING6			DSI_REG(0x008C)
#define DSI_VM_TIMING7			DSI_REG(0x0090)
#define DSI_STOPCLK_TIMING		DSI_REG(0x0094)
#define DSI_VC_CTRL(n)			DSI_REG(0x0100 + (n * 0x20))
#define DSI_VC_TE(n)			DSI_REG(0x0104 + (n * 0x20))
#define DSI_VC_LONG_PACKET_HEADER(n)	DSI_REG(0x0108 + (n * 0x20))
#define DSI_VC_LONG_PACKET_PAYLOAD(n)	DSI_REG(0x010C + (n * 0x20))
#define DSI_VC_SHORT_PACKET_HEADER(n)	DSI_REG(0x0110 + (n * 0x20))
#define DSI_VC_IRQSTATUS(n)		DSI_REG(0x0118 + (n * 0x20))
#define DSI_VC_IRQENABLE(n)		DSI_REG(0x011C + (n * 0x20))

/* DSIPHY_SCP */

#define DSI_DSIPHY_CFG0			DSI_REG(0x200 + 0x0000)
#define DSI_DSIPHY_CFG1			DSI_REG(0x200 + 0x0004)
#define DSI_DSIPHY_CFG2			DSI_REG(0x200 + 0x0008)
#define DSI_DSIPHY_CFG5			DSI_REG(0x200 + 0x0014)

/* DSI_PLL_CTRL_SCP */

#define DSI_PLL_CONTROL			DSI_REG(0x300 + 0x0000)
#define DSI_PLL_STATUS			DSI_REG(0x300 + 0x0004)
#define DSI_PLL_GO			DSI_REG(0x300 + 0x0008)
#define DSI_PLL_CONFIGURATION1		DSI_REG(0x300 + 0x000C)
#define DSI_PLL_CONFIGURATION2		DSI_REG(0x300 + 0x0010)

#define REG_GET(idx, start, end) \
	FLD_GET(dsi_read_reg(idx), start, end)

#define REG_FLD_MOD(idx, val, start, end) \
	dsi_write_reg(idx, FLD_MOD(dsi_read_reg(idx), val, start, end))

/* Global interrupts */
#define DSI_IRQ_VC0		(1 << 0)
#define DSI_IRQ_VC1		(1 << 1)
#define DSI_IRQ_VC2		(1 << 2)
#define DSI_IRQ_VC3		(1 << 3)
#define DSI_IRQ_WAKEUP		(1 << 4)
#define DSI_IRQ_RESYNC		(1 << 5)
#define DSI_IRQ_PLL_LOCK	(1 << 7)
#define DSI_IRQ_PLL_UNLOCK	(1 << 8)
#define DSI_IRQ_PLL_RECALL	(1 << 9)
#define DSI_IRQ_COMPLEXIO_ERR	(1 << 10)
#define DSI_IRQ_HS_TX_TIMEOUT	(1 << 14)
#define DSI_IRQ_LP_RX_TIMEOUT	(1 << 15)
#define DSI_IRQ_TE_TRIGGER	(1 << 16)
#define DSI_IRQ_ACK_TRIGGER	(1 << 17)
#define DSI_IRQ_SYNC_LOST	(1 << 18)
#define DSI_IRQ_LDO_POWER_GOOD	(1 << 19)
#define DSI_IRQ_TA_TIMEOUT	(1 << 20)
#define DSI_IRQ_ERROR_MASK \
	(DSI_IRQ_HS_TX_TIMEOUT | DSI_IRQ_LP_RX_TIMEOUT | DSI_IRQ_SYNC_LOST | \
	DSI_IRQ_TA_TIMEOUT)
#define DSI_IRQ_CHANNEL_MASK	0xf

/* Virtual channel interrupts */
#define DSI_VC_IRQ_CS		(1 << 0)
#define DSI_VC_IRQ_ECC_CORR	(1 << 1)
#define DSI_VC_IRQ_PACKET_SENT	(1 << 2)
#define DSI_VC_IRQ_FIFO_TX_OVF	(1 << 3)
#define DSI_VC_IRQ_FIFO_RX_OVF	(1 << 4)
#define DSI_VC_IRQ_BTA		(1 << 5)
#define DSI_VC_IRQ_ECC_NO_CORR	(1 << 6)
#define DSI_VC_IRQ_FIFO_TX_UDF	(1 << 7)
#define DSI_VC_IRQ_PP_BUSY_CHANGE (1 << 8)
#define DSI_VC_IRQ_ERROR_MASK \
	(DSI_VC_IRQ_CS | DSI_VC_IRQ_ECC_CORR | DSI_VC_IRQ_FIFO_TX_OVF | \
	DSI_VC_IRQ_FIFO_RX_OVF | DSI_VC_IRQ_ECC_NO_CORR | \
	DSI_VC_IRQ_FIFO_TX_UDF)

/* ComplexIO interrupts */
#define DSI_CIO_IRQ_ERRSYNCESC1		(1 << 0)
#define DSI_CIO_IRQ_ERRSYNCESC2		(1 << 1)
#define DSI_CIO_IRQ_ERRSYNCESC3		(1 << 2)
#define DSI_CIO_IRQ_ERRESC1		(1 << 5)
#define DSI_CIO_IRQ_ERRESC2		(1 << 6)
#define DSI_CIO_IRQ_ERRESC3		(1 << 7)
#define DSI_CIO_IRQ_ERRCONTROL1		(1 << 10)
#define DSI_CIO_IRQ_ERRCONTROL2		(1 << 11)
#define DSI_CIO_IRQ_ERRCONTROL3		(1 << 12)
#define DSI_CIO_IRQ_STATEULPS1		(1 << 15)
#define DSI_CIO_IRQ_STATEULPS2		(1 << 16)
#define DSI_CIO_IRQ_STATEULPS3		(1 << 17)
#define DSI_CIO_IRQ_ERRCONTENTIONLP0_1	(1 << 20)
#define DSI_CIO_IRQ_ERRCONTENTIONLP1_1	(1 << 21)
#define DSI_CIO_IRQ_ERRCONTENTIONLP0_2	(1 << 22)
#define DSI_CIO_IRQ_ERRCONTENTIONLP1_2	(1 << 23)
#define DSI_CIO_IRQ_ERRCONTENTIONLP0_3	(1 << 24)
#define DSI_CIO_IRQ_ERRCONTENTIONLP1_3	(1 << 25)
#define DSI_CIO_IRQ_ULPSACTIVENOT_ALL0	(1 << 30)
#define DSI_CIO_IRQ_ULPSACTIVENOT_ALL1	(1 << 31)

#define DSI_DT_DCS_SHORT_WRITE_0	0x05
#define DSI_DT_DCS_SHORT_WRITE_1	0x15
#define DSI_DT_DCS_READ			0x06
#define DSI_DT_SET_MAX_RET_PKG_SIZE	0x37
#define DSI_DT_NULL_PACKET		0x09
#define DSI_DT_DCS_LONG_WRITE		0x39

#define DSI_DT_RX_ACK_WITH_ERR		0x02
#define DSI_DT_RX_DCS_LONG_READ		0x1c
#define DSI_DT_RX_SHORT_READ_1		0x21
#define DSI_DT_RX_SHORT_READ_2		0x22

#define FINT_MAX 2200000
#define FINT_MIN 750000
#define REGN_MAX (1 << 7)
#define REGM_MAX ((1 << 11) - 1)
#define REGM3_MAX (1 << 4)
#define REGM4_MAX (1 << 4)
#define LP_DIV_MAX ((1 << 13) - 1)

#define MAX_FRAMEDONE_TIMEOUTS	3
#define MIN_FRAMEDONE_OK        6

enum fifo_size {
	DSI_FIFO_SIZE_0		= 0,
	DSI_FIFO_SIZE_32	= 1,
	DSI_FIFO_SIZE_64	= 2,
	DSI_FIFO_SIZE_96	= 3,
	DSI_FIFO_SIZE_128	= 4,
};

enum dsi_vc_mode {
	DSI_VC_MODE_L4 = 0,
	DSI_VC_MODE_VP,
};

struct dsi_update_region {
	bool dirty;
	u16 x, y, w, h;
	struct omap_dss_device *device;
};

static struct
{
	void __iomem	*base;

	struct dsi_clock_info current_cinfo;

	struct regulator *vdds_dsi_reg;

	struct {
		enum dsi_vc_mode mode;
		struct omap_dss_device *dssdev;
		enum fifo_size fifo_size;
		int dest_per;	/* destination peripheral 0-3 */
	} vc[4];

	struct mutex lock;
	struct semaphore bus_lock;

	unsigned pll_locked;

	struct completion bta_completion;
	struct completion packet_sent_completion;


	struct task_struct *thread;
	wait_queue_head_t waitqueue;

	spinlock_t update_lock;
	bool framedone_received;
	struct dsi_update_region update_region;
	struct dsi_update_region active_update_region;
	struct completion update_completion;

	enum omap_dss_update_mode user_update_mode;
	enum omap_dss_update_mode update_mode;
	bool te_enabled;
	bool use_ext_te;
	bool te_trigger;
	bool sw_te_sup;

	struct hrtimer hrtimer;

#ifdef DSI_CATCH_MISSING_TE
	struct timer_list te_timer;
#endif

	unsigned long cache_req_pck;
	unsigned long cache_clk_freq;
	struct dsi_clock_info cache_cinfo;

	u32		errors;
	spinlock_t	errors_lock;
#ifdef DEBUG
	ktime_t perf_setup_time;
	ktime_t perf_start_time;
	ktime_t perf_start_time_auto;
	int perf_measure_frames;
#endif
	int debug_read;
	int debug_write;

	struct {
		struct work_struct work;
		struct omap_dss_device *dssdev;
		bool enabled;
		bool recovering;
	} error_recovery;
} dsi;

#ifdef DEBUG
static unsigned int dsi_perf;
module_param_named(dsi_perf, dsi_perf, bool, 0644);
#endif

static inline void dsi_write_reg(const struct dsi_reg idx, u32 val)
{
	__raw_writel(val, dsi.base + idx.idx);
}

static inline u32 dsi_read_reg(const struct dsi_reg idx)
{
	return __raw_readl(dsi.base + idx.idx);
}


void dsi_save_context(void)
{
}

void dsi_restore_context(void)
{
}

void dsi_bus_lock(void)
{
	down(&dsi.bus_lock);
}
EXPORT_SYMBOL(dsi_bus_lock);

void dsi_bus_unlock(void)
{
	up(&dsi.bus_lock);
}
EXPORT_SYMBOL(dsi_bus_unlock);

static bool dsi_bus_is_locked(void)
{
	return dsi.bus_lock.count == 0;
}

static inline int wait_for_bit_change(const struct dsi_reg idx, int bitnum,
		int value)
{
	int t = 100000;

	while (REG_GET(idx, bitnum, bitnum) != value) {
		if (--t == 0)
			return !value;
	}

	return value;
}

#ifdef DEBUG
static void dsi_perf_mark_setup(void)
{
	dsi.perf_setup_time = ktime_get();
}

static void dsi_perf_mark_start(void)
{
	dsi.perf_start_time = ktime_get();
}

static void dsi_perf_mark_start_auto(void)
{
	dsi.perf_measure_frames = 0;
	dsi.perf_start_time_auto = ktime_get();
}

static void dsi_perf_show(const char *name)
{
	ktime_t t, setup_time, trans_time;
	u32 total_bytes;
	u32 setup_us, trans_us, total_us;

	if (!dsi_perf)
		return;

	if (dsi.update_mode == OMAP_DSS_UPDATE_DISABLED)
		return;

	t = ktime_get();

	setup_time = ktime_sub(dsi.perf_start_time, dsi.perf_setup_time);
	setup_us = (u32)ktime_to_us(setup_time);
	if (setup_us == 0)
		setup_us = 1;

	trans_time = ktime_sub(t, dsi.perf_start_time);
	trans_us = (u32)ktime_to_us(trans_time);
	if (trans_us == 0)
		trans_us = 1;

	total_us = setup_us + trans_us;

	total_bytes = dsi.active_update_region.w *
		dsi.active_update_region.h *
		dsi.active_update_region.device->ctrl.pixel_size / 8;

	if (dsi.update_mode == OMAP_DSS_UPDATE_AUTO) {
		static u32 s_total_trans_us, s_total_setup_us;
		static u32 s_min_trans_us = 0xffffffff, s_min_setup_us;
		static u32 s_max_trans_us, s_max_setup_us;
		const int numframes = 100;
		ktime_t total_time_auto;
		u32 total_time_auto_us;

		dsi.perf_measure_frames++;

		if (setup_us < s_min_setup_us)
			s_min_setup_us = setup_us;

		if (setup_us > s_max_setup_us)
			s_max_setup_us = setup_us;

		s_total_setup_us += setup_us;

		if (trans_us < s_min_trans_us)
			s_min_trans_us = trans_us;

		if (trans_us > s_max_trans_us)
			s_max_trans_us = trans_us;

		s_total_trans_us += trans_us;

		if (dsi.perf_measure_frames < numframes)
			return;

		total_time_auto = ktime_sub(t, dsi.perf_start_time_auto);
		total_time_auto_us = (u32)ktime_to_us(total_time_auto);

		printk(KERN_INFO "DSI(%s): %u fps, setup %u/%u/%u, "
				"trans %u/%u/%u\n",
				name,
				1000 * 1000 * numframes / total_time_auto_us,
				s_min_setup_us,
				s_max_setup_us,
				s_total_setup_us / numframes,
				s_min_trans_us,
				s_max_trans_us,
				s_total_trans_us / numframes);

		s_total_setup_us = 0;
		s_min_setup_us = 0xffffffff;
		s_max_setup_us = 0;
		s_total_trans_us = 0;
		s_min_trans_us = 0xffffffff;
		s_max_trans_us = 0;
		dsi_perf_mark_start_auto();
	} else {
		printk(KERN_INFO "DSI(%s): %u us + %u us = %u us (%uHz), "
				"%u bytes, %u kbytes/sec\n",
				name,
				setup_us,
				trans_us,
				total_us,
				1000*1000 / total_us,
				total_bytes,
				total_bytes * 1000 / total_us);
	}
}
#else
#define dsi_perf_mark_setup()
#define dsi_perf_mark_start()
#define dsi_perf_mark_start_auto()
#define dsi_perf_show(x)
#endif

static void print_irq_status(u32 status)
{
#ifndef VERBOSE_IRQ
	if ((status & ~DSI_IRQ_CHANNEL_MASK) == 0)
		return;
#endif
	printk(KERN_DEBUG "DSI IRQ: 0x%x: ", status);

#define PIS(x) \
	if (status & DSI_IRQ_##x) \
		printk(#x " ");
#ifdef VERBOSE_IRQ
	PIS(VC0);
	PIS(VC1);
	PIS(VC2);
	PIS(VC3);
#endif
	PIS(WAKEUP);
	PIS(RESYNC);
	PIS(PLL_LOCK);
	PIS(PLL_UNLOCK);
	PIS(PLL_RECALL);
	PIS(COMPLEXIO_ERR);
	PIS(HS_TX_TIMEOUT);
	PIS(LP_RX_TIMEOUT);
	PIS(TE_TRIGGER);
	PIS(ACK_TRIGGER);
	PIS(SYNC_LOST);
	PIS(LDO_POWER_GOOD);
	PIS(TA_TIMEOUT);
#undef PIS

	printk("\n");
}

static void print_irq_status_vc(int channel, u32 status)
{
#ifndef VERBOSE_IRQ
	if ((status & ~DSI_VC_IRQ_PACKET_SENT) == 0)
		return;
#endif
	printk(KERN_DEBUG "DSI VC(%d) IRQ 0x%x: ", channel, status);

#define PIS(x) \
	if (status & DSI_VC_IRQ_##x) \
		printk(#x " ");
	PIS(CS);
	PIS(ECC_CORR);
#ifdef VERBOSE_IRQ
	PIS(PACKET_SENT);
#endif
	PIS(FIFO_TX_OVF);
	PIS(FIFO_RX_OVF);
	PIS(BTA);
	PIS(ECC_NO_CORR);
	PIS(FIFO_TX_UDF);
	PIS(PP_BUSY_CHANGE);
#undef PIS
	printk("\n");
}

static void print_irq_status_cio(u32 status)
{
	printk(KERN_DEBUG "DSI CIO IRQ 0x%x: ", status);

#define PIS(x) \
	if (status & DSI_CIO_IRQ_##x) \
		printk(#x " ");
	PIS(ERRSYNCESC1);
	PIS(ERRSYNCESC2);
	PIS(ERRSYNCESC3);
	PIS(ERRESC1);
	PIS(ERRESC2);
	PIS(ERRESC3);
	PIS(ERRCONTROL1);
	PIS(ERRCONTROL2);
	PIS(ERRCONTROL3);
	PIS(STATEULPS1);
	PIS(STATEULPS2);
	PIS(STATEULPS3);
	PIS(ERRCONTENTIONLP0_1);
	PIS(ERRCONTENTIONLP1_1);
	PIS(ERRCONTENTIONLP0_2);
	PIS(ERRCONTENTIONLP1_2);
	PIS(ERRCONTENTIONLP0_3);
	PIS(ERRCONTENTIONLP1_3);
	PIS(ULPSACTIVENOT_ALL0);
	PIS(ULPSACTIVENOT_ALL1);
#undef PIS

	printk("\n");
}

static int debug_irq;

static void schedule_error_recovery(void)
{
	if (dsi.error_recovery.enabled && !dsi.error_recovery.recovering)
		schedule_work(&dsi.error_recovery.work);
}

/* called from dss */
void dsi_irq_handler(void)
{
	u32 irqstatus, vcstatus, ciostatus;
	int i;

	irqstatus = dsi_read_reg(DSI_IRQSTATUS);

	if (irqstatus & DSI_IRQ_ERROR_MASK) {
		DSSERR("DSI error, irqstatus %x\n", irqstatus);
		print_irq_status(irqstatus);
		spin_lock(&dsi.errors_lock);
		dsi.errors |= irqstatus & DSI_IRQ_ERROR_MASK;
		spin_unlock(&dsi.errors_lock);
		if (irqstatus & DSI_IRQ_TA_TIMEOUT)
		{
			schedule_error_recovery();
		}
	} else if (debug_irq) {
		print_irq_status(irqstatus);
	}

#ifdef DSI_CATCH_MISSING_TE
	if (irqstatus & DSI_IRQ_TE_TRIGGER) {
		del_timer(&dsi.te_timer);
		dsi.te_trigger = true;
	}
#else
	if (irqstatus & DSI_IRQ_TE_TRIGGER)
		dsi.te_trigger = true;
#endif
	for (i = 0; i < 4; ++i) {
		if ((irqstatus & (1<<i)) == 0)
			continue;

		vcstatus = dsi_read_reg(DSI_VC_IRQSTATUS(i));

		if (vcstatus & DSI_VC_IRQ_BTA)
			complete(&dsi.bta_completion);

		if (vcstatus & DSI_VC_IRQ_PACKET_SENT)
			complete(&dsi.packet_sent_completion);


		if (vcstatus & DSI_VC_IRQ_ERROR_MASK) {
			DSSERR("DSI VC(%d) error, vc irqstatus %x\n",
				       i, vcstatus);
			if (vcstatus & DSI_VC_IRQ_BTA)
			{
				schedule_error_recovery();
			}

			print_irq_status_vc(i, vcstatus);
		} else if (debug_irq) {
			print_irq_status_vc(i, vcstatus);
		}

		dsi_write_reg(DSI_VC_IRQSTATUS(i), vcstatus);
		/* flush posted write */
		dsi_read_reg(DSI_VC_IRQSTATUS(i));
	}

	if (irqstatus & DSI_IRQ_COMPLEXIO_ERR) {
		ciostatus = dsi_read_reg(DSI_COMPLEXIO_IRQ_STATUS);

		dsi_write_reg(DSI_COMPLEXIO_IRQ_STATUS, ciostatus);
		/* flush posted write */
		dsi_read_reg(DSI_COMPLEXIO_IRQ_STATUS);

		DSSERR("DSI CIO error, cio irqstatus %x\n", ciostatus);
		print_irq_status_cio(ciostatus);
	}

	dsi_write_reg(DSI_IRQSTATUS, irqstatus & ~DSI_IRQ_CHANNEL_MASK);
	/* flush posted write */
	dsi_read_reg(DSI_IRQSTATUS);
}


static void _dsi_initialize_irq(void)
{
	u32 l;
	int i;

	/* disable all interrupts */
	dsi_write_reg(DSI_IRQENABLE, 0);
	for (i = 0; i < 4; ++i)
		dsi_write_reg(DSI_VC_IRQENABLE(i), 0);
	dsi_write_reg(DSI_COMPLEXIO_IRQ_ENABLE, 0);

	/* clear interrupt status */
	l = dsi_read_reg(DSI_IRQSTATUS);
	dsi_write_reg(DSI_IRQSTATUS, l & ~DSI_IRQ_CHANNEL_MASK);

	for (i = 0; i < 4; ++i) {
		l = dsi_read_reg(DSI_VC_IRQSTATUS(i));
		dsi_write_reg(DSI_VC_IRQSTATUS(i), l);
	}

	l = dsi_read_reg(DSI_COMPLEXIO_IRQ_STATUS);
	dsi_write_reg(DSI_COMPLEXIO_IRQ_STATUS, l);

	/* enable error irqs */
	l = DSI_IRQ_ERROR_MASK;
	l |= DSI_IRQ_TE_TRIGGER;
	dsi_write_reg(DSI_IRQENABLE, l);

	l = DSI_VC_IRQ_ERROR_MASK;
	for (i = 0; i < 4; ++i)
		dsi_write_reg(DSI_VC_IRQENABLE(i), l);

	/* XXX zonda responds incorrectly, causing control error:
	   Exit from LP-ESC mode to LP11 uses wrong transition states on the
	   data lines LP0 and LN0. */
	dsi_write_reg(DSI_COMPLEXIO_IRQ_ENABLE,
			-1 & (~DSI_CIO_IRQ_ERRCONTROL2));
}

static u32 dsi_get_errors(void)
{
	unsigned long flags;
	u32 e;
	spin_lock_irqsave(&dsi.errors_lock, flags);
	e = dsi.errors;
	dsi.errors = 0;
	spin_unlock_irqrestore(&dsi.errors_lock, flags);
	return e;
}

static void dsi_vc_enable_bta_irq(int channel)
{
	u32 l;

	dsi_write_reg(DSI_VC_IRQSTATUS(channel), DSI_VC_IRQ_BTA);

	l = dsi_read_reg(DSI_VC_IRQENABLE(channel));
	l |= DSI_VC_IRQ_BTA;
	dsi_write_reg(DSI_VC_IRQENABLE(channel), l);
}

static void dsi_vc_disable_bta_irq(int channel)
{
	u32 l;

	l = dsi_read_reg(DSI_VC_IRQENABLE(channel));
	l &= ~DSI_VC_IRQ_BTA;
	dsi_write_reg(DSI_VC_IRQENABLE(channel), l);
}


static void dsi_vc_enable_packet_sent_irq(int channel)
{
	u32 l;

	l = dsi_read_reg(DSI_VC_IRQENABLE(channel));
	l |= DSI_VC_IRQ_PACKET_SENT;
	dsi_write_reg(DSI_VC_IRQENABLE(channel), l);
}

static void dsi_vc_disable_packet_sent_irq(int channel)
{
	u32 l;

	l = dsi_read_reg(DSI_VC_IRQENABLE(channel));
	l &= ~DSI_VC_IRQ_PACKET_SENT;
	dsi_write_reg(DSI_VC_IRQENABLE(channel), l);
}


/* DSI func clock. this could also be DSI2_PLL_FCLK */
static inline void enable_clocks(bool enable)
{
	if (enable)
		dss_clk_enable(DSS_CLK_ICK | DSS_CLK_FCK1);
	else
		dss_clk_disable(DSS_CLK_ICK | DSS_CLK_FCK1);
}

/* source clock for DSI PLL. this could also be PCLKFREE */
static inline void dsi_enable_pll_clock(bool enable)
{
	if (enable)
		dss_clk_enable(DSS_CLK_FCK2);
	else
		dss_clk_disable(DSS_CLK_FCK2);

	if (enable && dsi.pll_locked) {
		if (wait_for_bit_change(DSI_PLL_STATUS, 1, 1) != 1)
			DSSERR("cannot lock PLL when enabling clocks\n");
	}
}

#ifdef DEBUG
static void _dsi_print_reset_status(void)
{
	u32 l;

	if (!dss_debug)
		return;

	/* A dummy read using the SCP interface to any DSIPHY register is
	 * required after DSIPHY reset to complete the reset of the DSI complex
	 * I/O. */
	l = dsi_read_reg(DSI_DSIPHY_CFG5);

	printk(KERN_DEBUG "DSI resets: ");

	l = dsi_read_reg(DSI_PLL_STATUS);
	printk("PLL (%d) ", FLD_GET(l, 0, 0));

	l = dsi_read_reg(DSI_COMPLEXIO_CFG1);
	printk("CIO (%d) ", FLD_GET(l, 29, 29));

	l = dsi_read_reg(DSI_DSIPHY_CFG5);
	printk("PHY (%x, %d, %d, %d)\n",
			FLD_GET(l, 28, 26),
			FLD_GET(l, 29, 29),
			FLD_GET(l, 30, 30),
			FLD_GET(l, 31, 31));
}
#else
#define _dsi_print_reset_status()
#endif

static inline int dsi_if_enable(bool enable)
{
	DSSDBG("dsi_if_enable(%d)\n", enable);

	enable = enable ? 1 : 0;
	REG_FLD_MOD(DSI_CTRL, enable, 0, 0); /* IF_EN */

	if (wait_for_bit_change(DSI_CTRL, 0, enable) != enable) {
			DSSERR("Failed to set dsi_if_enable to %d\n", enable);
			return -EIO;
	}

	return 0;
}

unsigned long dsi_get_dsi1_pll_rate(void)
{
	return dsi.current_cinfo.dsi1_pll_fclk;
}

static unsigned long dsi_get_dsi2_pll_rate(void)
{
	return dsi.current_cinfo.dsi2_pll_fclk;
}

static unsigned long dsi_get_txbyteclkhs(void)
{
	return dsi.current_cinfo.clkin4ddr / 16;
}

static unsigned long dsi_fclk_rate(void)
{
	unsigned long r;

	if (dss_get_dsi_clk_source() == DSS_SRC_DSS1_ALWON_FCLK) {
		/* DSI FCLK source is DSS1_ALWON_FCK, which is dss1_fck */
		r = dss_clk_get_rate(DSS_CLK_FCK1);
	} else {
		/* DSI FCLK source is DSI2_PLL_FCLK */
		r = dsi_get_dsi2_pll_rate();
	}

	return r;
}

static int dsi_set_lp_clk_divisor(struct omap_dss_device *dssdev)
{
	unsigned long dsi_fclk;
	unsigned lp_clk_div;
	unsigned long lp_clk;

	lp_clk_div = dssdev->phy.dsi.div.lp_clk_div;

	if (lp_clk_div == 0 || lp_clk_div > LP_DIV_MAX)
		return -EINVAL;

	dsi_fclk = dsi_fclk_rate();

	lp_clk = dsi_fclk / 2 / lp_clk_div;

	DSSDBG("LP_CLK_DIV %u, LP_CLK %lu\n", lp_clk_div, lp_clk);
	dsi.current_cinfo.lp_clk = lp_clk;
	dsi.current_cinfo.lp_clk_div = lp_clk_div;

	REG_FLD_MOD(DSI_CLK_CTRL, lp_clk_div, 12, 0);   /* LP_CLK_DIVISOR */

	REG_FLD_MOD(DSI_CLK_CTRL, dsi_fclk > 30000000 ? 1 : 0,
			21, 21);		/* LP_RX_SYNCHRO_ENABLE */

	return 0;
}


enum dsi_pll_power_state {
	DSI_PLL_POWER_OFF	= 0x0,
	DSI_PLL_POWER_ON_HSCLK	= 0x1,
	DSI_PLL_POWER_ON_ALL	= 0x2,
	DSI_PLL_POWER_ON_DIV	= 0x3,
};

static int dsi_pll_power(enum dsi_pll_power_state state)
{
	int t = 0;

	REG_FLD_MOD(DSI_CLK_CTRL, state, 31, 30);	/* PLL_PWR_CMD */

	/* PLL_PWR_STATUS */
	while (FLD_GET(dsi_read_reg(DSI_CLK_CTRL), 29, 28) != state) {
		udelay(1);
		if (t++ > 1000) {
			DSSERR("Failed to set DSI PLL power mode to %d\n",
					state);
			return -ENODEV;
		}
	}

	return 0;
}

/* calculate clock rates using dividers in cinfo */
static int dsi_calc_clock_rates(struct dsi_clock_info *cinfo)
{
	if (cinfo->regn == 0 || cinfo->regn > REGN_MAX)
		return -EINVAL;

	if (cinfo->regm == 0 || cinfo->regm > REGM_MAX)
		return -EINVAL;

	if (cinfo->regm3 > REGM3_MAX)
		return -EINVAL;

	if (cinfo->regm4 > REGM4_MAX)
		return -EINVAL;

	if (cinfo->use_dss2_fck) {
		cinfo->clkin = dss_clk_get_rate(DSS_CLK_FCK2);
		/* XXX it is unclear if highfreq should be used
		 * with DSS2_FCK source also */
		cinfo->highfreq = 0;
	} else {
		cinfo->clkin = dispc_pclk_rate();

		if (cinfo->clkin < 32000000)
			cinfo->highfreq = 0;
		else
			cinfo->highfreq = 1;
	}

	cinfo->fint = cinfo->clkin / (cinfo->regn * (cinfo->highfreq ? 2 : 1));

	if (cinfo->fint > FINT_MAX || cinfo->fint < FINT_MIN)
		return -EINVAL;

	cinfo->clkin4ddr = 2 * cinfo->regm * cinfo->fint;

	if (cinfo->clkin4ddr > 1800 * 1000 * 1000)
		return -EINVAL;

	if (cinfo->regm3 > 0)
		cinfo->dsi1_pll_fclk = cinfo->clkin4ddr / cinfo->regm3;
	else
		cinfo->dsi1_pll_fclk = 0;

	if (cinfo->regm4 > 0)
		cinfo->dsi2_pll_fclk = cinfo->clkin4ddr / cinfo->regm4;
	else
		cinfo->dsi2_pll_fclk = 0;

	return 0;
}

int dsi_pll_calc_clock_div_pck(bool is_tft, unsigned long req_pck,
		struct dsi_clock_info *dsi_cinfo,
		struct dispc_clock_info *dispc_cinfo)
{
	struct dsi_clock_info cur, best;
	struct dispc_clock_info best_dispc;
	int min_fck_per_pck;
	int match = 0;
	unsigned long dss_clk_fck2;

	dss_clk_fck2 = dss_clk_get_rate(DSS_CLK_FCK2);

	if (req_pck == dsi.cache_req_pck &&
			dsi.cache_cinfo.clkin == dss_clk_fck2) {
		DSSDBG("DSI clock info found from cache\n");
		*dsi_cinfo = dsi.cache_cinfo;
		dispc_find_clk_divs(is_tft, req_pck, dsi_cinfo->dsi1_pll_fclk,
				dispc_cinfo);
		return 0;
	}

	min_fck_per_pck = CONFIG_OMAP2_DSS_MIN_FCK_PER_PCK;

	if (min_fck_per_pck &&
		req_pck * min_fck_per_pck > DISPC_MAX_FCK) {
		DSSERR("Requested pixel clock not possible with the current "
				"OMAP2_DSS_MIN_FCK_PER_PCK setting. Turning "
				"the constraint off.\n");
		min_fck_per_pck = 0;
	}

	DSSDBG("dsi_pll_calc\n");

retry:
	memset(&best, 0, sizeof(best));
	memset(&best_dispc, 0, sizeof(best_dispc));

	memset(&cur, 0, sizeof(cur));
	cur.clkin = dss_clk_fck2;
	cur.use_dss2_fck = 1;
	cur.highfreq = 0;

	/* no highfreq: 0.75MHz < Fint = clkin / regn < 2.1MHz */
	/* highfreq: 0.75MHz < Fint = clkin / (2*regn) < 2.1MHz */
	/* To reduce PLL lock time, keep Fint high (around 2 MHz) */
	for (cur.regn = 1; cur.regn < REGN_MAX; ++cur.regn) {
		if (cur.highfreq == 0)
			cur.fint = cur.clkin / cur.regn;
		else
			cur.fint = cur.clkin / (2 * cur.regn);

		if (cur.fint > FINT_MAX || cur.fint < FINT_MIN)
			continue;

		/* DSIPHY(MHz) = (2 * regm / regn) * (clkin / (highfreq + 1)) */
		for (cur.regm = 1; cur.regm < REGM_MAX; ++cur.regm) {
			unsigned long a, b;

			a = 2 * cur.regm * (cur.clkin/1000);
			b = cur.regn * (cur.highfreq + 1);
			cur.clkin4ddr = a / b * 1000;

			if (cur.clkin4ddr > 1800 * 1000 * 1000)
				break;

			/* DSI1_PLL_FCLK(MHz) = DSIPHY(MHz) / regm3  < 173MHz */
			for (cur.regm3 = 1; cur.regm3 < REGM3_MAX;
					++cur.regm3) {
				struct dispc_clock_info cur_dispc;
				cur.dsi1_pll_fclk = cur.clkin4ddr / cur.regm3;

				/* this will narrow down the search a bit,
				 * but still give pixclocks below what was
				 * requested */
				if (cur.dsi1_pll_fclk  < req_pck)
					break;

				if (cur.dsi1_pll_fclk > DISPC_MAX_FCK)
					continue;

				if (min_fck_per_pck &&
					cur.dsi1_pll_fclk <
						req_pck * min_fck_per_pck)
					continue;

				match = 1;

				dispc_find_clk_divs(is_tft, req_pck,
						cur.dsi1_pll_fclk,
						&cur_dispc);

				if (abs(cur_dispc.pck - req_pck) <
						abs(best_dispc.pck - req_pck)) {
					best = cur;
					best_dispc = cur_dispc;

					if (cur_dispc.pck == req_pck)
						goto found;
				}
			}
		}
	}
found:
	if (!match) {
		if (min_fck_per_pck) {
			DSSERR("Could not find suitable clock settings.\n"
					"Turning FCK/PCK constraint off and"
					"trying again.\n");
			min_fck_per_pck = 0;
			goto retry;
		}

		DSSERR("Could not find suitable clock settings.\n");

		return -EINVAL;
	}

	/* DSI2_PLL_FCLK (regm4) is not used */
	best.regm4 = 0;
	best.dsi2_pll_fclk = 0;

	if (dsi_cinfo)
		*dsi_cinfo = best;
	if (dispc_cinfo)
		*dispc_cinfo = best_dispc;

	dsi.cache_req_pck = req_pck;
	dsi.cache_clk_freq = 0;
	dsi.cache_cinfo = best;

	return 0;
}

int dsi_pll_set_clock_div(struct dsi_clock_info *cinfo)
{
	int r = 0;
	u32 l;
	int f;

	DSSDBGF();

	dsi.current_cinfo.fint = cinfo->fint;
	dsi.current_cinfo.clkin4ddr = cinfo->clkin4ddr;
	dsi.current_cinfo.dsi1_pll_fclk = cinfo->dsi1_pll_fclk;
	dsi.current_cinfo.dsi2_pll_fclk = cinfo->dsi2_pll_fclk;

	dsi.current_cinfo.regn = cinfo->regn;
	dsi.current_cinfo.regm = cinfo->regm;
	dsi.current_cinfo.regm3 = cinfo->regm3;
	dsi.current_cinfo.regm4 = cinfo->regm4;

	DSSDBG("DSI Fint %ld\n", cinfo->fint);

	DSSDBG("clkin (%s) rate %ld, highfreq %d\n",
			cinfo->use_dss2_fck ? "dss2_fck" : "pclkfree",
			cinfo->clkin,
			cinfo->highfreq);

	/* DSIPHY == CLKIN4DDR */
	DSSDBG("CLKIN4DDR = 2 * %d / %d * %lu / %d = %lu\n",
			cinfo->regm,
			cinfo->regn,
			cinfo->clkin,
			cinfo->highfreq + 1,
			cinfo->clkin4ddr);

	DSSDBG("Data rate on 1 DSI lane %ld Mbps\n",
			cinfo->clkin4ddr / 1000 / 1000 / 2);

	DSSDBG("Clock lane freq %ld Hz\n", cinfo->clkin4ddr / 4);

	DSSDBG("regm3 = %d, dsi1_pll_fclk = %lu\n",
			cinfo->regm3, cinfo->dsi1_pll_fclk);
	DSSDBG("regm4 = %d, dsi2_pll_fclk = %lu\n",
			cinfo->regm4, cinfo->dsi2_pll_fclk);

	REG_FLD_MOD(DSI_PLL_CONTROL, 0, 0, 0); /* DSI_PLL_AUTOMODE = manual */

	l = dsi_read_reg(DSI_PLL_CONFIGURATION1);
	l = FLD_MOD(l, 1, 0, 0);		/* DSI_PLL_STOPMODE */
	l = FLD_MOD(l, cinfo->regn - 1, 7, 1);	/* DSI_PLL_REGN */
	l = FLD_MOD(l, cinfo->regm, 18, 8);	/* DSI_PLL_REGM */
	l = FLD_MOD(l, cinfo->regm3 > 0 ? cinfo->regm3 - 1 : 0,
			22, 19);		/* DSI_CLOCK_DIV */
	l = FLD_MOD(l, cinfo->regm4 > 0 ? cinfo->regm4 - 1 : 0,
			26, 23);		/* DSIPROTO_CLOCK_DIV */
	dsi_write_reg(DSI_PLL_CONFIGURATION1, l);

	BUG_ON(cinfo->fint < FINT_MIN || cinfo->fint > FINT_MAX);
	if (cinfo->fint < 1000000)
		f = 0x3;
	else if (cinfo->fint < 1250000)
		f = 0x4;
	else if (cinfo->fint < 1500000)
		f = 0x5;
	else if (cinfo->fint < 1750000)
		f = 0x6;
	else
		f = 0x7;

	l = dsi_read_reg(DSI_PLL_CONFIGURATION2);
	l = FLD_MOD(l, f, 4, 1);		/* DSI_PLL_FREQSEL */
	l = FLD_MOD(l, cinfo->use_dss2_fck ? 0 : 1,
			11, 11);		/* DSI_PLL_CLKSEL */
	l = FLD_MOD(l, cinfo->highfreq,
			12, 12);		/* DSI_PLL_HIGHFREQ */
	l = FLD_MOD(l, 1, 13, 13);		/* DSI_PLL_REFEN */
	l = FLD_MOD(l, 0, 14, 14);		/* DSIPHY_CLKINEN */
	l = FLD_MOD(l, 1, 20, 20);		/* DSI_HSDIVBYPASS */
	dsi_write_reg(DSI_PLL_CONFIGURATION2, l);

	REG_FLD_MOD(DSI_PLL_GO, 1, 0, 0);	/* DSI_PLL_GO */

	if (wait_for_bit_change(DSI_PLL_GO, 0, 0) != 0) {
		DSSERR("dsi pll go bit not going down.\n");
		r = -EIO;
		goto err;
	}

	if (wait_for_bit_change(DSI_PLL_STATUS, 1, 1) != 1) {
		DSSERR("cannot lock PLL\n");
		r = -EIO;
		goto err;
	}

	dsi.pll_locked = 1;

	l = dsi_read_reg(DSI_PLL_CONFIGURATION2);
	l = FLD_MOD(l, 0, 0, 0);	/* DSI_PLL_IDLE */
	l = FLD_MOD(l, 0, 5, 5);	/* DSI_PLL_PLLLPMODE */
	l = FLD_MOD(l, 0, 6, 6);	/* DSI_PLL_LOWCURRSTBY */
	l = FLD_MOD(l, 0, 7, 7);	/* DSI_PLL_TIGHTPHASELOCK */
	l = FLD_MOD(l, 0, 8, 8);	/* DSI_PLL_DRIFTGUARDEN */
	l = FLD_MOD(l, 0, 10, 9);	/* DSI_PLL_LOCKSEL */
	l = FLD_MOD(l, 1, 13, 13);	/* DSI_PLL_REFEN */
	l = FLD_MOD(l, 1, 14, 14);	/* DSIPHY_CLKINEN */
	l = FLD_MOD(l, 0, 15, 15);	/* DSI_BYPASSEN */
	l = FLD_MOD(l, 1, 16, 16);	/* DSS_CLOCK_EN */
	l = FLD_MOD(l, 0, 17, 17);	/* DSS_CLOCK_PWDN */
	l = FLD_MOD(l, 1, 18, 18);	/* DSI_PROTO_CLOCK_EN */
	l = FLD_MOD(l, 0, 19, 19);	/* DSI_PROTO_CLOCK_PWDN */
	l = FLD_MOD(l, 0, 20, 20);	/* DSI_HSDIVBYPASS */
	dsi_write_reg(DSI_PLL_CONFIGURATION2, l);

	DSSDBG("PLL config done\n");
err:
	return r;
}

int dsi_pll_init(struct omap_dss_device *dssdev, bool enable_hsclk,
		bool enable_hsdiv)
{
	int r = 0;
	enum dsi_pll_power_state pwstate;

	DSSDBG("PLL init\n");

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	r = regulator_enable(dsi.vdds_dsi_reg);
	if (r)
		goto err0;

	 REG_FLD_MOD(DSI_CLK_CTRL, 0, 13, 13);   /* DDR_CLK_ALWAYS_ON */

	/* XXX PLL does not come out of reset without this... */
	dispc_pck_free_enable(1);

	if (wait_for_bit_change(DSI_PLL_STATUS, 0, 1) != 1) {
		DSSERR("PLL not coming out of reset.\n");
		r = -ENODEV;
		goto err1;
	}

	/* XXX ... but if left on, we get problems when planes do not
	 * fill the whole display. No idea about this */
	dispc_pck_free_enable(0);

	if (enable_hsclk && enable_hsdiv)
		pwstate = DSI_PLL_POWER_ON_ALL;
	else if (enable_hsclk)
		pwstate = DSI_PLL_POWER_ON_HSCLK;
	else if (enable_hsdiv)
		pwstate = DSI_PLL_POWER_ON_DIV;
	else
		pwstate = DSI_PLL_POWER_OFF;

	r = dsi_pll_power(pwstate);

	if (r)
		goto err1;

	enable_clocks(0);
	dsi_enable_pll_clock(0);

	DSSDBG("PLL init done\n");

	return 0;
err1:
	regulator_disable(dsi.vdds_dsi_reg);
err0:
	enable_clocks(0);
	dsi_enable_pll_clock(0);
	return r;
}

void dsi_pll_uninit(void)
{
	dsi.pll_locked = 0;
	dsi_pll_power(DSI_PLL_POWER_OFF);

	regulator_disable(dsi.vdds_dsi_reg);
	DSSDBG("PLL uninit done\n");
}

void dsi_dump_clocks(struct seq_file *s)
{
	int clksel;
	struct dsi_clock_info *cinfo = &dsi.current_cinfo;

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	clksel = REG_GET(DSI_PLL_CONFIGURATION2, 11, 11);

	seq_printf(s,	"- DSI PLL -\n");

	seq_printf(s,	"dsi pll source = %s\n",
			clksel == 0 ?
			"dss2_alwon_fclk" : "pclkfree");

	seq_printf(s,	"Fint\t\t%-16luregn %u\n", cinfo->fint, cinfo->regn);

	seq_printf(s,	"CLKIN4DDR\t%-16luregm %u\n",
			cinfo->clkin4ddr, cinfo->regm);

	seq_printf(s,	"dsi1_pll_fck\t%-16luregm3 %u\t(%s)\n",
			cinfo->dsi1_pll_fclk,
			cinfo->regm3,
			dss_get_dispc_clk_source() == DSS_SRC_DSS1_ALWON_FCLK ?
			"off" : "on");

	seq_printf(s,	"dsi2_pll_fck\t%-16luregm4 %u\t(%s)\n",
			cinfo->dsi2_pll_fclk,
			cinfo->regm4,
			dss_get_dsi_clk_source() == DSS_SRC_DSS1_ALWON_FCLK ?
			"off" : "on");

	seq_printf(s,	"- DSI -\n");

	seq_printf(s,	"dsi fclk source = %s\n",
			dss_get_dsi_clk_source() == DSS_SRC_DSS1_ALWON_FCLK ?
			"dss1_alwon_fclk" : "dsi2_pll_fclk");

	seq_printf(s,	"DSI_FCLK\t%lu\n", dsi_fclk_rate());

	seq_printf(s,	"DDR_CLK\t\t%lu\n",
			cinfo->clkin4ddr / 4);

	seq_printf(s,	"TxByteClkHS\t%lu\n", dsi_get_txbyteclkhs());

	seq_printf(s,	"LP_CLK\t\t%lu\n", cinfo->lp_clk);

	seq_printf(s,	"VP_CLK\t\t%lu\n"
			"VP_PCLK\t\t%lu\n",
			dispc_lclk_rate(),
			dispc_pclk_rate());

	enable_clocks(0);
	dsi_enable_pll_clock(0);
}

void dsi_dump_regs(struct seq_file *s)
{
#define DUMPREG(r) seq_printf(s, "%-35s %08x\n", #r, dsi_read_reg(r))

	dss_clk_enable(DSS_CLK_ICK | DSS_CLK_FCK1);
	dsi_enable_pll_clock(1);

	DUMPREG(DSI_REVISION);
	DUMPREG(DSI_SYSCONFIG);
	DUMPREG(DSI_SYSSTATUS);
	DUMPREG(DSI_IRQSTATUS);
	DUMPREG(DSI_IRQENABLE);
	DUMPREG(DSI_CTRL);
	DUMPREG(DSI_COMPLEXIO_CFG1);
	DUMPREG(DSI_COMPLEXIO_IRQ_STATUS);
	DUMPREG(DSI_COMPLEXIO_IRQ_ENABLE);
	DUMPREG(DSI_CLK_CTRL);
	DUMPREG(DSI_TIMING1);
	DUMPREG(DSI_TIMING2);
	DUMPREG(DSI_VM_TIMING1);
	DUMPREG(DSI_VM_TIMING2);
	DUMPREG(DSI_VM_TIMING3);
	DUMPREG(DSI_CLK_TIMING);
	DUMPREG(DSI_TX_FIFO_VC_SIZE);
	DUMPREG(DSI_RX_FIFO_VC_SIZE);
	DUMPREG(DSI_COMPLEXIO_CFG2);
	DUMPREG(DSI_RX_FIFO_VC_FULLNESS);
	DUMPREG(DSI_VM_TIMING4);
	DUMPREG(DSI_TX_FIFO_VC_EMPTINESS);
	DUMPREG(DSI_VM_TIMING5);
	DUMPREG(DSI_VM_TIMING6);
	DUMPREG(DSI_VM_TIMING7);
	DUMPREG(DSI_STOPCLK_TIMING);

	DUMPREG(DSI_VC_CTRL(0));
	DUMPREG(DSI_VC_TE(0));
	DUMPREG(DSI_VC_LONG_PACKET_HEADER(0));
	DUMPREG(DSI_VC_LONG_PACKET_PAYLOAD(0));
	DUMPREG(DSI_VC_SHORT_PACKET_HEADER(0));
	DUMPREG(DSI_VC_IRQSTATUS(0));
	DUMPREG(DSI_VC_IRQENABLE(0));

	DUMPREG(DSI_VC_CTRL(1));
	DUMPREG(DSI_VC_TE(1));
	DUMPREG(DSI_VC_LONG_PACKET_HEADER(1));
	DUMPREG(DSI_VC_LONG_PACKET_PAYLOAD(1));
	DUMPREG(DSI_VC_SHORT_PACKET_HEADER(1));
	DUMPREG(DSI_VC_IRQSTATUS(1));
	DUMPREG(DSI_VC_IRQENABLE(1));

	DUMPREG(DSI_VC_CTRL(2));
	DUMPREG(DSI_VC_TE(2));
	DUMPREG(DSI_VC_LONG_PACKET_HEADER(2));
	DUMPREG(DSI_VC_LONG_PACKET_PAYLOAD(2));
	DUMPREG(DSI_VC_SHORT_PACKET_HEADER(2));
	DUMPREG(DSI_VC_IRQSTATUS(2));
	DUMPREG(DSI_VC_IRQENABLE(2));

	DUMPREG(DSI_VC_CTRL(3));
	DUMPREG(DSI_VC_TE(3));
	DUMPREG(DSI_VC_LONG_PACKET_HEADER(3));
	DUMPREG(DSI_VC_LONG_PACKET_PAYLOAD(3));
	DUMPREG(DSI_VC_SHORT_PACKET_HEADER(3));
	DUMPREG(DSI_VC_IRQSTATUS(3));
	DUMPREG(DSI_VC_IRQENABLE(3));

	DUMPREG(DSI_DSIPHY_CFG0);
	DUMPREG(DSI_DSIPHY_CFG1);
	DUMPREG(DSI_DSIPHY_CFG2);
	DUMPREG(DSI_DSIPHY_CFG5);

	DUMPREG(DSI_PLL_CONTROL);
	DUMPREG(DSI_PLL_STATUS);
	DUMPREG(DSI_PLL_GO);
	DUMPREG(DSI_PLL_CONFIGURATION1);
	DUMPREG(DSI_PLL_CONFIGURATION2);

	dss_clk_disable(DSS_CLK_ICK | DSS_CLK_FCK1);
	dsi_enable_pll_clock(0);
#undef DUMPREG
}

enum dsi_complexio_power_state {
	DSI_COMPLEXIO_POWER_OFF		= 0x0,
	DSI_COMPLEXIO_POWER_ON		= 0x1,
	DSI_COMPLEXIO_POWER_ULPS	= 0x2,
};

static int dsi_complexio_power(enum dsi_complexio_power_state state)
{
	int t = 0;

	/* PWR_CMD */
	REG_FLD_MOD(DSI_COMPLEXIO_CFG1, state, 28, 27);

	/* PWR_STATUS */
	while (FLD_GET(dsi_read_reg(DSI_COMPLEXIO_CFG1), 26, 25) != state) {
		udelay(1);
		if (t++ > 1000) {
			DSSERR("failed to set complexio power state to "
					"%d\n", state);
			return -ENODEV;
		}
	}

	return 0;
}

static void dsi_complexio_config(struct omap_dss_device *dssdev)
{
	u32 r;

	int clk_lane   = dssdev->phy.dsi.clk_lane;
	int data1_lane = dssdev->phy.dsi.data1_lane;
	int data2_lane = dssdev->phy.dsi.data2_lane;
	int clk_pol    = dssdev->phy.dsi.clk_pol;
	int data1_pol  = dssdev->phy.dsi.data1_pol;
	int data2_pol  = dssdev->phy.dsi.data2_pol;

	r = dsi_read_reg(DSI_COMPLEXIO_CFG1);
	r = FLD_MOD(r, clk_lane, 2, 0);
	r = FLD_MOD(r, clk_pol, 3, 3);
	r = FLD_MOD(r, data1_lane, 6, 4);
	r = FLD_MOD(r, data1_pol, 7, 7);
	r = FLD_MOD(r, data2_lane, 10, 8);
	r = FLD_MOD(r, data2_pol, 11, 11);
	dsi_write_reg(DSI_COMPLEXIO_CFG1, r);

	/* The configuration of the DSI complex I/O (number of data lanes,
	   position, differential order) should not be changed while
	   DSS.DSI_CLK_CRTRL[20] LP_CLK_ENABLE bit is set to 1. In order for
	   the hardware to take into account a new configuration of the complex
	   I/O (done in DSS.DSI_COMPLEXIO_CFG1 register), it is recommended to
	   follow this sequence: First set the DSS.DSI_CTRL[0] IF_EN bit to 1,
	   then reset the DSS.DSI_CTRL[0] IF_EN to 0, then set
	   DSS.DSI_CLK_CTRL[20] LP_CLK_ENABLE to 1 and finally set again the
	   DSS.DSI_CTRL[0] IF_EN bit to 1. If the sequence is not followed, the
	   DSI complex I/O configuration is unknown. */

	/*
	REG_FLD_MOD(DSI_CTRL, 1, 0, 0);
	REG_FLD_MOD(DSI_CTRL, 0, 0, 0);
	REG_FLD_MOD(DSI_CLK_CTRL, 1, 20, 20);
	REG_FLD_MOD(DSI_CTRL, 1, 0, 0);
	*/
}

static inline unsigned ns2ddr(unsigned ns)
{
	/* convert time in ns to ddr ticks, rounding up */
	unsigned long ddr_clk = dsi.current_cinfo.clkin4ddr / 4;
	return (ns * (ddr_clk / 1000 / 1000) + 999) / 1000;
}

static inline unsigned ddr2ns(unsigned ddr)
{
	unsigned long ddr_clk = dsi.current_cinfo.clkin4ddr / 4;
	return ddr * 1000 * 1000 / (ddr_clk / 1000);
}

static void dsi_complexio_timings(struct omap_dss_device *dssdev)
{
	u32 r;
	u32 ths_prepare, ths_prepare_ths_zero, ths_trail, ths_exit;
	u32 tlpx_half, tclk_trail, tclk_zero;
	u32 tclk_prepare;

	/* calculate timings */
	if (dssdev->driver->hs_mode_timing) {
		dssdev->driver->hs_mode_timing(dssdev);
		ths_prepare = ns2ddr(dssdev->phy.dsi.hs_timing.ths_prepare) + 2;
		ths_prepare_ths_zero =
			ns2ddr(dssdev->phy.dsi.hs_timing.ths_prepare_ths_zero) +
			2;
		ths_trail = ns2ddr(dssdev->phy.dsi.hs_timing.ths_trail) + 5;
		ths_exit = ns2ddr(dssdev->phy.dsi.hs_timing.ths_exit);
		tlpx_half = ns2ddr(dssdev->phy.dsi.hs_timing.tlpx_half);
		tclk_trail = ns2ddr(dssdev->phy.dsi.hs_timing.tclk_trail) + 2;
		tclk_prepare = ns2ddr(dssdev->phy.dsi.hs_timing.tclk_prepare);
		tclk_zero = ns2ddr(dssdev->phy.dsi.hs_timing.tclk_zero);
	} else {
		/* 1 * DDR_CLK = 2 * UI */

		/* min 40ns + 4*UI	max 85ns + 6*UI */
		ths_prepare = ns2ddr(70) + 2;

		/* min 145ns + 10*UI */
		ths_prepare_ths_zero = ns2ddr(175) + 2;

		/* min max(8*UI, 60ns+4*UI) */
		ths_trail = ns2ddr(60) + 5;

		/* min 100ns */
		ths_exit = ns2ddr(145);

		/* tlpx min 50n */
		tlpx_half = ns2ddr(25);

		/* min 60ns */
		tclk_trail = ns2ddr(60) + 2;

		/* min 38ns, max 95ns */
		tclk_prepare = ns2ddr(65);

		/* min tclk-prepare + tclk-zero = 300ns */
		tclk_zero = ns2ddr(260);

	}
	DSSDBG("ths_prepare %u (%uns), ths_prepare_ths_zero %u (%uns)\n",
		ths_prepare, ddr2ns(ths_prepare),
		ths_prepare_ths_zero, ddr2ns(ths_prepare_ths_zero));

	DSSDBG("ths_trail %u (%uns), ths_exit %u (%uns)\n",
			ths_trail, ddr2ns(ths_trail),
			ths_exit, ddr2ns(ths_exit));

	DSSDBG("tlpx_half %u (%uns), tclk_trail %u (%uns), "
			"tclk_zero %u (%uns)\n",
			tlpx_half, ddr2ns(tlpx_half),
			tclk_trail, ddr2ns(tclk_trail),
			tclk_zero, ddr2ns(tclk_zero));
	DSSDBG("tclk_prepare %u (%uns)\n",
			tclk_prepare, ddr2ns(tclk_prepare));

	/* program timings */

	r = dsi_read_reg(DSI_DSIPHY_CFG0);
	r = FLD_MOD(r, ths_prepare, 31, 24);
	r = FLD_MOD(r, ths_prepare_ths_zero, 23, 16);
	r = FLD_MOD(r, ths_trail, 15, 8);
	r = FLD_MOD(r, ths_exit, 7, 0);
	dsi_write_reg(DSI_DSIPHY_CFG0, r);

	r = dsi_read_reg(DSI_DSIPHY_CFG1);
	r = FLD_MOD(r, tlpx_half, 22, 16);
	r = FLD_MOD(r, tclk_trail, 15, 8);
	r = FLD_MOD(r, tclk_zero, 7, 0);
	dsi_write_reg(DSI_DSIPHY_CFG1, r);

	r = dsi_read_reg(DSI_DSIPHY_CFG2);
	r = FLD_MOD(r, tclk_prepare, 7, 0);
	dsi_write_reg(DSI_DSIPHY_CFG2, r);
}


static int dsi_complexio_init(struct omap_dss_device *dssdev)
{
	int r = 0;

	DSSDBG("dsi_complexio_init\n");

	/* CIO_CLK_ICG, enable L3 clk to CIO */
	REG_FLD_MOD(DSI_CLK_CTRL, 1, 14, 14);

	/* A dummy read using the SCP interface to any DSIPHY register is
	 * required after DSIPHY reset to complete the reset of the DSI complex
	 * I/O. */
	dsi_read_reg(DSI_DSIPHY_CFG5);

	if (wait_for_bit_change(DSI_DSIPHY_CFG5, 30, 1) != 1) {
		DSSERR("ComplexIO PHY not coming out of reset.\n");
		r = -ENODEV;
		goto err;
	}

	dsi_complexio_config(dssdev);

	r = dsi_complexio_power(DSI_COMPLEXIO_POWER_ON);

	if (r)
		goto err;

	if (wait_for_bit_change(DSI_COMPLEXIO_CFG1, 29, 1) != 1) {
		DSSERR("ComplexIO not coming out of reset.\n");
		r = -ENODEV;
		goto err;
	}

	if (wait_for_bit_change(DSI_COMPLEXIO_CFG1, 21, 1) != 1) {
		DSSERR("ComplexIO LDO power down.\n");
		r = -ENODEV;
		goto err;
	}

	dsi_complexio_timings(dssdev);

	/*
	   The configuration of the DSI complex I/O (number of data lanes,
	   position, differential order) should not be changed while
	   DSS.DSI_CLK_CRTRL[20] LP_CLK_ENABLE bit is set to 1. For the
	   hardware to recognize a new configuration of the complex I/O (done
	   in DSS.DSI_COMPLEXIO_CFG1 register), it is recommended to follow
	   this sequence: First set the DSS.DSI_CTRL[0] IF_EN bit to 1, next
	   reset the DSS.DSI_CTRL[0] IF_EN to 0, then set DSS.DSI_CLK_CTRL[20]
	   LP_CLK_ENABLE to 1, and finally, set again the DSS.DSI_CTRL[0] IF_EN
	   bit to 1. If the sequence is not followed, the DSi complex I/O
	   configuration is undetermined.
	   */
	dsi_if_enable(1);
	dsi_if_enable(0);
	REG_FLD_MOD(DSI_CLK_CTRL, 1, 20, 20); /* LP_CLK_ENABLE */
	dsi_if_enable(1);
	dsi_if_enable(0);

	DSSDBG("CIO init done\n");
err:
	return r;
}

static void dsi_complexio_uninit(void)
{
	dsi_complexio_power(DSI_COMPLEXIO_POWER_OFF);
}

static int _dsi_wait_reset(void)
{
	int i = 0;

	while (REG_GET(DSI_SYSSTATUS, 0, 0) == 0) {
		if (i++ > 5) {
			DSSERR("soft reset failed\n");
			return -ENODEV;
		}
		udelay(1);
	}

	return 0;
}

static int _dsi_reset(void)
{
	/* Soft reset */
	REG_FLD_MOD(DSI_SYSCONFIG, 1, 1, 1);
	return _dsi_wait_reset();
}

static void dsi_reset_tx_fifo(int channel)
{
	u32 mask;
	u32 l;

	/* set fifosize of the channel to 0, then return the old size */
	l = dsi_read_reg(DSI_TX_FIFO_VC_SIZE);

	mask = FLD_MASK((8 * channel) + 7, (8 * channel) + 4);
	dsi_write_reg(DSI_TX_FIFO_VC_SIZE, l & ~mask);

	dsi_write_reg(DSI_TX_FIFO_VC_SIZE, l);
}

static void dsi_config_tx_fifo(enum fifo_size size1, enum fifo_size size2,
		enum fifo_size size3, enum fifo_size size4)
{
	u32 r = 0;
	int add = 0;
	int i;

	dsi.vc[0].fifo_size = size1;
	dsi.vc[1].fifo_size = size2;
	dsi.vc[2].fifo_size = size3;
	dsi.vc[3].fifo_size = size4;

	for (i = 0; i < 4; i++) {
		u8 v;
		int size = dsi.vc[i].fifo_size;

		if (add + size > 4) {
			DSSERR("Illegal FIFO configuration\n");
			BUG();
		}

		v = FLD_VAL(add, 2, 0) | FLD_VAL(size, 7, 4);
		r |= v << (8 * i);
		/*DSSDBG("TX FIFO vc %d: size %d, add %d\n", i, size, add); */
		add += size;
	}

	dsi_write_reg(DSI_TX_FIFO_VC_SIZE, r);
}

static void dsi_config_rx_fifo(enum fifo_size size1, enum fifo_size size2,
		enum fifo_size size3, enum fifo_size size4)
{
	u32 r = 0;
	int add = 0;
	int i;

	dsi.vc[0].fifo_size = size1;
	dsi.vc[1].fifo_size = size2;
	dsi.vc[2].fifo_size = size3;
	dsi.vc[3].fifo_size = size4;

	for (i = 0; i < 4; i++) {
		u8 v;
		int size = dsi.vc[i].fifo_size;

		if (add + size > 4) {
			DSSERR("Illegal FIFO configuration\n");
			BUG();
		}

		v = FLD_VAL(add, 2, 0) | FLD_VAL(size, 7, 4);
		r |= v << (8 * i);
		/*DSSDBG("RX FIFO vc %d: size %d, add %d\n", i, size, add); */
		add += size;
	}

	dsi_write_reg(DSI_RX_FIFO_VC_SIZE, r);
}

static int dsi_force_tx_stop_mode_io(void)
{
	u32 r;

	r = dsi_read_reg(DSI_TIMING1);
	r = FLD_MOD(r, 1, 15, 15);	/* FORCE_TX_STOP_MODE_IO */
	dsi_write_reg(DSI_TIMING1, r);

	if (wait_for_bit_change(DSI_TIMING1, 15, 0) != 0) {
		DSSERR("TX_STOP bit not going down\n");
		return -EIO;
	}

	return 0;
}

static void dsi_vc_print_status(int channel)
{
	u32 r;

	r = dsi_read_reg(DSI_VC_CTRL(channel));
	DSSDBG("vc %d: TX_FIFO_NOT_EMPTY %d, BTA_EN %d, VC_BUSY %d, "
			"TX_FIFO_FULL %d, RX_FIFO_NOT_EMPTY %d, ",
			channel,
			FLD_GET(r, 5, 5),
			FLD_GET(r, 6, 6),
			FLD_GET(r, 15, 15),
			FLD_GET(r, 16, 16),
			FLD_GET(r, 20, 20));

	r = dsi_read_reg(DSI_TX_FIFO_VC_EMPTINESS);
	DSSDBG("EMPTINESS %d\n", (r >> (8 * channel)) & 0xff);
}

static int dsi_vc_enable(int channel, bool enable)
{
	if (dsi.update_mode != OMAP_DSS_UPDATE_AUTO)
		DSSDBG("dsi_vc_enable channel %d, enable %d\n",
				channel, enable);

	enable = enable ? 1 : 0;

	REG_FLD_MOD(DSI_VC_CTRL(channel), enable, 0, 0);

	if (wait_for_bit_change(DSI_VC_CTRL(channel), 0, enable) != enable) {
			DSSERR("Failed to set dsi_vc_enable to %d\n", enable);
			return -EIO;
	}

	return 0;
}

static void dsi_vc_initial_config(int channel)
{
	u32 r;

	DSSDBGF("%d", channel);

	r = dsi_read_reg(DSI_VC_CTRL(channel));

	if (FLD_GET(r, 15, 15)) /* VC_BUSY */
		DSSERR("VC(%d) busy when trying to configure it!\n",
				channel);

	r = FLD_MOD(r, 0, 1, 1); /* SOURCE, 0 = L4 */
	r = FLD_MOD(r, 0, 2, 2); /* BTA_SHORT_EN  */
	r = FLD_MOD(r, 0, 3, 3); /* BTA_LONG_EN */
	r = FLD_MOD(r, 0, 4, 4); /* MODE, 0 = command */
	r = FLD_MOD(r, 1, 7, 7); /* CS_TX_EN */
	r = FLD_MOD(r, 1, 8, 8); /* ECC_TX_EN */
	r = FLD_MOD(r, 0, 9, 9); /* MODE_SPEED, high speed on/off */

	r = FLD_MOD(r, 4, 29, 27); /* DMA_RX_REQ_NB = no dma */
	r = FLD_MOD(r, 4, 23, 21); /* DMA_TX_REQ_NB = no dma */

	dsi_write_reg(DSI_VC_CTRL(channel), r);

	dsi.vc[channel].mode = DSI_VC_MODE_L4;
}

static void dsi_vc_config_l4(int channel)
{
	if (dsi.vc[channel].mode == DSI_VC_MODE_L4)
		return;

	DSSDBGF("%d", channel);

	dsi_vc_enable(channel, 0);

	if (REG_GET(DSI_VC_CTRL(channel), 15, 15)) /* VC_BUSY */
		DSSERR("vc(%d) busy when trying to config for L4\n", channel);

	REG_FLD_MOD(DSI_VC_CTRL(channel), 0, 1, 1); /* SOURCE, 0 = L4 */

	dsi_vc_enable(channel, 1);

	dsi.vc[channel].mode = DSI_VC_MODE_L4;
}

static void dsi_vc_config_vp(int channel)
{
	if (dsi.vc[channel].mode == DSI_VC_MODE_VP)
		return;

	DSSDBGF("%d", channel);

	dsi_vc_enable(channel, 0);

	if (REG_GET(DSI_VC_CTRL(channel), 15, 15)) /* VC_BUSY */
		DSSERR("vc(%d) busy when trying to config for VP\n", channel);

	REG_FLD_MOD(DSI_VC_CTRL(channel), 1, 1, 1); /* SOURCE, 1 = video port */
	REG_FLD_MOD(DSI_VC_CTRL(channel), 1, 9, 9); /* MODE_SPEED 1 = HS */

	dsi_vc_enable(channel, 1);

	dsi.vc[channel].mode = DSI_VC_MODE_VP;
}

void dsi_vc_enable_hs(int channel, bool enable)
{
	DSSDBG("dsi_vc_enable_hs(%d, %d)\n", channel, enable);

	dsi_vc_enable(channel, 0);
	dsi_if_enable(0);

	REG_FLD_MOD(DSI_VC_CTRL(channel), enable, 9, 9);

	dsi_vc_enable(channel, 1);
	dsi_if_enable(1);

	dsi_force_tx_stop_mode_io();
}

void dsi_disable_vid_vc_enable_cmd_vc(void)
{
	dsi_if_enable(0);
	dsi_vc_enable(DSI_VIDEO_VC, 0);
	dsi_vc_enable(DSI_CMD_VC, 1);
	dsi_if_enable(1);
	dsi_force_tx_stop_mode_io();
}

void dsi_disable_cmd_vc_enable_vid_vc(void)
{
	dsi_if_enable(0);
	dsi_vc_enable(DSI_CMD_VC, 0);
	dsi_vc_enable_hs(DSI_VIDEO_VC, 1);
	dsi_if_enable(1);
	dsi_force_tx_stop_mode_io();
}

static void dsi_enable_sidle(void)
{
	REG_FLD_MOD(DSI_SYSCONFIG, 2, 4, 3); /* SIDLEMODE: smart idle */
}

static void dsi_disable_sidle(void)
{
	REG_FLD_MOD(DSI_SYSCONFIG, 1, 4, 3); /* SIDLEMODE: no idle */
}

static void dsi_vc_flush_long_data(int channel)
{
	while (REG_GET(DSI_VC_CTRL(channel), 20, 20)) {
		u32 val;
		val = dsi_read_reg(DSI_VC_SHORT_PACKET_HEADER(channel));
		DSSDBG("\t\tb1 %#02x b2 %#02x b3 %#02x b4 %#02x\n",
				(val >> 0) & 0xff,
				(val >> 8) & 0xff,
				(val >> 16) & 0xff,
				(val >> 24) & 0xff);
	}
}

static void dsi_show_rx_ack_with_err(u16 err)
{
	DSSERR("\tACK with ERROR (%#x):\n", err);
	if (err & (1 << 0))
		DSSERR("\t\tSoT Error\n");
	if (err & (1 << 1))
		DSSERR("\t\tSoT Sync Error\n");
	if (err & (1 << 2))
		DSSERR("\t\tEoT Sync Error\n");
	if (err & (1 << 3))
		DSSERR("\t\tEscape Mode Entry Command Error\n");
	if (err & (1 << 4))
		DSSERR("\t\tLP Transmit Sync Error\n");
	if (err & (1 << 5))
		DSSERR("\t\tHS Receive Timeout Error\n");
	if (err & (1 << 6))
		DSSERR("\t\tFalse Control Error\n");
	if (err & (1 << 7))
		DSSERR("\t\t(reserved7)\n");
	if (err & (1 << 8))
		DSSERR("\t\tECC Error, single-bit (corrected)\n");
	if (err & (1 << 9))
		DSSERR("\t\tECC Error, multi-bit (not corrected)\n");
	if (err & (1 << 10))
		DSSERR("\t\tChecksum Error\n");
	if (err & (1 << 11))
		DSSERR("\t\tData type not recognized\n");
	if (err & (1 << 12))
		DSSERR("\t\tInvalid VC ID\n");
	if (err & (1 << 13))
		DSSERR("\t\tInvalid Transmission Length\n");
	if (err & (1 << 14))
		DSSERR("\t\t(reserved14)\n");
	if (err & (1 << 15))
		DSSERR("\t\tDSI Protocol Violation\n");
}

static u16 dsi_vc_flush_receive_data(int channel)
{
	/* RX_FIFO_NOT_EMPTY */
	while (REG_GET(DSI_VC_CTRL(channel), 20, 20)) {
		u32 val;
		u8 dt;
		val = dsi_read_reg(DSI_VC_SHORT_PACKET_HEADER(channel));
		DSSDBG("\trawval %#08x\n", val);
		dt = FLD_GET(val, 5, 0);
		if (dt == DSI_DT_RX_ACK_WITH_ERR) {
			u16 err = FLD_GET(val, 23, 8);
			dsi_show_rx_ack_with_err(err);
		} else if (dt == DSI_DT_RX_SHORT_READ_1) {
			DSSDBG("\tDCS short response, 1 byte: %#x\n",
					FLD_GET(val, 23, 8));
		} else if (dt == DSI_DT_RX_SHORT_READ_2) {
			DSSDBG("\tDCS short response, 2 byte: %#x\n",
					FLD_GET(val, 23, 8));
		} else if (dt == DSI_DT_RX_DCS_LONG_READ) {
			DSSDBG("\tDCS long response, len %d\n",
					FLD_GET(val, 23, 8));
			dsi_vc_flush_long_data(channel);
		} else {
			DSSERR("\tunknown datatype 0x%02x\n", dt);
		}
	}
	return 0;
}

static int dsi_vc_send_bta(int channel)
{
	if (dsi.update_mode != OMAP_DSS_UPDATE_AUTO &&
			(dsi.debug_write || dsi.debug_read))
		DSSDBG("dsi_vc_send_bta %d\n", channel);

	WARN_ON(!dsi_bus_is_locked());

	if (REG_GET(DSI_VC_CTRL(channel), 20, 20)) {	/* RX_FIFO_NOT_EMPTY */
		DSSERR("rx fifo not empty when sending BTA, dumping data:\n");
		dsi_vc_flush_receive_data(channel);
	}

	REG_FLD_MOD(DSI_VC_CTRL(channel), 1, 6, 6); /* BTA_EN */

	return 0;
}

int dsi_vc_send_bta_sync(int channel)
{
	int r = 0;
	u32 err;

	INIT_COMPLETION(dsi.bta_completion);

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	dsi_vc_enable_bta_irq(channel);

	/* After the BTA is sending out and before the ACK is getting back,
	 * the DSI might get into the IDLDE mode. If the DSI is in IDLE
	 * and it can miss the ACK coming back from the Display */
	dsi_disable_sidle();

	r = dsi_vc_send_bta(channel);
	if (r)
		goto err;

	if (wait_for_completion_timeout(&dsi.bta_completion,
				msecs_to_jiffies(500)) == 0) {
		DSSERR("Failed to receive BTA\n");
		r = -EIO;
		goto err;
	}

	err = dsi_get_errors();
	if (err) {
		DSSERR("Error while sending BTA: %x\n", err);
		r = -EIO;
		goto err;
	}
err:
	dsi_vc_disable_bta_irq(channel);
	dsi_enable_sidle();

	enable_clocks(0);
	dsi_enable_pll_clock(0);

	return r;
}
EXPORT_SYMBOL(dsi_vc_send_bta_sync);

static inline void dsi_vc_write_long_header(int channel, u8 data_type,
		u16 len, u8 ecc)
{
	u32 val;
	u8 data_id;

	WARN_ON(!dsi_bus_is_locked());

	/*data_id = data_type | channel << 6; */
	data_id = data_type | dsi.vc[channel].dest_per << 6;

	val = FLD_VAL(data_id, 7, 0) | FLD_VAL(len, 23, 8) |
		FLD_VAL(ecc, 31, 24);

	dsi_write_reg(DSI_VC_LONG_PACKET_HEADER(channel), val);
}

static inline void dsi_vc_write_long_payload(int channel,
		u8 b1, u8 b2, u8 b3, u8 b4)
{
	u32 val;

	val = b4 << 24 | b3 << 16 | b2 << 8  | b1 << 0;

/*	DSSDBG("\twriting %02x, %02x, %02x, %02x (%#010x)\n",
			b1, b2, b3, b4, val); */

	dsi_write_reg(DSI_VC_LONG_PACKET_PAYLOAD(channel), val);
}

static int dsi_vc_send_long(int channel, u8 data_type, u8 *data, u16 len,
		u8 ecc)
{
	/*u32 val; */
	int i;
	u8 *p;
	int r = 0;
	u8 b1, b2, b3, b4;

	if (dsi.debug_write)
		DSSDBG("dsi_vc_send_long, %d bytes\n", len);

	/* len + header */
	if (dsi.vc[channel].fifo_size * 32 * 4 < len + 4) {
		DSSERR("unable to send long packet: packet too long.\n");
		return -EINVAL;
	}

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	INIT_COMPLETION(dsi.packet_sent_completion);
	dsi_vc_enable_packet_sent_irq(channel);

	dsi_vc_config_l4(channel);

	dsi_vc_write_long_header(channel, data_type, len, ecc);

	/*dsi_vc_print_status(0); */

	p = data;
	for (i = 0; i < len >> 2; i++) {
		if (dsi.debug_write)
			DSSDBG("\tsending full packet %d\n", i);
		/*dsi_vc_print_status(0); */

		b1 = *p++;
		b2 = *p++;
		b3 = *p++;
		b4 = *p++;

		dsi_vc_write_long_payload(channel, b1, b2, b3, b4);
	}

	i = len % 4;
	if (i) {
		b1 = 0; b2 = 0; b3 = 0;

		if (dsi.debug_write)
			DSSDBG("\tsending remainder bytes %d\n", i);

		switch (i) {
		case 3:
			b1 = *p++;
			b2 = *p++;
			b3 = *p++;
			break;
		case 2:
			b1 = *p++;
			b2 = *p++;
			break;
		case 1:
			b1 = *p++;
			break;
		}

		dsi_vc_write_long_payload(channel, b1, b2, b3, 0);
	}

	if (wait_for_completion_timeout(&dsi.packet_sent_completion,
					msecs_to_jiffies(10)) == 0)
		DSSERR("Failed to send long packet\n");

	dsi_vc_disable_packet_sent_irq(channel);

	enable_clocks(0);
	dsi_enable_pll_clock(0);

	return r;
}

static int dsi_vc_send_short(int channel, u8 data_type, u16 data, u8 ecc)
{
	u32 r;
	u8 data_id;

	WARN_ON(!dsi_bus_is_locked());

	if (dsi.debug_write)
		DSSDBG("dsi_vc_send_short(ch%d, dt %#x, b1 %#x, b2 %#x)\n",
				channel,
				data_type, data & 0xff, (data >> 8) & 0xff);

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	dsi_vc_config_l4(channel);

	if (FLD_GET(dsi_read_reg(DSI_VC_CTRL(channel)), 16, 16)) {
		DSSERR("ERROR FIFO FULL, aborting transfer\n");
		return -EINVAL;
	}

	INIT_COMPLETION(dsi.packet_sent_completion);
	dsi_vc_enable_packet_sent_irq(channel);


	data_id = data_type | dsi.vc[channel].dest_per << 6;

	r = (data_id << 0) | (data << 8) | (ecc << 24);

	dsi_write_reg(DSI_VC_SHORT_PACKET_HEADER(channel), r);

	if (wait_for_completion_timeout(&dsi.packet_sent_completion,
						msecs_to_jiffies(10)) == 0)
		DSSERR("Failed to send short packet\n");

	dsi_vc_disable_packet_sent_irq(channel);


	enable_clocks(0);
	dsi_enable_pll_clock(0);

	return 0;
}

int dsi_vc_send_null(int channel)
{
	u8 nullpkg[] = {0, 0, 0, 0};
	return dsi_vc_send_long(channel, DSI_DT_NULL_PACKET, nullpkg, 4, 0);
}
EXPORT_SYMBOL(dsi_vc_send_null);

int dsi_vc_write_nosync(int channel, u8 data_type, u8 *data, int len)
{
	int r;
	BUG_ON(len == 0);

	if (len == 1)
		r = dsi_vc_send_short(channel, data_type, data[0], 0);
	else if (len == 2)
		r = dsi_vc_send_short(channel, data_type,
				data[0] | (data[1] << 8), 0);
	else
		r = dsi_vc_send_long(channel, data_type, data, len, 0);

	return r;
}
EXPORT_SYMBOL(dsi_vc_write_nosync);

int dsi_vc_write(int channel, u8 data_type, u8 *data, int len)
{
	int r;

	r = dsi_vc_write_nosync(channel, data_type, data, len);
	if (r)
		return r;
	r = dsi_vc_send_bta_sync(channel);

	return r;

}
EXPORT_SYMBOL(dsi_vc_write);

int dsi_vc_dcs_write_nosync(int channel, u8 *data, int len)
{
	u8 data_type;

	BUG_ON(len == 0);

	if (len == 1)
		data_type = DSI_DT_DCS_SHORT_WRITE_0;
	else if (len == 2)
		data_type = DSI_DT_DCS_SHORT_WRITE_1;
	else
		data_type = DSI_DT_DCS_LONG_WRITE;

	return dsi_vc_write_nosync(channel, data_type, data, len);
}
EXPORT_SYMBOL(dsi_vc_dcs_write_nosync);

int dsi_vc_dcs_write(int channel, u8 *data, int len)
{
	int r;

	r = dsi_vc_dcs_write_nosync(channel, data, len);
	if (r)
		return r;

	r = dsi_vc_send_bta_sync(channel);

	return r;
}
EXPORT_SYMBOL(dsi_vc_dcs_write);

int dsi_vc_dcs_write_0(int channel, u8 dcs_cmd)
{
	return dsi_vc_dcs_write(channel, &dcs_cmd, 1);
}
EXPORT_SYMBOL(dsi_vc_dcs_write_0);

int dsi_vc_dcs_write_1(int channel, u8 dcs_cmd, u8 param)
{
	u8 buf[2];
	buf[0] = dcs_cmd;
	buf[1] = param;
	return dsi_vc_dcs_write(channel, buf, 2);
}
EXPORT_SYMBOL(dsi_vc_dcs_write_1);

int dsi_vc_dcs_read(int channel, u8 dcs_cmd, u8 *buf, int buflen)
{
	u32 val;
	u8 dt;
	int r;

	if (dsi.debug_read)
		DSSDBG("dsi_vc_dcs_read(ch%d, dcs_cmd %u)\n", channel, dcs_cmd);

	r = dsi_vc_send_short(channel, DSI_DT_DCS_READ, dcs_cmd, 0);
	if (r)
		return r;

	r = dsi_vc_send_bta_sync(channel);
	if (r)
		return r;

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	/* RX_FIFO_NOT_EMPTY */
	if (REG_GET(DSI_VC_CTRL(channel), 20, 20) == 0) {
		DSSERR("RX fifo empty when trying to read.\n");
		r = -EIO;
		goto err;
	}

	val = dsi_read_reg(DSI_VC_SHORT_PACKET_HEADER(channel));
	if (dsi.debug_read)
		DSSDBG("\theader: %08x\n", val);
	dt = FLD_GET(val, 5, 0);
	if (dt == DSI_DT_RX_ACK_WITH_ERR) {
		u16 err = FLD_GET(val, 23, 8);
		dsi_show_rx_ack_with_err(err);
		r = -EIO;

	} else if (dt == DSI_DT_RX_SHORT_READ_1) {
		u8 data = FLD_GET(val, 15, 8);
		if (dsi.debug_read)
			DSSDBG("\tDCS short response, 1 byte: %02x\n", data);

		if (buflen < 1) {
			r = -EIO;
			goto err;
		}

		buf[0] = data;

		r = 1;
	} else if (dt == DSI_DT_RX_SHORT_READ_2) {
		u16 data = FLD_GET(val, 23, 8);
		if (dsi.debug_read)
			DSSDBG("\tDCS short response, 2 byte: %04x\n", data);

		if (buflen < 2) {
			r = -EIO;
			goto err;
		}

		buf[0] = data & 0xff;
		buf[1] = (data >> 8) & 0xff;

		r = 2;
	} else if (dt == DSI_DT_RX_DCS_LONG_READ) {
		int w;
		int len = FLD_GET(val, 23, 8);
		if (dsi.debug_read)
			DSSDBG("\tDCS long response, len %d\n", len);

		if (len > buflen) {
			r = -EIO;
			goto err;
		}

		/* two byte checksum ends the packet, not included in len */
		for (w = 0; w < len + 2;) {
			int b;
			val = dsi_read_reg(DSI_VC_SHORT_PACKET_HEADER(channel));
			if (dsi.debug_read)
				DSSDBG("\t\t%02x %02x %02x %02x\n",
						(val >> 0) & 0xff,
						(val >> 8) & 0xff,
						(val >> 16) & 0xff,
						(val >> 24) & 0xff);

			for (b = 0; b < 4; ++b) {
				if (w < len)
					buf[w] = (val >> (b * 8)) & 0xff;
				/* we discard the 2 byte checksum */
				++w;
			}
		}

		r = len;

	} else {
		DSSERR("\tunknown datatype 0x%02x\n", dt);
		r = -EIO;
	}
err:
	enable_clocks(0);
	dsi_enable_pll_clock(0);

	return r;

}
EXPORT_SYMBOL(dsi_vc_dcs_read);

int dsi_vc_dcs_read_1(int channel, u8 dcs_cmd, u8 *data)
{
	int r;

	r = dsi_vc_dcs_read(channel, dcs_cmd, data, 1);

	if (r < 0)
		return r;

	if (r != 1)
		return -EIO;

	return 0;
}
EXPORT_SYMBOL(dsi_vc_dcs_read_1);

int dsi_vc_set_max_rx_packet_size(int channel, u16 len)
{
	int r;
	r = dsi_vc_send_short(channel, DSI_DT_SET_MAX_RET_PKG_SIZE,
			len, 0);

	if (r)
		return r;

	r = dsi_vc_send_bta_sync(channel);

	return r;
}
EXPORT_SYMBOL(dsi_vc_set_max_rx_packet_size);

static void dsi_set_lp_rx_timeout(unsigned long ns)
{
	u32 r;
	unsigned x4, x16;
	unsigned long fck;
	unsigned long ticks;

	/* ticks in DSI_FCK */

	fck = dsi_fclk_rate();
	ticks = (fck / 1000 / 1000) * ns / 1000;
	x4 = 0;
	x16 = 0;

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / 4;
		x4 = 1;
		x16 = 0;
	}

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / 16;
		x4 = 0;
		x16 = 1;
	}

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / (4 * 16);
		x4 = 1;
		x16 = 1;
	}

	if (ticks > 0x1fff) {
		DSSWARN("LP_TX_TO over limit, setting it to max\n");
		ticks = 0x1fff;
		x4 = 1;
		x16 = 1;
	}

	r = dsi_read_reg(DSI_TIMING2);
	r = FLD_MOD(r, 1, 15, 15);	/* LP_RX_TO */
	r = FLD_MOD(r, x16, 14, 14);	/* LP_RX_TO_X16 */
	r = FLD_MOD(r, x4, 13, 13);	/* LP_RX_TO_X4 */
	r = FLD_MOD(r, ticks, 12, 0);	/* LP_RX_COUNTER */
	dsi_write_reg(DSI_TIMING2, r);

	DSSDBG("LP_RX_TO %lu ns (%#lx ticks%s%s)\n",
			(ticks * (x16 ? 16 : 1) * (x4 ? 4 : 1) * 1000) /
			(fck / 1000 / 1000),
			ticks, x4 ? " x4" : "", x16 ? " x16" : "");
}

static void dsi_set_ta_timeout(unsigned long ns)
{
	u32 r;
	unsigned x8, x16;
	unsigned long fck;
	unsigned long ticks;

	/* ticks in DSI_FCK */
	fck = dsi_fclk_rate();
	ticks = (fck / 1000 / 1000) * ns / 1000;
	x8 = 0;
	x16 = 0;

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / 8;
		x8 = 1;
		x16 = 0;
	}

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / 16;
		x8 = 0;
		x16 = 1;
	}

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / (8 * 16);
		x8 = 1;
		x16 = 1;
	}

	if (ticks > 0x1fff) {
		DSSWARN("TA_TO over limit, setting it to max\n");
		ticks = 0x1fff;
		x8 = 1;
		x16 = 1;
	}

	r = dsi_read_reg(DSI_TIMING1);
	r = FLD_MOD(r, 1, 31, 31);	/* TA_TO */
	r = FLD_MOD(r, x16, 30, 30);	/* TA_TO_X16 */
	r = FLD_MOD(r, x8, 29, 29);	/* TA_TO_X8 */
	r = FLD_MOD(r, ticks, 28, 16);	/* TA_TO_COUNTER */
	dsi_write_reg(DSI_TIMING1, r);

	DSSDBG("TA_TO %lu ns (%#lx ticks%s%s)\n",
			(ticks * (x16 ? 16 : 1) * (x8 ? 8 : 1) * 1000) /
			(fck / 1000 / 1000),
			ticks, x8 ? " x8" : "", x16 ? " x16" : "");
}

static void dsi_set_stop_state_counter(unsigned long ns)
{
	u32 r;
	unsigned x4, x16;
	unsigned long fck;
	unsigned long ticks;

	/* ticks in DSI_FCK */

	fck = dsi_fclk_rate();
	ticks = (fck / 1000 / 1000) * ns / 1000;
	x4 = 0;
	x16 = 0;

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / 4;
		x4 = 1;
		x16 = 0;
	}

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / 16;
		x4 = 0;
		x16 = 1;
	}

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / (4 * 16);
		x4 = 1;
		x16 = 1;
	}

	if (ticks > 0x1fff) {
		DSSWARN("STOP_STATE_COUNTER_IO over limit, "
				"setting it to max\n");
		ticks = 0x1fff;
		x4 = 1;
		x16 = 1;
	}

	r = dsi_read_reg(DSI_TIMING1);
	r = FLD_MOD(r, 1, 15, 15);	/* FORCE_TX_STOP_MODE_IO */
	r = FLD_MOD(r, x16, 14, 14);	/* STOP_STATE_X16_IO */
	r = FLD_MOD(r, x4, 13, 13);	/* STOP_STATE_X4_IO */
	r = FLD_MOD(r, ticks, 12, 0);	/* STOP_STATE_COUNTER_IO */
	dsi_write_reg(DSI_TIMING1, r);

	DSSDBG("STOP_STATE_COUNTER %lu ns (%#lx ticks%s%s)\n",
			(ticks * (x16 ? 16 : 1) * (x4 ? 4 : 1) * 1000) /
			(fck / 1000 / 1000),
			ticks, x4 ? " x4" : "", x16 ? " x16" : "");
}

static void dsi_set_hs_tx_timeout(unsigned long ns)
{
	u32 r;
	unsigned x4, x16;
	unsigned long fck;
	unsigned long ticks;

	/* ticks in TxByteClkHS */

	fck = dsi_get_txbyteclkhs();
	ticks = (fck / 1000 / 1000) * ns / 1000;
	x4 = 0;
	x16 = 0;

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / 4;
		x4 = 1;
		x16 = 0;
	}

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / 16;
		x4 = 0;
		x16 = 1;
	}

	if (ticks > 0x1fff) {
		ticks = (fck / 1000 / 1000) * ns / 1000 / (4 * 16);
		x4 = 1;
		x16 = 1;
	}

	if (ticks > 0x1fff) {
		DSSWARN("HS_TX_TO over limit, setting it to max\n");
		ticks = 0x1fff;
		x4 = 1;
		x16 = 1;
	}

	r = dsi_read_reg(DSI_TIMING2);
	r = FLD_MOD(r, 1, 31, 31);	/* HS_TX_TO */
	r = FLD_MOD(r, x16, 30, 30);	/* HS_TX_TO_X16 */
	r = FLD_MOD(r, x4, 29, 29);	/* HS_TX_TO_X8 (4 really) */
	r = FLD_MOD(r, ticks, 28, 16);	/* HS_TX_TO_COUNTER */
	dsi_write_reg(DSI_TIMING2, r);

	DSSDBG("HS_TX_TO %lu ns (%#lx ticks%s%s)\n",
			(ticks * (x16 ? 16 : 1) * (x4 ? 4 : 1) * 1000) /
			(fck / 1000 / 1000),
			ticks, x4 ? " x4" : "", x16 ? " x16" : "");
}
static int dsi_proto_config(struct omap_dss_device *dssdev)
{
	u32 r;
	int buswidth = 0;

	enum fifo_size size1, size2, size3, size4;

	size1 = DSI_FIFO_SIZE_0;
	size2 = DSI_FIFO_SIZE_0;
	size3 = DSI_FIFO_SIZE_0;
	size4 = DSI_FIFO_SIZE_0;

	switch (DSI_CMD_VC) {
	case 0:
		size1 = DSI_FIFO_SIZE_128;
		break;
	case 1:
		size2 = DSI_FIFO_SIZE_128;
		break;
	case 2:
		size3 = DSI_FIFO_SIZE_128;
		break;
	case 3:
		size4 = DSI_FIFO_SIZE_128;
		break;
	default:
		BUG();
		break;
	}

	dsi_config_tx_fifo(size1, size2, size3, size4);
	dsi_config_rx_fifo(size1, size2, size3, size4);

	/* XXX what values for the timeouts? */
	dsi_set_stop_state_counter(1000);
	dsi_set_ta_timeout(6400000);
	dsi_set_lp_rx_timeout(80000);
	dsi_set_hs_tx_timeout(1000000);

	switch (dssdev->ctrl.pixel_size) {
	case 16:
		buswidth = 0;
		break;
	case 18:
		buswidth = 1;
		break;
	case 24:
		buswidth = 2;
		break;
	default:
		BUG();
	}

	r = dsi_read_reg(DSI_CTRL);
	r = FLD_MOD(r, 1, 1, 1);	/* CS_RX_EN */
	r = FLD_MOD(r, 1, 2, 2);	/* ECC_RX_EN */
	r = FLD_MOD(r, 1, 3, 3);	/* TX_FIFO_ARBITRATION */
	r = FLD_MOD(r, 1, 4, 4);	/* VP_CLK_RATIO, always 1, see errata*/
	r = FLD_MOD(r, buswidth, 7, 6); /* VP_DATA_BUS_WIDTH */
	r = FLD_MOD(r, 0, 8, 8);	/* VP_CLK_POL */
	r = FLD_MOD(r, 2, 13, 12);	/* LINE_BUFFER, 2 lines */
	r = FLD_MOD(r, 1, 14, 14);	/* TRIGGER_RESET_MODE */
	r = FLD_MOD(r, 1, 19, 19);	/* EOT_ENABLE */
	r = FLD_MOD(r, 1, 24, 24);	/* DCS_CMD_ENABLE */
	r = FLD_MOD(r, 0, 25, 25);	/* DCS_CMD_CODE, 1=start, 0=continue */

	dsi_write_reg(DSI_CTRL, r);

	dsi_vc_initial_config(DSI_CMD_VC);
	dsi_vc_initial_config(DSI_VIDEO_VC);

	/* set all vc targets to peripheral 0 */
	dsi.vc[0].dest_per = 0;
	dsi.vc[1].dest_per = 0;
	dsi.vc[2].dest_per = 0;
	dsi.vc[3].dest_per = 0;

	return 0;
}

static void dsi_proto_timings(struct omap_dss_device *dssdev)
{
	unsigned tlpx, tclk_zero, tclk_prepare, tclk_trail;
	unsigned tclk_pre, tclk_post;
	unsigned ths_prepare, ths_prepare_ths_zero, ths_zero;
	unsigned ths_trail, ths_exit;
	unsigned ddr_clk_pre, ddr_clk_post;
	unsigned enter_hs_mode_lat, exit_hs_mode_lat;
	unsigned ths_eot;
	u32 r;

	r = dsi_read_reg(DSI_DSIPHY_CFG0);
	ths_prepare = FLD_GET(r, 31, 24);
	ths_prepare_ths_zero = FLD_GET(r, 23, 16);
	ths_zero = ths_prepare_ths_zero - ths_prepare;
	ths_trail = FLD_GET(r, 15, 8);
	ths_exit = FLD_GET(r, 7, 0);

	r = dsi_read_reg(DSI_DSIPHY_CFG1);
	tlpx = FLD_GET(r, 22, 16) * 2;
	tclk_trail = FLD_GET(r, 15, 8);
	tclk_zero = FLD_GET(r, 7, 0);

	r = dsi_read_reg(DSI_DSIPHY_CFG2);
	tclk_prepare = FLD_GET(r, 7, 0);

	/* min 8*UI */
	tclk_pre = 20;
	/* min 60ns + 52*UI */
	tclk_post = ns2ddr(60) + 26;

	/* ths_eot is 2 for 2 datalanes and 4 for 1 datalane */
	if (dssdev->phy.dsi.data1_lane != 0 &&
			dssdev->phy.dsi.data2_lane != 0)
		ths_eot = 2;
	else
		ths_eot = 4;

	ddr_clk_pre = DIV_ROUND_UP(tclk_pre + tlpx + tclk_zero + tclk_prepare,
			4);
	ddr_clk_post = DIV_ROUND_UP(tclk_post + ths_trail, 4) + ths_eot;

	BUG_ON(ddr_clk_pre == 0 || ddr_clk_pre > 255);
	BUG_ON(ddr_clk_post == 0 || ddr_clk_post > 255);

	r = dsi_read_reg(DSI_CLK_TIMING);
	r = FLD_MOD(r, ddr_clk_pre, 15, 8);
	r = FLD_MOD(r, ddr_clk_post, 7, 0);
	dsi_write_reg(DSI_CLK_TIMING, r);

	DSSDBG("ddr_clk_pre %u, ddr_clk_post %u\n",
			ddr_clk_pre,
			ddr_clk_post);

	enter_hs_mode_lat = 1 + DIV_ROUND_UP(tlpx, 4) +
		DIV_ROUND_UP(ths_prepare, 4) +
		DIV_ROUND_UP(ths_zero + 3, 4);

	exit_hs_mode_lat = DIV_ROUND_UP(ths_trail + ths_exit, 4) + 1 + ths_eot;

	r = FLD_VAL(enter_hs_mode_lat, 31, 16) |
		FLD_VAL(exit_hs_mode_lat, 15, 0);
	dsi_write_reg(DSI_VM_TIMING7, r);

	DSSDBG("enter_hs_mode_lat %u, exit_hs_mode_lat %u\n",
			enter_hs_mode_lat, exit_hs_mode_lat);
}


#define DSI_DECL_VARS \
	int __dsi_cb = 0; u32 __dsi_cv = 0;

#define DSI_FLUSH(ch) \
	if (__dsi_cb > 0) { \
		/*DSSDBG("sending long packet %#010x\n", __dsi_cv);*/ \
		dsi_write_reg(DSI_VC_LONG_PACKET_PAYLOAD(ch), __dsi_cv); \
		__dsi_cb = __dsi_cv = 0; \
	}

#define DSI_PUSH(ch, data) \
	do { \
		__dsi_cv |= (data) << (__dsi_cb * 8); \
		/*DSSDBG("cv = %#010x, cb = %d\n", __dsi_cv, __dsi_cb);*/ \
		if (++__dsi_cb > 3) \
			DSI_FLUSH(ch); \
	} while (0)

static int dsi_update_screen_l4(struct omap_dss_device *dssdev,
			int x, int y, int w, int h)
{
	/* Note: supports only 24bit colors in 32bit container */
	int first = 1;
	int fifo_stalls = 0;
	int max_dsi_packet_size;
	int max_data_per_packet;
	int max_pixels_per_packet;
	int pixels_left;
	int bytespp = dssdev->ctrl.pixel_size / 8;
	int scr_width;
	u32 __iomem *data;
	int start_offset;
	int horiz_inc;
	int current_x;
	struct omap_overlay *ovl;

	debug_irq = 0;

	DSSDBG("dsi_update_screen_l4 (%d,%d %dx%d)\n",
			x, y, w, h);

	ovl = dssdev->manager->overlays[0];

	if (ovl->info.color_mode != OMAP_DSS_COLOR_RGB24U)
		return -EINVAL;

	if (dssdev->ctrl.pixel_size != 24)
		return -EINVAL;

	scr_width = ovl->info.screen_width;
	data = ovl->info.vaddr;

	start_offset = scr_width * y + x;
	horiz_inc = scr_width - w;
	current_x = x;

	/* We need header(4) + DCSCMD(1) + pixels(numpix*bytespp) bytes
	 * in fifo */

	/* When using CPU, max long packet size is TX buffer size */
	max_dsi_packet_size = dsi.vc[DSI_VIDEO_VC].fifo_size * 32 * 4;

	/* we seem to get better perf if we divide the tx fifo to half,
	   and while the other half is being sent, we fill the other half
	   max_dsi_packet_size /= 2; */

	max_data_per_packet = max_dsi_packet_size - 4 - 1;

	max_pixels_per_packet = max_data_per_packet / bytespp;

	DSSDBG("max_pixels_per_packet %d\n", max_pixels_per_packet);

	pixels_left = w * h;

	DSSDBG("total pixels %d\n", pixels_left);

	data += start_offset;

	while (pixels_left > 0) {
		/* 0x2c = write_memory_start */
		/* 0x3c = write_memory_continue */
		u8 dcs_cmd = first ? 0x2c : 0x3c;
		int pixels;
		DSI_DECL_VARS;
		first = 0;

#if 1
		/* using fifo not empty */
		/* TX_FIFO_NOT_EMPTY */
		while (FLD_GET(dsi_read_reg(DSI_VC_CTRL(DSI_VIDEO_VC)), 5, 5)) {
			udelay(1);
			fifo_stalls++;
			if (fifo_stalls > 0xfffff) {
				DSSERR("fifo stalls overflow, pixels left %d\n",
						pixels_left);
				dsi_if_enable(0);
				return -EIO;
			}
		}
#elif 1
		/* using fifo emptiness */
		while ((REG_GET(DSI_TX_FIFO_VC_EMPTINESS, 7, 0)+1)*4 <
				max_dsi_packet_size) {
			fifo_stalls++;
			if (fifo_stalls > 0xfffff) {
				DSSERR("fifo stalls overflow, pixels left %d\n",
					       pixels_left);
				dsi_if_enable(0);
				return -EIO;
			}
		}
#else
		while ((REG_GET(DSI_TX_FIFO_VC_EMPTINESS, 7, 0)+1)*4 == 0) {
			fifo_stalls++;
			if (fifo_stalls > 0xfffff) {
				DSSERR("fifo stalls overflow, pixels left %d\n",
					       pixels_left);
				dsi_if_enable(0);
				return -EIO;
			}
		}
#endif
		pixels = min(max_pixels_per_packet, pixels_left);

		pixels_left -= pixels;

		dsi_vc_write_long_header(DSI_VIDEO_VC, DSI_DT_DCS_LONG_WRITE,
				1 + pixels * bytespp, 0);

		DSI_PUSH(0, dcs_cmd);

		while (pixels-- > 0) {
			u32 pix = __raw_readl(data++);

			DSI_PUSH(0, (pix >> 16) & 0xff);
			DSI_PUSH(0, (pix >> 8) & 0xff);
			DSI_PUSH(0, (pix >> 0) & 0xff);

			current_x++;
			if (current_x == x+w) {
				current_x = x;
				data += horiz_inc;
			}
		}

		DSI_FLUSH(0);
	}

	return 0;
}

static void dsi_update_screen_dispc(struct omap_dss_device *dssdev,
		u16 x, u16 y, u16 w, u16 h)
{
	unsigned bytespp;
	unsigned bytespl;
	unsigned bytespf;
	unsigned total_len;
	unsigned packet_payload;
	unsigned packet_len;
	u32 l;
	bool use_te_trigger;
	const unsigned channel = DSI_VIDEO_VC;

	/* line buffer is 1024 x 24bits */
	/* XXX: for some reason using full buffer size causes considerable TX
	 * slowdown with update sizes that fill the whole buffer */
	const unsigned line_buf_size = 1023 * 3;

	use_te_trigger = dsi.te_enabled && !dsi.use_ext_te;

	if (dsi.update_mode != OMAP_DSS_UPDATE_AUTO)
		DSSDBG("dsi_update_screen_dispc(%d,%d %dx%d)\n",
				x, y, w, h);

	bytespp	= dssdev->ctrl.pixel_size / 8;
	bytespl = w * bytespp;
	bytespf = bytespl * h;

	/* NOTE: packet_payload has to be equal to N * bytespl, where N is
	 * number of lines in a packet.  See errata about VP_CLK_RATIO */

	if (bytespf < line_buf_size)
		packet_payload = bytespf;
	else
		packet_payload = (line_buf_size) / bytespl * bytespl;

	packet_len = packet_payload + 1;	/* 1 byte for DCS cmd */
	total_len = (bytespf / packet_payload) * packet_len;

	if (bytespf % packet_payload)
		total_len += (bytespf % packet_payload) + 1;

	if (0)
		dsi_vc_print_status(channel);

	l = FLD_VAL(total_len, 23, 0); /* TE_SIZE */
	dsi_write_reg(DSI_VC_TE(channel), l);

	dsi_vc_write_long_header(channel, DSI_DT_DCS_LONG_WRITE, packet_len, 0);

	if (use_te_trigger)
		l = FLD_MOD(l, 1, 30, 30); /* TE_EN */
	else if (dsi.sw_te_sup == false)
		l = FLD_MOD(l, 1, 31, 31); /* TE_START */
	else
		l = FLD_MOD(l, 0, 31, 30); /* clear TE_START */

	dsi_write_reg(DSI_VC_TE(channel), l);

	/* We put SIDLEMODE to no-idle for the duration of the transfer,
	 * because DSS interrupts are not capable of waking up the CPU and the
	 * framedone interrupt could be delayed for quite a long time. I think
	 * the same goes for any DSS interrupts, but for some reason I have not
	 * seen the problem anywhere else than here.
	 */
	dispc_disable_sidle();

	dss_start_update(dssdev);

	if (use_te_trigger) {
		if (dssdev->driver->manual_te_trigger) {
			if (dssdev->driver->manual_te_trigger(dssdev) == true)
				dssdev->driver->enable_te(dssdev, true);
		}

		/* disable LP_RX_TO, so that we can receive TE.  Time to wait
		 * for TE is longer than the timer allows */
		REG_FLD_MOD(DSI_TIMING2, 0, 15, 15); /* LP_RX_TO */

		dsi_vc_send_bta_sync(DSI_CMD_VC);
		dsi_vc_send_bta(DSI_CMD_VC);

#ifdef DSI_CATCH_MISSING_TE
		mod_timer(&dsi.te_timer, jiffies + msecs_to_jiffies(250));
#endif
	}
}

#ifdef DSI_CATCH_MISSING_TE
static void dsi_te_timeout(unsigned long arg)
{
	DSSERR("TE not received for 250ms!\n");
}
#endif

static void dsi_framedone_irq_callback(void *data, u32 mask)
{
	/* Note: We get FRAMEDONE when DISPC has finished sending pixels and
	 * turns itself off. However, DSI still has the pixels in its buffers,
	 * and is sending the data.
	 */

	/* SIDLEMODE back to smart-idle */
	dispc_enable_sidle();

	dsi.framedone_received = true;
	wake_up(&dsi.waitqueue);
}

static void dsi_set_update_region(struct omap_dss_device *dssdev,
		u16 x, u16 y, u16 w, u16 h)
{
	spin_lock(&dsi.update_lock);
	if (dsi.update_region.dirty) {
		dsi.update_region.x = min(x, dsi.update_region.x);
		dsi.update_region.y = min(y, dsi.update_region.y);
		dsi.update_region.w = max(w, dsi.update_region.w);
		dsi.update_region.h = max(h, dsi.update_region.h);
	} else {
		dsi.update_region.x = x;
		dsi.update_region.y = y;
		dsi.update_region.w = w;
		dsi.update_region.h = h;
	}

	dsi.update_region.device = dssdev;
	dsi.update_region.dirty = true;

	spin_unlock(&dsi.update_lock);

}

static int dsi_set_update_mode(struct omap_dss_device *dssdev,
		enum omap_dss_update_mode mode)
{
	int r = 0;
	int i;

	WARN_ON(!dsi_bus_is_locked());

	if (dsi.update_mode != mode) {
		dsi.update_mode = mode;

		/* Mark the overlays dirty, and do apply(), so that we get the
		 * overlays configured properly after update mode change. */
		for (i = 0; i < omap_dss_get_num_overlays(); ++i) {
			struct omap_overlay *ovl;
			ovl = omap_dss_get_overlay(i);
			if (ovl->manager == dssdev->manager)
				ovl->info_dirty = true;
		}

		r = dssdev->manager->apply(dssdev->manager);

		if (dssdev->state == OMAP_DSS_DISPLAY_ACTIVE &&
				mode == OMAP_DSS_UPDATE_AUTO) {
			u16 w, h;

			DSSDBG("starting auto update\n");

			dssdev->get_resolution(dssdev, &w, &h);

			dsi_set_update_region(dssdev, 0, 0, w, h);

			dsi_perf_mark_start_auto();

			wake_up(&dsi.waitqueue);
		}
	}

	return r;
}

static int dsi_set_te(struct omap_dss_device *dssdev, bool enable)
{
	int r;
	r = dssdev->driver->enable_te(dssdev, enable);
	/* XXX for some reason, DSI TE breaks if we don't wait here.
	 * Panel bug? Needs more studying */
	msleep(100);
	return r;
}

static void dsi_handle_framedone(void)
{
	const int channel = DSI_VIDEO_VC;
	bool use_te_trigger;

	use_te_trigger = dsi.te_enabled && !dsi.use_ext_te;

	if (dsi.update_mode != OMAP_DSS_UPDATE_AUTO)
		DSSDBG("FRAMEDONE\n");

	if (use_te_trigger) {
		/* enable LP_RX_TO again after the TE */
		REG_FLD_MOD(DSI_TIMING2, 1, 15, 15); /* LP_RX_TO */
	}

	/* RX_FIFO_NOT_EMPTY */
	if (REG_GET(DSI_VC_CTRL(channel), 20, 20)) {
		DSSERR("Received error during frame transfer:\n");
		dsi_vc_flush_receive_data(channel);
	}

#ifdef CONFIG_OMAP2_DSS_FAKE_VSYNC
	dispc_fake_vsync_irq();
#endif
}

static int dsi_sw_te(struct omap_dss_device *dssdev)
{
	int cur_scan_line = 0;
	int scan_line_setting = 0;
	int sw_te_wait_usec = 0;
	static int scanline_time;
	int total_scan_line;

	if (dssdev->driver->read_scl)
		cur_scan_line = dssdev->driver->read_scl(dssdev);

	if (dssdev->driver->get_scl_setting)
		scan_line_setting = dssdev->driver->get_scl_setting(dssdev);

	/* scanline_time is time to take to display 1 line in usec * 100
	 * scanline_time = (1 / 60 frames) * 100 / number_of_lines
	 *               = (16.666msec * 1000) * 100 /
	 *                   (back_porch + height+ front_porch)
	 *               = (1*1000*1000 usec/ 60 frames) *100 / ( 4 + height+ 4)
	 * Ex: height = 480 => scanline_time = 34.15usec * 100 = 3415
	 */
	total_scan_line = 4 + dssdev->panel.timings.y_res + 4;

	if (scanline_time == 0)
		scanline_time = (16666 * 100) / total_scan_line;

	/* From testing, we saw it will have delay around 300 usec to set
	 * timer until timer to call back. So if there is a cur_scan_line is
	 * at scan_line_setting +/- 8 lines then we will not set the timer
	 */

	if ((cur_scan_line == 0xffff) ||
			(((scan_line_setting + 4 - 8) <= cur_scan_line) &&
			((scan_line_setting + 4 + 8) >= cur_scan_line)))
		sw_te_wait_usec = 0;
	else if ((scan_line_setting + 4 + 8) < cur_scan_line)
		sw_te_wait_usec = 16666 -
			(((cur_scan_line + 1) - (scan_line_setting + 4 + 8)) *
			scanline_time) / 100;
	else
		sw_te_wait_usec = (((scan_line_setting + 4) -
				(cur_scan_line + 1)) * scanline_time) / 100;

	if (sw_te_wait_usec == 0)
		REG_FLD_MOD(DSI_VC_TE(DSI_VIDEO_VC), 1, 31, 31);

	else
		hrtimer_start(&dsi.hrtimer,
				ktime_set(0, sw_te_wait_usec*1000),
				HRTIMER_MODE_REL);

	return 0;
}



static int dsi_wait_framedone(struct omap_dss_device *dssdev)
{
	unsigned long timeout = 0;
	bool use_te_trigger;
	u16 i;
	bool manual_te_trigger = false;
	int ret = -1;
	const int channel = DSI_CMD_VC;

	use_te_trigger = dsi.te_enabled && !dsi.use_ext_te;
	if (dssdev->driver->manual_te_trigger)
		manual_te_trigger = dssdev->driver->manual_te_trigger(dssdev);


	if (use_te_trigger && manual_te_trigger) {
		dsi.te_trigger = false;

		for (i = 0 ; i < 2 && use_te_trigger; i++) {
			/* In some displays, we need to request TE for
			each frame to receive TE trigger. Sometimes, TE req
			come down just before display hits that requested
			scanline but display did not receive BTA to send and
			the display driver will not receive TE trigger which
			willc ause the framedone timeout

			In this case, if we miss a TE triger, then we will
			send out TE request again after 1.5 frame length
			( 60hz=> 17 msec + 8 msec = 25 msec to avoid we will
			hit the same above critical time again */

			timeout = msecs_to_jiffies(25);
			timeout = wait_event_timeout(dsi.waitqueue,
					(dsi.framedone_received == true ||
					dsi.te_trigger == true),
					timeout);
			if (dsi.framedone_received || dsi.te_trigger)
				break;
			else {
				/* Timeout but we don't receive framedone or
				TE_trigger then we request for TE trigger
				 again */
				dssdev->driver->enable_te(dssdev, true);
				dsi_vc_send_bta_sync(channel);
				dsi_vc_send_bta(channel);
			}
		}

		if (dsi.framedone_received != true) {
			timeout = msecs_to_jiffies(51);
			timeout = wait_event_timeout(dsi.waitqueue,
					dsi.framedone_received == true,
					timeout);
		}

	} else {
		if (dsi.sw_te_sup == true)
			dsi_sw_te(dssdev);

		timeout = msecs_to_jiffies(100);
		timeout = wait_event_timeout(dsi.waitqueue,
					dsi.framedone_received == true,
					timeout);

		if (dsi.sw_te_sup == true)
			hrtimer_cancel(&dsi.hrtimer);
	}

	if (!dsi.framedone_received) {
		DSSERR("framedone timeout\n");
		if (use_te_trigger)
			DSSERR("TE_trigger = %d\n", dsi.te_trigger);

		dispc_enable_sidle();
		dispc_enable_lcd_out(0);

		dsi_reset_tx_fifo(0);
		dsi.framedone_received = false;
		ret = -1;
	} else {
		dsi_handle_framedone();
		dsi_perf_show("DISPC");
		if (dssdev->driver->framedone)
			dssdev->driver->framedone(dssdev);
		ret = 0;
	}

	if (use_te_trigger)
		dsi.te_trigger = false;

	return ret;
}

static void dsi_wait_te_size_clear(int channel)
{
	u16 wait_count = 0;

	/* Framedone interrupt is generated by DISPC, but it doesn't mean DSI
	 * block is done with sending data. If the DSI clock is cut off when
	 * DSI HW is still sending data, it can harm the DSI HW. Check the
	 * TE_SIZE before cut off the DSI PLL to make sure DSI is done with
	 * transmiting*/
	wait_count = 0;
	while ((dsi_read_reg(DSI_VC_TE(channel)) & 0xffff) != 0) {
		ndelay(10);
		/* wait for 1usec (10nsec * 100) is good enough */
		if (wait_count++ >= 100) {
			DSSERR("TE_SIZE is not reset after FD IRQ \n");
			break;
		}
	}
}


static int dsi_update_thread(void *data)
{
	struct omap_dss_device *device;
	u16 x, y, w, h;
	u8 num_timeouts = 0;
	u8 num_success = 0;

	while (1) {
		wait_event_interruptible(dsi.waitqueue,
				dsi.update_mode == OMAP_DSS_UPDATE_AUTO ||
				(dsi.update_mode == OMAP_DSS_UPDATE_MANUAL &&
				 dsi.update_region.dirty == true) ||
				kthread_should_stop());

		if (kthread_should_stop())
			break;

		dsi_bus_lock();
		enable_clocks(1);
		dsi_enable_pll_clock(1);

		dsi_perf_mark_setup();

		if (dsi.update_region.dirty) {
			spin_lock(&dsi.update_lock);
			dsi.active_update_region = dsi.update_region;
			dsi.update_region.dirty = false;
			spin_unlock(&dsi.update_lock);
		}

		if (dsi.update_mode == OMAP_DSS_UPDATE_DISABLED) {
			enable_clocks(0);
			dsi_enable_pll_clock(0);
			dsi_bus_unlock();
			continue;
		}

		device = dsi.active_update_region.device;
		x = dsi.active_update_region.x;
		y = dsi.active_update_region.y;
		w = dsi.active_update_region.w;
		h = dsi.active_update_region.h;

		if (device->manager->caps & OMAP_DSS_OVL_MGR_CAP_DISPC) {

			if (dsi.update_mode == OMAP_DSS_UPDATE_MANUAL)
				dss_setup_partial_planes(device,
						&x, &y, &w, &h);

			dispc_set_lcd_size(w, h);
		}

		if (dsi.active_update_region.dirty) {
			dsi.active_update_region.dirty = false;
			/* XXX TODO we don't need to send the coords, if they
			 * are the same that are already programmed to the
			 * panel. That should speed up manual update a bit */
			device->driver->setup_update(device, x, y, w, h);
		}

		dsi_perf_mark_start();

		if (device->manager->caps & OMAP_DSS_OVL_MGR_CAP_DISPC) {
			dsi_vc_config_vp(DSI_VIDEO_VC);

			if (dsi.te_enabled && dsi.use_ext_te)
				device->driver->wait_for_te(device);

			dsi.framedone_received = false;

			dsi_update_screen_dispc(device, x, y, w, h);

			if (dsi_wait_framedone(device) != 0) {
				DSSERR("failed update %d,%d %dx%d\n",
							x, y, w, h);

				num_success = 0;

				if (++num_timeouts >= MAX_FRAMEDONE_TIMEOUTS) {
					DSSERR("Too many framedone, "
						"scheduling error recovery\n");
					schedule_error_recovery();
				}
			} else {
				dsi_wait_te_size_clear(DSI_VIDEO_VC);
				if (num_timeouts != 0) {
					if (++num_success >= MIN_FRAMEDONE_OK) {
						num_timeouts = 0;
					}
				}
			}
		} else {
			dsi_update_screen_l4(device, x, y, w, h);
			dsi_perf_show("L4");
		}

		complete_all(&dsi.update_completion);

		enable_clocks(0);
		dsi_enable_pll_clock(0);

		dsi_bus_unlock();
	}

	DSSDBG("update thread exiting\n");

	return 0;
}



/* Display funcs */

static int dsi_display_init_dispc(struct omap_dss_device *dssdev)
{
	int r;

	r = omap_dispc_register_isr(dsi_framedone_irq_callback, NULL,
			DISPC_IRQ_FRAMEDONE);
	if (r) {
		DSSERR("can't get FRAMEDONE irq\n");
		return r;
	}

	dispc_set_lcd_display_type(OMAP_DSS_LCD_DISPLAY_TFT);

	dispc_set_parallel_interface_mode(OMAP_DSS_PARALLELMODE_DSI);
	dispc_enable_fifohandcheck(1);

	dispc_set_tft_data_lines(dssdev->ctrl.pixel_size);

	{
		struct omap_video_timings timings = {
			.hsw		= 1,
			.hfp		= 1,
			.hbp		= 1,
			.vsw		= 1,
			.vfp		= 0,
			.vbp		= 0,
		};

		dispc_set_lcd_timings(&timings);
	}

	return 0;
}

static void dsi_display_uninit_dispc(struct omap_dss_device *dssdev)
{
	omap_dispc_unregister_isr(dsi_framedone_irq_callback, NULL,
			DISPC_IRQ_FRAMEDONE);
}

static int dsi_configure_dsi_clocks(struct omap_dss_device *dssdev)
{
	struct dsi_clock_info cinfo;
	int r;

	/* we always use DSS2_FCK as input clock */
	cinfo.use_dss2_fck = true;
	cinfo.regn  = dssdev->phy.dsi.div.regn;
	cinfo.regm  = dssdev->phy.dsi.div.regm;
	cinfo.regm3 = dssdev->phy.dsi.div.regm3;
	cinfo.regm4 = dssdev->phy.dsi.div.regm4;
	r = dsi_calc_clock_rates(&cinfo);
	if (r)
		return r;

	r = dsi_pll_set_clock_div(&cinfo);
	if (r) {
		DSSERR("Failed to set dsi clocks\n");
		return r;
	}

	return 0;
}

static int dsi_configure_dispc_clocks(struct omap_dss_device *dssdev)
{
	struct dispc_clock_info dispc_cinfo;
	int r;
	unsigned long long fck;

	fck = dsi_get_dsi1_pll_rate();

	dispc_cinfo.lck_div = dssdev->phy.dsi.div.lck_div;
	dispc_cinfo.pck_div = dssdev->phy.dsi.div.pck_div;

	r = dispc_calc_clock_rates(fck, &dispc_cinfo);
	if (r) {
		DSSERR("Failed to calc dispc clocks\n");
		return r;
	}

	r = dispc_set_clock_div(&dispc_cinfo);
	if (r) {
		DSSERR("Failed to set dispc clocks\n");
		return r;
	}

	return 0;
}

static int dsi_display_init_dsi(struct omap_dss_device *dssdev)
{
	int r;

	_dsi_print_reset_status();

	r = dsi_pll_init(dssdev, true, true);
	if (r)
		goto err0;

	r = dsi_configure_dsi_clocks(dssdev);
	if (r)
		goto err1;

	dss_select_dispc_clk_source(DSS_SRC_DSI1_PLL_FCLK);
	dss_select_dsi_clk_source(DSS_SRC_DSI2_PLL_FCLK);

	DSSDBG("PLL OK\n");

	r = dsi_configure_dispc_clocks(dssdev);
	if (r)
		goto err2;

	r = dsi_complexio_init(dssdev);
	if (r)
		goto err2;

	_dsi_print_reset_status();

	dsi_proto_timings(dssdev);
	dsi_set_lp_clk_divisor(dssdev);

	if (1)
		_dsi_print_reset_status();

	r = dsi_proto_config(dssdev);
	if (r)
		goto err3;

	/* enable interface */
	dsi_vc_enable(DSI_CMD_VC, 1);
	dsi_if_enable(1);
	dsi_force_tx_stop_mode_io();

	if (dssdev->driver->enable) {
		r = dssdev->driver->enable(dssdev);
		if (r)
			goto err4;
	}

	/* enable high-speed after initial config */
	dsi_vc_enable_hs(DSI_CMD_VC, 1);
	dsi_vc_enable(DSI_VIDEO_VC, 1);

	return 0;
err4:
	dsi_if_enable(0);
err3:
	dsi_complexio_uninit();
err2:
	dss_select_dispc_clk_source(DSS_SRC_DSS1_ALWON_FCLK);
	dss_select_dsi_clk_source(DSS_SRC_DSS1_ALWON_FCLK);
err1:
	dsi_pll_uninit();
err0:
	return r;
}

static void dsi_display_uninit_dsi(struct omap_dss_device *dssdev)
{
	if (dssdev->driver->disable)
		dssdev->driver->disable(dssdev);

	dss_select_dispc_clk_source(DSS_SRC_DSS1_ALWON_FCLK);
	dss_select_dsi_clk_source(DSS_SRC_DSS1_ALWON_FCLK);
	dsi_complexio_uninit();
	dsi_pll_uninit();
}

static int dsi_core_init(void)
{
	/* Autoidle */
	REG_FLD_MOD(DSI_SYSCONFIG, 1, 0, 0);

	/* ENWAKEUP */
	REG_FLD_MOD(DSI_SYSCONFIG, 1, 2, 2);

	/* SIDLEMODE smart-idle */
	REG_FLD_MOD(DSI_SYSCONFIG, 2, 4, 3);

	_dsi_initialize_irq();

	return 0;
}

static int dsi_display_enable(struct omap_dss_device *dssdev)
{
	int r = 0;

	DSSDBG("dsi_display_enable\n");

	mutex_lock(&dsi.lock);
	dsi_bus_lock();

	r = omap_dss_start_device(dssdev);
	if (r) {
		DSSERR("failed to start device\n");
		goto err0;
	}

	if (dssdev->state != OMAP_DSS_DISPLAY_DISABLED) {
		DSSERR("dssdev already enabled\n");
		r = -EINVAL;
		goto err1;
	}

	enable_clocks(1);
	dsi_enable_pll_clock(1);

#ifndef CONFIG_FB_OMAP_BOOTLOADER_INIT
	r = _dsi_reset();
	if (r)
		goto err2;
#else
	msleep(1);	/* wait for DSI stable before go on */
	dsi_vc_enable(DSI_CMD_VC, 0);
	dsi_vc_enable(DSI_VIDEO_VC, 0);
#endif


	dsi_core_init();

	r = dsi_display_init_dispc(dssdev);
	if (r)
		goto err2;

	r = dsi_display_init_dsi(dssdev);
	if (r)
		goto err3;

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	dsi.use_ext_te = dssdev->phy.dsi.ext_te;
	r = dsi_set_te(dssdev, dsi.te_enabled);
	if (r)
		goto err4;

	dsi_set_update_mode(dssdev, dsi.user_update_mode);

	enable_clocks(0);
	dsi_enable_pll_clock(0);
	dsi.error_recovery.enabled = true;

	dsi_bus_unlock();
	mutex_unlock(&dsi.lock);

	return 0;

err4:

	dsi_display_uninit_dsi(dssdev);
err3:
	dsi_display_uninit_dispc(dssdev);
err2:
	enable_clocks(0);
	dsi_enable_pll_clock(0);
err1:
	omap_dss_stop_device(dssdev);
err0:
	dsi_bus_unlock();
	mutex_unlock(&dsi.lock);
	DSSDBG("dsi_display_enable FAILED\n");
	return r;
}

static void dsi_display_disable(struct omap_dss_device *dssdev)
{
	DSSDBG("dsi_display_disable\n");

	dsi.error_recovery.enabled = false;
	cancel_work_sync(&dsi.error_recovery.work);

	mutex_lock(&dsi.lock);
	dsi_bus_lock();

	complete_all(&dsi.update_completion);

	if (dssdev->state == OMAP_DSS_DISPLAY_DISABLED ||
			dssdev->state == OMAP_DSS_DISPLAY_SUSPENDED)
		goto end;

	dsi.update_mode = OMAP_DSS_UPDATE_DISABLED;
	dssdev->state = OMAP_DSS_DISPLAY_DISABLED;

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	dsi_display_uninit_dispc(dssdev);

	dsi_display_uninit_dsi(dssdev);

	enable_clocks(0);
	dsi_enable_pll_clock(0);

	omap_dss_stop_device(dssdev);
end:
	dsi_bus_unlock();
	mutex_unlock(&dsi.lock);
}

static int dsi_do_display_suspend(struct omap_dss_device *dssdev)
{
	DSSDBG("dsi_do_display_suspend\n");

	complete_all(&dsi.update_completion);

	if (dssdev->state == OMAP_DSS_DISPLAY_DISABLED ||
			dssdev->state == OMAP_DSS_DISPLAY_SUSPENDED)
		return 0;

	dsi.update_mode = OMAP_DSS_UPDATE_DISABLED;
	dssdev->state = OMAP_DSS_DISPLAY_SUSPENDED;

	dsi.update_region.dirty = false;

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	dsi_display_uninit_dispc(dssdev);

	dsi_display_uninit_dsi(dssdev);

	enable_clocks(0);
	dsi_enable_pll_clock(0);

	return 0;
}

static int dsi_display_suspend(struct omap_dss_device *dssdev)
{
	mutex_lock(&dsi.lock);
	dsi_bus_lock();

	dsi.error_recovery.enabled = false;
	cancel_work_sync(&dsi.error_recovery.work);

	dsi_do_display_suspend(dssdev);

	dsi_bus_unlock();
	mutex_unlock(&dsi.lock);

	return 0;
}

static int dsi_do_display_resume(struct omap_dss_device *dssdev)
{
	int r;

	DSSDBG("dsi_do_display_resume\n");

	if (dssdev->state != OMAP_DSS_DISPLAY_SUSPENDED) {
		DSSDBG("dsi_do_display_resume FAILED\n");
		return 0;
	}

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	r = _dsi_reset();
	if (r)
		goto err1;

	dsi_core_init();

	r = dsi_display_init_dispc(dssdev);
	if (r)
		goto err1;

	r = dsi_display_init_dsi(dssdev);
	if (r)
		goto err2;

	dssdev->state = OMAP_DSS_DISPLAY_ACTIVE;

	r = dsi_set_te(dssdev, dsi.te_enabled);
	if (r)
		goto err2;

	dsi_set_update_mode(dssdev, dsi.user_update_mode);

	enable_clocks(0);
	dsi_enable_pll_clock(0);

	return 0;

err2:
	dsi_display_uninit_dispc(dssdev);
err1:
	enable_clocks(0);
	dsi_enable_pll_clock(0);
	return r;
}

static int dsi_display_resume(struct omap_dss_device *dssdev)
{
	int r;

	mutex_lock(&dsi.lock);
	dsi_bus_lock();

	r = dsi_do_display_resume(dssdev);
	if (r == 0)
		dsi.error_recovery.enabled = true;

	dsi_bus_unlock();
	mutex_unlock(&dsi.lock);
	return r;
}

static int dsi_display_update(struct omap_dss_device *dssdev,
			u16 x, u16 y, u16 w, u16 h)
{
	int r = 0;
	u16 dw, dh;
#if PRINT_FPS
        int64_t dt;
        ktime_t now;
        static int64_t frame_count;
        static ktime_t last_sec;
#endif


	DSSDBG("dsi_display_update(%d,%d %dx%d)\n", x, y, w, h);

	mutex_lock(&dsi.lock);

	if (dsi.update_mode != OMAP_DSS_UPDATE_MANUAL)
		goto end;

	if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE)
		goto end;

	dssdev->get_resolution(dssdev, &dw, &dh);

	if  (x > dw || y > dh)
		goto end;

	if (x + w > dw)
		w = dw - x;

	if (y + h > dh)
		h = dh - y;

	if (w == 0 || h == 0)
		goto end;

	if (w == 1) {
		r = -EINVAL;
		goto end;
	}

	dsi_set_update_region(dssdev, x, y, w, h);

	wake_up(&dsi.waitqueue);

end:
	mutex_unlock(&dsi.lock);

#if PRINT_FPS
	if (dsi_fps) {
		now = ktime_get();
		dt = ktime_to_ns(ktime_sub(now, last_sec));
		frame_count++;
		if (dt > NSEC_PER_SEC) {
			int64_t fps = frame_count * NSEC_PER_SEC * 100;
			frame_count = 0;
			last_sec = ktime_get();
			do_div(fps, dt);
			printk(KERN_INFO "fps * 100: %llu\n", fps);
		}
	}
#endif
	return r;
}

static int dsi_display_sync(struct omap_dss_device *dssdev)
{
	bool wait;

	DSSDBG("dsi_display_sync()\n");

	mutex_lock(&dsi.lock);
	dsi_bus_lock();

	if (dsi.update_mode == OMAP_DSS_UPDATE_MANUAL &&
			dsi.update_region.dirty) {
		INIT_COMPLETION(dsi.update_completion);
		wait = true;
	} else {
		wait = false;
	}

	dsi_bus_unlock();
	mutex_unlock(&dsi.lock);

	if (wait)
		wait_for_completion_interruptible(&dsi.update_completion);

	DSSDBG("dsi_display_sync() done\n");
	return 0;
}

static int dsi_display_set_update_mode(struct omap_dss_device *dssdev,
		enum omap_dss_update_mode mode)
{
	int r = 0;

	DSSDBGF("%d", mode);

	mutex_lock(&dsi.lock);
	dsi_bus_lock();

	dsi.user_update_mode = mode;
	r = dsi_set_update_mode(dssdev, mode);

	dsi_bus_unlock();
	mutex_unlock(&dsi.lock);

	return r;
}

static enum omap_dss_update_mode dsi_display_get_update_mode(
		struct omap_dss_device *dssdev)
{
	return dsi.update_mode;
}


static int dsi_display_enable_te(struct omap_dss_device *dssdev, bool enable)
{
	int r = 0;

	DSSDBGF("%d", enable);

	if (!dssdev->driver->enable_te)
		return -ENOENT;

	dsi_bus_lock();

	enable_clocks(1);
	dsi_enable_pll_clock(1);

	dsi.te_enabled = enable;

	if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE)
		goto end;

	r = dsi_set_te(dssdev, enable);
end:
	enable_clocks(0);
	dsi_enable_pll_clock(0);

	dsi_bus_unlock();

	return r;
}

static int dsi_display_get_te(struct omap_dss_device *dssdev)
{
	return dsi.te_enabled;
}

static bool dsi_sw_te_sup(struct omap_dss_device *dssdev)
{
	if (dssdev->driver->sw_te_sup)
		dsi.sw_te_sup = dssdev->driver->sw_te_sup(dssdev);
	else
		dsi.sw_te_sup = false;

	return dsi.sw_te_sup;
}

static int dsi_display_set_rotate(struct omap_dss_device *dssdev, u8 rotate)
{

	DSSDBGF("%d", rotate);

	if (!dssdev->driver->set_rotate || !dssdev->driver->get_rotate)
		return -EINVAL;

	dsi_bus_lock();
	dssdev->driver->set_rotate(dssdev, rotate);
	if (dsi.update_mode == OMAP_DSS_UPDATE_AUTO) {
		u16 w, h;
		/* the display dimensions may have changed, so set a new
		 * update region */
		dssdev->get_resolution(dssdev, &w, &h);
		dsi_set_update_region(dssdev, 0, 0, w, h);
	}
	dsi_bus_unlock();

	return 0;
}

static u8 dsi_display_get_rotate(struct omap_dss_device *dssdev)
{
	if (!dssdev->driver->set_rotate || !dssdev->driver->get_rotate)
		return 0;

	return dssdev->driver->get_rotate(dssdev);
}

static int dsi_display_set_mirror(struct omap_dss_device *dssdev, bool mirror)
{
	DSSDBGF("%d", mirror);

	if (!dssdev->driver->set_mirror || !dssdev->driver->get_mirror)
		return -EINVAL;

	dsi_bus_lock();
	dssdev->driver->set_mirror(dssdev, mirror);
	dsi_bus_unlock();

	return 0;
}

static bool dsi_display_get_mirror(struct omap_dss_device *dssdev)
{
	if (!dssdev->driver->set_mirror || !dssdev->driver->get_mirror)
		return 0;

	return dssdev->driver->get_mirror(dssdev);
}

static int dsi_display_run_test(struct omap_dss_device *dssdev, int test_num)
{
	int r;
	int channel = DSI_CMD_VC;

	if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE)
		return -EIO;

	DSSDBGF("%d", test_num);

	dsi_bus_lock();

	/* run test first in low speed mode */
	dsi_vc_enable_hs(channel, 0);

	if (dssdev->driver->run_test) {
		r = dssdev->driver->run_test(dssdev, test_num);
		if (r)
			goto end;
	}

	/* then in high speed */
	dsi_vc_enable_hs(channel, 1);

	if (dssdev->driver->run_test) {
		r = dssdev->driver->run_test(dssdev, test_num);
		if (r)
			goto end;
	}

end:
	dsi_vc_enable_hs(channel, 1);

	dsi_bus_unlock();

	return r;
}

static int dsi_display_memory_read(struct omap_dss_device *dssdev,
		void *buf, size_t size,
		u16 x, u16 y, u16 w, u16 h)
{
	int r;

	DSSDBGF("");

	if (!dssdev->driver->memory_read)
		return -EINVAL;

	if (dssdev->state != OMAP_DSS_DISPLAY_ACTIVE)
		return -EIO;

	dsi_bus_lock();

	r = dssdev->driver->memory_read(dssdev, buf, size,
			x, y, w, h);

	/* Memory read usually changes the update area. This will
	 * force the next update to re-set the update area */
	dsi.active_update_region.dirty = true;

	dsi_bus_unlock();

	return r;
}

void dsi_get_overlay_fifo_thresholds(enum omap_plane plane,
		u32 fifo_size, enum omap_burst_size *burst_size,
		u32 *fifo_low, u32 *fifo_high)
{
	unsigned burst_size_bytes;

	*burst_size = OMAP_DSS_BURST_16x32;
	burst_size_bytes = 16 * 32 / 8;

	*fifo_high = fifo_size - burst_size_bytes;
	*fifo_low = fifo_size - burst_size_bytes * 8;
}

int dsi_init_display(struct omap_dss_device *dssdev)
{
	DSSDBG("DSI init\n");

	dssdev->enable = dsi_display_enable;
	dssdev->disable = dsi_display_disable;
	dssdev->suspend = dsi_display_suspend;
	dssdev->resume = dsi_display_resume;
	dssdev->update = dsi_display_update;
	dssdev->sync = dsi_display_sync;
	dssdev->set_update_mode = dsi_display_set_update_mode;
	dssdev->get_update_mode = dsi_display_get_update_mode;
	dssdev->enable_te = dsi_display_enable_te;
	dssdev->get_te = dsi_display_get_te;
	dssdev->sw_te_sup = dsi_sw_te_sup;

	dssdev->get_rotate = dsi_display_get_rotate;
	dssdev->set_rotate = dsi_display_set_rotate;

	dssdev->get_mirror = dsi_display_get_mirror;
	dssdev->set_mirror = dsi_display_set_mirror;

	dssdev->run_test = dsi_display_run_test;
	dssdev->memory_read = dsi_display_memory_read;

	/* XXX these should be figured out dynamically */
	dssdev->caps = OMAP_DSS_DISPLAY_CAP_MANUAL_UPDATE |
		OMAP_DSS_DISPLAY_CAP_TEAR_ELIM;

	dsi.vc[0].dssdev = dssdev;
	dsi.vc[1].dssdev = dssdev;

	dsi.error_recovery.dssdev = dssdev;

	return 0;
}

static void dsi_error_recovery_worker(struct work_struct *work)
{
	u32 r;
	struct omap_dss_device *dssdev = dsi.error_recovery.dssdev;

	DSSERR("DSI error, ESD detected\n");

	mutex_lock(&dsi.lock);

	if (!dsi.error_recovery.enabled)
		goto end;

	dsi_bus_lock();

	dsi.error_recovery.recovering = true;

	enable_clocks(1);
	dsi_enable_pll_clock(1);
	dsi_force_tx_stop_mode_io();

	r = dsi_read_reg(DSI_TIMING1);
	r = FLD_MOD(r, 0, 15, 15);	/* FORCE_TX_STOP_MODE_IO */
	dsi_write_reg(DSI_TIMING1, r);

	dsi_vc_enable(DSI_CMD_VC, 0);
	dsi_vc_enable(DSI_VIDEO_VC, 0);

	dsi_if_enable(0);

	dsi_vc_enable(DSI_CMD_VC, 1);
	dsi_vc_enable(DSI_VIDEO_VC, 1);

	dsi_if_enable(1);

	dsi_force_tx_stop_mode_io();

	enable_clocks(0);
	dsi_enable_pll_clock(0);

	/* Now check to ensure there is communication. */
	/* If not, we need to hard reset */
	if (dssdev->driver->run_test) {
		if (dssdev->driver->run_test(dssdev, 1) != 0) {
			DSSERR("DSS IF reset failed, resetting panel\n");

			dsi_do_display_suspend(dssdev);
			dsi_do_display_resume(dssdev);
		}
	}

	dsi.error_recovery.recovering = false;

	dsi_bus_unlock();

end:
	mutex_unlock(&dsi.lock);
}

static enum hrtimer_restart dsi_sw_te_hrtimer_func(struct hrtimer *hrtimer)
{
	REG_FLD_MOD(DSI_VC_TE(DSI_VIDEO_VC), 1, 31, 31);
	return HRTIMER_NORESTART;
}

int dsi_init(struct platform_device *pdev)
{
	u32 rev;
	int r;

	spin_lock_init(&dsi.errors_lock);
	dsi.errors = 0;

	init_completion(&dsi.bta_completion);
	init_completion(&dsi.update_completion);
	init_completion(&dsi.packet_sent_completion);

	hrtimer_init(&dsi.hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	dsi.hrtimer.function = dsi_sw_te_hrtimer_func;

	dsi.thread = kthread_create(dsi_update_thread, NULL, "dsi");
	if (IS_ERR(dsi.thread)) {
		DSSERR("cannot create kthread\n");
		r = PTR_ERR(dsi.thread);
		goto err0;
	}

	init_waitqueue_head(&dsi.waitqueue);
	spin_lock_init(&dsi.update_lock);

	mutex_init(&dsi.lock);
	sema_init(&dsi.bus_lock, 1);

#ifdef DSI_CATCH_MISSING_TE
	init_timer(&dsi.te_timer);
	dsi.te_timer.function = dsi_te_timeout;
	dsi.te_timer.data = 0;
#endif

	dsi.error_recovery.dssdev = 0;
	dsi.error_recovery.enabled = false;
	dsi.error_recovery.recovering = false;
	INIT_WORK(&dsi.error_recovery.work, dsi_error_recovery_worker);

	dsi.update_mode = OMAP_DSS_UPDATE_DISABLED;
	dsi.user_update_mode = OMAP_DSS_UPDATE_DISABLED;

	dsi.base = ioremap(DSI_BASE, DSI_SZ_REGS);
	if (!dsi.base) {
		DSSERR("can't ioremap DSI\n");
		r = -ENOMEM;
		goto err1;
	}

	dsi.vdds_dsi_reg = regulator_get(&pdev->dev, "vdds_dsi");
	if (IS_ERR(dsi.vdds_dsi_reg)) {
		iounmap(dsi.base);
		DSSERR("can't get VDDS_DSI regulator\n");
		r = PTR_ERR(dsi.vdds_dsi_reg);
		goto err2;
	}

	enable_clocks(1);

	rev = dsi_read_reg(DSI_REVISION);
	printk(KERN_INFO "OMAP DSI rev %d.%d\n",
	       FLD_GET(rev, 7, 4), FLD_GET(rev, 3, 0));

	enable_clocks(0);

	wake_up_process(dsi.thread);

	return 0;
err2:
	iounmap(dsi.base);
err1:
	kthread_stop(dsi.thread);
err0:
	return r;
}

void dsi_exit(void)
{
	kthread_stop(dsi.thread);

	regulator_put(dsi.vdds_dsi_reg);

	iounmap(dsi.base);

	DSSDBG("omap_dsi_exit\n");
}
