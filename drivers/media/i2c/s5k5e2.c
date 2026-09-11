// SPDX-License-Identifier: GPL-2.0
/*
 * Samsung S5K5E2 1/5" 5Mpx CMOS image sensor driver
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <linux/units.h>
#include <media/v4l2-cci.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define S5K5E2_LINK_FREQ_448MHZ		(448 * HZ_PER_MHZ)
#define S5K5E2_MCLK_FREQ_24MHZ		(24 * HZ_PER_MHZ)
#define S5K5E2_DATA_LANES		2
#define S5K5E2_BPP			10

/* Register map follows MIPI CCS */
#define S5K5E2_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5K5E2_CHIP_ID			0x5e20

/* Streaming and orientation share one 16-bit access */
#define S5K5E2_REG_CTRL_MODE		CCI_REG16(0x0100)
#define S5K5E2_MODE_STREAMING		BIT(8)
#define S5K5E2_VFLIP			BIT(1)
#define S5K5E2_HFLIP			BIT(0)

#define S5K5E2_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5K5E2_EXPOSURE_MIN		4
#define S5K5E2_EXPOSURE_STEP		1
#define S5K5E2_EXPOSURE_MARGIN		4

#define S5K5E2_REG_AGAIN		CCI_REG16(0x0204)
#define S5K5E2_AGAIN_MIN		32	/* 32 = x1 */
#define S5K5E2_AGAIN_MAX		512	/* 512 = x16 */
#define S5K5E2_AGAIN_STEP		1
#define S5K5E2_AGAIN_DEFAULT		32

#define S5K5E2_REG_VTS			CCI_REG16(0x0340)
#define S5K5E2_VTS_MAX			0xffff

#define S5K5E2_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define S5K5E2_NATIVE_WIDTH		2576
#define S5K5E2_NATIVE_HEIGHT		1936
#define S5K5E2_ACTIVE_LEFT		8
#define S5K5E2_ACTIVE_TOP		8
#define S5K5E2_ACTIVE_WIDTH		2560
#define S5K5E2_ACTIVE_HEIGHT		1920

#define to_s5k5e2(_sd)			container_of(_sd, struct s5k5e2, sd)

static const s64 s5k5e2_link_freq_menu[] = {
	S5K5E2_LINK_FREQ_448MHZ,
};

/* List of supported formats to cover horizontal and vertical flip controls */
static const u32 s5k5e2_mbus_formats[] = {
	MEDIA_BUS_FMT_SGRBG10_1X10,	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR10_1X10,	MEDIA_BUS_FMT_SGBRG10_1X10,
};

struct s5k5e2_reg_list {
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

struct s5k5e2_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts;
	u32 exposure;

	const struct s5k5e2_reg_list reg_list;
};

static const char * const s5k5e2_test_pattern_menu[] = {
	"Disabled",
	"Solid colour",
	"Colour bars",
	"Fade to grey colour bars",
	"PN9",
};

static const char * const s5k5e2_supply_names[] = {
	"vdda",		/* Analog power */
	"vddd",		/* Digital core power */
	"vddio",	/* Digital I/O power */
};

#define S5K5E2_NUM_SUPPLIES	ARRAY_SIZE(s5k5e2_supply_names)

struct s5k5e2 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct gpio_desc *reset_gpio;
	struct regulator_bulk_data supplies[S5K5E2_NUM_SUPPLIES];

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vflip;
	struct v4l2_ctrl *hflip;

	const struct s5k5e2_mode *mode;
};

