/*
 *  p10_wm1811.c
 *
 *  Copyright (c) 2011 Samsung Electronics Co. Ltd
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 */

#include <linux/platform_device.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/mfd/wm8994/registers.h>
#include <linux/input.h>

#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/jack.h>

#include <mach/regs-clock.h>

#include "i2s.h"
#include "s3c-i2s-v2.h"
#include "../codecs/wm8994.h"


/* SMDK has a 16.934MHZ crystal attached to WM8994 */
#define SMDK_WM8994_OSC_FREQ	16934400
#define WM8994_DAI_AIF1		0
#define WM8994_DAI_AIF2		1
#define WM8994_DAI_AIF3		2

#define EAR_SEL EXYNOS4210_GPJ0(4)
#define MANAGE_MCLK1

static bool p10_fll1_active;

static void p10_set_mclk(bool on)
{
	u32 val;
	u32 __iomem *xusbxti_sys_pwr;
	u32 __iomem *pmu_debug;

	xusbxti_sys_pwr = ioremap(0x10041280, 4);
	pmu_debug = ioremap(0x10040A00, 4);

	if (on) {
		val = readl(xusbxti_sys_pwr);
		val |= 0x0001;			/* SYS_PWR_CFG is enabled */
		writel(val, xusbxti_sys_pwr);

		val = readl(pmu_debug);
		val &= ~(0b11111 << 8);
		val |= 0b10000 << 8;		/* Selected XUSBXTI */
		val &= ~(0x0001);		/* CLKOUT is enabled */
		writel(val, pmu_debug);
	} else {
		val = readl(xusbxti_sys_pwr);
		val &= ~(0x0001);		/* SYS_PWR_CFG is disabled */
		writel(val, xusbxti_sys_pwr);

		val = readl(pmu_debug);
		val |= 0x0001;			/* CLKOUT is disabled */
		writel(val, pmu_debug);
	}

	iounmap(xusbxti_sys_pwr);
	iounmap(pmu_debug);

	mdelay(10);
}

static void p10_start_fll1(struct snd_soc_dai *aif1_dai)
{
	int ret;

	if (p10_fll1_active)
		return;

	dev_info(aif1_dai->dev, "Moving to audio clocking settings\n");

	/* Switch AIF1 to MCLK2 while we bring stuff up */
	ret = snd_soc_dai_set_sysclk(aif1_dai,
				     WM8994_SYSCLK_MCLK2,
				     32768,
				     SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(aif1_dai->dev, "Unable to switch to MCLK2: %d\n", ret);

	/* Start the 24MHz clock to provide a high frequency reference to
	 * provide a high frequency reference for the FLL, giving improved
	 * performance.
	 */
#ifdef MANAGE_MCLK1
	p10_set_mclk(1);
#endif

	/* Switch the FLL */
	ret = snd_soc_dai_set_pll(aif1_dai, WM8994_FLL1, WM8994_FLL_SRC_MCLK1,
					 24000000, 44100 * 256);
	if (ret < 0)
		dev_err(aif1_dai->dev, "Unable to start FLL1: %d\n", ret);

#ifdef MANAGE_MCLK1
	/* Now the FLL is running we can stop the reference clock, the
	 * FLL will maintain frequency with no reference so this saves
	 * power from the reference clock.
	 */
	/*
	p10_set_mclk(0);
	*/
#endif

	/* Then switch AIF1CLK to it */
	ret = snd_soc_dai_set_sysclk(aif1_dai,
					WM8994_SYSCLK_FLL1,
					44100 * 256,
					SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(aif1_dai->dev, "Unable to switch to FLL1: %d\n", ret);

	p10_fll1_active = true;
}

#ifdef CONFIG_SND_SAMSUNG_I2S_MASTER
static int set_epll_rate(unsigned long rate)
{
	struct clk *fout_epll;

	fout_epll = clk_get(NULL, "fout_epll");
	if (IS_ERR(fout_epll)) {
		printk(KERN_ERR "%s: failed to get fout_epll\n", __func__);
		return -ENOENT;
	}

	if (rate == clk_get_rate(fout_epll))
		goto out;

	clk_set_rate(fout_epll, rate);
out:
	clk_put(fout_epll);

	return 0;
}
#endif /* CONFIG_SND_SAMSUNG_I2S_MASTER */

#ifndef CONFIG_SND_SAMSUNG_I2S_MASTER
static int p10_wm1811_aif1_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	unsigned int pll_out;
	int ret;

	/* AIF1CLK should be >=3MHz for optimal performance */
	if (params_rate(params) == 8000 || params_rate(params) == 11025)
		pll_out = params_rate(params) * 512;
	else
		pll_out = params_rate(params) * 256;

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S
					| SND_SOC_DAIFMT_NB_NF
					| SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0)
		return ret;

	/* Set the cpu DAI configuration */
	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S
					| SND_SOC_DAIFMT_NB_NF
					| SND_SOC_DAIFMT_CBM_CFM);
	if (ret < 0)
		return ret;

