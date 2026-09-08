// SPDX-License-Identifier: GPL-2.0-only
// ZTE Blade S6 (P839F30) — JDI TD4291 720p DSI panel
// Generated from downstream DT via linux-mdss-dsi-panel-driver-generator, adjusted for mainline.

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

struct zte_blade_s6_td4291 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct gpio_desc *reset_gpio;

	struct work_struct bl_work;
	atomic_t bl_pending;	/* latest level, -1 when nothing queued */
};

static inline struct zte_blade_s6_td4291 *to_zte_blade_s6_td4291(struct drm_panel *panel)
{
	return container_of(panel, struct zte_blade_s6_td4291, panel);
}

static void zte_blade_s6_td4291_reset(struct zte_blade_s6_td4291 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(5000, 6000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(150);
}

static int zte_blade_s6_td4291_on(struct zte_blade_s6_td4291 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags |= MIPI_DSI_MODE_LPM;

	/* 0x05 in the stock sequence: no parameter, unlike the writes it brackets */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xde);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb4, 0x32);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb3, 0x70);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xb0, 0x7c);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd5, 0x65);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd6, 0x77);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd7, 0x76);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xd8, 0x13);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, 0xdf);
	/* BCTRL | BL: brightness block and backlight on, dimming ramp (DD) off */
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x24);
	mipi_dsi_usleep_range(&dsi_ctx, 5000, 6000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS, 0x00);
	mipi_dsi_usleep_range(&dsi_ctx, 5000, 6000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_SET_CABC_MIN_BRIGHTNESS, 0x00);
	mipi_dsi_usleep_range(&dsi_ctx, 5000, 6000);
	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_POWER_SAVE, 0x01);
	mipi_dsi_usleep_range(&dsi_ctx, 5000, 6000);
	mipi_dsi_dcs_exit_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50);
	mipi_dsi_dcs_set_display_on_multi(&dsi_ctx);
	mipi_dsi_usleep_range(&dsi_ctx, 1000, 2000);

	return dsi_ctx.accum_err;
}

static int zte_blade_s6_td4291_off(struct zte_blade_s6_td4291 *ctx)
{
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	mipi_dsi_dcs_write_seq_multi(&dsi_ctx, MIPI_DCS_WRITE_CONTROL_DISPLAY, 0x20);
	mipi_dsi_usleep_range(&dsi_ctx, 5000, 6000);
	mipi_dsi_dcs_set_display_off_multi(&dsi_ctx);
	mipi_dsi_usleep_range(&dsi_ctx, 10000, 11000);
	mipi_dsi_dcs_enter_sleep_mode_multi(&dsi_ctx);
	mipi_dsi_msleep(&dsi_ctx, 50);

	return dsi_ctx.accum_err;
}

