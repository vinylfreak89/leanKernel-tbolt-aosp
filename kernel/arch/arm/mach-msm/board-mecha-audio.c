/* linux/arch/arm/mach-msm/board-mecha-audio.c
 *
 * Copyright (C) 2009 Google, Inc.
 * Copyright (C) 2010 HTC Corporation.
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
#include <linux/android_pmem.h>
#include <linux/mfd/pmic8058.h>
#include <linux/mfd/marimba.h>
#include <linux/delay.h>
#include <mach/gpio.h>
#include <mach/dal.h>
#include "board-mecha.h"
#include <mach/tpa2051d3.h>

#include <mach/qdsp5v2/snddev_icodec.h>
#include <mach/qdsp5v2/snddev_ecodec.h>
#include <mach/qdsp5v2/audio_def.h>
#include <mach/qdsp5v2/voice.h>
#include <mach/htc_acoustic_7x30.h>
#include <mach/htc_acdb_7x30.h>
#include <linux/spi/spi_aic3254.h>

static struct mutex bt_sco_lock;
static int curr_rx_mode;
static atomic_t aic3254_ctl = ATOMIC_INIT(0);
void mecha_back_mic_enable(int);

#define BIT_SPEAKER	(1 << 0)
#define BIT_HEADSET	(1 << 1)
#define BIT_RECEIVER	(1 << 2)
#define BIT_FM_SPK	(1 << 3)
#define BIT_FM_HS	(1 << 4)

#define MECHA_ACDB_RADIO_BUFFER_SIZE (1024 * 2304)

static struct q5v2_hw_info q5v2_audio_hw[Q5V2_HW_COUNT] = {
	[Q5V2_HW_HANDSET] = {
		.max_gain[VOC_NB_INDEX] = 500,
		.min_gain[VOC_NB_INDEX] = -1600,
		.max_gain[VOC_WB_INDEX] = 500,
		.min_gain[VOC_WB_INDEX] = -1600,
	},
	[Q5V2_HW_HEADSET] = {
		.max_gain[VOC_NB_INDEX] = 1125,
		.min_gain[VOC_NB_INDEX] = -1100,
		.max_gain[VOC_WB_INDEX] = 1125,
		.min_gain[VOC_WB_INDEX] = -1100,
	},
	[Q5V2_HW_SPEAKER] = {
		.max_gain[VOC_NB_INDEX] = 1250,
		.min_gain[VOC_NB_INDEX] = -500,
		.max_gain[VOC_WB_INDEX] = 1250,
		.min_gain[VOC_WB_INDEX] = -500,
	},
	[Q5V2_HW_BT_SCO] = {
		.max_gain[VOC_NB_INDEX] = 750,
		.min_gain[VOC_NB_INDEX] = -900,
		.max_gain[VOC_WB_INDEX] = 0,
		.min_gain[VOC_WB_INDEX] = -1500,
	},
	[Q5V2_HW_TTY] = {
		.max_gain[VOC_NB_INDEX] = 0,
		.min_gain[VOC_NB_INDEX] = 0,
		.max_gain[VOC_WB_INDEX] = 0,
		.min_gain[VOC_WB_INDEX] = 0,
	},
	[Q5V2_HW_HS_SPKR] = {
		.max_gain[VOC_NB_INDEX] = -500,
		.min_gain[VOC_NB_INDEX] = -2000,
		.max_gain[VOC_WB_INDEX] = -500,
		.min_gain[VOC_WB_INDEX] = -2000,
	},
	[Q5V2_HW_USB_HS] = {
		.max_gain[VOC_NB_INDEX] = 1250,
		.min_gain[VOC_NB_INDEX] = -500,
		.max_gain[VOC_WB_INDEX] = 1250,
		.min_gain[VOC_WB_INDEX] = -500,
	},
	[Q5V2_HW_HAC] = {
		.max_gain[VOC_NB_INDEX] = 1250,
		.min_gain[VOC_NB_INDEX] = -500,
		.max_gain[VOC_WB_INDEX] = 1250,
		.min_gain[VOC_WB_INDEX] = -500,
	},
};

static unsigned aux_pcm_gpio_off[] = {
	PCOM_GPIO_CFG(MECHA_GPIO_BT_PCM_OUT, 0, GPIO_OUTPUT,
			GPIO_NO_PULL, 0),
	PCOM_GPIO_CFG(MECHA_GPIO_BT_PCM_IN, 0, GPIO_INPUT,
			GPIO_PULL_DOWN, GPIO_2MA),
	PCOM_GPIO_CFG(MECHA_GPIO_BT_PCM_SYNC, 0, GPIO_OUTPUT,
			GPIO_NO_PULL, 0),
	PCOM_GPIO_CFG(MECHA_GPIO_BT_PCM_CLK, 0, GPIO_OUTPUT,
			GPIO_NO_PULL, 0),
};

static unsigned aux_pcm_gpio_on[] = {
	PCOM_GPIO_CFG(MECHA_GPIO_BT_PCM_OUT, 1, GPIO_OUTPUT,
			GPIO_NO_PULL, GPIO_2MA),
	PCOM_GPIO_CFG(MECHA_GPIO_BT_PCM_IN, 1, GPIO_INPUT,
			GPIO_NO_PULL, GPIO_2MA),
	PCOM_GPIO_CFG(MECHA_GPIO_BT_PCM_SYNC, 1, GPIO_OUTPUT,
			GPIO_NO_PULL, GPIO_2MA),
	PCOM_GPIO_CFG(MECHA_GPIO_BT_PCM_CLK, 1, GPIO_OUTPUT,
			GPIO_NO_PULL, GPIO_2MA),
};

void mecha_snddev_poweramp_on(int en)
{
	pr_info("%s %d\n", __func__, en);
	if (en) {
		gpio_set_value(PM8058_GPIO_PM_TO_SYS(MECHA_AUD_SPK_SD), 1);
		mdelay(30);
		if (!atomic_read(&aic3254_ctl))
			curr_rx_mode |= BIT_SPEAKER;
	} else {
		/* Reset AIC3254 */
		gpio_set_value(PM8058_GPIO_PM_TO_SYS(MECHA_AUD_SPK_SD), 0);
		mdelay(20);
		if (!atomic_read(&aic3254_ctl))
			curr_rx_mode &= ~BIT_SPEAKER;
	}
}

