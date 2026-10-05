// SPDX-License-Identifier: GPL-2.0
/*
 * DRM panel driver for the AUO/Topwin H497TLB01 4.97" 720x1280 AMOLED
 * (Raydium RM69052 driver IC, MIPI DSI video mode).
 *
 * Init sequence from the H497TLB01 v1.8 datasheet (an I2C capture of a
 * working TC358870 HDMI->DSI bridge board was used until driver 1.16).
 * Power/reset timing from the v0.2 datasheet. Video timings from the EDID
 * the bridge presented (66.9 MHz, 60 Hz); override with module parameters.
 *
 * Brightness is exposed as /sys/class/backlight/panel-h497 and sent to the
 * panel with the standard DCS Set Display Brightness (0x51) command.
 */

#include <linux/backlight.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>
#include <linux/uaccess.h>

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
static bool noncont = true;
module_param(noncont, bool, 0444);
MODULE_PARM_DESC(noncont, "Non-continuous DSI clock: clock lane returns to LP between transfers (default Y; the panel then also locks onto the clock after blank/unblank)");
static bool no_eot;
module_param(no_eot, bool, 0444);
MODULE_PARM_DESC(no_eot, "Don't send EoT packets after HS bursts, for DSI 1.0 receivers (default N)");
static bool late_init;
module_param(late_init, bool, 0444);
MODULE_PARM_DESC(late_init, "Power and reset the panel before the DSI host starts, send init once the link is up (default N)");
static int init_set;
module_param(init_set, int, 0644);
MODULE_PARM_DESC(init_set, "Init sequence: 0 = datasheet v1.8, 1 = datasheet v0.2 verbatim, 2 = none (OTP defaults) (default 0)");
static int init_brightness = -1;
module_param(init_brightness, int, 0644);
MODULE_PARM_DESC(init_brightness, "Brightness (0x51) sent before video, 0-255; -1 = current backlight level (default -1)");
static bool early_display_on = true;
module_param(early_display_on, bool, 0644);
MODULE_PARM_DESC(early_display_on, "Send Display On before video starts, as the TC358870 does (default Y)");
static bool early_power = true;
module_param(early_power, bool, 0444);
MODULE_PARM_DESC(early_power, "Power up and reset the panel at probe, before the DSI host starts its clock (default Y)");
static bool tc_exact;
module_param(tc_exact, bool, 0444);
MODULE_PARM_DESC(tc_exact, "Replay the TC358870 board exactly: all init as DCS long + 0x03 spacers, no 53/51, Sleep Out 300 ms, Display On before video, non-burst sync pulses, continuous clock (default N)");
static int lane_reg = -1;
module_param(lane_reg, int, 0644);
MODULE_PARM_DESC(lane_reg, "Value for page 0 register BA (MIPI lane count); -1 = derive from lanes, (lanes-1)<<5 (default -1)");
static bool debug;
module_param(debug, bool, 0644);
MODULE_PARM_DESC(debug, "Read back panel ID and power mode during power-up (default N)");

#define HACTIVE 720
#define VACTIVE 1280
#define MAX_BRIGHTNESS 255
#define DEFAULT_BRIGHTNESS 128

struct h497 {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	struct regulator *vddi;	/* 1.8 V digital I/O */
	struct regulator *vdd;	/* 3.1 V analog */
	struct gpio_desc *reset;
	enum drm_panel_orientation orientation;
	bool prepared;		/* panel powered and out of sleep */
	bool powered;		/* supplies on and reset released */
	bool display_on_sent;	/* Display On already sent before video */
	struct dentry *debugfs;
	struct mutex dbg_lock;	/* protects dbg_result */
	char dbg_result[128];
};

static inline struct h497 *to_h497(struct drm_panel *panel)
{
	return container_of(panel, struct h497, panel);
}

/*
 * Send a DCS command while video may be running. vc4 has to fit low-power
 * commands into the video stream and gives up after 500 ms with -ETIMEDOUT,
 * which happens to a few percent of commands (about 7 % measured), so retry.
 */
#define H497_VIDEO_CMD_TRIES 3

