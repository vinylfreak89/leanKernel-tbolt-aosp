/*
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2007-2009, Code Aurora Forum. All rights reserved.
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

// 1 = lean, 2 = 246Mhz
#define SLEVEL 2

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/mutex.h>
#include <linux/io.h>
#include <linux/sort.h>
#include <mach/board.h>
#include <mach/msm_iomap.h>
#include <asm/mach-types.h>
#include <mach/acpuclock_debug.h>

#include "smd_private.h"
#include "clock.h"
#include "acpuclock.h"
#include "socinfo.h"
#include "spm.h"

#define SCSS_CLK_CTL_ADDR	(MSM_ACC_BASE + 0x04)
#define SCSS_CLK_SEL_ADDR	(MSM_ACC_BASE + 0x08)

#define MIN_UV_MV 750
#define MAX_UV_MV 1475

#define dprintk(msg...) \
	cpufreq_debug_printk(CPUFREQ_DEBUG_DRIVER, "cpufreq-msm", msg)

#define VREF_SEL     1	/* 0: 0.625V (50mV step), 1: 0.3125V (25mV step). */
#define V_STEP       (25 * (2 - VREF_SEL)) /* Minimum voltage step size. */
#define VREG_DATA    (VREG_CONFIG | (VREF_SEL << 5))
#define VREG_CONFIG  (BIT(7) | BIT(6)) /* Enable VREG, pull-down if disabled. */
/* Cause a compile error if the voltage is not a multiple of the step size. */
#define MV(mv)      ((mv) / (!((mv) % V_STEP)))
/* mv = (750mV + (raw * 25mV)) * (2 - VREF_SEL) */
#define VDD_RAW(mv) (((MV(mv) / V_STEP) - 30) | VREG_DATA)

#define MAX_AXI_KHZ 192000

#define PLL2_L_VAL_ADDR  (MSM_CLK_CTL_BASE + 0x33c)

struct clock_state {
	struct clkctl_acpu_speed	*current_speed;
	struct mutex			lock;
	uint32_t			acpu_switch_time_us;
	uint32_t			vdd_switch_time_us;
	unsigned long			wait_for_irq_khz;
	int				wfi_ramp_down;
	int				pwrc_ramp_down;
};

struct clkctl_acpu_speed {
	unsigned int	acpu_clk_khz;
	int		src;
	unsigned int	acpu_src_sel;
	unsigned int	acpu_src_div;
	unsigned int	axi_clk_khz;
	unsigned int	vdd_mv;
	unsigned int	vdd_raw;
	unsigned long	lpj; /* loops_per_jiffy */
};

static struct clock_state drv_state = { 0 };

static struct cpufreq_frequency_table freq_table[] = {
#if SLEVEL == 1
        { 0, 184320 },
        { 1, 245760 },
        { 2, 368640 },
        { 3, 768000 },
        { 4, 1024000 },
        { 5, 1222400 },
        { 6, 1408000 },
        { 7, 1593600 },
        { 8, 1766400 },
        { 9, 1920000 },
        { 10, CPUFREQ_TABLE_END },
#else 
        { 0, 245760 },
        { 1, 368640 },
        { 2, 768000 },
        { 3, 1024000 },
        { 4, 1222400 },
        { 5, 1408000 },
        { 6, 1593600 },
        { 7, 1766400 },
        { 8, 1920000 },
        { 9, CPUFREQ_TABLE_END },
#endif
};

