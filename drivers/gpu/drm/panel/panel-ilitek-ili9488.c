// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) STMicroelectronics SA 2017
 *
 * Authors: Philippe Cornu <philippe.cornu@st.com>
 *          Yannick Fertre <yannick.fertre@st.com>
 */

#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>
#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/regulator/consumer.h>
#include <video/mipi_display.h>

/* Manufacturer Command Set */
#define ILI9488_CMD_INTERFACEMODECTRL		0xb0
#define ILI9488_CMD_FRAMERATECTRL		0xb1
#define ILI9488_CMD_DISPLAYINVERSIONCTRL	0xb4
#define ILI9488_CMD_DISPLAYFUNCTIONCTRL	0xb6
#define ILI9488_CMD_POWERCONTROL1		0xc0
#define ILI9488_CMD_POWERCONTROL2		0xc1
#define ILI9488_CMD_VCOMCONTROL		0xc5
#define ILI9488_CMD_POSITIVEGAMMA		0xe0
#define ILI9488_CMD_NEGATIVEGAMMA		0xe1
#define ILI9488_CMD_SETIMAGEFUNCTION		0xe9
#define ILI9488_CMD_ADJUSTCONTROL3		0xf7

struct ili9488 {
	struct device *dev;
	struct drm_panel panel;
	struct gpio_desc *reset_gpio;
	struct regulator *supply;
	bool prepared;
	bool enabled;
};

static const struct drm_display_mode default_mode = {
	.hdisplay	= 320,
	.hsync_start	= 320 + 130,
	.hsync_end	= 320 + 130 + 4,
	.htotal		= 320 + 130 + 4 + 130,
	.vdisplay	= 480,
	.vsync_start	= 480 + 2,
	.vsync_end	= 480 + 2 + 1,
	.vtotal		= 480 + 2 + 1 + 2,
	.vrefresh	= 60,
	.clock		= 17000,
	.width_mm	= 42,
	.height_mm	= 82,
};

static inline struct ili9488 *panel_to_ili9488(struct drm_panel *panel)
{
	return container_of(panel, struct ili9488, panel);
}

static void ili9488_dcs_write_buf(struct ili9488 *ctx, const void *data,
				   size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);

	if (mipi_dsi_dcs_write_buffer(dsi, data, len) < 0)
		DRM_WARN("mipi dsi dcs write buffer failed\n");
}


#define dsi_dcs_write_seq(dsi, cmd, seq...) do {			\
		static const u8 d[] = { seq };				\
		int ret;						\
		ret = mipi_dsi_dcs_write(dsi, cmd, d, ARRAY_SIZE(d));	\
		if (ret < 0)						\
			return ret;					\
	} while (0)

static int ili9488_init_sequence(struct ili9488 *ctx)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	struct device *dev = ctx->dev;
	int ret;

	dsi_dcs_write_seq(dsi, ILI9488_CMD_POSITIVEGAMMA,
			  0x00, 0x13, 0x18, 0x04, 0x0f, 0x06, 0x3a, 0x56,
			  0x4d, 0x03, 0x0a, 0x06, 0x30, 0x3e, 0x0f);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_NEGATIVEGAMMA,
			  0x00, 0x13, 0x18, 0x01, 0x11, 0x06, 0x38, 0x34,
			  0x4d, 0x06, 0x0d, 0x0b, 0x31, 0x37, 0x0f);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_POWERCONTROL1, 0x18, 0x17);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_POWERCONTROL2, 0x41);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_VCOMCONTROL, 0x00, 0x1a, 0x80);
	dsi_dcs_write_seq(dsi, MIPI_DCS_SET_ADDRESS_MODE, 0x48);
	dsi_dcs_write_seq(dsi, MIPI_DCS_SET_PIXEL_FORMAT, 0x55);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_INTERFACEMODECTRL, 0x00);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_FRAMERATECTRL, 0xa0);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_DISPLAYINVERSIONCTRL, 0x02);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_DISPLAYFUNCTIONCTRL,
			  0x20, 0x02);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_SETIMAGEFUNCTION, 0x00);
	dsi_dcs_write_seq(dsi, ILI9488_CMD_ADJUSTCONTROL3,
			  0xa9, 0x51, 0x2c, 0x82);
	mipi_dsi_dcs_write(dsi, MIPI_DCS_ENTER_INVERT_MODE, NULL, 0);

	dev_err(dev, "Panel init sequence done\n");

	return 0;
}