void mecha_snddev_hsed_pamp_on(int en)
{
	pr_info("%s %d\n", __func__, en);
	if (en) {
		if (system_rev == 0)
			gpio_set_value(PM8058_GPIO_PM_TO_SYS(MECHA_GPIO_AUD_AMP_EN_XA), 1);
		else
			gpio_set_value(MECHA_GPIO_AUD_AMP_EN, 1);
		mdelay(30);
		set_headset_amp(1);
		if (!atomic_read(&aic3254_ctl))
			curr_rx_mode |= BIT_HEADSET;
	} else {
		set_headset_amp(0);
		if (system_rev == 0)
			gpio_set_value(PM8058_GPIO_PM_TO_SYS(MECHA_GPIO_AUD_AMP_EN_XA), 0);
		else
			gpio_set_value(MECHA_GPIO_AUD_AMP_EN, 0);

		if (!atomic_read(&aic3254_ctl))
			curr_rx_mode &= ~BIT_HEADSET;
	}
}

void mecha_snddev_hs_spk_pamp_on(int en)
{
	mecha_snddev_poweramp_on(en);
	mecha_snddev_hsed_pamp_on(en);
}

void mecha_snddev_receiver_pamp_on(int en)
{
	pr_info("%s %d\n", __func__, en);
	if (en) {
		mdelay(20);
		gpio_set_value(MECHA_GPIO_AUD_AMP_EN, 1);
		set_handset_amp(1);
		if (!atomic_read(&aic3254_ctl))
			curr_rx_mode |= BIT_RECEIVER;
	} else {
		set_handset_amp(0);
		gpio_set_value(MECHA_GPIO_AUD_AMP_EN, 0);
		if (!atomic_read(&aic3254_ctl))
			curr_rx_mode &= ~BIT_RECEIVER;
	}
}

void mecha_snddev_bt_sco_pamp_on(int en)
{
	static int bt_sco_refcount;
	pr_info("%s %d\n", __func__, en);
	mutex_lock(&bt_sco_lock);
	if (en) {
		if (++bt_sco_refcount == 1)
			config_gpio_table(aux_pcm_gpio_on,
					ARRAY_SIZE(aux_pcm_gpio_on));
	} else {
		if (--bt_sco_refcount == 0) {
			config_gpio_table(aux_pcm_gpio_off,
					ARRAY_SIZE(aux_pcm_gpio_off));
			gpio_set_value(MECHA_GPIO_BT_PCM_OUT, 0);
			gpio_set_value(MECHA_GPIO_BT_PCM_SYNC, 0);
			gpio_set_value(MECHA_GPIO_BT_PCM_CLK, 0);
		}
	}
	mutex_unlock(&bt_sco_lock);
}

