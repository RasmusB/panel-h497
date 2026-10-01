// SPDX-License-Identifier: GPL-2.0
/*
 * DRM panel driver for the AUO/Topwin H497TLB01 4.97" 720x1280 AMOLED
 * (Raydium RM69052 driver IC, MIPI DSI video mode).
 *
 * Init sequence taken from an I2C capture of a working TC358870
 * HDMI->DSI bridge board driving this panel. Power/reset timing from
 * the H497TLB01 v0.2 datasheet. Video timings from the EDID the
 * bridge presented (66.9 MHz, 60 Hz); override with module parameters.
 *
 * Brightness is exposed as /sys/class/backlight/panel-h497 and sent to the
 * panel with the standard DCS Set Display Brightness (0x51) command.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>

#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>

static unsigned int clock_khz = 66900;
static unsigned int hfp = 60, hsync = 40, hbp = 35;
static unsigned int vfp = 8, vsync = 8, vbp = 8;
static bool burst = true;

module_param(clock_khz, uint, 0444);
MODULE_PARM_DESC(clock_khz, "Pixel clock in kHz (default 66900)");
module_param(hfp, uint, 0444);
MODULE_PARM_DESC(hfp, "Horizontal front porch (default 60)");
module_param(hsync, uint, 0444);
MODULE_PARM_DESC(hsync, "Horizontal sync length (default 40)");
module_param(hbp, uint, 0444);
MODULE_PARM_DESC(hbp, "Horizontal back porch (default 35)");
module_param(vfp, uint, 0444);
MODULE_PARM_DESC(vfp, "Vertical front porch (default 8)");
module_param(vsync, uint, 0444);
MODULE_PARM_DESC(vsync, "Vertical sync length (default 8)");
module_param(vbp, uint, 0444);
MODULE_PARM_DESC(vbp, "Vertical back porch (default 8)");
module_param(burst, bool, 0444);
MODULE_PARM_DESC(burst, "Use DSI burst mode (default Y); N = non-burst sync pulses");

#define HACTIVE 720
#define VACTIVE 1280
#define MAX_BRIGHTNESS 255

struct h497 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *vddi;	/* 1.8 V digital I/O */
	struct regulator *vdd;	/* 3.1 V analog */
	struct gpio_desc *reset;
	enum drm_panel_orientation orientation;
	bool prepared;		/* panel powered and out of sleep */
};

static inline struct h497 *to_h497(struct drm_panel *panel)
{
	return container_of(panel, struct h497, panel);
}

/*
 * Each entry: length, then <length> bytes (DCS command + parameters).
 * Sent as DCS writes (0x15 for 1 parameter, 0x39 otherwise), in LP mode.
 */
static const u8 h497_init[] = {
	/* Page 0 */
	6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00,
	4, 0xB0, 0x00, 0x10, 0x10,
	2, 0xBA, 0x60,
	8, 0xBB, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
	/* Page 2 */
	6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x02,
	2, 0xCA, 0x04,
	2, 0xE1, 0x00,
	2, 0xE2, 0x0A,
	2, 0xE3, 0x40,
	5, 0xE7, 0x00, 0x00, 0x00, 0x00,
	9, 0xED, 0x48, 0x00, 0xE0, 0x13, 0x08, 0x00, 0x91, 0x08,
	7, 0xFD, 0x00, 0x08, 0x1C, 0x00, 0x00, 0x01,
	12, 0xC3, 0x11, 0x24, 0x04, 0x0A, 0x02, 0x04, 0x00, 0x1C, 0x10, 0xF0, 0x00,
	/* Page 3 */
	6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x03,
	2, 0xE0, 0x00,
	7, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x15,
	2, 0xF6, 0x08,
	/* Page 5 */
	6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x05,
	3, 0xC4, 0x00, 0x14,
	2, 0xC9, 0x04,
	/* Page 1 */
	6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01,
	4, 0xB0, 0x06, 0x06, 0x06,
	4, 0xB1, 0x14, 0x14, 0x14,
	4, 0xB2, 0x00, 0x00, 0x00,
	4, 0xB4, 0x66, 0x66, 0x66,
	4, 0xB5, 0x44, 0x44, 0x44,
	4, 0xB6, 0x54, 0x54, 0x54,
	4, 0xB7, 0x24, 0x24, 0x24,
	4, 0xB9, 0x04, 0x04, 0x04,
	4, 0xBA, 0x14, 0x14, 0x14,
	4, 0xBE, 0x32, 0x38, 0x78,
	/* Tearing effect output on, V-blank only */
	2, 0x35, 0x00,
	/* Write CTRL Display: enable brightness control (BCTRL) */
	2, 0x53, 0x20,
};