static ssize_t h497_dcs_write_video(struct h497 *ctx, u8 cmd,
				    const void *data, size_t len)
{
	ssize_t ret;
	int i;

	for (i = 0; i < H497_VIDEO_CMD_TRIES; i++) {
		ret = mipi_dsi_dcs_write(ctx->dsi, cmd, data, len);
		if (ret != -ETIMEDOUT)
			break;
	}

	return ret;
}

#define G 1	/* generic write (0x23 / 0x29) */
#define D 0	/* DCS write (0x15 / 0x39) */

/*
 * Datasheet (H497TLB01 v0.2, p.18-21) recommended sequence, verbatim.
 * Each entry: length, packet type, bytes. Obvious typos in the table are
 * corrected: second "C30B" is C30C, "B001" in step 113 is BA02.
 */
static const u8 h497_init_datasheet[] = {
	/* Page 0 */
	6, G, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00,
	4, G, 0xB0, 0x00, 0x10, 0x10,
	7, G, 0xB7, 0x10, 0x10, 0x10, 0x10, 0x10, 0x10,
	2, G, 0xBA, 0x60,
	8, G, 0xBB, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77,
	/* Page 2 */
	6, G, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x02,
	2, G, 0xCA, 0x04,
	2, G, 0xE2, 0x2A,
	2, G, 0xE3, 0x40,
	6, G, 0xE7, 0x00, 0x00, 0x00, 0x00, 0x00,
	8, G, 0xED, 0x48, 0x00, 0xE0, 0x13, 0x08, 0x00, 0x90,
	7, G, 0xFD, 0x00, 0x08, 0x1C, 0x00, 0x00, 0x01,
	18, D, 0xC3, 0x11, 0x24, 0x04, 0x0A, 0x07, 0x04, 0x00, 0x1C,
	       0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03,
	/* Page 3 */
	6, G, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x03,
	7, G, 0xF1, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B,
	2, G, 0xF6, 0x08,
	/* Page 1 */
	6, G, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x01,
	4, G, 0xB0, 0x06, 0x06, 0x06,
	4, G, 0xB1, 0x14, 0x14, 0x14,
	4, G, 0xB2, 0x06, 0x06, 0x06,
	4, G, 0xB4, 0x14, 0x14, 0x14,
	4, G, 0xB5, 0x44, 0x44, 0x44,
	4, G, 0xB6, 0x44, 0x44, 0x44,
	4, G, 0xB9, 0x24, 0x24, 0x24,
	4, G, 0xBA, 0x14, 0x14, 0x14,
	4, G, 0xBE, 0x23, 0x78, 0x78,
	/* Tearing effect output on */
	2, D, 0x35, 0x00,
};

/*
 * Datasheet v1.8 (2015-07-21, p.17) "Display Initial Setting", verbatim.
 * The datasheet has no 53; it is added so brightness control works.
 * Page 2 EA (all five bytes) stops the panel's per-frame checksum error
 * reports during video, which the earlier capture-based table produced.
 *
 * Each entry: length, then <length> bytes (DCS command + parameters).
 * Sent as DCS writes (0x15 for 1 parameter, 0x39 otherwise), in LP mode.
 */
static const u8 h497_init[] = {
	/* Page 0 */
	6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x00,
	4, 0xB0, 0x00, 0x10, 0x10,
	2, 0xBA, 0x60,		/* lane count, replaced at runtime */
	8, 0xBB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	9, 0xC0, 0xC0, 0x04, 0x00, 0x20, 0x02, 0xE4, 0xE1, 0xC0,
	9, 0xC1, 0xC0, 0x04, 0x00, 0x20, 0x04, 0xE4, 0xE1, 0xC0,
	/* Page 2 */
	6, 0xF0, 0x55, 0xAA, 0x52, 0x08, 0x02,
	6, 0xEA, 0x7F, 0x20, 0x00, 0x00, 0x00,
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
	6, 0xC3, 0x00, 0x10, 0x50, 0x50, 0x50,
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
	4, 0xBE, 0x22, 0x38, 0x78,
	/* Tearing effect output on */
	2, 0x35, 0x00,
	/* Write CTRL Display: enable brightness control (BCTRL) */
	2, 0x53, 0x20,
};