static const struct cci_reg_sequence s5k5e2_init_regs[] = {
	{ CCI_REG8(0x3000), 0x04 },
	{ CCI_REG8(0x3002), 0x03 },
	{ CCI_REG8(0x3003), 0x04 },
	{ CCI_REG8(0x3004), 0x02 },
	{ CCI_REG8(0x3005), 0x00 },
	{ CCI_REG8(0x3006), 0x10 },
	{ CCI_REG8(0x3007), 0x03 },
	{ CCI_REG8(0x3008), 0x55 },
	{ CCI_REG8(0x3039), 0x00 },
	{ CCI_REG8(0x303a), 0x00 },
	{ CCI_REG8(0x303b), 0x00 },
	{ CCI_REG8(0x3009), 0x05 },
	{ CCI_REG8(0x300a), 0x55 },
	{ CCI_REG8(0x300b), 0x38 },
	{ CCI_REG8(0x300c), 0x10 },
	{ CCI_REG8(0x3012), 0x05 },
	{ CCI_REG8(0x3013), 0x00 },
	{ CCI_REG8(0x3014), 0x22 },
	{ CCI_REG8(0x300e), 0x79 },
	{ CCI_REG8(0x3010), 0x68 },
	{ CCI_REG8(0x3019), 0x03 },
	{ CCI_REG8(0x301a), 0x00 },
	{ CCI_REG8(0x301b), 0x06 },
	{ CCI_REG8(0x301c), 0x00 },
	{ CCI_REG8(0x301d), 0x22 },
	{ CCI_REG8(0x301e), 0x00 },
	{ CCI_REG8(0x301f), 0x10 },
	{ CCI_REG8(0x3020), 0x00 },
	{ CCI_REG8(0x3021), 0x00 },
	{ CCI_REG8(0x3022), 0x0a },
	{ CCI_REG8(0x3023), 0x1e },
	{ CCI_REG8(0x3024), 0x00 },
	{ CCI_REG8(0x3025), 0x00 },
	{ CCI_REG8(0x3026), 0x00 },
	{ CCI_REG8(0x3027), 0x00 },
	{ CCI_REG8(0x3028), 0x1a },
	{ CCI_REG8(0x3015), 0x00 },
	{ CCI_REG8(0x3016), 0x84 },
	{ CCI_REG8(0x3017), 0x00 },
	{ CCI_REG8(0x3018), 0xa0 },
	{ CCI_REG8(0x302b), 0x10 },
	{ CCI_REG8(0x302c), 0x0a },
	{ CCI_REG8(0x302d), 0x06 },
	{ CCI_REG8(0x302e), 0x05 },
	{ CCI_REG8(0x302f), 0x0e },
	{ CCI_REG8(0x3030), 0x2f },
	{ CCI_REG8(0x3031), 0x08 },
	{ CCI_REG8(0x3032), 0x05 },
	{ CCI_REG8(0x3033), 0x09 },
	{ CCI_REG8(0x3034), 0x05 },
	{ CCI_REG8(0x3035), 0x00 },
	{ CCI_REG8(0x3036), 0x00 },
	{ CCI_REG8(0x3037), 0x00 },
	{ CCI_REG8(0x3038), 0x00 },
	{ CCI_REG8(0x3088), 0x06 },
	{ CCI_REG8(0x308a), 0x08 },
	{ CCI_REG8(0x308c), 0x05 },
	{ CCI_REG8(0x308e), 0x07 },
	{ CCI_REG8(0x3090), 0x06 },
	{ CCI_REG8(0x3092), 0x08 },
	{ CCI_REG8(0x3094), 0x05 },
	{ CCI_REG8(0x3096), 0x21 },
	{ CCI_REG8(0x3099), 0x0e },
	{ CCI_REG8(0x3070), 0x10 },
	{ CCI_REG8(0x3085), 0x11 },
	{ CCI_REG8(0x3086), 0x01 },
	{ CCI_REG8(0x3064), 0x00 },
	{ CCI_REG8(0x3062), 0x08 },
	{ CCI_REG8(0x3061), 0x11 },
	{ CCI_REG8(0x307b), 0x20 },
	{ CCI_REG8(0x3068), 0x00 },
	{ CCI_REG8(0x3074), 0x00 },
	{ CCI_REG8(0x307d), 0x00 },
	{ CCI_REG8(0x3045), 0x01 },
	{ CCI_REG8(0x3046), 0x05 },
	{ CCI_REG8(0x3047), 0x78 },
	{ CCI_REG8(0x307f), 0xb1 },
	{ CCI_REG8(0x3098), 0x01 },
	{ CCI_REG8(0x305c), 0xf6 },
	{ CCI_REG8(0x306b), 0x10 },
	{ CCI_REG8(0x3063), 0x27 },
	{ CCI_REG8(0x3400), 0x01 },
	{ CCI_REG8(0x3235), 0x49 },
	{ CCI_REG8(0x3233), 0x00 },
	{ CCI_REG8(0x3234), 0x00 },
	{ CCI_REG8(0x3300), 0x0d },
	{ CCI_REG8(0x3203), 0x45 },
	{ CCI_REG8(0x3205), 0x4d },
	{ CCI_REG8(0x320b), 0x40 },
	{ CCI_REG8(0x320c), 0x06 },
	{ CCI_REG8(0x320d), 0xc0 },
	{ CCI_REG8(0x0305), 0x06 },
	{ CCI_REG8(0x0306), 0x00 },
	{ CCI_REG8(0x0307), 0xe0 },
	{ CCI_REG8(0x3c1f), 0x00 },
	{ CCI_REG8(0x0820), 0x03 },
	{ CCI_REG8(0x0821), 0x80 },
	{ CCI_REG8(0x3c1c), 0x58 },
	{ CCI_REG8(0x0114), 0x01 },
	{ CCI_REG8(0x0204), 0x00 },
	{ CCI_REG8(0x0205), 0x80 },
	{ CCI_REG8(0x0202), 0x02 },
	{ CCI_REG8(0x0203), 0x00 },
	{ CCI_REG8(0x0200), 0x04 },
	{ CCI_REG8(0x0201), 0x98 },
	{ CCI_REG8(0x340b), 0x00 },
	{ CCI_REG8(0x340c), 0x00 },
	{ CCI_REG8(0x340d), 0x00 },
	{ CCI_REG8(0x340e), 0x00 },
	{ CCI_REG8(0x3401), 0x50 },
	{ CCI_REG8(0x3402), 0x3c },
	{ CCI_REG8(0x3403), 0x03 },
	{ CCI_REG8(0x3404), 0x33 },
	{ CCI_REG8(0x3405), 0x04 },
	{ CCI_REG8(0x3406), 0x44 },
	{ CCI_REG8(0x3458), 0x03 },
	{ CCI_REG8(0x3459), 0x33 },
	{ CCI_REG8(0x345a), 0x04 },
	{ CCI_REG8(0x345b), 0x44 },
	{ CCI_REG8(0x3400), 0x01 },
};