static int h497_send_init(struct h497 *ctx)
{
	const u8 *p = h497_init;
	const u8 *end = h497_init + sizeof(h497_init);
	ssize_t ret;

	while (p < end) {
		u8 len = *p++;

		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, p, len);
		if (ret < 0) {
			dev_err(&ctx->dsi->dev, "init cmd 0x%02x failed: %zd\n",
				p[0], ret);
			return ret;
		}
		p += len;
	}

	return 0;
}

static int h497_prepare(struct drm_panel *panel)
{
	struct h497 *ctx = to_h497(panel);
	struct device *dev = &ctx->dsi->dev;
	int ret;

	gpiod_set_value_cansleep(ctx->reset, 1);

	ret = regulator_enable(ctx->vddi);
	if (ret) {
		dev_err(dev, "failed to enable vddi: %d\n", ret);
		return ret;
	}

	ret = regulator_enable(ctx->vdd);
	if (ret) {
		dev_err(dev, "failed to enable vdd: %d\n", ret);
		goto err_vddi;
	}

	msleep(40);

	/* Reset pulse must be >= 15 ms to be accepted */
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(15);
	gpiod_set_value_cansleep(ctx->reset, 0);
	/* OTP load (tREST) can take up to 120 ms before Sleep Out is allowed */
	msleep(120);

	ret = h497_send_init(ctx);
	if (ret)
		goto err_reset;

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret < 0) {
		dev_err(dev, "sleep out failed: %d\n", ret);
		goto err_reset;
	}
	msleep(120);

	ctx->prepared = true;

	return 0;

err_reset:
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->vdd);
err_vddi:
	regulator_disable(ctx->vddi);
	return ret;
}

static int h497_enable(struct drm_panel *panel)
{
	struct h497 *ctx = to_h497(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
	if (ret < 0) {
		dev_err(&ctx->dsi->dev, "display on failed: %d\n", ret);
		return ret;
	}
	msleep(20);

	return 0;
}

static int h497_disable(struct drm_panel *panel)
{
	struct h497 *ctx = to_h497(panel);
	int ret;

	ret = mipi_dsi_dcs_set_display_off(ctx->dsi);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "display off failed: %d\n", ret);

	return 0;
}

static int h497_unprepare(struct drm_panel *panel)
{
	struct h497 *ctx = to_h497(panel);
	int ret;

	ctx->prepared = false;

	ret = mipi_dsi_dcs_enter_sleep_mode(ctx->dsi);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "sleep in failed: %d\n", ret);
	msleep(120);

	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->vdd);
	regulator_disable(ctx->vddi);

	return 0;
}

static int h497_get_modes(struct drm_panel *panel,
			  struct drm_connector *connector)
{
	struct drm_display_mode *mode;

	mode = drm_mode_create(connector->dev);
	if (!mode)
		return -ENOMEM;

	mode->clock = clock_khz;
	mode->hdisplay = HACTIVE;
	mode->hsync_start = HACTIVE + hfp;
	mode->hsync_end = HACTIVE + hfp + hsync;
	mode->htotal = HACTIVE + hfp + hsync + hbp;
	mode->vdisplay = VACTIVE;
	mode->vsync_start = VACTIVE + vfp;
	mode->vsync_end = VACTIVE + vfp + vsync;
	mode->vtotal = VACTIVE + vfp + vsync + vbp;
	mode->flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 62;
	connector->display_info.height_mm = 110;