#if 0
	ret = snd_soc_dai_set_sysclk(codec_dai, WM8994_SYSCLK_FLL1,
					pll_out, SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_OPCLK,
					0, MOD_OPCLK_PCLK);
	if (ret < 0)
		return ret;
#else
	p10_start_fll1(codec_dai);
#endif

	if (ret < 0)
		return ret;

	return 0;
}
#else /* CONFIG_SND_SAMSUNG_I2S_MASTER */
static int p10_wm1811_aif1_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int bfs, psr, rfs, ret;
	unsigned long rclk;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_U24:
	case SNDRV_PCM_FORMAT_S24:
		bfs = 48;
		break;
	case SNDRV_PCM_FORMAT_U16_LE:
	case SNDRV_PCM_FORMAT_S16_LE:
		bfs = 32;
		break;
	default:
		return -EINVAL;
	}

	switch (params_rate(params)) {
	case 16000:
	case 22050:
	case 24000:
	case 32000:
	case 44100:
	case 48000:
	case 88200:
	case 96000:
		if (bfs == 48)
			rfs = 384;
		else
			rfs = 256;
		break;
	case 64000:
		rfs = 384;
		break;
	case 8000:
	case 11025:
	case 12000:
		if (bfs == 48)
			rfs = 768;
		else
			rfs = 512;
		break;
	default:
		return -EINVAL;
	}

	rclk = params_rate(params) * rfs;

	switch (rclk) {
	case 4096000:
	case 5644800:
	case 6144000:
	case 8467200:
	case 9216000:
		psr = 8;
		break;
	case 8192000:
	case 11289600:
	case 12288000:
	case 16934400:
	case 18432000:
		psr = 4;
		break;
	case 22579200:
	case 24576000:
	case 33868800:
	case 36864000:
		psr = 2;
		break;
	case 67737600:
	case 73728000:
		psr = 1;
		break;
	default:
		printk(KERN_INFO "Not yet supported!\n");
		return -EINVAL;
	}

	set_epll_rate(rclk * psr);

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S
					| SND_SOC_DAIFMT_NB_NF
					| SND_SOC_DAIFMT_CBS_CFS);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S
					| SND_SOC_DAIFMT_NB_NF
					| SND_SOC_DAIFMT_CBS_CFS);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8994_SYSCLK_MCLK1,
					rclk, SND_SOC_CLOCK_IN);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_CDCLK,
					0, SND_SOC_CLOCK_OUT);
	if (ret < 0)
		return ret;

	ret = snd_soc_dai_set_clkdiv(cpu_dai, SAMSUNG_I2S_DIV_BCLK, bfs);
	if (ret < 0)
		return ret;

	return 0;
}
#endif /* CONFIG_SND_SAMSUNG_I2S_MASTER */

/*
 * P10 WM1811 DAI operations.
 */
static struct snd_soc_ops p10_wm1811_aif1_ops = {
	.hw_params = p10_wm1811_aif1_hw_params,
};

static int p10_wm1811_aif2_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int ret;
	int prate;
	int bclk;

	prate = params_rate(params);
	switch (params_rate(params)) {
	case 8000:
	case 16000:
	       break;
	default:
		dev_warn(codec_dai->dev, "Unsupported LRCLK %d, falling back to 8000Hz\n",
				(int)params_rate(params));
		prate = 8000;
	}

	/* Set the codec DAI configuration */
	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S
					| SND_SOC_DAIFMT_IB_NF
					| SND_SOC_DAIFMT_CBS_CFS);
	if (ret < 0)
		return ret;

	switch (prate) {
	case 8000:
		bclk = 256000;
		break;
	case 16000:
		bclk = 512000;
		break;
	default:
		return -EINVAL;
	}

	ret = snd_soc_dai_set_pll(codec_dai, WM8994_FLL2,
					WM8994_FLL_SRC_BCLK,
					bclk, prate * 256);
	if (ret < 0)
		dev_err(codec_dai->dev, "Unable to configure FLL2: %d\n", ret);

	ret = snd_soc_dai_set_sysclk(codec_dai, WM8994_SYSCLK_FLL2,
					prate * 256, SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(codec_dai->dev, "Unable to switch to FLL2: %d\n", ret);

	return 0;
}