/* No manufacturer setup: only TE on, rely on OTP defaults */
static const u8 h497_init_none[] = {
	2, D, 0x35, 0x00,
};

#undef G
#undef D

/* Typed tables (length, type, bytes) as used for init_set 1 and 2 */
static int h497_send_typed(struct h497 *ctx, const u8 *p, size_t size)
{
	const u8 *end = p + size;
	ssize_t ret;

	while (p < end) {
		u8 len = p[0], generic = p[1];

		p += 2;
		if (generic)
			ret = mipi_dsi_generic_write(ctx->dsi, p, len);
		else
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

/*
 * Send one packet of exactly the given data type in LP mode. The core
 * helpers choose the type from the length (2 bytes -> 0x15), but the
 * TC358870 board sends every init command as a DCS long write (0x39).
 */
static ssize_t h497_write_typed(struct h497 *ctx, u8 type, const u8 *buf,
				size_t len)
{
	const struct mipi_dsi_host_ops *ops = ctx->dsi->host->ops;
	struct mipi_dsi_msg msg = {
		.channel = ctx->dsi->channel,
		.type = type,
		.flags = MIPI_DSI_MSG_USE_LPM,
		.tx_buf = buf,
		.tx_len = len,
	};

	if (!ops || !ops->transfer)
		return -ENOSYS;

	return ops->transfer(ctx->dsi->host, &msg);
}

/* TC358870 form: DCS long write, then an empty generic short write (0x03) */
static ssize_t h497_write_tc(struct h497 *ctx, const u8 *buf, size_t len)
{
	ssize_t ret;

	ret = h497_write_typed(ctx, MIPI_DSI_DCS_LONG_WRITE, buf, len);
	if (ret < 0)
		return ret;

	return h497_write_typed(ctx, MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM,
				NULL, 0);
}

static int h497_send_init(struct h497 *ctx)
{
	const u8 *p = h497_init;
	const u8 *end = h497_init + sizeof(h497_init);
	ssize_t ret;

	if (debug)
		dev_info(&ctx->dsi->dev, "init_set %d\n", init_set);
	if (init_set == 1)
		return h497_send_typed(ctx, h497_init_datasheet,
				       sizeof(h497_init_datasheet));
	if (init_set == 2)
		return h497_send_typed(ctx, h497_init_none,
				       sizeof(h497_init_none));

	while (p < end) {
		u8 len = *p++;
		u8 lanes_cmd[2];
		const u8 *cmd = p;

		/*
		 * Page 0 BA sets the MIPI lane count in bits 6:5 as lanes - 1.
		 * The table has BA 60 (4 lanes); follow the configured lanes.
		 */
		if (len == 2 && p[0] == 0xBA) {
			lanes_cmd[0] = 0xBA;
			lanes_cmd[1] = lane_reg >= 0 ? lane_reg :
				       (ctx->dsi->lanes - 1) << 5;
			cmd = lanes_cmd;
			if (debug)
				dev_info(&ctx->dsi->dev, "BA = 0x%02x\n", lanes_cmd[1]);
		}

		if (tc_exact) {
			/* The TC358870 board sent no Write CTRL Display (53) */
			if (p[0] == MIPI_DCS_WRITE_CONTROL_DISPLAY) {
				p += len;
				continue;
			}
			ret = h497_write_tc(ctx, cmd, len);
		} else {
			ret = mipi_dsi_dcs_write_buffer(ctx->dsi, cmd, len);
		}
		if (ret < 0) {
			dev_err(&ctx->dsi->dev, "init cmd 0x%02x failed: %zd\n",
				p[0], ret);
			return ret;
		}
		p += len;
	}

	return 0;
}

/*
 * Read one DCS register. The vc4 DSI host returns 0 (not the byte count) on
 * a successful read, so only negative values are errors.
 */
static int h497_read_reg(struct h497 *ctx, u8 cmd, u8 *buf, size_t len)
{
	ssize_t ret;

	ret = mipi_dsi_set_maximum_return_packet_size(ctx->dsi, len);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_read(ctx->dsi, cmd, buf, len);
	return ret < 0 ? ret : 0;
}

/* Acknowledge and Error Report flags (MIPI DSI spec) */
static void h497_decode_errors(struct h497 *ctx, const char *stage, u16 e)
{
	static const char * const names[16] = {
		"SoT", "SoT sync", "EoT sync", "escape entry", "LP tx sync",
		"timeout", "false control", "contention", "ECC 1-bit",
		"ECC multi-bit", "checksum", "data type", "VC ID",
		"length", "reserved", "protocol",
	};
	char buf[160];
	int i, pos = 0;

	for (i = 0; i < 16; i++)
		if (e & BIT(i))
			pos += scnprintf(buf + pos, sizeof(buf) - pos, " %s",
					 names[i]);
	dev_info(&ctx->dsi->dev, "[%s] possible error report 0x%04x:%s\n",
		 stage, e, pos ? buf : " none");
}

/*
 * Debug: read the panel's ID, power mode and DSI error count (needs a bus
 * turnaround on lane 0). A reply proves the link, power and reset work; the
 * power mode bits show which commands took effect.
 */
static void h497_debug_read(struct h497 *ctx, const char *stage)
{
	struct device *dev = &ctx->dsi->dev;
	u8 id[3] = { 0 }, mode = 0, errs = 0;
	int r1, r2, r3, ret;

	if (!debug)
		return;

	r1 = h497_read_reg(ctx, 0xDA, &id[0], 1);
	r2 = h497_read_reg(ctx, 0xDB, &id[1], 1);
	r3 = h497_read_reg(ctx, 0xDC, &id[2], 1);
	dev_info(dev, "[%s] ID DA/DB/DC = %02x %02x %02x (ret %d %d %d)\n",
		 stage, id[0], id[1], id[2], r1, r2, r3);

	ret = h497_read_reg(ctx, MIPI_DCS_GET_POWER_MODE, &mode, 1);
	if (ret < 0)
		dev_info(dev, "[%s] power mode read failed: %d\n", stage, ret);
	else
		/* Bit 7 (booster) has no kernel define */
		dev_info(dev, "[%s] power mode 0x%02x: booster %s, sleep %s, normal mode %s, display %s\n",
			 stage, mode,
			 mode & BIT(7) ? "on" : "off",
			 mode & MIPI_DSI_DCS_POWER_MODE_SLEEP ? "out" : "in",
			 mode & MIPI_DSI_DCS_POWER_MODE_NORMAL ? "on" : "off",
			 mode & MIPI_DSI_DCS_POWER_MODE_DISPLAY ? "on" : "off");

	/* If the panel answers with an error report, bytes are its flags */
	if (h497_read_reg(ctx, 0x45, id, 2) == 0 && (id[0] || id[1]))
		h497_decode_errors(ctx, stage, id[0] | id[1] << 8);

	/* Get Error Count on DSI: bit 7 = overflow, bits 6:0 = count */
	ret = h497_read_reg(ctx, MIPI_DCS_GET_ERROR_COUNT_ON_DSI, &errs, 1);
	if (ret < 0)
		dev_info(dev, "[%s] DSI error count read failed: %d\n", stage, ret);
	else
		dev_info(dev, "[%s] DSI errors seen by panel: %u%s\n", stage,
			 errs & 0x7f, errs & 0x80 ? " (overflowed)" : "");
}

/*
 * Debugfs interface for bring-up: /sys/kernel/debug/panel-h497/dcs
 *   echo "r 0a"       > dcs   read 1 byte from register 0x0A
 *   echo "r da 3"     > dcs   read 3 bytes
 *   echo "w 51 80"    > dcs   write command 0x51 with parameter 0x80
 *   echo "h 51 80"    > dcs   same, but sent in high-speed mode (tests the
 *                             clock lane and lane 0 HS receivers)
 *   cat dcs                   show the result of the last command
 * Only works while the panel is powered. Not serialized against the DRM
 * core's own commands; for debugging only.
 */
static ssize_t h497_dcs_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	struct h497 *ctx = file->private_data;
	char kbuf[256], *p = kbuf, *tok;
	u8 bytes[64];
	unsigned int n = 0, val;
	char op;
	int ret;

	if (count >= sizeof(kbuf))
		return -EINVAL;
	if (copy_from_user(kbuf, ubuf, count))
		return -EFAULT;
	kbuf[count] = '\0';

	tok = strsep(&p, " \t\n");
	if (!tok || strlen(tok) != 1 || !strchr("rwh", tok[0]))
		return -EINVAL;
	op = tok[0];

	while ((tok = strsep(&p, " \t\n")) && n < ARRAY_SIZE(bytes)) {
		if (!*tok)
			continue;
		if (kstrtouint(tok, 16, &val) || val > 0xff)
			return -EINVAL;
		bytes[n++] = val;
	}
	if (n == 0)
		return -EINVAL;

	mutex_lock(&ctx->dbg_lock);

	if (!ctx->prepared) {
		snprintf(ctx->dbg_result, sizeof(ctx->dbg_result),
			 "error: panel not powered\n");
		ret = -ENODEV;
		goto out;
	}

	if (op == 'w' || op == 'h') {
		unsigned long flags = ctx->dsi->mode_flags;

		/* The DSI core sends in LP mode only if MIPI_DSI_MODE_LPM is set */
		if (op == 'h')
			ctx->dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;
		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, bytes, n);
		ctx->dsi->mode_flags = flags;

		if (ret < 0)
			snprintf(ctx->dbg_result, sizeof(ctx->dbg_result),
				 "%c %02x: error %d\n", op, bytes[0], ret);
		else
			snprintf(ctx->dbg_result, sizeof(ctx->dbg_result),
				 "%c %02x: ok\n", op, bytes[0]);
	} else {
		u8 rx[32] = { 0 };
		size_t len = n > 1 ? bytes[1] : 1;
		int i, pos;

		if (len < 1 || len > sizeof(rx)) {
			ret = -EINVAL;
			goto out;
		}
		ret = h497_read_reg(ctx, bytes[0], rx, len);
		if (ret < 0) {
			snprintf(ctx->dbg_result, sizeof(ctx->dbg_result),
				 "r %02x: error %d\n", bytes[0], ret);
		} else {
			pos = snprintf(ctx->dbg_result, sizeof(ctx->dbg_result),
				       "r %02x:", bytes[0]);
			for (i = 0; i < len; i++)
				pos += snprintf(ctx->dbg_result + pos,
						sizeof(ctx->dbg_result) - pos,
						" %02x", rx[i]);
			snprintf(ctx->dbg_result + pos,
				 sizeof(ctx->dbg_result) - pos, "\n");
		}
	}

out:
	mutex_unlock(&ctx->dbg_lock);
	return ret < 0 ? ret : count;
}