static int zte_blade_s6_td4291_prepare(struct drm_panel *panel)
{
	struct zte_blade_s6_td4291 *ctx = to_zte_blade_s6_td4291(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	zte_blade_s6_td4291_reset(ctx);

	ret = zte_blade_s6_td4291_on(ctx);
	if (ret < 0) {
		dev_err(dev, "Failed to initialize panel: %d\n", ret);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		return ret;
	}

	return 0;
}

static int zte_blade_s6_td4291_unprepare(struct drm_panel *panel)
{
	struct zte_blade_s6_td4291 *ctx = to_zte_blade_s6_td4291(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	ret = zte_blade_s6_td4291_off(ctx);
	if (ret < 0)
		dev_err(dev, "Failed to un-initialize panel: %d\n", ret);

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	return 0;
}

static const struct drm_display_mode zte_blade_s6_td4291_mode = {
	.clock = (720 + 96 + 40 + 72) * (1280 + 12 + 4 + 4) * 60 / 1000,
	.hdisplay = 720,
	.hsync_start = 720 + 96,
	.hsync_end = 720 + 96 + 40,
	.htotal = 720 + 96 + 40 + 72,
	.vdisplay = 1280,
	.vsync_start = 1280 + 12,
	.vsync_end = 1280 + 12 + 4,
	.vtotal = 1280 + 12 + 4 + 4,
	.width_mm = 65,
	.height_mm = 118,
	.type = DRM_MODE_TYPE_DRIVER,
};

static int zte_blade_s6_td4291_get_modes(struct drm_panel *panel,
					  struct drm_connector *connector)
{
	return drm_connector_helper_get_modes_fixed(connector, &zte_blade_s6_td4291_mode);
}

static const struct drm_panel_funcs zte_blade_s6_td4291_panel_funcs = {
	.prepare = zte_blade_s6_td4291_prepare,
	.unprepare = zte_blade_s6_td4291_unprepare,
	.get_modes = zte_blade_s6_td4291_get_modes,
};

static int bl_mode;
module_param(bl_mode, int, 0644);
MODULE_PARM_DESC(bl_mode, "brightness DCS: 0=HS, 1=LP, 2=skip");

/* The DSI controller defers a command to the next frame boundary, so each DCS
 * write blocks for a whole frame while video streams. Coalesce updates onto a
 * work item: a slider drag then costs one write, not one per step.
 */
static void zte_blade_s6_td4291_bl_work(struct work_struct *work)
{
	struct zte_blade_s6_td4291 *ctx =
		container_of(work, struct zte_blade_s6_td4291, bl_work);
	struct mipi_dsi_device *dsi = ctx->dsi;
	int level;

	while ((level = atomic_xchg(&ctx->bl_pending, -1)) >= 0) {
		u8 brightness = level;

		/* 0 = HS, 1 = LP, 2 = drop the write entirely */
		if (bl_mode == 2)
			continue;

		if (bl_mode == 1)
			dsi->mode_flags |= MIPI_DSI_MODE_LPM;
		else
			dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

		mipi_dsi_dcs_write(dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				   &brightness, 1);
		dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	}
}

static int zte_blade_s6_td4291_bl_update_status(struct backlight_device *bl)
{
	struct mipi_dsi_device *dsi = bl_get_data(bl);
	struct zte_blade_s6_td4291 *ctx = mipi_dsi_get_drvdata(dsi);
	u8 brightness = backlight_get_brightness(bl);

	/* downstream floors the DCS level at 10 of 205; below that the panel
	 * driver misbehaves rather than dimming further */
	if (brightness > 0 && brightness < 10)
		brightness = 10;

	atomic_set(&ctx->bl_pending, brightness);
	schedule_work(&ctx->bl_work);

	return 0;
}

static const struct backlight_ops zte_blade_s6_td4291_bl_ops = {
	.update_status = zte_blade_s6_td4291_bl_update_status,
};

static struct backlight_device *
zte_blade_s6_td4291_create_backlight(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	const struct backlight_properties props = {
		.type = BACKLIGHT_RAW,
		.brightness = 205,
		.max_brightness = 205,
	};

	return devm_backlight_device_register(dev, dev_name(dev), dev, dsi,
					      &zte_blade_s6_td4291_bl_ops, &props);
}

static int zte_blade_s6_td4291_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct zte_blade_s6_td4291 *ctx;
	int ret;

	ctx = devm_drm_panel_alloc(dev, struct zte_blade_s6_td4291, panel,
				   &zte_blade_s6_td4291_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio),
				     "Failed to get reset-gpios\n");

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	INIT_WORK(&ctx->bl_work, zte_blade_s6_td4291_bl_work);
	atomic_set(&ctx->bl_pending, -1);

	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_VIDEO_HSE |
			  MIPI_DSI_CLOCK_NON_CONTINUOUS;

	ctx->panel.prepare_prev_first = true;

	ctx->panel.backlight = zte_blade_s6_td4291_create_backlight(dsi);
	if (IS_ERR(ctx->panel.backlight))
		return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
				     "Failed to create backlight\n");

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "Failed to attach to DSI host\n");
	}

	return 0;
}

static void zte_blade_s6_td4291_remove(struct mipi_dsi_device *dsi)
{
	struct zte_blade_s6_td4291 *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	cancel_work_sync(&ctx->bl_work);

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "Failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id zte_blade_s6_td4291_of_match[] = {
	{ .compatible = "zte,blade-s6-td4291-jdi" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, zte_blade_s6_td4291_of_match);

static struct mipi_dsi_driver zte_blade_s6_td4291_driver = {
	.probe = zte_blade_s6_td4291_probe,
	.remove = zte_blade_s6_td4291_remove,
	.driver = {
		.name = "panel-zte-blade-s6-td4291-jdi",
		.of_match_table = zte_blade_s6_td4291_of_match,
	},
};
module_mipi_dsi_driver(zte_blade_s6_td4291_driver);

MODULE_AUTHOR("ZTE Blade S6 postmarketOS port");
MODULE_DESCRIPTION("DRM panel driver for ZTE Blade S6 TD4291 JDI 720p DSI");
MODULE_LICENSE("GPL");
