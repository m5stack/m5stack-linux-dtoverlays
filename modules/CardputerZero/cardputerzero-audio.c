// SPDX-License-Identifier: GPL-2.0-only
/*
 * Cardputer Zero ASoC machine driver.
 *
 * This is intentionally close to simple-audio-card, with one board policy:
 * when headphones are inserted, disable the DAPM pins listed in
 * m5stack,hp-mute-pins; when removed, enable them again.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/version.h>
#include <sound/control.h>
#include <sound/jack.h>
#include <sound/simple_card_utils.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include <sound/soc-jack.h>

#define DAI	"sound-dai"
#define CELL	"#sound-dai-cells"
#define PREFIX	"simple-audio-card,"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define CARDPUTERZERO_CARD_DAPM(card)	((card)->dapm)
#else
#define CARDPUTERZERO_CARD_DAPM(card)	(&(card)->dapm)
#endif

struct cardputerzero_audio {
	struct simple_util_priv simple;
	struct notifier_block hp_notifier;
	bool hp_notifier_registered;
	const char **hp_mute_pins;
	unsigned int num_hp_mute_pins;
	struct cardputerzero_control_setting *hp_absent_controls;
	unsigned int num_hp_absent_controls;
	struct cardputerzero_control_setting *hp_present_controls;
	unsigned int num_hp_present_controls;
};

struct cardputerzero_control_setting {
	const char *control;
	const char *value;
};

static const struct snd_soc_ops cardputerzero_ops = {
	.startup = simple_util_startup,
	.shutdown = simple_util_shutdown,
	.hw_params = simple_util_hw_params,
};

static struct cardputerzero_audio *
cardputerzero_from_priv(struct simple_util_priv *priv)
{
	return container_of(priv, struct cardputerzero_audio, simple);
}

static int cardputerzero_set_hp_mute_pins(struct cardputerzero_audio *cz,
					  bool enable)
{
	struct snd_soc_card *card = simple_priv_to_card(&cz->simple);
	unsigned int i;
	int ret;

	if (!cz->num_hp_mute_pins)
		return 0;

	for (i = 0; i < cz->num_hp_mute_pins; i++) {
		if (enable)
			ret = snd_soc_dapm_enable_pin(CARDPUTERZERO_CARD_DAPM(card),
						      cz->hp_mute_pins[i]);
		else
			ret = snd_soc_dapm_disable_pin(CARDPUTERZERO_CARD_DAPM(card),
						       cz->hp_mute_pins[i]);

		if (ret)
			return ret;
	}

	return snd_soc_dapm_sync(CARDPUTERZERO_CARD_DAPM(card));
}

static void cardputerzero_unregister_hp_notifier(struct cardputerzero_audio *cz)
{
	struct simple_util_priv *priv = &cz->simple;

	if (!cz->hp_notifier_registered)
		return;

	snd_soc_jack_notifier_unregister(&priv->hp_jack.jack, &cz->hp_notifier);
	cz->hp_notifier_registered = false;
}

static int cardputerzero_set_enum_control_locked(struct snd_soc_card *card,
						 const char *control,
						 const char *value)
{
	struct snd_kcontrol *kcontrol;
	struct soc_enum *e;
	struct snd_ctl_elem_value *ucontrol;
	unsigned int i;
	int ret;

	kcontrol = snd_soc_card_get_kcontrol(card, control);
	if (!kcontrol || !kcontrol->put)
		return -ENOENT;

	if (kcontrol->info != snd_soc_info_enum_double)
		return -EINVAL;

	e = (struct soc_enum *)kcontrol->private_value;
	for (i = 0; i < e->items; i++) {
		if (!strcmp(e->texts[i], value))
			break;
	}

	if (i == e->items)
		return -EINVAL;

	ucontrol = kzalloc(sizeof(*ucontrol), GFP_KERNEL);
	if (!ucontrol)
		return -ENOMEM;

	ucontrol->value.enumerated.item[0] = i;
	ret = kcontrol->put(kcontrol, ucontrol);
	if (ret > 0)
		snd_ctl_notify_one(card->snd_card, SNDRV_CTL_EVENT_MASK_VALUE,
				   kcontrol, 0);
	kfree(ucontrol);

	return ret < 0 ? ret : 0;
}

static int cardputerzero_set_enum_control(struct snd_soc_card *card,
					  const char *control,
					  const char *value)
{
	int ret;

	down_read(&card->snd_card->controls_rwsem);
	ret = cardputerzero_set_enum_control_locked(card, control, value);
	up_read(&card->snd_card->controls_rwsem);

	return ret;
}

static void cardputerzero_apply_controls(struct cardputerzero_audio *cz,
					 bool hp_inserted)
{
	struct snd_soc_card *card = simple_priv_to_card(&cz->simple);
	struct device *dev = simple_priv_to_dev(&cz->simple);
	struct cardputerzero_control_setting *settings;
	unsigned int num_settings;
	unsigned int i;
	int ret;

	if (hp_inserted) {
		settings = cz->hp_present_controls;
		num_settings = cz->num_hp_present_controls;
	} else {
		settings = cz->hp_absent_controls;
		num_settings = cz->num_hp_absent_controls;
	}

	for (i = 0; i < num_settings; i++) {
		ret = cardputerzero_set_enum_control(card, settings[i].control,
						     settings[i].value);
		if (ret)
			dev_warn(dev, "failed to set %s=%s: %d\n",
				 settings[i].control, settings[i].value, ret);
	}
}

static int cardputerzero_hp_event(struct notifier_block *nb,
				  unsigned long event, void *data)
{
	struct cardputerzero_audio *cz =
		container_of(nb, struct cardputerzero_audio, hp_notifier);
	bool hp_inserted = event & SND_JACK_HEADPHONE;
	int ret;

	ret = cardputerzero_set_hp_mute_pins(cz, !hp_inserted);
	if (!ret)
		cardputerzero_apply_controls(cz, hp_inserted);

	return notifier_from_errno(ret);
}

static int cardputerzero_parse_control_settings(struct cardputerzero_audio *cz,
						struct device *dev,
						const char *prop,
						struct cardputerzero_control_setting **settings,
						unsigned int *num_settings)
{
	struct cardputerzero_control_setting *parsed;
	int count;
	int i;
	int ret;

	count = of_property_count_strings(dev->of_node, prop);
	if (count <= 0) {
		*settings = NULL;
		*num_settings = 0;
		return 0;
	}

	parsed = devm_kcalloc(dev, count, sizeof(*parsed), GFP_KERNEL);
	if (!parsed)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		const char *entry;
		char *copy;
		char *sep;

		ret = of_property_read_string_index(dev->of_node, prop, i,
						    &entry);
		if (ret)
			return ret;

		copy = devm_kstrdup(dev, entry, GFP_KERNEL);
		if (!copy)
			return -ENOMEM;

		sep = strchr(copy, '=');
		if (!sep || sep == copy || !sep[1])
			return dev_err_probe(dev, -EINVAL,
					     "invalid %s entry: %s\n",
					     prop, entry);

		*sep = '\0';
		parsed[i].control = copy;
		parsed[i].value = sep + 1;
	}

	*settings = parsed;
	*num_settings = count;

	return 0;
}

static int cardputerzero_parse_hp_mute_pins(struct cardputerzero_audio *cz,
					    struct device *dev)
{
	int count;
	int i;
	int ret;

	count = of_property_count_strings(dev->of_node, "m5stack,hp-mute-pins");
	if (count <= 0) {
		cz->hp_mute_pins = NULL;
		cz->num_hp_mute_pins = 0;
		return 0;
	}

	cz->hp_mute_pins = devm_kcalloc(dev, count, sizeof(*cz->hp_mute_pins),
					GFP_KERNEL);
	if (!cz->hp_mute_pins)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		ret = of_property_read_string_index(dev->of_node,
						    "m5stack,hp-mute-pins",
						    i, &cz->hp_mute_pins[i]);
		if (ret)
			return ret;
	}

	cz->num_hp_mute_pins = count;

	return 0;
}

static int cardputerzero_parse_dlc(struct simple_util_priv *priv,
				   struct device_node *node,
				   struct snd_soc_dai_link_component *dlc,
				   int *is_single_link)
{
	struct device *dev = simple_priv_to_dev(priv);
	struct of_phandle_args args;
	struct snd_soc_dai_link_component resolved_dlc = {};
	struct snd_soc_dai *dai;
	const char *fallback_dai_name;
	int ret;

	if (!node)
		return 0;

	ret = of_parse_phandle_with_args(node, DAI, CELL, 0, &args);
	if (ret)
		return ret;

	dai = snd_soc_get_dai_via_args(&args);
	if (dai) {
		dlc->of_node = args.np;
		dlc->dai_name = snd_soc_dai_name_get(dai);
		dlc->dai_args = snd_soc_copy_dai_args(dev, &args);
		if (!dlc->dai_args) {
			of_node_put(dlc->of_node);
			dlc->of_node = NULL;
			dlc->dai_name = NULL;
			return -ENOMEM;
		}
	} else {
		ret = snd_soc_get_dlc(&args, &resolved_dlc);
		if (ret < 0) {
			of_node_put(args.np);
			return ret;
		}

		fallback_dai_name = resolved_dlc.dai_name;
		if (fallback_dai_name) {
			fallback_dai_name = devm_kstrdup_const(dev,
							       fallback_dai_name,
							       GFP_KERNEL);
			if (!fallback_dai_name) {
				of_node_put(resolved_dlc.of_node);
				return -ENOMEM;
			}
		}

		dlc->of_node = resolved_dlc.of_node;
		dlc->dai_name = fallback_dai_name;
		dlc->dai_args = resolved_dlc.dai_args;
	}

	if (is_single_link)
		*is_single_link = !args.args_count;

	return 0;
}

static int cardputerzero_parse_platform(struct simple_util_priv *priv,
					struct device_node *node,
					struct snd_soc_dai_link_component *dlc)
{
	return cardputerzero_parse_dlc(priv, node, dlc, NULL);
}

static int cardputerzero_parse_node(struct simple_util_priv *priv,
				    struct device_node *np,
				    struct link_info *li,
				    char *prefix,
				    int *cpu)
{
	struct device *dev = simple_priv_to_dev(priv);
	struct snd_soc_dai_link *dai_link = simple_priv_to_link(priv, li->link);
	struct simple_dai_props *dai_props = simple_priv_to_props(priv, li->link);
	struct snd_soc_dai_link_component *dlc;
	struct simple_util_dai *dai;
	int ret;

	if (cpu) {
		dlc = snd_soc_link_to_cpu(dai_link, 0);
		dai = simple_props_to_dai_cpu(dai_props, 0);
	} else {
		dlc = snd_soc_link_to_codec(dai_link, 0);
		dai = simple_props_to_dai_codec(dai_props, 0);
	}

	ret = cardputerzero_parse_dlc(priv, np, dlc, cpu);
	if (ret)
		return ret;

	ret = simple_util_parse_clk(dev, np, dai, dlc);
	if (ret)
		return ret;

	return simple_util_parse_tdm(np, dai);
}

static int cardputerzero_link_init(struct simple_util_priv *priv,
				   struct device_node *cpu,
				   struct device_node *codec,
				   struct link_info *li,
				   char *prefix)
{
	struct device_node *node = of_get_parent(cpu);
	struct snd_soc_dai_link *dai_link = simple_priv_to_link(priv, li->link);
	struct snd_soc_dai_link_component *cpus = snd_soc_link_to_cpu(dai_link, 0);
	struct snd_soc_dai_link_component *codecs = snd_soc_link_to_codec(dai_link, 0);
	char dai_name[64];
	int ret;

	ret = simple_util_parse_daifmt(simple_priv_to_dev(priv), node, codec,
				       prefix, &dai_link->dai_fmt);
	of_node_put(node);
	if (ret < 0)
		return ret;

	dai_link->init = simple_util_dai_init;
	dai_link->ops = &cardputerzero_ops;

	snprintf(dai_name, sizeof(dai_name), "%s-%s",
		 cpus->dai_name, codecs->dai_name);

	return simple_util_set_dailink_name(priv, dai_link, dai_name);
}

static int cardputerzero_parse_link(struct simple_util_priv *priv,
				    struct link_info *li)
{
	struct device *dev = simple_priv_to_dev(priv);
	struct device_node *top = dev->of_node;
	struct device_node *link;
	struct device_node *cpu;
	struct device_node *codec;
	struct device_node *plat;
	struct snd_soc_dai_link *dai_link = simple_priv_to_link(priv, 0);
	struct snd_soc_dai_link_component *cpus = snd_soc_link_to_cpu(dai_link, 0);
	struct snd_soc_dai_link_component *platforms = snd_soc_link_to_platform(dai_link, 0);
	char *prefix = "";
	int ret;
	int single_cpu = 0;

	link = of_get_child_by_name(top, PREFIX "dai-link");
	if (!link) {
		link = of_node_get(top);
		prefix = PREFIX;
	}

	cpu = of_get_child_by_name(link, prefix[0] ? PREFIX "cpu" : "cpu");
	codec = of_get_child_by_name(link, prefix[0] ? PREFIX "codec" : "codec");
	plat = of_get_child_by_name(link, prefix[0] ? PREFIX "plat" : "plat");

	if (!cpu || !codec) {
		ret = -ENODEV;
		goto out;
	}

	ret = cardputerzero_parse_node(priv, cpu, li, prefix, &single_cpu);
	if (ret)
		goto out;

	ret = cardputerzero_parse_node(priv, codec, li, prefix, NULL);
	if (ret)
		goto out;

	ret = cardputerzero_parse_platform(priv, plat, platforms);
	if (ret)
		goto out;

	simple_util_canonicalize_cpu(cpus, single_cpu);
	simple_util_canonicalize_platform(platforms, cpus);

	ret = cardputerzero_link_init(priv, cpu, codec, li, prefix);

out:
	of_node_put(plat);
	of_node_put(codec);
	of_node_put(cpu);
	of_node_put(link);

	return ret;
}

static int cardputerzero_parse_of(struct simple_util_priv *priv,
				  struct link_info *li)
{
	struct snd_soc_card *card = simple_priv_to_card(priv);
	int ret;

	ret = simple_util_parse_widgets(card, PREFIX);
	if (ret)
		return ret;

	ret = simple_util_parse_routing(card, PREFIX);
	if (ret)
		return ret;

	ret = simple_util_parse_pin_switches(card, PREFIX);
	if (ret)
		return ret;

	ret = cardputerzero_parse_link(priv, li);
	if (ret)
		return ret;

	li->link = 1;

	ret = simple_util_parse_card_name(priv, PREFIX);
	if (ret)
		return ret;

	return snd_soc_of_parse_aux_devs(card, PREFIX "aux-devs");
}

static int cardputerzero_soc_probe(struct snd_soc_card *card)
{
	struct simple_util_priv *priv = snd_soc_card_get_drvdata(card);
	struct cardputerzero_audio *cz = cardputerzero_from_priv(priv);
	bool hp_inserted;
	int ret;

	ret = simple_util_init_hp(card, &priv->hp_jack, PREFIX);
	if (ret < 0)
		return ret;

	cz->hp_notifier.notifier_call = cardputerzero_hp_event;
	snd_soc_jack_notifier_register(&priv->hp_jack.jack, &cz->hp_notifier);
	cz->hp_notifier_registered = true;

	hp_inserted = priv->hp_jack.jack.status & SND_JACK_HEADPHONE;
	ret = cardputerzero_set_hp_mute_pins(cz, !hp_inserted);
	if (ret)
		goto err_unregister_hp_notifier;
	cardputerzero_apply_controls(cz, hp_inserted);

	ret = simple_util_init_mic(card, &priv->mic_jack, PREFIX);
	if (ret < 0)
		goto err_unregister_hp_notifier;

	ret = simple_util_init_aux_jacks(priv, PREFIX);
	if (ret < 0)
		goto err_unregister_hp_notifier;

	return 0;

err_unregister_hp_notifier:
	cardputerzero_unregister_hp_notifier(cz);
	return ret;
}

static int cardputerzero_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cardputerzero_audio *cz;
	struct simple_util_priv *priv;
	struct snd_soc_card *card;
	struct link_info *li;
	int ret;

	cz = devm_kzalloc(dev, sizeof(*cz), GFP_KERNEL);
	if (!cz)
		return -ENOMEM;

	priv = &cz->simple;
	card = simple_priv_to_card(priv);
	card->owner = THIS_MODULE;
	card->dev = dev;
	card->probe = cardputerzero_soc_probe;
	card->driver_name = "cardputerzero-audio";
	card->fully_routed = of_property_read_bool(dev->of_node,
						   PREFIX "fully-routed");

	ret = cardputerzero_parse_hp_mute_pins(cz, dev);
	if (ret)
		return dev_err_probe(dev, ret, "failed to parse hp mute pins\n");

	ret = cardputerzero_parse_control_settings(cz, dev,
						   "m5stack,hp-absent-controls",
						   &cz->hp_absent_controls,
						   &cz->num_hp_absent_controls);
	if (ret)
		return ret;

	ret = cardputerzero_parse_control_settings(cz, dev,
						   "m5stack,hp-present-controls",
						   &cz->hp_present_controls,
						   &cz->num_hp_present_controls);
	if (ret)
		return ret;

	li = kzalloc(sizeof(*li), GFP_KERNEL);
	if (!li)
		return -ENOMEM;

	li->link = 1;
	li->num[0].cpus = 1;
	li->num[0].codecs = 1;
	li->num[0].platforms = 1;

	ret = simple_util_init_priv(priv, li);
	if (ret)
		goto err_free_li;

	memset(li, 0, sizeof(*li));
	ret = cardputerzero_parse_of(priv, li);
	if (ret) {
		dev_err_probe(dev, ret, "parse error\n");
		goto err_clean_ref;
	}

	snd_soc_card_set_drvdata(card, priv);

	ret = devm_snd_soc_register_card(dev, card);
	if (ret)
		goto err_clean_ref;

	kfree(li);
	return 0;

err_clean_ref:
	simple_util_clean_reference(card);
err_free_li:
	kfree(li);
	return dev_err_probe(dev, ret, "probe failed\n");
}

static void cardputerzero_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);
	struct simple_util_priv *priv = snd_soc_card_get_drvdata(card);
	struct cardputerzero_audio *cz = cardputerzero_from_priv(priv);

	cardputerzero_unregister_hp_notifier(cz);
	simple_util_remove(pdev);
}

static const struct of_device_id cardputerzero_audio_of_match[] = {
	{ .compatible = "m5stack,cardputerzero-audio" },
	{ }
};
MODULE_DEVICE_TABLE(of, cardputerzero_audio_of_match);

static struct platform_driver cardputerzero_audio_driver = {
	.driver = {
		.name = "cardputerzero-audio",
		.pm = &snd_soc_pm_ops,
		.of_match_table = cardputerzero_audio_of_match,
	},
	.probe = cardputerzero_probe,
	.remove = cardputerzero_remove,
};
module_platform_driver(cardputerzero_audio_driver);

MODULE_DESCRIPTION("Cardputer Zero ASoC machine driver");
MODULE_AUTHOR("M5Stack");
MODULE_LICENSE("GPL");