static ssize_t h497_dcs_read(struct file *file, char __user *ubuf,
			     size_t count, loff_t *ppos)
{
	struct h497 *ctx = file->private_data;
	ssize_t ret;

	mutex_lock(&ctx->dbg_lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, ctx->dbg_result,
				      strlen(ctx->dbg_result));
	mutex_unlock(&ctx->dbg_lock);

	return ret;
}

static const struct file_operations h497_dcs_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = h497_dcs_read,
	.write = h497_dcs_write,
	.llseek = default_llseek,
};

/* Send the init sequence and take the panel out of sleep. Needs the DSI host up. */
static int h497_init_and_wake(struct h497 *ctx)
{
	struct device *dev = &ctx->dsi->dev;
	int ret;

	h497_debug_read(ctx, "after reset");

	ret = h497_send_init(ctx);
	if (ret)
		return ret;

	/*
	 * TC358870 order: Sleep Out and Display On as 0x15 with a 0x00
	 * parameter, 300 ms and 40 ms waits, no brightness, all before video.
	 */
	if (tc_exact) {
		static const u8 sleep_out[] = { MIPI_DCS_EXIT_SLEEP_MODE, 0x00 };
		static const u8 display_on[] = { MIPI_DCS_SET_DISPLAY_ON, 0x00 };

		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, sleep_out,
						sizeof(sleep_out));
		if (ret < 0) {
			dev_err(dev, "sleep out failed: %d\n", ret);
			return ret;
		}
		msleep(300);
		h497_debug_read(ctx, "after sleep out");

		ret = mipi_dsi_dcs_write_buffer(ctx->dsi, display_on,
						sizeof(display_on));
		if (ret < 0) {
			dev_err(dev, "display on failed: %d\n", ret);
			return ret;
		}
		msleep(40);
		ctx->display_on_sent = true;
		h497_debug_read(ctx, "after display on (before video)");

		ctx->prepared = true;
		return 0;
	}

	ret = mipi_dsi_dcs_exit_sleep_mode(ctx->dsi);
	if (ret < 0) {
		dev_err(dev, "sleep out failed: %d\n", ret);
		return ret;
	}
	msleep(120);

	h497_debug_read(ctx, "after sleep out");

	/*
	 * The TC358870 board sends Display On in LP mode before starting
	 * video (Sleep Out, 300 ms, Display On, 40 ms, video). Once video
	 * runs, the link may have no LP window left for commands.
	 */
	/*
	 * Set brightness while the link is still in LP. Commands sent after
	 * video starts may never arrive, leaving the panel at its power-on
	 * default (maximum). Use the backlight's stored level, not
	 * backlight_get_brightness(), which returns 0 while it is blanked.
	 */
	{
		int level = init_brightness >= 0 ? init_brightness :
			    ctx->panel.backlight->props.brightness;
		u8 val = clamp(level, 0, MAX_BRIGHTNESS);

		ret = mipi_dsi_dcs_write(ctx->dsi, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
					 &val, 1);
		if (ret < 0)
			dev_warn(dev, "initial brightness failed: %d\n", ret);
	}

	ctx->display_on_sent = false;
	if (early_display_on) {
		msleep(180);
		ret = mipi_dsi_dcs_set_display_on(ctx->dsi);
		if (ret < 0) {
			dev_err(dev, "display on failed: %d\n", ret);
			return ret;
		}
		msleep(40);
		ctx->display_on_sent = true;
		h497_debug_read(ctx, "after display on (before video)");
	}

	ctx->prepared = true;

	return 0;
}