	/*
	 * Tell userspace (fbcon, compositors) how the panel is mounted. The
	 * property must be set here, before the connector is registered.
	 */
	drm_connector_set_panel_orientation(connector, to_h497(panel)->orientation);

	return 1;
}

static enum drm_panel_orientation h497_get_orientation(struct drm_panel *panel)
{
	return to_h497(panel)->orientation;
}

static int h497_bl_update_status(struct backlight_device *bl)
{
	struct h497 *ctx = bl_get_data(bl);
	u8 level = backlight_get_brightness(bl);
	ssize_t ret;

	/*
	 * The panel can only receive commands while powered. The value is kept
	 * by the backlight core and applied by drm_panel_enable() on power-up.
	 */
	if (!ctx->prepared)
		return 0;

	/* One-byte form; the kernel helper sends a 16-bit value */
	ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
				 &level, 1);
	if (ret < 0)
		return ret;

	return 0;
}

static const struct backlight_ops h497_bl_ops = {
	.update_status = h497_bl_update_status,
};

static const struct drm_panel_funcs h497_funcs = {
	.prepare = h497_prepare,
	.unprepare = h497_unprepare,
	.enable = h497_enable,
	.disable = h497_disable,
	.get_modes = h497_get_modes,
	.get_orientation = h497_get_orientation,
};

static int h497_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct h497 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->vddi = devm_regulator_get(dev, "vddi");
	if (IS_ERR(ctx->vddi))
		return dev_err_probe(dev, PTR_ERR(ctx->vddi), "failed to get vddi\n");

	ctx->vdd = devm_regulator_get(dev, "vdd");
	if (IS_ERR(ctx->vdd))
		return dev_err_probe(dev, PTR_ERR(ctx->vdd), "failed to get vdd\n");

	/* Start with the panel held in reset */
	ctx->reset = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset))
		return dev_err_probe(dev, PTR_ERR(ctx->reset), "failed to get reset gpio\n");

	/* The FPC has 4 lanes; boards like the Pi 4B connector only wire 2 */
	ret = of_property_read_u32(dev->of_node, "dsi-lanes", &dsi->lanes);
	if (ret)
		dsi->lanes = 4;
	if (dsi->lanes < 1 || dsi->lanes > 4)
		return dev_err_probe(dev, -EINVAL, "invalid dsi-lanes %u (1-4)\n",
				     dsi->lanes);

	/* Standard "rotation" property: 0, 90, 180 or 270 degrees */
	ret = of_drm_get_panel_orientation(dev->of_node, &ctx->orientation);
	if (ret)
		return dev_err_probe(dev, ret, "invalid rotation property\n");

	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM;
	if (burst)
		dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST;
	else
		dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;

	drm_panel_init(&ctx->panel, dev, &h497_funcs, DRM_MODE_CONNECTOR_DSI);
	/* DSI host must be powered up before we send init commands */
	ctx->panel.prepare_prev_first = true;

	/*
	 * Attached to the drm_panel, so the DRM core turns it on after
	 * enable() and off before disable().
	 */
	{
		const struct backlight_properties props = {
			.type = BACKLIGHT_RAW,
			.brightness = MAX_BRIGHTNESS,
			.max_brightness = MAX_BRIGHTNESS,
		};

		ctx->panel.backlight = devm_backlight_device_register(dev,
					"panel-h497", dev, ctx, &h497_bl_ops, &props);
		if (IS_ERR(ctx->panel.backlight))
			return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
					     "failed to register backlight\n");
	}

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		return ret;
	}

	return 0;
}

static void h497_remove(struct mipi_dsi_device *dsi)
{
	struct h497 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id h497_of_match[] = {
	{ .compatible = "auo,h497tlb01" },
	{ }
};
MODULE_DEVICE_TABLE(of, h497_of_match);

static struct mipi_dsi_driver h497_driver = {
	.probe = h497_probe,
	.remove = h497_remove,
	.driver = {
		.name = "panel-h497",
		.of_match_table = h497_of_match,
	},
};
module_mipi_dsi_driver(h497_driver);

MODULE_DESCRIPTION("AUO H497TLB01 (RM69052) AMOLED DSI panel driver");
MODULE_LICENSE("GPL");