static const struct cci_reg_sequence s5k5e2_2560x1920_regs[] = {
	{ CCI_REG8(0x0340), 0x07 },
	{ CCI_REG8(0x0341), 0xe9 },
	{ CCI_REG8(0x0342), 0x0b },
	{ CCI_REG8(0x0343), 0x86 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x07 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x87 },
	{ CCI_REG8(0x034c), 0x0a },
	{ CCI_REG8(0x034d), 0x00 },
	{ CCI_REG8(0x034e), 0x07 },
	{ CCI_REG8(0x034f), 0x80 },
	{ CCI_REG8(0x0900), 0x00 },
	{ CCI_REG8(0x0901), 0x20 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x01 },
};

static const struct cci_reg_sequence s5k5e2_1280x960_regs[] = {
	{ CCI_REG8(0x0340), 0x07 },
	{ CCI_REG8(0x0341), 0xe9 },
	{ CCI_REG8(0x0342), 0x0b },
	{ CCI_REG8(0x0343), 0x86 },
	{ CCI_REG8(0x0344), 0x00 },
	{ CCI_REG8(0x0345), 0x08 },
	{ CCI_REG8(0x0346), 0x00 },
	{ CCI_REG8(0x0347), 0x08 },
	{ CCI_REG8(0x0348), 0x0a },
	{ CCI_REG8(0x0349), 0x07 },
	{ CCI_REG8(0x034a), 0x07 },
	{ CCI_REG8(0x034b), 0x87 },
	{ CCI_REG8(0x034c), 0x05 },
	{ CCI_REG8(0x034d), 0x00 },
	{ CCI_REG8(0x034e), 0x03 },
	{ CCI_REG8(0x034f), 0xc0 },
	{ CCI_REG8(0x0900), 0x01 },
	{ CCI_REG8(0x0901), 0x22 },
	{ CCI_REG8(0x0381), 0x01 },
	{ CCI_REG8(0x0383), 0x01 },
	{ CCI_REG8(0x0385), 0x01 },
	{ CCI_REG8(0x0387), 0x03 },
};

