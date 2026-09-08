// SPDX-License-Identifier: GPL-2.0
/*
 * Elida jd9852 3.5" MIPI-DSI panel driver
 * Copyright (C) 2020 Theobroma Systems Design und Consulting GmbH
 *
 * based on
 *
 * Rockteck jh057n00900 5.5" MIPI-DSI panel driver
 * Copyright (C) Purism SPC 2019
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/media-bus-format.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/display_timing.h>
#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

struct jd9852 {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	struct regulator *vdd;
	struct regulator *iovcc;
	enum drm_panel_orientation orientation;
};

static inline struct jd9852 *panel_to_jd9852(struct drm_panel *panel)
{
	return container_of(panel, struct jd9852, panel);
}

static void jd9852_init_sequence(struct mipi_dsi_multi_context *dsi_ctx)
{
	/*
	 * Init sequence was supplied by the panel vendor with minimal
	 * documentation.
	 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xdf, 0x98, 0x51, 0xe9);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb2, 0x00, 0x37);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb7, 0x01, 0x74, 0x01, 0x74);

	/* Gamma 2.2 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc8,
					0x3f, 0x2a, 0x23, 0x22,
					0x25, 0x2b, 0x27, 0x27,
					0x25, 0x23, 0x1e, 0x0f,
					0x09, 0x03, 0x00, 0x00,
					0x3f, 0x2a, 0x23, 0x22,
					0x25, 0x2b, 0x27, 0x27,
					0x25, 0x23, 0x1e, 0x0f,
					0x09, 0x03, 0x00, 0x00);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbb,
					0x46, 0x2f, 0x40, 0x40,
					0x7c, 0x60, 0x70, 0x70);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc0, 0x22, 0x20);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc1, 0x12);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc3,
					0x08, 0x00, 0x04, 0x08,
					0x06, 0x4d, 0x4e, 0x78,
					0x2c);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc4,
					0x00, 0xa0, 0x79, 0x0e,
					0x0a, 0x28, 0x79, 0x0e,
					0x0a, 0x28, 0x79, 0x0e,
					0x0a, 0x28, 0x82, 0x00,
					0x03);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd0,
					0x04, 0x00, 0x6b, 0x0f,
					0x01, 0x03);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xd7, 0x1a, 0x30);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x02);

	/* VGH = 15 V */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb8,
					0x1f, 0x18, 0x2f, 0x25,
					0x1e);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xb7,
					0x63, 0x00, 0x0c, 0x03);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xbe, 0x00);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc1,
					0x10, 0x66, 0x66, 0x01);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xc4, 0x72, 0x02);

	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0xde, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x20); /* NINV */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x35); /* TE ON */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x36, 0x00);
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x3a, 0x66); /* RGB666 */

	/* Column address: X = 35..204 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2a,
					0x00, 0x23, 0x00, 0xcc);

	/* Page address: Y = 0..319 */
	mipi_dsi_dcs_write_seq_multi(dsi_ctx, 0x2b,
					0x00, 0x00, 0x01, 0x3f);

}

static int jd9852_unprepare(struct drm_panel *panel)
{
	struct jd9852 *ctx = panel_to_jd9852(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	if (dsi_ctx.accum_err)
		return dsi_ctx.accum_err;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	regulator_disable(ctx->iovcc);
	regulator_disable(ctx->vdd);

	return 0;
}

static int jd9852_prepare(struct drm_panel *panel)
{
	struct jd9852 *ctx = panel_to_jd9852(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = dsi };

	dev_dbg(ctx->dev, "Resetting the panel\n");
	dsi_ctx.accum_err = regulator_enable(ctx->vdd);
	if (dsi_ctx.accum_err) {
		dev_err(ctx->dev, "Failed to enable vdd supply: %d\n",
			dsi_ctx.accum_err);
		return dsi_ctx.accum_err;
	}

	dsi_ctx.accum_err = regulator_enable(ctx->iovcc);
	if (dsi_ctx.accum_err) {
		dev_err(ctx->dev, "Failed to enable iovcc supply: %d\n",
			dsi_ctx.accum_err);
		goto disable_vdd;
	}

	/* Vendor sequence: reset high 20 ms, low 20 ms, high 120 ms. */
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(120);

	mipi_dsi_dcs_soft_reset_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	jd9852_init_sequence(&dsi_ctx);
	if (!dsi_ctx.accum_err)
		dev_dbg(ctx->dev, "Panel init sequence done\n");
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 120);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 20);

	if (dsi_ctx.accum_err)
		goto disable_iovcc;

	return 0;