static int ili9488_disable(struct drm_panel *panel)
{
	struct ili9488 *ctx = panel_to_ili9488(panel);

	if (!ctx->enabled)
		return 0; /* This is not an issue so we return 0 here */

	backlight_disable(ctx->bl_dev);

	ctx->enabled = false;

	return 0;
}

static int ili9488_unprepare(struct drm_panel *panel)
{
	struct ili9488 *ctx = panel_to_ili9488(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (!ctx->prepared)
		return 0;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret)
		return ret;

	mdelay(10);

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret)
		return ret;

	mdelay(10);

	regulator_disable(ctx->supply);

	ctx->prepared = false;

	return 0;
}

static int ili9488_prepare(struct drm_panel *panel)
{
	struct ili9488 *ctx = panel_to_ili9488(panel);
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	int ret;

	if (ctx->prepared)
		return 0;

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	}

	mdelay(20);

	ret = regulator_enable(ctx->supply);
	if (ret < 0) {
		dev_err(ctx->dev, "failed to enable supply: %d\n", ret);
		return ret;
	}

	mdelay(120);

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
		mdelay(20);
	}

	ret = mipi_dsi_dcs_exit_sleep_mode(dsi);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to exit sleep mode: %d\n", ret);
		return ret;
	}

	ret = ili9488_init_sequence(ctx);
	if (ret)
		return ret;

	ret = mipi_dsi_dcs_set_display_on(dsi);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to set display on: %d\n", ret);
		return ret;
	}

	msleep(50);	

	ctx->prepared = true;

	return 0;
}

static int ili9488_enable(struct drm_panel *panel)
{
	struct ili9488 *ctx = panel_to_ili9488(panel);

	if (ctx->enabled)
		return 0;

	backlight_enable(ctx->bl_dev);

	ctx->enabled = true;

	return 0;
}

static int ili9488_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		printk("failed to add mode %ux%ux@%u\n",
			  default_mode.hdisplay, default_mode.vdisplay,
			  default_mode.vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode);

	panel->connector->display_info.width_mm = mode->width_mm;
	panel->connector->display_info.height_mm = mode->height_mm;

	return 1;
}

static const struct drm_panel_funcs ili9488_drm_funcs = {
	.disable   = ili9488_disable,
	.unprepare = ili9488_unprepare,
	.prepare   = ili9488_prepare,
	.enable    = ili9488_enable,
	.get_modes = ili9488_get_modes,
};

static int ili9488_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct ili9488 *ctx;
	int ret;

	ctx = devm_kzalloc(dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->reset_gpio = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "cannot get reset-gpio\n");
		return PTR_ERR(ctx->reset_gpio);
	}

	/*
	 * Due to a common reset between panel & touchscreen, the reset pin
	 * must be set to low level first and leave at high level at the
	 * end of probe
	 */
	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		mdelay(1);
		gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	}

	ctx->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(ctx->supply)) {
		ret = PTR_ERR(ctx->supply);
		if (ret != -EPROBE_DEFER)
			dev_err(dev, "failed to request regulator: %d\n", ret);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;

	dsi->lanes = 1;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
			  MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &ili9488_drm_funcs;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "mipi_dsi_attach failed. Is host ready?\n");
		drm_panel_remove(&ctx->panel);
		backlight_device_unregister(ctx->bl_dev);
		return ret;
	}

	return 0;
}

static int ili9488_remove(struct mipi_dsi_device *dsi)
{
	struct ili9488 *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	if (ctx->reset_gpio) {
		gpiod_set_value_cansleep(ctx->reset_gpio, 1);
		mdelay(20);
	}

	regulator_disable(ctx->supply);

	return 0;
}

static const struct of_device_id ilitek_ili9488_of_match[] = {
	{ .compatible = "ilitek,ili9488" },
	{ }
};
MODULE_DEVICE_TABLE(of, ilitek_ili9488_of_match);

static struct mipi_dsi_driver ilitek_ili9488_driver = {
	.probe  = ili9488_probe,
	.remove = ili9488_remove,
	.driver = {
		.name = "panel-ilitek-ili9488",
		.of_match_table = ilitek_ili9488_of_match,
	},
};
module_mipi_dsi_driver(ilitek_ili9488_driver);

MODULE_AUTHOR("Philippe Cornu <philippe.cornu@st.com>");
MODULE_AUTHOR("Yannick Fertre <yannick.fertre@st.com>");
MODULE_DESCRIPTION("DRM driver for ILI9488 MIPI DSI panel");
MODULE_LICENSE("GPL v2");