static const struct s5k5e2_mode s5k5e2_supported_modes[] = {
	{
		.width = 2560,
		.height = 1920,
		.hts = 2950,
		.vts = 2025,
		.exposure = 512,
		.reg_list = {
			.regs = s5k5e2_2560x1920_regs,
			.num_regs = ARRAY_SIZE(s5k5e2_2560x1920_regs),
		},
	},
	{
		.width = 1280,
		.height = 960,
		.hts = 2950,
		.vts = 2025,
		.exposure = 512,
		.reg_list = {
			.regs = s5k5e2_1280x960_regs,
			.num_regs = ARRAY_SIZE(s5k5e2_1280x960_regs),
		},
	},
};

static int s5k5e2_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k5e2 *s5k5e2 = container_of(ctrl->handler, struct s5k5e2,
					     ctrl_handler);
	const struct s5k5e2_mode *mode = s5k5e2->mode;
	s64 exposure_max;
	int ret;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		/* The orientation settings are applied along with streaming */
		return 0;
	case V4L2_CID_VBLANK:
		exposure_max = mode->height + ctrl->val - S5K5E2_EXPOSURE_MARGIN;
		__v4l2_ctrl_modify_range(s5k5e2->exposure,
					 s5k5e2->exposure->minimum,
					 exposure_max,
					 s5k5e2->exposure->step,
					 s5k5e2->exposure->default_value);
		break;
	}

	if (!pm_runtime_get_if_active(s5k5e2->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(s5k5e2->regmap, S5K5E2_REG_AGAIN,
				ctrl->val, NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(s5k5e2->regmap, S5K5E2_REG_EXPOSURE,
				ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(s5k5e2->regmap, S5K5E2_REG_VTS,
				ctrl->val + mode->height, NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(s5k5e2->regmap, S5K5E2_REG_TEST_PATTERN,
				ctrl->val, NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(s5k5e2->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k5e2_ctrl_ops = {
	.s_ctrl = s5k5e2_set_ctrl,
};

static inline u64 s5k5e2_freq_to_pixel_rate(const u64 freq)
{
	return div_u64(freq * 2 * S5K5E2_DATA_LANES, S5K5E2_BPP);
}

static int s5k5e2_init_controls(struct s5k5e2 *s5k5e2)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &s5k5e2->ctrl_handler;
	const struct s5k5e2_mode *mode = s5k5e2->mode;
	s64 pixel_rate, hblank, vblank, exposure_max;
	struct v4l2_fwnode_device_properties props;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 9);

	s5k5e2->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &s5k5e2_ctrl_ops,
					V4L2_CID_LINK_FREQ,
					ARRAY_SIZE(s5k5e2_link_freq_menu) - 1,
					0, s5k5e2_link_freq_menu);
	if (s5k5e2->link_freq)
		s5k5e2->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	pixel_rate = s5k5e2_freq_to_pixel_rate(s5k5e2_link_freq_menu[0]);
	s5k5e2->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e2_ctrl_ops,
					       V4L2_CID_PIXEL_RATE,
					       0, pixel_rate, 1, pixel_rate);

	hblank = mode->hts - mode->width;
	s5k5e2->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e2_ctrl_ops,
					   V4L2_CID_HBLANK, hblank,
					   hblank, 1, hblank);
	if (s5k5e2->hblank)
		s5k5e2->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	vblank = mode->vts - mode->height;
	s5k5e2->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e2_ctrl_ops,
					   V4L2_CID_VBLANK, vblank,
					   S5K5E2_VTS_MAX - mode->height, 1,
					   vblank);

	v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e2_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5K5E2_AGAIN_MIN, S5K5E2_AGAIN_MAX,
			  S5K5E2_AGAIN_STEP, S5K5E2_AGAIN_DEFAULT);

	exposure_max = mode->vts - S5K5E2_EXPOSURE_MARGIN;
	s5k5e2->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e2_ctrl_ops,
					     V4L2_CID_EXPOSURE,
					     S5K5E2_EXPOSURE_MIN,
					     exposure_max,
					     S5K5E2_EXPOSURE_STEP,
					     mode->exposure);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &s5k5e2_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k5e2_test_pattern_menu) - 1,
				     0, 0, s5k5e2_test_pattern_menu);

	s5k5e2->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e2_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	if (s5k5e2->hflip)
		s5k5e2->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	s5k5e2->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k5e2_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s5k5e2->vflip)
		s5k5e2->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;

	ret = v4l2_fwnode_device_parse(s5k5e2->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &s5k5e2_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}

	s5k5e2->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