/* Use negative numbers for sources that can't be enabled/disabled */
#define SRC_LPXO (-2)
#define SRC_AXI  (-1)
static struct clkctl_acpu_speed acpu_freq_tbl[] = {
/*
	{ 24576,  SRC_LPXO, 0, 0,  30720,  1000, VDD_RAW(1000) },
	{ 61440,  PLL_3,    5, 11, 61440,  1000, VDD_RAW(1000) },
	{ 122880, PLL_3,    5, 5,  61440,  1000, VDD_RAW(1000) },
//	{ 184320, PLL_3,    5, 4,  61440,  1000, VDD_RAW(1000) },
	{ MAX_AXI_KHZ, SRC_AXI, 1, 0, 61440, 1000, VDD_RAW(1000) },
*/

#if SLEVEL == 1
	{ 184320, PLL_3,    5, 4,  61440,  900, VDD_RAW(900) },
#endif
	{ 245760, PLL_3,    5, 2,  61440,  975, VDD_RAW(975) },
        { 368640, PLL_3,    5, 1,  122800, 1025, VDD_RAW(1025) },
	{ 768000, PLL_1,    2, 0,  153600, 1075, VDD_RAW(1075) },
	{ 1024000, PLL_2,    3, 0,  192000, 1100, VDD_RAW(1100) },
	{ 1222400, PLL_2,    3, 0,  192000, 1150, VDD_RAW(1150) },
	{ 1408000, PLL_2,    3, 0,  192000, 1250, VDD_RAW(1250) },
	{ 1593600, PLL_2,   3, 0,  192000, 1325, VDD_RAW(1325) },
	{ 1766400, PLL_2,   3, 0,  192000, 1375, VDD_RAW(1375) },
        { 1920000, PLL_2,   3, 0,  192000, 1450, VDD_RAW(1450) },
	{ 0 }
};
static unsigned long max_axi_rate;

#define POWER_COLLAPSE_HZ (MAX_AXI_KHZ * 1000)
unsigned long acpuclk_power_collapse(int from_idle)
{
	int ret = acpuclk_get_rate();
	if (drv_state.pwrc_ramp_down)
		acpuclk_set_rate(POWER_COLLAPSE_HZ, SETRATE_PC);
	return ret * 1000;
}

unsigned long acpuclk_get_wfi_rate(void)
{
	return drv_state.wait_for_irq_khz * 1000;
}

#define WAIT_FOR_IRQ_HZ (MAX_AXI_KHZ * 1000)
unsigned long acpuclk_wait_for_irq(void)
{
	int ret = acpuclk_get_rate();
	if (drv_state.wfi_ramp_down)
		acpuclk_set_rate(WAIT_FOR_IRQ_HZ, SETRATE_SWFI);
	return ret * 1000;
}

#ifdef CONFIG_HTC_SMEM_MSMC1C2_DEBUG
/* mARM */
#define HTC_SMEM_MSMC1		(MSM_SHARED_RAM_BASE + 0x000F8000)
#define HTC_SMEM_AXI_SPEED	(MSM_SHARED_RAM_BASE + 0x000F8004)
/* aARM */
#define HTC_SMEM_MSMC2_STAT	(MSM_SHARED_RAM_BASE + 0x000F8100)
#define HTC_SMEM_MSMC2_LAST	(MSM_SHARED_RAM_BASE + 0x000F8104)
#define HTC_SMEM_MSMC2_CURR	(MSM_SHARED_RAM_BASE + 0x000F8108)
#define HTC_SMEM_MSMC2_MSMC1	(MSM_SHARED_RAM_BASE + 0x000F810C)
#define HTC_SMEM_MSMC2_AXI	(MSM_SHARED_RAM_BASE + 0x000F8110)
#endif