disable_iovcc:
	regulator_disable(ctx->iovcc);
disable_vdd:
	regulator_disable(ctx->vdd);
	return dsi_ctx.accum_err;
}

static const struct drm_display_mode default_mode = {
	.clock = 8334,
	.hdisplay = 170,
	.hsync_start = 170 + 187,
	.hsync_end = 170 + 187 + 28,
	.htotal = 170 + 187 + 28 + 28,
	.vdisplay = 320,
	.vsync_start = 320 + 8,
	.vsync_end = 320 + 8 + 2,
	.vtotal = 320 + 8 + 2 + 6,
	.width_mm = 22,
	.height_mm = 42,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static int jd9852_get_modes(struct drm_panel *panel,
				struct drm_connector *connector)
{
	struct jd9852 *ctx = panel_to_jd9852(panel);
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(connector->dev, &default_mode);
	if (!mode) {
		dev_err(ctx->dev, "Failed to add mode %ux%u@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			drm_mode_vrefresh(&default_mode));
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	connector->display_info.width_mm = mode->width_mm;
	connector->display_info.height_mm = mode->height_mm;
	drm_mode_probed_add(connector, mode);

	return 1;
}

static enum drm_panel_orientation jd9852_get_orientation(struct drm_panel *panel)
{
	struct jd9852 *ctx = panel_to_jd9852(panel);

	return ctx->orientation;
}

static const struct drm_panel_funcs jd9852_funcs = {
	.unprepare	= jd9852_unprepare,
	.prepare	= jd9852_prepare,
	.get_modes	= jd9852_get_modes,
	.get_orientation = jd9852_get_orientation,
};

static int jd9852_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct jd9852 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct jd9852, panel,
				   &jd9852_funcs, DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset gpio\n");
		return PTR_ERR(ctx->reset_gpio);
	}

	ctx->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->vdd)) {
		ret = PTR_ERR(ctx->vdd);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to request vdd regulator: %d\n", ret);
		return ret;
	}

	ctx->iovcc = devm_regulator_get(dev, "iovcc");
	if (IS_ERR(ctx->iovcc)) {
		ret = PTR_ERR(ctx->iovcc);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "Failed to request iovcc regulator: %d\n", ret);
		return ret;
	}

	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret < 0) {
		dev_err(dev, "%pOF: failed to get orientation %d\n", dev->of_node, ret);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	ctx->panel.prepare_prev_first = true;

	dsi->lanes = 1;
	dsi->format = MIPI_DSI_FMT_RGB666;
	dsi->hs_rate = 200000000;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO |
			  MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM |
			  MIPI_DSI_MODE_NO_EOT_PACKET |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	dev_info(dev,
		 "DSI config: 170x320 lanes=%u format=%u hs_rate=%lu flags=0x%lx\n",
		 dsi->lanes, dsi->format, dsi->hs_rate, dsi->mode_flags);


	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return ret;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach failed: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void jd9852_remove(struct mipi_dsi_device *dsi)
{
	struct jd9852 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id jd9852_of_match[] = {
	{ .compatible = "elida,jd9852" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, jd9852_of_match);

static struct mipi_dsi_driver jd9852_driver = {
	.driver = {
		.name = "panel-elida-jd9852",
		.of_match_table = jd9852_of_match,
	},
	.probe	= jd9852_probe,
	.remove = jd9852_remove,
};
module_mipi_dsi_driver(jd9852_driver);

MODULE_AUTHOR("dianjixz <dianjixz@m5stack.com>");
MODULE_DESCRIPTION("DRM driver for Elida jd9852 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