static struct snd_soc_ops p10_wm1811_aif2_ops = {
	.hw_params = p10_wm1811_aif2_hw_params,
};

static int p10_wm1811_aif3_hw_params(struct snd_pcm_substream *substream,
					struct snd_pcm_hw_params *params)
{
	pr_err("%s: enter\n", __func__);
	return 0;
}

static struct snd_soc_ops p10_wm1811_aif3_ops = {
	.hw_params = p10_wm1811_aif3_hw_params,
};

static int mic_sel;

static int mic_sel_get(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = mic_sel;

	return 0;
}

static int mic_sel_set(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	int val = ucontrol->value.integer.value[0];

	if (val < 0 || val > 1)
		return -EINVAL;

	if (val == mic_sel)
		return 0;

#if 0
	gpio_set_value(EAR_SEL, val);
	mic_sel = val;
#endif

	return 1;
}

static int ext_amp_bias(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *k, int event)
{
	struct snd_soc_codec *codec = w->codec;

	printk("[%s] %s: event %d\n", codec->name,  __func__, SND_SOC_DAPM_EVENT_ON(event));

	if(SND_SOC_DAPM_EVENT_ON(event)){
		gpio_set_value(GPIO_AMP_L_INT, 1);
		gpio_set_value(GPIO_AMP_R_INT, 1);
	} else {
		gpio_set_value(GPIO_AMP_L_INT, 0);
		gpio_set_value(GPIO_AMP_R_INT, 0);
	}

	return 0;
}

const char *mic_sel_text[] = {
	"Sub", "Headset"
};

static const struct soc_enum mic_sel_enum =
	SOC_ENUM_SINGLE_EXT(2, mic_sel_text);

static const struct snd_kcontrol_new p10_controls[] = {
	SOC_DAPM_PIN_SWITCH("HP"),
	SOC_DAPM_PIN_SWITCH("SPK"),
	SOC_DAPM_PIN_SWITCH("RCV"),
	SOC_DAPM_PIN_SWITCH("LINE"),

	SOC_ENUM_EXT("MIC Select", mic_sel_enum, mic_sel_get, mic_sel_set),
};

const struct snd_soc_dapm_widget p10_dapm_widgets[] = {
	SND_SOC_DAPM_HP("HP", NULL),
	SND_SOC_DAPM_SPK("SPK", NULL),
	SND_SOC_DAPM_SPK("RCV", NULL),
	SND_SOC_DAPM_SPK("LINE", ext_amp_bias),

	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_MIC("Sub Mic", NULL),

	SND_SOC_DAPM_INPUT("S5P RP"),
};

const struct snd_soc_dapm_route p10_dapm_routes[] = {
	{ "HP", NULL, "HPOUT1L" },
	{ "HP", NULL, "HPOUT1R" },

	{ "SPK", NULL, "SPKOUTLN" },
	{ "SPK", NULL, "SPKOUTLP" },
	{ "SPK", NULL, "SPKOUTRN" },
	{ "SPK", NULL, "SPKOUTRP" },

	{ "RCV", NULL, "HPOUT2N" },
	{ "RCV", NULL, "HPOUT2P" },

	{ "LINE", NULL, "LINEOUT1N" },
	{ "LINE", NULL, "LINEOUT1P" },

	{ "IN1LP", NULL, "MICBIAS1" },
	{ "IN1LN", NULL, "MICBIAS1" },
	{ "MICBIAS1", NULL, "Main Mic" },

	{ "IN1RP", NULL, "MICBIAS1" },
	{ "IN1RN", NULL, "MICBIAS1" },
	{ "MICBIAS1", NULL, "Sub Mic" },

	{ "IN2RP:VXRP", NULL, "MICBIAS2" },
	{ "MICBIAS2", NULL, "Headset Mic" },

	{ "AIF1DAC1L", NULL, "S5P RP" },
	{ "AIF1DAC1R", NULL, "S5P RP" },
};

struct wm1811_machine_priv {
	struct snd_soc_jack jack;
	struct snd_soc_codec *codec;
	struct delayed_work mic_work;
};