/* Supplies on, reset pulse, wait for OTP load. Leaves the panel in Sleep In. */
static int h497_power_on(struct h497 *ctx)
{
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
		regulator_disable(ctx->vddi);
		return ret;
	}

	msleep(40);

	/* Reset pulse must be >= 15 ms to be accepted */
	gpiod_set_value_cansleep(ctx->reset, 1);
	msleep(15);
	gpiod_set_value_cansleep(ctx->reset, 0);
	/* OTP load (tREST) can take up to 120 ms before Sleep Out is allowed */
	msleep(120);

	ctx->powered = true;

	return 0;
}

static void h497_power_off(struct h497 *ctx)
{
	gpiod_set_value_cansleep(ctx->reset, 1);
	regulator_disable(ctx->vdd);
	regulator_disable(ctx->vddi);
	ctx->powered = false;
}

static int h497_prepare(struct drm_panel *panel)
{
	struct h497 *ctx = to_h497(panel);
	int ret;

	/* With early_power the panel was already powered at probe */
	if (!ctx->powered) {
		ret = h497_power_on(ctx);
		if (ret)
			return ret;
	}

	if (!late_init) {
		ret = h497_init_and_wake(ctx);
		if (ret) {
			h497_power_off(ctx);
			return ret;
		}
	}

	return 0;
}

