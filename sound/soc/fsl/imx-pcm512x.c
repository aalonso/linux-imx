/*
 * Copyright 2020 NXP.
 *
 * The code contained herein is licensed under the GNU General Public
 * License. You may obtain a copy of the GNU General Public License
 * Version 2 or later at the following locations:
 *
 * http://www.opensource.org/licenses/gpl-license.html
 * http://www.gnu.org/copyleft/gpl.html
 */

#include <linux/module.h>
#include <linux/gpio/consumer.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/clk.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>

struct imx_pcm512x_data {
	struct snd_soc_dai_link dai;
	struct snd_soc_card card;
	struct gpio_desc *mute_gpio;
	bool digital_gain_limit;
	bool gpio_unmute;
	bool auto_mute;
};

static const struct snd_soc_dapm_widget imx_pcm512x_dapm_widgets[] = {
	SND_SOC_DAPM_LINE("Line Out Jack", NULL),
	SND_SOC_DAPM_LINE("Line In Jack", NULL),
};

static int imx_pcm512x_dai_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_card *card = rtd->card;
	struct imx_pcm512x_data *data = snd_soc_card_get_drvdata(card);
	int ret;

	if (data->digital_gain_limit) {
		ret = snd_soc_limit_volume(card,
				"Digital Playback Volume", 207);
		if (ret)
			dev_warn(card->dev, "fail to set volume limit\n");
	}

	return 0;
}

static int imx_pcm512x_set_bias_level(struct snd_soc_card *card,
	struct snd_soc_dapm_context *dapm, enum snd_soc_bias_level level)
{
	struct imx_pcm512x_data *data = snd_soc_card_get_drvdata(card);
	struct snd_soc_pcm_runtime *rtd;
	struct snd_soc_dai *codec_dai;

	rtd = snd_soc_get_pcm_runtime(card, card->dai_link->name);
	codec_dai = rtd->codec_dai;

	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level != SND_SOC_BIAS_STANDBY)
			break;
		/* unmute */
		gpiod_set_value_cansleep(data->mute_gpio, 1);
		break;
	case SND_SOC_BIAS_STANDBY:
		if (dapm->bias_level != SND_SOC_BIAS_PREPARE)
			break;
		/* mute */
		gpiod_set_value_cansleep(data->mute_gpio, 0);
		break;
	default:
		break;
	}

	return 0;
}

static int imx_pcm512x_hw_params(struct snd_pcm_substream *substream,
		struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	struct snd_soc_card *card = rtd->card;
	struct imx_pcm512x_data *data = snd_soc_card_get_drvdata(card);
	unsigned int channels = params_channels(params);
	unsigned int fmt;
	int ret;

	/* Slave mode */
	fmt = SND_SOC_DAIFMT_I2S | SND_SOC_DAIFMT_NB_NF |
		SND_SOC_DAIFMT_CBS_CFS;

	ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
	if (ret) {
		dev_err(card->dev, "fail to set cpu dai fmt\n");
		return ret;
	}

	ret = snd_soc_dai_set_fmt(codec_dai, fmt);
	if (ret) {
		dev_err(card->dev, "fail to set codec dai fmt\n");
		return ret;
	}

	ret = snd_soc_dai_set_tdm_slot(cpu_dai, BIT(channels) - 1,
			BIT(channels) - 1, 2, params_physical_width(params));
	if (ret) {
		dev_err(card->dev, "fail to set cpu dai tdm slot\n");
		return ret;
	}

	return 0;
}

static const struct snd_soc_ops imx_pcm512x_ops = {
	.hw_params = imx_pcm512x_hw_params,
};