static void wm1811_mic_work(struct work_struct *work)
{
	int report = 0;
	struct wm1811_machine_priv *wm1811;
	struct snd_soc_codec *codec;
	int status;

	wm1811 = container_of(work, struct wm1811_machine_priv,
							mic_work.work);
	codec = wm1811->codec;

	status = snd_soc_read(codec, WM8958_MIC_DETECT_3);
	if (status < 0) {
		dev_err(codec->dev, "Failed to read mic detect status: %d\n",
				status);
		return;
	}

	/* If nothing present then clear our statuses */
	if (!(status & WM8958_MICD_STS))
		goto done;

	report = SND_JACK_HEADSET;

	/* Everything else is buttons; just assign slots */
	if (status & 0x4)
		report |= SND_JACK_BTN_0;
	if (status & 0x8)
		report |= SND_JACK_BTN_1;
	if (status & 0x10)
		report |= SND_JACK_BTN_2;

	if (report & SND_JACK_MICROPHONE)
		dev_crit(codec->dev, "Reporting microphone\n");
	if (report & SND_JACK_HEADPHONE)
		dev_crit(codec->dev, "Reporting headphone\n");
	if (report & SND_JACK_BTN_0)
		dev_crit(codec->dev, "Reporting button 0\n");
	if (report & SND_JACK_BTN_1)
		dev_crit(codec->dev, "Reporting button 1\n");
	if (report & SND_JACK_BTN_2)
		dev_crit(codec->dev, "Reporting button 2\n");

done:
	if (!report)
		dev_crit(codec->dev, "Reporting open circuit\n");

	snd_soc_jack_report(&wm1811->jack, report,
				SND_JACK_BTN_0 | SND_JACK_BTN_1 |
				SND_JACK_BTN_2 | SND_JACK_HEADSET);
}