static int s5k5e2_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);
	const struct s5k5e2_reg_list *reg_list = &s5k5e2->mode->reg_list;
	int ret;

	ret = pm_runtime_resume_and_get(s5k5e2->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(s5k5e2->regmap, s5k5e2_init_regs,
			    ARRAY_SIZE(s5k5e2_init_regs), &ret);
	cci_multi_reg_write(s5k5e2->regmap, reg_list->regs,
			    reg_list->num_regs, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(s5k5e2->sd.ctrl_handler);
	if (ret)
		goto error;

	cci_write(s5k5e2->regmap, S5K5E2_REG_CTRL_MODE,
		  S5K5E2_MODE_STREAMING |
		  (s5k5e2->vflip->val ? S5K5E2_VFLIP : 0) |
		  (s5k5e2->hflip->val ? S5K5E2_HFLIP : 0), &ret);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(s5k5e2->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s5k5e2->dev);

	return ret;
}

static int s5k5e2_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);
	int ret;

	ret = cci_write(s5k5e2->regmap, S5K5E2_REG_CTRL_MODE, 0, NULL);
	if (ret)
		dev_err(s5k5e2->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put_autosuspend(s5k5e2->dev);

	return ret;
}

static u32 s5k5e2_get_format_code(struct s5k5e2 *s5k5e2)
{
	unsigned int i;

	i = (s5k5e2->vflip->val ? 2 : 0) | (s5k5e2->hflip->val ? 1 : 0);

	return s5k5e2_mbus_formats[i];
}

static void s5k5e2_update_pad_format(struct s5k5e2 *s5k5e2,
				     const struct s5k5e2_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = s5k5e2_get_format_code(s5k5e2);
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int s5k5e2_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);
	s64 hblank, vblank, exposure_max;
	const struct s5k5e2_mode *mode;

	mode = v4l2_find_nearest_size(s5k5e2_supported_modes,
				      ARRAY_SIZE(s5k5e2_supported_modes),
				      width, height,
				      fmt->format.width, fmt->format.height);

	s5k5e2_update_pad_format(s5k5e2, mode, &fmt->format);

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY || s5k5e2->mode == mode)
		goto set_format;

	hblank = mode->hts - mode->width;
	__v4l2_ctrl_modify_range(s5k5e2->hblank, hblank, hblank, 1, hblank);

	vblank = mode->vts - mode->height;
	__v4l2_ctrl_modify_range(s5k5e2->vblank, vblank,
				 S5K5E2_VTS_MAX - mode->height, 1, vblank);
	__v4l2_ctrl_s_ctrl(s5k5e2->vblank, vblank);

	exposure_max = mode->vts - S5K5E2_EXPOSURE_MARGIN;
	__v4l2_ctrl_modify_range(s5k5e2->exposure, S5K5E2_EXPOSURE_MIN,
				 exposure_max, S5K5E2_EXPOSURE_STEP,
				 mode->exposure);
	__v4l2_ctrl_s_ctrl(s5k5e2->exposure, mode->exposure);

	if (s5k5e2->sd.ctrl_handler->error)
		return s5k5e2->sd.ctrl_handler->error;

	s5k5e2->mode = mode;

set_format:
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	return 0;
}

static int s5k5e2_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *sd_state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);

	if (code->index > 0)
		return -EINVAL;

	code->code = s5k5e2_get_format_code(s5k5e2);

	return 0;
}

static int s5k5e2_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *sd_state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);

	if (fse->index >= ARRAY_SIZE(s5k5e2_supported_modes))
		return -EINVAL;

	if (fse->code != s5k5e2_get_format_code(s5k5e2))
		return -EINVAL;

	fse->min_width = s5k5e2_supported_modes[fse->index].width;
	fse->max_width = fse->min_width;
	fse->min_height = s5k5e2_supported_modes[fse->index].height;
	fse->max_height = fse->min_height;

	return 0;
}