static int acpuclk_set_acpu_vdd(struct clkctl_acpu_speed *s)
{
#ifdef CONFIG_HTC_SMEM_MSMC1C2_DEBUG
	unsigned int smem_val;
	int ret;

	/* store current MSMC1 and AXI */
	smem_val = readl(HTC_SMEM_MSMC1);
	writel(smem_val, HTC_SMEM_MSMC2_MSMC1);
	smem_val = readl(HTC_SMEM_AXI_SPEED);
	writel(smem_val, HTC_SMEM_MSMC2_AXI);

	/* store last and current(target) MSMC2 */
	smem_val = readl(HTC_SMEM_MSMC2_CURR);
	if (smem_val)
		writel(smem_val, HTC_SMEM_MSMC2_LAST);
	writel(s->vdd_mv, HTC_SMEM_MSMC2_CURR);

	writel(0x11112222, HTC_SMEM_MSMC2_STAT);
	ret = msm_spm_set_vdd(0,s->vdd_raw);
	if (ret)
		return ret;
	/* HTC_SMEM_MSMC2_CURR is valid only when STAT:0x33334444 */
	writel(0x33334444, HTC_SMEM_MSMC2_STAT);
#else
	int ret = msm_spm_set_vdd(0, s->vdd_raw);
	if (ret)
		return ret;
#endif

	/* Wait for voltage to stabilize. */
	udelay(drv_state.vdd_switch_time_us);
	return 0;
}

/* Set clock source and divider given a clock speed */
static void acpuclk_set_src(const struct clkctl_acpu_speed *s)
{
	uint32_t reg_clksel, reg_clkctl, src_sel;
	unsigned int lval;

	reg_clksel = readl(SCSS_CLK_SEL_ADDR);

	/* CLK_SEL_SRC1NO */
	src_sel = reg_clksel & 1;

	/* Program clock source and divider. */
	reg_clkctl = readl(SCSS_CLK_CTL_ADDR);
	reg_clkctl &= ~(0xFF << (8 * src_sel));
	reg_clkctl |= s->acpu_src_sel << (4 + 8 * src_sel);
	reg_clkctl |= s->acpu_src_div << (0 + 8 * src_sel);
	writel(reg_clkctl, SCSS_CLK_CTL_ADDR);

	/* Program PLL2 L val for overclocked speeds. */
	if(s->src == PLL_2) {
		lval = s->acpu_clk_khz/19200;	
                // cheat a bit here clock down one when high freq
                if (s->acpu_clk_khz > 1500000) lval--;
		writel(lval, PLL2_L_VAL_ADDR);
	}

	/* Toggle clock source. */
	reg_clksel ^= 1;

	/* Program clock source selection. */
	writel(reg_clksel, SCSS_CLK_SEL_ADDR);
}

static struct clk *ebi1_clk;

int acpuclk_set_rate(unsigned long rate, enum setrate_reason reason)
{
	struct clkctl_acpu_speed *tgt_s, *strt_s;
	int res, rc = 0;

	if (reason == SETRATE_CPUFREQ)
		mutex_lock(&drv_state.lock);

	strt_s = drv_state.current_speed;

	if (rate == (strt_s->acpu_clk_khz * 1000))
		goto out;

	for (tgt_s = acpu_freq_tbl; tgt_s->acpu_clk_khz != 0; tgt_s++) {
		if (tgt_s->acpu_clk_khz == (rate / 1000))
			break;
	}
	if (tgt_s->acpu_clk_khz == 0) {
		rc = -EINVAL;
		goto out;
	}

	if (reason == SETRATE_CPUFREQ) {
		/* Increase VDD if needed. */
		if (tgt_s->vdd_mv > strt_s->vdd_mv) {
			rc = acpuclk_set_acpu_vdd(tgt_s);
			if (rc < 0) {
				pr_err("ACPU VDD increase to %d mV failed "
					"(%d)\n", tgt_s->vdd_mv, rc);
				goto out;
			}
		}
	}

	dprintk("Switching from ACPU rate %u KHz -> %u KHz\n",
	       strt_s->acpu_clk_khz, tgt_s->acpu_clk_khz);

	/* Increase the AXI bus frequency if needed. This must be done before
	 * increasing the ACPU frequency, since voting for high AXI rates
	 * implicitly takes care of increasing the MSMC1 voltage, as needed. */
	if (tgt_s->axi_clk_khz > strt_s->axi_clk_khz) {
		res = clk_set_rate(ebi1_clk, tgt_s->axi_clk_khz * 1000);
		if (rc < 0) {
			pr_err("Setting AXI min rate failed (%d)\n", rc);
			goto out;
		}
	}

	/* Make sure target PLL is on. */
	if (strt_s->src != tgt_s->src && tgt_s->src >= 0) {
		dprintk("Enabling PLL %d\n", tgt_s->src);
		pll_enable(tgt_s->src);
	}

	/* Perform the frequency switch */
	acpuclk_set_src(tgt_s);
	drv_state.current_speed = tgt_s;
	loops_per_jiffy = tgt_s->lpj;

	/* Nothing else to do for SWFI. */
	if (reason == SETRATE_SWFI)
		goto out;

	/* Turn off previous PLL if not used. */
	if (strt_s->src != tgt_s->src && strt_s->src >= 0) {
		dprintk("Disabling PLL %d\n", strt_s->src);
		pll_disable(strt_s->src);
	}

	/* Decrease the AXI bus frequency if we can. */
	if (tgt_s->axi_clk_khz < strt_s->axi_clk_khz) {
		res = clk_set_rate(ebi1_clk, tgt_s->axi_clk_khz * 1000);
		if (res < 0)
			pr_warning("Setting AXI min rate failed (%d)\n", res);
	}

	/* Nothing else to do for power collapse. */
	if (reason == SETRATE_PC)
		goto out;

	/* Drop VDD level if we can. */
	if (tgt_s->vdd_mv < strt_s->vdd_mv) {
		res = acpuclk_set_acpu_vdd(tgt_s);
		if (res < 0) {
			pr_warning("ACPU VDD decrease to %d mV failed (%d)\n",
					tgt_s->vdd_mv, res);
		}
	}

	dprintk("ACPU speed change complete\n");
out:
	if (reason == SETRATE_CPUFREQ)
		mutex_unlock(&drv_state.lock);

	return rc;
}