static struct snd_soc_dai_driver p10_ext_dai[] = {
	{
		.name = "p10.cp",
		.playback = {
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.capture = {
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
	},
	{
		.name = "p10.bt",
		.playback = {
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
		.capture = {
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_8000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE,
		},
	},
};

static int p10_wm1811_init_paiftx(struct snd_soc_pcm_runtime *rtd)
{
#if 0
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dapm_context *dapm = &codec->dapm;

	/* HeadPhone */
	snd_soc_dapm_enable_pin(dapm, "HPOUT1R");
	snd_soc_dapm_enable_pin(dapm, "HPOUT1L");

	/* MicIn */
	snd_soc_dapm_enable_pin(dapm, "IN1LN");
	snd_soc_dapm_enable_pin(dapm, "IN1RN");

	/* LineIn */
	snd_soc_dapm_enable_pin(dapm, "IN2LN");
	snd_soc_dapm_enable_pin(dapm, "IN2RN");

	/* Other pins NC */
	snd_soc_dapm_nc_pin(dapm, "HPOUT2P");
	snd_soc_dapm_nc_pin(dapm, "HPOUT2N");
	snd_soc_dapm_nc_pin(dapm, "SPKOUTLN");
	snd_soc_dapm_nc_pin(dapm, "SPKOUTLP");
	snd_soc_dapm_nc_pin(dapm, "SPKOUTRP");
	snd_soc_dapm_nc_pin(dapm, "SPKOUTRN");
	snd_soc_dapm_nc_pin(dapm, "LINEOUT1N");
	snd_soc_dapm_nc_pin(dapm, "LINEOUT1P");
	snd_soc_dapm_nc_pin(dapm, "LINEOUT2N");
	snd_soc_dapm_nc_pin(dapm, "LINEOUT2P");
	snd_soc_dapm_nc_pin(dapm, "IN1LP");
	snd_soc_dapm_nc_pin(dapm, "IN2LP:VXRN");
	snd_soc_dapm_nc_pin(dapm, "IN1RP");
	snd_soc_dapm_nc_pin(dapm, "IN2RP:VXRP");

	snd_soc_dapm_sync(dapm);

	return 0;
#else
	struct wm1811_machine_priv *wm1811;
	struct snd_soc_codec *codec = rtd->codec;
	struct snd_soc_dai *aif1_dai = rtd->codec_dai;
	int ret;

#ifndef MANAGE_MCLK1
	p10_set_mclk(1);
#endif

	ret = snd_soc_add_controls(codec, p10_controls,
					ARRAY_SIZE(p10_controls));

	ret = snd_soc_dapm_new_controls(&codec->dapm, p10_dapm_widgets,
					   ARRAY_SIZE(p10_dapm_widgets));
	if (ret != 0)
		dev_err(codec->dev, "Failed to add DAPM widgets: %d\n", ret);

	ret = snd_soc_dapm_add_routes(&codec->dapm, p10_dapm_routes,
					   ARRAY_SIZE(p10_dapm_routes));
	if (ret != 0)
		dev_err(codec->dev, "Failed to add DAPM routes: %d\n", ret);

	ret = snd_soc_dai_set_sysclk(aif1_dai, WM8994_SYSCLK_MCLK2,
				     32768, SND_SOC_CLOCK_IN);
	if (ret < 0)
		dev_err(codec->dev, "Failed to boot clocking\n");

	/* Force AIF1CLK on as it will be master for jack detection */
	ret = snd_soc_dapm_force_enable_pin(&codec->dapm, "AIF1CLK");
	if (ret < 0)
		dev_err(codec->dev, "Failed to enable AIF1CLK: %d\n", ret);

	ret = snd_soc_dapm_disable_pin(&codec->dapm, "S5P RP");
	if (ret < 0)
		dev_err(codec->dev, "Failed to disable S5P RP: %d\n", ret);

	wm1811 = kmalloc(sizeof *wm1811, GFP_KERNEL);
	if (!wm1811) {
		dev_err(codec->dev, "Failed to allocate memory!");
		return -ENOMEM;
	}

	wm1811->codec = codec;

	INIT_DELAYED_WORK(&wm1811->mic_work, wm1811_mic_work);

	ret = snd_soc_jack_new(codec, "P10 Jack",
				SND_JACK_HEADSET | SND_JACK_BTN_0 |
				SND_JACK_BTN_1 | SND_JACK_BTN_2,
				&wm1811->jack);

	if (ret < 0)
		dev_err(codec->dev, "Failed to create jack: %d\n", ret);

	ret = snd_jack_set_key(wm1811->jack.jack, SND_JACK_BTN_0, KEY_MEDIA);
	if (ret < 0)
		dev_err(codec->dev, "Failed to set KEY_MEDIA: %d\n", ret);

	ret = snd_jack_set_key(wm1811->jack.jack, SND_JACK_BTN_1, KEY_VOLUMEUP);
	if (ret < 0)
		dev_err(codec->dev, "Failed to set KEY_VOLUMEUP: %d\n", ret);

	ret = snd_jack_set_key(wm1811->jack.jack, SND_JACK_BTN_2,
							KEY_VOLUMEDOWN);
	if (ret < 0)
		dev_err(codec->dev, "Failed to set KEY_VOLUMEDOWN: %d\n", ret);

	ret = wm8958_mic_detect(codec, &wm1811->jack, NULL, NULL);
	if (ret < 0)
		dev_err(codec->dev, "Failed start detection: %d\n", ret);

	return snd_soc_dapm_sync(&codec->dapm);

#endif
}

static struct snd_soc_dai_link p10_dai[] = {
	{ /* Sec_Fifo DAI i/f */
		.name = "Sec_FIFO TX",
		.stream_name = "Sec_Dai",
		.cpu_dai_name = "samsung-i2s.4",
		.codec_dai_name = "wm8994-aif1",
#ifndef CONFIG_SND_SOC_SAMSUNG_USE_DMA_WRAPPER
		.platform_name = "samsung-audio-idma",
#else
		.platform_name = "samsung-audio",
#endif
		.codec_name = "wm8994-codec",
		.init = p10_wm1811_init_paiftx,
		.ops = &p10_wm1811_aif1_ops,
	},
	{
		.name = "P10_WM1811 Voice",
		.stream_name = "Voice Tx/Rx",
		.cpu_dai_name = "p10.cp",
		.codec_dai_name = "wm8994-aif2",
		.platform_name = "snd-soc-dummy",
		.codec_name = "wm8994-codec",
		.ops = &p10_wm1811_aif2_ops,
	},
	{
		.name = "P10_WM1811 BT",
		.stream_name = "BT Tx/Rx",
		.cpu_dai_name = "p10.bt",
		.codec_dai_name = "wm8994-aif3",
		.platform_name = "snd-soc-dummy",
		.codec_name = "wm8994-codec",
		.ops = &p10_wm1811_aif3_ops,
	},
	{ /* Primary DAI i/f */
		.name = "WM8994 AIF1",
		.stream_name = "Pri_Dai",
		.cpu_dai_name = "samsung-i2s.0",
		.codec_dai_name = "wm8994-aif1",
		.platform_name = "samsung-audio",
		.codec_name = "wm8994-codec",
		.ops = &p10_wm1811_aif1_ops,
	},
};

#if 0	/* To Do */
static int p10_set_bias_level(struct snd_soc_card *card,
				enum snd_soc_bias_level level)
{
	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		/* When transitioning to active modes set AIF1 up for
		* 44.1kHz so we can always activate AIF1 without reclocking.
		*/
		if (card->bias_level == SND_SOC_BIAS_STANDBY)
			p10_start_fll1(aif1_dai);
		break;

	default:
		break;
	}

	return 0;
}

static int p10_set_bias_level_post(struct snd_soc_card *card,
					 enum snd_soc_bias_level level)
{
	int ret;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		/* When going idle stop FLL1 and revert to using MCLK2
		 * directly for minimum power consumptin for accessory
		 * detection.
		 */
		if (card->bias_level == SND_SOC_BIAS_PREPARE) {
			dev_info(aif1_dai->dev, "Moving to STANDBY\n");

			ret = snd_soc_dai_set_sysclk(aif2_dai,
							WM8994_SYSCLK_MCLK2,
							32768,
							SND_SOC_CLOCK_IN);
			if (ret < 0)
				dev_err(codec->dev, "Failed to switch to MCLK2\n");

			ret = snd_soc_dai_set_pll(aif2_dai, WM8994_FLL2,
							WM8994_FLL_SRC_MCLK2,
							32768, 0);

			if (ret < 0)
				dev_err(codec->dev,
					"Failed to change FLL2\n");

			ret = snd_soc_dai_set_sysclk(aif1_dai,
						     WM8994_SYSCLK_MCLK2,
						     32768,
						     SND_SOC_CLOCK_IN);
			if (ret < 0)
				dev_err(codec->dev,
					"Failed to switch to MCLK2\n");

			ret = snd_soc_dai_set_pll(aif1_dai, WM8994_FLL1,
						  WM8994_FLL_SRC_MCLK2,
						  32768, 0);
			if (ret < 0)
				dev_err(codec->dev,
					"Failed to stop FLL1\n");

			p10_fll1_active = false;
		}
		break;
	default:
		break;
	}