void mecha_snddev_usb_headset_on(int en)
{
	struct vreg *vreg_ncp;
	int ret;

	vreg_ncp = vreg_get(NULL, "ncp");
	if (IS_ERR(vreg_ncp)) {
		pr_err("%s: vreg_get(%s) failed (%ld)\n",
		__func__, "ncp", PTR_ERR(vreg_ncp));
		return;
	}
	pr_err("%s %d\n",__func__, en);

	if (en) {
		gpio_set_value(MECHA_GPIO_AUD_UART_SWITCH, 0);
		gpio_set_value(MECHA_GPIO_USB_AUD_UART_SWITCH, 1);
		ret = vreg_enable(vreg_ncp);
	} else {
		ret = vreg_disable(vreg_ncp);
		gpio_set_value(MECHA_GPIO_AUD_UART_SWITCH, 1);
		gpio_set_value(MECHA_GPIO_USB_AUD_UART_SWITCH, 0);
	}
}

void mecha_snddev_imic_pamp_on(int en)
{
	pr_info("%s %d\n", __func__, en);
	if (en) {
		pmic_hsed_enable(PM_HSED_CONTROLLER_0, PM_HSED_ENABLE_ALWAYS);
		mecha_back_mic_enable(1);
	} else {
		pmic_hsed_enable(PM_HSED_CONTROLLER_0, PM_HSED_ENABLE_OFF);
		mecha_back_mic_enable(0);
	}
}

void mecha_snddev_emic_pamp_on(int en)
{
	pr_info("%s %d\n", __func__, en);
	if (en) {
		gpio_set_value(MECHA_AUD_MICPATH_SEL, 1);
	} else
		gpio_set_value(MECHA_AUD_MICPATH_SEL, 0);
}

void mecha_back_mic_enable(int en)
{
	pr_info("%s %d\n", __func__, en);
	if (en) {
		gpio_set_value(MECHA_AUD_MICPATH_SEL, 0);
		pmic_hsed_enable(PM_HSED_CONTROLLER_1, PM_HSED_ENABLE_ALWAYS);
	} else {
		gpio_set_value(MECHA_AUD_MICPATH_SEL, 1);
		pmic_hsed_enable(PM_HSED_CONTROLLER_1, PM_HSED_ENABLE_OFF);
	}
}

int mecha_get_rx_vol(uint8_t hw, int network, int level)
{
	struct q5v2_hw_info *info;
	int vol, maxv, minv;

	info = &q5v2_audio_hw[hw];
	maxv = info->max_gain[network];
	minv = info->min_gain[network];
	vol = minv + ((maxv - minv) * level) / 100;
	pr_info("%s(%d, %d, %d) => %d\n", __func__, hw, network, level, vol);
	return vol;
}

void mecha_mic_bias_enable(int en, int shift)
{
	pr_info("%s: %d\n", __func__, en);

	if (en)
		pmic_hsed_enable(PM_HSED_CONTROLLER_2, PM_HSED_ENABLE_ALWAYS);
	else
		pmic_hsed_enable(PM_HSED_CONTROLLER_2, PM_HSED_ENABLE_OFF);
}

void mecha_rx_amp_enable(int en)
{
	if (curr_rx_mode != 0) {
		atomic_set(&aic3254_ctl, 1);
		pr_info("%s: curr_rx_mode 0x%x, en %d\n",
			__func__, curr_rx_mode, en);
		if (curr_rx_mode & BIT_SPEAKER)
			mecha_snddev_poweramp_on(en);
		if (curr_rx_mode & BIT_HEADSET)
			mecha_snddev_hsed_pamp_on(en);
		if (curr_rx_mode & BIT_RECEIVER)
			mecha_snddev_receiver_pamp_on(en);
		atomic_set(&aic3254_ctl, 0);;
	}
}

uint32_t mecha_get_acdb_radio_buffer_size(void)
{
	return MECHA_ACDB_RADIO_BUFFER_SIZE;
}

int mecha_support_aic3254(void)
{
	return 1;
}

int mecha_support_back_mic(void)
{
#ifdef CONFIG_HTC_VOICE_DUALMIC
	return 1;
#else
	return 0;
#endif
}

void mecha_get_acoustic_tables(struct acoustic_tables *tb)
{
	unsigned int engineerID = mecha_get_engineerid();

	strcpy(tb->aic3254_dsp, "CodecDSPID_BCLK.txt\0");
	strcpy(tb->adie, "AdieHWCodec.csv\0");


	if (system_rev <= 2) {
		strcpy(tb->aic3254,
				"AIC3254_REG_DualMic.csv\0");
		strcpy(tb->adie,
				"AdieHWCodec_XC.csv\0");
	} else {
		if (engineerID < 0x2 && system_rev == 3) {
			strcpy(tb->aic3254,
				"AIC3254_REG_DualMicXD01.csv\0");
		} else {
			strcpy(tb->aic3254,
				"AIC3254_REG_DualMicXD02.csv\0");
			strcpy(tb->aic3254_dsp,
				"CodecDSPID.txt\0");
		}
	}
}