unsigned long acpuclk_get_max_axi_rate(void)
{
	return max_axi_rate;
}
EXPORT_SYMBOL(acpuclk_get_max_axi_rate);

unsigned long acpuclk_get_rate(void)
{
	WARN_ONCE(drv_state.current_speed == NULL,
		  "acpuclk_get_rate: not initialized\n");
	if (drv_state.current_speed)
		return drv_state.current_speed->acpu_clk_khz;
	else
		return 0;
}

uint32_t acpuclk_get_switch_time(void)
{
	return drv_state.acpu_switch_time_us;
}

unsigned long clk_get_max_axi_khz(void)
{
	return MAX_AXI_KHZ;
}
EXPORT_SYMBOL(clk_get_max_axi_khz);

static void acpuclk_set_wfi_ramp_down(int enable)
{
	drv_state.wfi_ramp_down = enable;
}

static void acpuclk_set_pwrc_ramp_down(int enable)
{
	drv_state.pwrc_ramp_down = enable;
}

static int acpuclk_get_wfi_ramp_down(void)
{
	return drv_state.wfi_ramp_down;
}

static int acpuclk_get_pwrc_ramp_down(void)
{
	return drv_state.pwrc_ramp_down;
}

static unsigned int acpuclk_get_current_vdd(void)
{
	unsigned int vdd_raw;
	unsigned int vdd_mv;

	vdd_raw = msm_spm_get_vdd();
	for (vdd_mv = MIN_UV_MV; vdd_mv <= MAX_UV_MV; vdd_mv += 25)
		if (VDD_RAW(vdd_mv) == vdd_raw)
			break;

	if (vdd_mv > MAX_UV_MV)
		return 0;

	return vdd_mv;
}