	card->bias_level = level;

	return 0;
}
#endif

static struct snd_soc_card p10 = {
	.name = "P10_WM1811",
	.dai_link = p10_dai,

	/* If you want to use sec_fifo device,
	 * changes the num_link = 2 or ARRAY_SIZE(p10_dai). */
	.num_links = ARRAY_SIZE(p10_dai),

#if 0	/* To Do */
	.set_bias_level = p10_set_bias_level,
	.set_bias_level_post = p10_set_bias_level_post
#endif
};

static struct platform_device *p10_snd_device;

static int __init p10_audio_init(void)
{
	int ret;

#if 0
	/* EAR_SEL switches SUB and EAR mics - force to SUB mic */
	ret = gpio_request(EAR_SEL, "EAR_SEL");
	if (ret < 0)
		pr_err("Failed to request EAR_SEL: %d\n", ret);

	ret = gpio_direction_output(EAR_SEL, 0);
	if (ret < 0)
		pr_err("Failed to request EAR_SEL: %d\n", ret);
#endif

	gpio_request_one(GPIO_AMP_L_INT, GPIOF_OUT_INIT_LOW, "AMP_L_INT");
	gpio_set_value(GPIO_AMP_L_INT, 0);

	gpio_request_one(GPIO_AMP_R_INT, GPIOF_OUT_INIT_LOW, "AMP_R_INT");
	gpio_set_value(GPIO_AMP_R_INT, 0);

	p10_snd_device = platform_device_alloc("soc-audio", -1);
	if (!p10_snd_device)
		return -ENOMEM;

	ret = snd_soc_register_dais(&p10_snd_device->dev,
					p10_ext_dai, ARRAY_SIZE(p10_ext_dai));
	if (ret != 0)
		pr_err("Failed to register external DAIs: %d\n", ret);

	platform_set_drvdata(p10_snd_device, &p10);

	ret = platform_device_add(p10_snd_device);
	if (ret)
		platform_device_put(p10_snd_device);

	return ret;
}
module_init(p10_audio_init);

static void __exit p10_audio_exit(void)
{
	platform_device_unregister(p10_snd_device);
}
module_exit(p10_audio_exit);

MODULE_AUTHOR("JS. Park <aitdark.park@samsung.com>");
MODULE_DESCRIPTION("ALSA SoC P10 WM1811");
MODULE_LICENSE("GPL");