static struct acdb_ops acdb = {
	.get_acdb_radio_buffer_size = mecha_get_acdb_radio_buffer_size,
};

static struct q5v2audio_analog_ops ops = {
	.speaker_enable	= mecha_snddev_poweramp_on,
	.headset_enable	= mecha_snddev_hsed_pamp_on,
	.handset_enable	= mecha_snddev_receiver_pamp_on,
	.headset_speaker_enable	= mecha_snddev_hs_spk_pamp_on,
	.bt_sco_enable	= mecha_snddev_bt_sco_pamp_on,
	.usb_headset_enable = mecha_snddev_usb_headset_on,
	.int_mic_enable = mecha_snddev_imic_pamp_on,
	.ext_mic_enable = mecha_snddev_emic_pamp_on,
	.fm_headset_enable = mecha_snddev_hsed_pamp_on,
	.fm_speaker_enable = mecha_snddev_poweramp_on,
};

static struct q5v2audio_icodec_ops iops = {
	.support_aic3254 = mecha_support_aic3254,
};

static struct q5v2audio_ecodec_ops eops = {
	.bt_sco_enable  = mecha_snddev_bt_sco_pamp_on,
};

static struct q5v2voice_ops vops = {
	.get_rx_vol = mecha_get_rx_vol,
};

static struct acoustic_ops acoustic = {
	.enable_mic_bias = mecha_mic_bias_enable,
	.support_aic3254 = mecha_support_aic3254,
	.support_back_mic = mecha_support_back_mic,
	.enable_back_mic =  mecha_back_mic_enable,
	.get_acoustic_tables = mecha_get_acoustic_tables
};

static struct aic3254_ctl_ops cops = {
	.rx_amp_enable = mecha_rx_amp_enable,
};

void __init mecha_audio_init(void)
{
	static struct pm8058_gpio tpa2051_pwr = {
		.direction      = PM_GPIO_DIR_OUT,
		.output_buffer  = PM_GPIO_OUT_BUF_CMOS,
		.output_value   = 0,
		.pull	        = PM_GPIO_PULL_NO,
		.vin_sel	= 6,	  /* S3 1.8 V */
		.out_strength   = PM_GPIO_STRENGTH_HIGH,
		.function	= PM_GPIO_FUNC_NORMAL,
	};

	mutex_init(&bt_sco_lock);

#ifdef CONFIG_MSM7KV2_AUDIO
	htc_7x30_register_analog_ops(&ops);
	htc_7x30_register_icodec_ops(&iops);
	htc_7x30_register_ecodec_ops(&eops);
	htc_7x30_register_voice_ops(&vops);
	acoustic_register_ops(&acoustic);
	acdb_register_ops(&acdb);
#endif
	aic3254_register_ctl_ops(&cops);

	pm8058_gpio_config(MECHA_AUD_SPK_SD, &tpa2051_pwr);

	if (system_rev == 0)
		pm8058_gpio_config(MECHA_GPIO_AUD_AMP_EN_XA, &tpa2051_pwr);
	else {
		gpio_request(MECHA_GPIO_AUD_AMP_EN, "aud_amp_en");
		gpio_direction_output(MECHA_GPIO_AUD_AMP_EN, 1);
		gpio_set_value(MECHA_GPIO_AUD_AMP_EN, 0);
	}

	gpio_request(MECHA_AUD_MICPATH_SEL, "aud_mic_sel");
	gpio_direction_output(MECHA_AUD_MICPATH_SEL, 1);
	gpio_set_value(MECHA_AUD_MICPATH_SEL, 0);

	gpio_set_value(MECHA_AUD_CODEC_RST, 0);
	mdelay(1);
	gpio_set_value(MECHA_AUD_CODEC_RST, 1);

	mutex_lock(&bt_sco_lock);
	config_gpio_table(aux_pcm_gpio_off, ARRAY_SIZE(aux_pcm_gpio_off));
	gpio_set_value(MECHA_GPIO_BT_PCM_OUT, 0);
	gpio_set_value(MECHA_GPIO_BT_PCM_SYNC, 0);
	gpio_set_value(MECHA_GPIO_BT_PCM_CLK, 0);
	mutex_unlock(&bt_sco_lock);
}