static int acpuclk_update_freq_tbl(unsigned int acpu_khz, unsigned int acpu_vdd)
{
	struct clkctl_acpu_speed *s;

	/* Check frequency table for matching sel/div pair. */
	for (s = acpu_freq_tbl; s->acpu_clk_khz != 0; s++) {
		if (s->acpu_clk_khz == acpu_khz)
			break;
	}
	if (s->acpu_clk_khz == 0) {
		pr_err("%s: acpuclk invalid speed %d\n", __func__, acpu_khz);
		return -1;
	}
	if (acpu_vdd > MAX_UV_MV || acpu_vdd < MIN_UV_MV) {
		pr_err("%s: acpuclk vdd out of ranage, %d\n",
			__func__, acpu_vdd);
		return -2;
	}

	s->vdd_mv = acpu_vdd;
	s->vdd_raw = VDD_RAW(acpu_vdd);
	if (drv_state.current_speed->acpu_clk_khz == acpu_khz)
		return acpuclk_set_acpu_vdd(s);

	return 0;
}

static struct acpuclock_debug_dev acpu_debug_7x30 = {
	.name = "acpu-7x30",
	.set_wfi_ramp_down = acpuclk_set_wfi_ramp_down,
	.set_pwrc_ramp_down = acpuclk_set_pwrc_ramp_down,
	.get_wfi_ramp_down = acpuclk_get_wfi_ramp_down,
	.get_pwrc_ramp_down = acpuclk_get_pwrc_ramp_down,
	.get_current_vdd = acpuclk_get_current_vdd,
	.update_freq_tbl = acpuclk_update_freq_tbl,
};

/*----------------------------------------------------------------------------
 * Clock driver initialization
 *---------------------------------------------------------------------------*/

static void __init acpuclk_init(void)
{
	struct clkctl_acpu_speed *s;
	uint32_t div, sel, src_num;
	uint32_t reg_clksel, reg_clkctl;
	int res;

	reg_clksel = readl(SCSS_CLK_SEL_ADDR);

	/* Determine the ACPU clock rate. */
	switch ((reg_clksel >> 1) & 0x3) {
	case 0:	/* Running off the output of the raw clock source mux. */
		reg_clkctl = readl(SCSS_CLK_CTL_ADDR);
		src_num = reg_clksel & 0x1;
		sel = (reg_clkctl >> (12 - (8 * src_num))) & 0x7;
		div = (reg_clkctl >> (8 -  (8 * src_num))) & 0xF;

		/* Check frequency table for matching sel/div pair. */
		for (s = acpu_freq_tbl; s->acpu_clk_khz != 0; s++) {
			if (s->acpu_src_sel == sel && s->acpu_src_div == div)
				break;
		}
		if (s->acpu_clk_khz == 0) {
			pr_err("Error - ACPU clock reports invalid speed\n");
			return;
		}
		break;
	case 2:	/* Running off of the SCPLL selected through the core mux. */
		/* Switch to run off of the SCPLL selected through the raw
		 * clock source mux. */
		for (s = acpu_freq_tbl; s->acpu_clk_khz != 0
			&& s->src != PLL_2 && s->acpu_src_div == 0; s++)
			;
		if (s->acpu_clk_khz != 0) {
			/* Program raw clock source mux. */
			acpuclk_set_src(s);

			/* Switch to raw clock source input of the core mux. */
			reg_clksel = readl(SCSS_CLK_SEL_ADDR);
			reg_clksel &= ~(0x3 << 1);
			writel(reg_clksel, SCSS_CLK_SEL_ADDR);
			break;
		}
		/* else fall through */
	default:
		pr_err("Error - ACPU clock reports invalid source\n");
		return;
	}

	// start at 1Ghz
	if (s->acpu_clk_khz < 1000000) s++;

	/* Set initial ACPU VDD. */
	acpuclk_set_acpu_vdd(s);

	drv_state.current_speed = s;

	/* Initialize current PLL's reference count. */
	if (s->src >= 0)
		pll_enable(s->src);

	ebi1_clk = clk_get(NULL, "ebi1_clk");
	BUG_ON(ebi1_clk == NULL);

	res = clk_set_rate(ebi1_clk, s->axi_clk_khz * 1000);
	if (res < 0)
		pr_warning("Setting AXI min rate failed!\n");

	pr_info("ACPU running at %d KHz\n", s->acpu_clk_khz);

	s = acpu_freq_tbl + ARRAY_SIZE(acpu_freq_tbl) - 2;
	max_axi_rate = s->axi_clk_khz * 1000;
	return;
}