static int h497_enable(struct drm_panel *panel)
{
	struct h497 *ctx = to_h497(panel);
	int ret;

	if (late_init) {
		ret = h497_init_and_wake(ctx);
		if (ret)
			return ret;
	}

	if (!ctx->display_on_sent) {
		ret = h497_dcs_write_video(ctx, MIPI_DCS_SET_DISPLAY_ON, NULL, 0);
		if (ret < 0) {
			dev_err(&ctx->dsi->dev, "display on failed: %d\n", ret);
			return ret;
		}
		msleep(20);
	}

	h497_debug_read(ctx, "after display on");

	return 0;
}

/*
 * All shutdown commands are sent here: with prepare_prev_first, the DSI
 * host is powered down before unprepare() runs, so the link is gone by then.
 */
static int h497_disable(struct drm_panel *panel)
{
	struct h497 *ctx = to_h497(panel);
	int ret;

	ctx->prepared = false;

	ret = h497_dcs_write_video(ctx, MIPI_DCS_SET_DISPLAY_OFF, NULL, 0);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "display off failed: %d\n", ret);

	ret = h497_dcs_write_video(ctx, MIPI_DCS_ENTER_SLEEP_MODE, NULL, 0);
	if (ret < 0)
		dev_warn(&ctx->dsi->dev, "sleep in failed: %d\n", ret);
	/* Panel needs 120 ms in Sleep In before power can be removed */
	msleep(120);

	return 0;
}