static int imx_pcm512x_probe(struct platform_device *pdev)
{
	struct device_node *cpu_np, *codec_np = NULL;
	struct device_node *np = pdev->dev.of_node;
	struct platform_device *cpu_pdev = NULL;
	struct i2c_client *codec_dev = NULL;
	struct imx_pcm512x_data *data;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	cpu_np = of_parse_phandle(np, "audio-cpu", 0);
	if (!cpu_np) {
		dev_err(&pdev->dev, "cpu dai phandle missing or invalid\n");
		ret = -EINVAL;
		goto fail;
	}

	cpu_pdev = of_find_device_by_node(cpu_np);
	if (!cpu_pdev) {
		dev_err(&pdev->dev, "fail to find SAI platform device\n");
		ret = -EINVAL;
		goto fail;
	}

	codec_np = of_parse_phandle(np, "audio-codec", 0);
	if (!codec_np) {
		dev_err(&pdev->dev, "codec dai phandle missing or invalid\n");
		ret = -EINVAL;
		goto fail;
	}

	codec_dev = of_find_i2c_device_by_node(codec_np);
	if (!codec_dev || !codec_dev->dev.driver) {
		dev_err(&pdev->dev, "fail to find codec i2c device\n");
		ret = -EPROBE_DEFER;
		goto fail;
	}

	data->digital_gain_limit = !of_property_read_bool(np,
			"pidac,24db_digital_gain");
	data->gpio_unmute = of_property_read_bool(np, "pidac,unmute_amp");
	data->auto_mute = of_property_read_bool(np, "pidac,auto_mute_amp");

	if (data->gpio_unmute || data->auto_mute) {
		data->mute_gpio = devm_gpiod_get_optional(&pdev->dev,
			"mute", GPIOD_OUT_LOW);
		if (IS_ERR(data->mute_gpio)) {
			dev_err(&pdev->dev, "fail to get mute gpio\n");
			ret = PTR_ERR(data->mute_gpio);
			goto fail;
		}
	}

	data->dai.name = "imx-pcm512x";
	data->dai.stream_name = "imx-pcm512x";
	data->dai.cpu_dai_name = dev_name(&cpu_pdev->dev);
	data->dai.codec_dai_name = "pcm512x-hifi";
	data->dai.codec_of_node = codec_np;
	data->dai.cpu_of_node = cpu_np;
	data->dai.platform_of_node = cpu_np;
	data->dai.ops = &imx_pcm512x_ops;
	data->dai.init = imx_pcm512x_dai_init;
	data->dai.dai_fmt = SND_SOC_DAIFMT_I2S |
			SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBS_CFS;

	data->card.dev = &pdev->dev;
	data->card.num_links = 1;
	data->card.owner = THIS_MODULE;
	data->card.dai_link = &data->dai;
	data->card.dapm_widgets = imx_pcm512x_dapm_widgets;
	data->card.num_dapm_widgets = ARRAY_SIZE(imx_pcm512x_dapm_widgets);

	if (data->auto_mute && data->gpio_unmute)
		data->card.set_bias_level = imx_pcm512x_set_bias_level;

	ret = snd_soc_of_parse_card_name(&data->card, "model");
	if (ret) {
		dev_err(&pdev->dev, "fail to find card model name\n");
		goto fail;
	}

	ret = snd_soc_of_parse_audio_routing(&data->card, "audio-routing");
	if (ret) {
		dev_err(&pdev->dev, "fail to parse audio routing\n");
		goto fail;
	}

	platform_set_drvdata(pdev, &data->card);
	snd_soc_card_set_drvdata(&data->card, data);

	ret = devm_snd_soc_register_card(&pdev->dev, &data->card);
	if (ret) {
		dev_err(&pdev->dev, "snd soc register card failed: %d\n", ret);
		goto fail;
	}

	if (data->gpio_unmute && data->mute_gpio)
		gpiod_set_value_cansleep(data->mute_gpio, 1);

	ret = 0;
fail:
	if (codec_np)
		of_node_put(codec_np);
	if (cpu_np)
		of_node_put(cpu_np);

	return ret;
}

static int imx_pcm512x_remove(struct platform_device *pdev)
{
	struct imx_pcm512x_data *data = platform_get_drvdata(pdev);
	/* mute */
	if (data->gpio_unmute)
		gpiod_set_value_cansleep(data->mute_gpio, 0);
	/* unregister card */
	return snd_soc_unregister_card(&data->card);
}

static const struct of_device_id imx_pcm512x_dt_ids[] = {
	{ .compatible = "fsl,imx-audio-pcm512x", },
	{ /* sentinel*/ }
};
MODULE_DEVICE_TABLE(of, imx_pcm512x_dt_ids);

static struct platform_driver imx_pcm512x_driver = {
	.driver = {
		.name = "imx-pcm512x",
		.pm = &snd_soc_pm_ops,
		.of_match_table = imx_pcm512x_dt_ids,
	},
	.probe = imx_pcm512x_probe,
	.remove = imx_pcm512x_remove,
};
module_platform_driver(imx_pcm512x_driver);

MODULE_DESCRIPTION("NXP i.MX pcm512x ASoC machine driver");
MODULE_AUTHOR("Adrian Alonso <adrian.alonso@nxp.com>");
MODULE_ALIAS("platform:imx-pcm512x");
MODULE_LICENSE("GPL v2");