static int s5k5e2_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *sd_state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
		sel->r.left = S5K5E2_ACTIVE_LEFT;
		sel->r.top = S5K5E2_ACTIVE_TOP;
		sel->r.width = S5K5E2_ACTIVE_WIDTH;
		sel->r.height = S5K5E2_ACTIVE_HEIGHT;
		return 0;
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = S5K5E2_NATIVE_WIDTH;
		sel->r.height = S5K5E2_NATIVE_HEIGHT;
		return 0;
	default:
		return -EINVAL;
	}
}

static int s5k5e2_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.pad = 0,
		.format = {
			.width = s5k5e2->mode->width,
			.height = s5k5e2->mode->height,
		},
	};

	s5k5e2_set_pad_format(sd, state, &fmt);

	return 0;
}

static const struct v4l2_subdev_video_ops s5k5e2_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k5e2_pad_ops = {
	.set_fmt = s5k5e2_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = s5k5e2_get_selection,
	.enum_mbus_code = s5k5e2_enum_mbus_code,
	.enum_frame_size = s5k5e2_enum_frame_size,
	.enable_streams = s5k5e2_enable_streams,
	.disable_streams = s5k5e2_disable_streams,
};

static const struct v4l2_subdev_ops s5k5e2_subdev_ops = {
	.video = &s5k5e2_video_ops,
	.pad = &s5k5e2_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k5e2_internal_ops = {
	.init_state = s5k5e2_init_state,
};

static const struct media_entity_operations s5k5e2_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5k5e2_identify_sensor(struct s5k5e2 *s5k5e2)
{
	u64 val;
	int ret;

	ret = cci_read(s5k5e2->regmap, S5K5E2_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(s5k5e2->dev, ret,
				     "failed to read chip id\n");

	if (val != S5K5E2_CHIP_ID)
		return dev_err_probe(s5k5e2->dev, -ENODEV,
				     "chip id mismatch: %x!=%llx\n",
				     S5K5E2_CHIP_ID, val);

	return 0;
}

static int s5k5e2_check_hwcfg(struct s5k5e2 *s5k5e2)
{
	struct fwnode_handle *fwnode = dev_fwnode(s5k5e2->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	if (!fwnode)
		return -ENODEV;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5K5E2_DATA_LANES) {
		dev_err(s5k5e2->dev, "invalid number of data lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(s5k5e2->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5k5e2_link_freq_menu,
				       ARRAY_SIZE(s5k5e2_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int s5k5e2_power_on(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);
	int ret;

	ret = regulator_bulk_enable(S5K5E2_NUM_SUPPLIES, s5k5e2->supplies);
	if (ret)
		return ret;

	ret = clk_prepare_enable(s5k5e2->mclk);
	if (ret)
		goto disable_regulators;

	gpiod_set_value_cansleep(s5k5e2->reset_gpio, 0);
	usleep_range(30 * USEC_PER_MSEC, 35 * USEC_PER_MSEC);

	return 0;

disable_regulators:
	regulator_bulk_disable(S5K5E2_NUM_SUPPLIES, s5k5e2->supplies);

	return ret;
}

static int s5k5e2_power_off(struct device *dev)
{
	struct v4l2_subdev *sd = dev_get_drvdata(dev);
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);

	gpiod_set_value_cansleep(s5k5e2->reset_gpio, 1);

	clk_disable_unprepare(s5k5e2->mclk);

	regulator_bulk_disable(S5K5E2_NUM_SUPPLIES, s5k5e2->supplies);

	return 0;
}

static int s5k5e2_probe(struct i2c_client *client)
{
	struct s5k5e2 *s5k5e2;
	unsigned long freq;
	unsigned int i;
	int ret;

	s5k5e2 = devm_kzalloc(&client->dev, sizeof(*s5k5e2), GFP_KERNEL);
	if (!s5k5e2)
		return -ENOMEM;

	s5k5e2->dev = &client->dev;
	v4l2_i2c_subdev_init(&s5k5e2->sd, client, &s5k5e2_subdev_ops);

	s5k5e2->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5k5e2->regmap))
		return dev_err_probe(s5k5e2->dev, PTR_ERR(s5k5e2->regmap),
				     "failed to init CCI\n");

	s5k5e2->mclk = devm_v4l2_sensor_clk_get(s5k5e2->dev, NULL);
	if (IS_ERR(s5k5e2->mclk))
		return dev_err_probe(s5k5e2->dev, PTR_ERR(s5k5e2->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(s5k5e2->mclk);
	if (freq != S5K5E2_MCLK_FREQ_24MHZ)
		return dev_err_probe(s5k5e2->dev, -EINVAL,
				     "MCLK clock frequency %lu is not supported\n",
				     freq);

	ret = s5k5e2_check_hwcfg(s5k5e2);
	if (ret)
		return dev_err_probe(s5k5e2->dev, ret,
				     "failed to check HW configuration\n");

	s5k5e2->reset_gpio = devm_gpiod_get_optional(s5k5e2->dev, "reset",
						     GPIOD_OUT_HIGH);
	if (IS_ERR(s5k5e2->reset_gpio))
		return dev_err_probe(s5k5e2->dev, PTR_ERR(s5k5e2->reset_gpio),
				     "cannot get reset GPIO\n");

	for (i = 0; i < S5K5E2_NUM_SUPPLIES; i++)
		s5k5e2->supplies[i].supply = s5k5e2_supply_names[i];

	ret = devm_regulator_bulk_get(s5k5e2->dev, S5K5E2_NUM_SUPPLIES,
				      s5k5e2->supplies);
	if (ret)
		return dev_err_probe(s5k5e2->dev, ret,
				     "failed to get supply regulators\n");

	/* The sensor must be powered on to read the CHIP_ID register */
	ret = s5k5e2_power_on(s5k5e2->dev);
	if (ret)
		return ret;

	ret = s5k5e2_identify_sensor(s5k5e2);
	if (ret)
		goto power_off;

	s5k5e2->mode = &s5k5e2_supported_modes[0];
	ret = s5k5e2_init_controls(s5k5e2);
	if (ret) {
		dev_err_probe(s5k5e2->dev, ret, "failed to init controls\n");
		goto power_off;
	}

	s5k5e2->sd.state_lock = s5k5e2->ctrl_handler.lock;
	s5k5e2->sd.internal_ops = &s5k5e2_internal_ops;
	s5k5e2->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5k5e2->sd.entity.ops = &s5k5e2_subdev_entity_ops;
	s5k5e2->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s5k5e2->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s5k5e2->sd.entity, 1, &s5k5e2->pad);
	if (ret) {
		dev_err_probe(s5k5e2->dev, ret,
			      "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&s5k5e2->sd);
	if (ret < 0) {
		dev_err_probe(s5k5e2->dev, ret, "failed to init subdev\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(s5k5e2->dev);
	pm_runtime_enable(s5k5e2->dev);

	ret = v4l2_async_register_subdev_sensor(&s5k5e2->sd);
	if (ret < 0) {
		dev_err_probe(s5k5e2->dev, ret,
			      "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(s5k5e2->dev, 1000);
	pm_runtime_use_autosuspend(s5k5e2->dev);
	pm_runtime_idle(s5k5e2->dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&s5k5e2->sd);
	pm_runtime_disable(s5k5e2->dev);
	pm_runtime_set_suspended(s5k5e2->dev);

media_entity_cleanup:
	media_entity_cleanup(&s5k5e2->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(s5k5e2->sd.ctrl_handler);

power_off:
	s5k5e2_power_off(s5k5e2->dev);

	return ret;
}

static void s5k5e2_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k5e2 *s5k5e2 = to_s5k5e2(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(s5k5e2->dev);

	if (!pm_runtime_status_suspended(s5k5e2->dev)) {
		s5k5e2_power_off(s5k5e2->dev);
		pm_runtime_set_suspended(s5k5e2->dev);
	}
}

static const struct dev_pm_ops s5k5e2_pm_ops = {
	SET_RUNTIME_PM_OPS(s5k5e2_power_off, s5k5e2_power_on, NULL)
};

static const struct of_device_id s5k5e2_of_match[] = {
	{ .compatible = "samsung,s5k5e2" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5k5e2_of_match);

static struct i2c_driver s5k5e2_i2c_driver = {
	.driver = {
		.name = "s5k5e2",
		.pm = &s5k5e2_pm_ops,
		.of_match_table = s5k5e2_of_match,
	},
	.probe = s5k5e2_probe,
	.remove = s5k5e2_remove,
};

module_i2c_driver(s5k5e2_i2c_driver);

MODULE_DESCRIPTION("Samsung S5K5E2 image sensor driver");
MODULE_LICENSE("GPL");