static int h497_unprepare(struct drm_panel *panel)
{
	h497_power_off(to_h497(panel));

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

	/*
	 * Blanking: Display Off follows right away, and a command sent while
	 * video still runs often times out on vc4. The stored level is sent
	 * again before video on the next power-up.
	 */
	if (backlight_is_blank(bl))
		return 0;

	/* The TC358870 board never sends brightness */
	if (tc_exact)
		return 0;

	/* One-byte form; the kernel helper sends a 16-bit value */
	ret = h497_dcs_write_video(ctx, MIPI_DCS_SET_DISPLAY_BRIGHTNESS,
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
	if (tc_exact)
		/* Non-burst sync pulses, continuous clock; ignores burst/noncont */
		dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	else if (burst)
		dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST;
	else
		dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	if (noncont && !tc_exact)
		dsi->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;
	if (no_eot)
		dsi->mode_flags |= MIPI_DSI_MODE_NO_EOT_PACKET;

	drm_panel_init(&ctx->panel, dev, &h497_funcs, DRM_MODE_CONNECTOR_DSI);
	/*
	 * Default: DSI host powered up before prepare(), which sends the init
	 * sequence. With late_init the panel is powered and reset before the
	 * host starts (so its clock lane receiver sees the HS entry sequence),
	 * and the init sequence is sent from enable() instead.
	 */
	ctx->panel.prepare_prev_first = !late_init;

	/*
	 * Attached to the drm_panel, so the DRM core turns it on after
	 * enable() and off before disable().
	 */
	{
		const struct backlight_properties props = {
			.type = BACKLIGHT_RAW,
			/* Until systemd restores the saved level */
			.brightness = DEFAULT_BRIGHTNESS,
			.max_brightness = MAX_BRIGHTNESS,
		};

		ctx->panel.backlight = devm_backlight_device_register(dev,
					"panel-h497", dev, ctx, &h497_bl_ops, &props);
		if (IS_ERR(ctx->panel.backlight))
			return dev_err_probe(dev, PTR_ERR(ctx->panel.backlight),
					     "failed to register backlight\n");
	}

	/*
	 * Bring the panel out of reset before the DSI host starts, so its
	 * clock lane receiver sees the host's LP-to-HS entry sequence. Only
	 * affects the first power-up; later ones follow prepare().
	 */
	if (early_power) {
		ret = h497_power_on(ctx);
		if (ret)
			dev_warn(dev, "early power-on failed: %d\n", ret);
	}

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0) {
		dev_err(dev, "failed to attach to DSI host: %d\n", ret);
		drm_panel_remove(&ctx->panel);
		if (ctx->powered)
			h497_power_off(ctx);
		return ret;
	}

	mutex_init(&ctx->dbg_lock);
	ctx->debugfs = debugfs_create_dir("panel-h497", NULL);
	debugfs_create_file("dcs", 0600, ctx->debugfs, ctx, &h497_dcs_fops);

	return 0;
}

static void h497_remove(struct mipi_dsi_device *dsi)
{
	struct h497 *ctx = mipi_dsi_get_drvdata(dsi);

	debugfs_remove_recursive(ctx->debugfs);
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