/* Initalize the lpj field in the acpu_freq_tbl. */
static void __init lpj_init(void)
{
	int i;
	const struct clkctl_acpu_speed *base_clk = drv_state.current_speed;

	for (i = 0; acpu_freq_tbl[i].acpu_clk_khz; i++) {
		acpu_freq_tbl[i].lpj = cpufreq_scale(loops_per_jiffy,
						base_clk->acpu_clk_khz,
						acpu_freq_tbl[i].acpu_clk_khz);
	}
}

/* Update frequency tables for a 1024MHz PLL2. */
/*
void __init pll2_1024mhz_fixup(void)
{
	if (acpu_freq_tbl[ARRAY_SIZE(acpu_freq_tbl)-2].acpu_clk_khz != 806400
		  || freq_table[ARRAY_SIZE(freq_table)-2].frequency != 806400) {
		pr_err("Frequency table fixups for PLL2 rate failed.\n");
		BUG();
	}
	acpu_freq_tbl[ARRAY_SIZE(acpu_freq_tbl)-2].acpu_clk_khz = 1100000;
	freq_table[ARRAY_SIZE(freq_table)-2].frequency = 1100000;
}
*/

#define RPM_BYPASS_MASK	(1 << 3)
#define PMIC_MODE_MASK	(1 << 4)
void __init msm_acpu_clock_init(struct msm_acpu_clock_platform_data *clkdata)
{
	pr_info("acpu_clock_init()\n");

	mutex_init(&drv_state.lock);
	drv_state.acpu_switch_time_us = clkdata->acpu_switch_time_us;
	drv_state.vdd_switch_time_us = clkdata->vdd_switch_time_us;
	drv_state.wfi_ramp_down = 1;
	drv_state.pwrc_ramp_down = 1;
	/* PLL2 runs at 1024MHz for MSM8x55. */
/*
	if (cpu_is_msm8x55())
		pll2_1024mhz_fixup();
*/
	acpuclk_init();
	lpj_init();

	cpufreq_frequency_table_get_attr(freq_table, smp_processor_id());
	register_acpuclock_debug_dev(&acpu_debug_7x30);
}

#ifdef CONFIG_CPU_FREQ_VDD_LEVELS

ssize_t acpuclk_get_vdd_levels_str(char *buf)
{
	int i, len = 0;
	if (buf)
	{
		mutex_lock(&drv_state.lock);
		for (i = 0; acpu_freq_tbl[i].acpu_clk_khz; i++)
		{
			len += sprintf(buf + len, "%8u: %4d\n", acpu_freq_tbl[i].acpu_clk_khz, acpu_freq_tbl[i].vdd_mv);
		}
		mutex_unlock(&drv_state.lock);
	}
	return len;
}

void acpuclk_set_vdd(unsigned int khz, int vdd)
{
	int i;
	unsigned int new_vdd;
// wtf	vdd = vdd / V_STEP * V_STEP;
	mutex_lock(&drv_state.lock);
	for (i = 0; acpu_freq_tbl[i].acpu_clk_khz; i++)
	{
		if (khz == 0)
			new_vdd = min(max((acpu_freq_tbl[i].vdd_mv + (unsigned int)vdd), (unsigned int)MIN_UV_MV), (unsigned int)MAX_UV_MV);
		else if (acpu_freq_tbl[i].acpu_clk_khz == khz)
			new_vdd = min(max((unsigned int)vdd, (unsigned int)MIN_UV_MV), (unsigned int)MAX_UV_MV);
		else continue;

		acpu_freq_tbl[i].vdd_mv = new_vdd;
		acpu_freq_tbl[i].vdd_raw = VDD_RAW(new_vdd);
	}
	mutex_unlock(&drv_state.lock);
}

#endif
