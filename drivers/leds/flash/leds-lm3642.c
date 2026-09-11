// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for Texas Instruments LM3642 LED Flash driver chip
 * Copyright (C) 2012 Texas Instruments
 */

#include <linux/bitfield.h>
#include <linux/cleanup.h>
#include <linux/i2c.h>
#include <linux/led-class-flash.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/property.h>
#include <linux/regmap.h>
#include <media/v4l2-flash-led-class.h>

#define LM3642_REG_IVFM_MODE	0x01
#define LM3642_REG_TORCH_TIME	0x06
#define LM3642_REG_FLASH	0x08
#define LM3642_REG_I_CTRL	0x09
#define LM3642_REG_ENABLE	0x0a
#define LM3642_REG_FLAG		0x0b
#define LM3642_REG_MAX		0x0b

#define LM3642_FLASH_TOUT_MASK	GENMASK(2, 0)
#define LM3642_TORCH_I_MASK	GENMASK(6, 4)
#define LM3642_FLASH_I_MASK	GENMASK(3, 0)

#define LM3642_MODE_MASK	GENMASK(1, 0)
#define LM3642_MODE_STANDBY	0x0
#define LM3642_MODE_INDICATOR	0x1
#define LM3642_MODE_TORCH	0x2
#define LM3642_MODE_FLASH	0x3
#define LM3642_TORCH_PIN_EN	BIT(4)
#define LM3642_STROBE_PIN_EN	BIT(5)
#define LM3642_ENABLE_MASK	GENMASK(5, 0)

#define LM3642_FLAG_TIMEOUT	BIT(0)
#define LM3642_FLAG_THERMAL	BIT(1)
#define LM3642_FLAG_SHORT	BIT(2)
#define LM3642_FLAG_OVP		BIT(3)
#define LM3642_FLAG_UVLO	BIT(4)

/* Torch level n yields (n + 1) * 46.875 mA, flash level n (n + 1) * 93.75 mA */
#define LM3642_ITORCH_STPUA	46875
#define LM3642_ITORCH_MAXUA	375000
#define LM3642_ITORCH_LEVELS	8
#define LM3642_IFLASH_STPUA	93750
#define LM3642_IFLASH_MAXUA	1500000
#define LM3642_FLASHTO_MINUS	100000
#define LM3642_FLASHTO_MAXUS	800000
#define LM3642_FLASHTO_STPUS	100000

struct lm3642 {
	struct device *dev;
	struct regmap *regmap;
	struct mutex lock;

	struct led_classdev_flash flash;
	struct led_classdev indicator;
	struct v4l2_flash *v4l2_flash;
};

static int lm3642_set_mode(struct lm3642 *chip, u8 mode)
{
	return regmap_update_bits(chip->regmap, LM3642_REG_ENABLE,
				  LM3642_ENABLE_MASK, mode);
}

static int lm3642_torch_brightness_set(struct led_classdev *lcdev,
				       enum led_brightness level)
{
	struct lm3642 *chip =
		container_of(lcdev, struct lm3642, flash.led_cdev);
	int ret;

	guard(mutex)(&chip->lock);

	if (level == LED_OFF)
		return lm3642_set_mode(chip, LM3642_MODE_STANDBY);

	ret = regmap_update_bits(chip->regmap, LM3642_REG_I_CTRL,
				 LM3642_TORCH_I_MASK,
				 FIELD_PREP(LM3642_TORCH_I_MASK, level - 1));
	if (ret)
		return ret;

	return lm3642_set_mode(chip, LM3642_MODE_TORCH);
}

static int lm3642_indicator_brightness_set(struct led_classdev *lcdev,
					   enum led_brightness level)
{
	struct lm3642 *chip = container_of(lcdev, struct lm3642, indicator);
	int ret;

	guard(mutex)(&chip->lock);

	if (level == LED_OFF)
		return lm3642_set_mode(chip, LM3642_MODE_STANDBY);

	ret = regmap_update_bits(chip->regmap, LM3642_REG_I_CTRL,
				 LM3642_TORCH_I_MASK,
				 FIELD_PREP(LM3642_TORCH_I_MASK, level - 1));
	if (ret)
		return ret;

	return lm3642_set_mode(chip, LM3642_MODE_INDICATOR);
}

static int lm3642_flash_brightness_set(struct led_classdev_flash *fled_cdev,
				       u32 brightness)
{
	struct lm3642 *chip = container_of(fled_cdev, struct lm3642, flash);
	struct led_flash_setting *s = &fled_cdev->brightness;
	u32 level = brightness / s->step - 1;

	guard(mutex)(&chip->lock);

	return regmap_update_bits(chip->regmap, LM3642_REG_I_CTRL,
				  LM3642_FLASH_I_MASK,
				  FIELD_PREP(LM3642_FLASH_I_MASK, level));
}

static int lm3642_flash_strobe_set(struct led_classdev_flash *fled_cdev,
				   bool state)
{
	struct lm3642 *chip = container_of(fled_cdev, struct lm3642, flash);

	guard(mutex)(&chip->lock);

	return lm3642_set_mode(chip, state ? LM3642_MODE_FLASH :
					     LM3642_MODE_STANDBY);
}

static int lm3642_flash_strobe_get(struct led_classdev_flash *fled_cdev,
				   bool *state)
{
	struct lm3642 *chip = container_of(fled_cdev, struct lm3642, flash);
	unsigned int val;
	int ret;

	guard(mutex)(&chip->lock);

	ret = regmap_read(chip->regmap, LM3642_REG_ENABLE, &val);
	if (ret)
		return ret;

	*state = (val & LM3642_MODE_MASK) == LM3642_MODE_FLASH;

	return 0;
}

static int lm3642_flash_timeout_set(struct led_classdev_flash *fled_cdev,
				    u32 timeout)
{
	struct lm3642 *chip = container_of(fled_cdev, struct lm3642, flash);
	struct led_flash_setting *s = &fled_cdev->timeout;
	u32 level = (timeout - s->min) / s->step;

	guard(mutex)(&chip->lock);

	return regmap_update_bits(chip->regmap, LM3642_REG_FLASH,
				  LM3642_FLASH_TOUT_MASK,
				  FIELD_PREP(LM3642_FLASH_TOUT_MASK, level));
}

/* The flags register latches until read */
static int lm3642_fault_get(struct led_classdev_flash *fled_cdev, u32 *fault)
{
	struct lm3642 *chip = container_of(fled_cdev, struct lm3642, flash);
	u32 led_faults = 0;
	unsigned int val;
	int ret;

	ret = regmap_read(chip->regmap, LM3642_REG_FLAG, &val);
	if (ret)
		return ret;

	if (val & LM3642_FLAG_TIMEOUT)
		led_faults |= LED_FAULT_TIMEOUT;
	if (val & LM3642_FLAG_THERMAL)
		led_faults |= LED_FAULT_OVER_TEMPERATURE;
	if (val & LM3642_FLAG_SHORT)
		led_faults |= LED_FAULT_SHORT_CIRCUIT;
	if (val & LM3642_FLAG_OVP)
		led_faults |= LED_FAULT_OVER_VOLTAGE;
	if (val & LM3642_FLAG_UVLO)
		led_faults |= LED_FAULT_INPUT_VOLTAGE;

	*fault = led_faults;

	return 0;
}

static const struct led_flash_ops lm3642_flash_ops = {
	.flash_brightness_set = lm3642_flash_brightness_set,
	.strobe_set = lm3642_flash_strobe_set,
	.strobe_get = lm3642_flash_strobe_get,
	.timeout_set = lm3642_flash_timeout_set,
	.fault_get = lm3642_fault_get,
};

static const struct regmap_config lm3642_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = LM3642_REG_MAX,
};

#if IS_ENABLED(CONFIG_V4L2_FLASH_LED_CLASS)
static int lm3642_external_strobe_set(struct v4l2_flash *v4l2_flash,
				      bool enable)
{
	struct led_classdev_flash *fled_cdev = v4l2_flash->fled_cdev;
	struct lm3642 *chip = container_of(fled_cdev, struct lm3642, flash);

	guard(mutex)(&chip->lock);

	return lm3642_set_mode(chip, enable ? LM3642_STROBE_PIN_EN |
					      LM3642_MODE_FLASH : 0);
}

static const struct v4l2_flash_ops lm3642_v4l2_flash_ops = {
	.external_strobe_set = lm3642_external_strobe_set,
};

static void lm3642_init_v4l2_config(struct lm3642 *chip,
				    struct v4l2_flash_config *config)
{
	struct led_classdev *lcdev = &chip->flash.led_cdev;
	struct led_flash_setting *s = &config->intensity;

	strscpy(config->dev_name, lcdev->dev->kobj.name,
		sizeof(config->dev_name));

	s->min = s->step = LM3642_ITORCH_STPUA;
	s->max = s->val = s->min + (lcdev->max_brightness - 1) * s->step;

	config->flash_faults = LED_FAULT_TIMEOUT |
			       LED_FAULT_OVER_TEMPERATURE |
			       LED_FAULT_SHORT_CIRCUIT |
			       LED_FAULT_OVER_VOLTAGE |
			       LED_FAULT_INPUT_VOLTAGE;
	config->has_external_strobe = 1;
}
#else
static const struct v4l2_flash_ops lm3642_v4l2_flash_ops;
static void lm3642_init_v4l2_config(struct lm3642 *chip,
				    struct v4l2_flash_config *config)
{
}
#endif

static void lm3642_init_flash_properties(struct lm3642 *chip,
					 struct fwnode_handle *fwnode)
{
	struct led_classdev_flash *flash = &chip->flash;
	struct led_classdev *lcdev = &flash->led_cdev;
	struct led_flash_setting *s;
	u32 val;

	if (fwnode_property_read_u32(fwnode, "led-max-microamp", &val))
		val = LM3642_ITORCH_MAXUA;
	val = clamp_val(val, LM3642_ITORCH_STPUA, LM3642_ITORCH_MAXUA);

	lcdev->max_brightness = val / LM3642_ITORCH_STPUA;
	lcdev->brightness_set_blocking = lm3642_torch_brightness_set;
	lcdev->flags |= LED_DEV_CAP_FLASH;

	if (fwnode_property_read_u32(fwnode, "flash-max-microamp", &val))
		val = LM3642_IFLASH_MAXUA;

	s = &flash->brightness;
	s->min = s->step = LM3642_IFLASH_STPUA;
	s->max = s->val = clamp_val(val, LM3642_IFLASH_STPUA,
				    LM3642_IFLASH_MAXUA);

	if (fwnode_property_read_u32(fwnode, "flash-max-timeout-us", &val))
		val = LM3642_FLASHTO_MAXUS;

	s = &flash->timeout;
	s->min = s->step = LM3642_FLASHTO_STPUS;
	s->max = s->val = clamp_val(val, LM3642_FLASHTO_MINUS,
				    LM3642_FLASHTO_MAXUS);

	flash->ops = &lm3642_flash_ops;
}

static int lm3642_register_indicator(struct lm3642 *chip)
{
	struct fwnode_handle *fwnode __free(fwnode_handle) =
		device_get_named_child_node(chip->dev, "led-indicator");
	struct led_init_data init_data = { .fwnode = fwnode };

	if (!fwnode)
		return 0;

	chip->indicator.max_brightness = LM3642_ITORCH_LEVELS;
	chip->indicator.brightness_set_blocking =
					lm3642_indicator_brightness_set;

	return devm_led_classdev_register_ext(chip->dev, &chip->indicator,
					      &init_data);
}

static int lm3642_probe(struct i2c_client *client)
{
	struct fwnode_handle *fwnode __free(fwnode_handle) =
		device_get_named_child_node(&client->dev, "led-flash");
	struct led_init_data init_data = {};
	struct v4l2_flash_config v4l2_config = {};
	struct lm3642 *chip;
	int ret;

	if (!fwnode)
		return dev_err_probe(&client->dev, -EINVAL,
				     "missing led-flash node\n");

	chip = devm_kzalloc(&client->dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->dev = &client->dev;

	ret = devm_mutex_init(chip->dev, &chip->lock);
	if (ret)
		return ret;

	chip->regmap = devm_regmap_init_i2c(client, &lm3642_regmap_config);
	if (IS_ERR(chip->regmap))
		return dev_err_probe(chip->dev, PTR_ERR(chip->regmap),
				     "failed to allocate register map\n");

	ret = regmap_write(chip->regmap, LM3642_REG_ENABLE, 0);
	if (ret)
		return dev_err_probe(chip->dev, ret, "failed to reach standby\n");

	init_data.fwnode = fwnode;
	lm3642_init_flash_properties(chip, fwnode);

	ret = devm_led_classdev_flash_register_ext(chip->dev, &chip->flash,
						   &init_data);
	if (ret)
		return dev_err_probe(chip->dev, ret, "failed to register flash\n");

	ret = lm3642_register_indicator(chip);
	if (ret)
		return dev_err_probe(chip->dev, ret,
				     "failed to register indicator\n");

	lm3642_init_v4l2_config(chip, &v4l2_config);
	chip->v4l2_flash = v4l2_flash_init(chip->dev, init_data.fwnode,
					   &chip->flash, &lm3642_v4l2_flash_ops,
					   &v4l2_config);
	if (IS_ERR(chip->v4l2_flash))
		return dev_err_probe(chip->dev, PTR_ERR(chip->v4l2_flash),
				     "failed to register v4l2 flash\n");

	i2c_set_clientdata(client, chip);

	return 0;
}

static void lm3642_remove(struct i2c_client *client)
{
	struct lm3642 *chip = i2c_get_clientdata(client);

	v4l2_flash_release(chip->v4l2_flash);
	regmap_write(chip->regmap, LM3642_REG_ENABLE, 0);
}

static void lm3642_shutdown(struct i2c_client *client)
{
	struct lm3642 *chip = i2c_get_clientdata(client);

	regmap_write(chip->regmap, LM3642_REG_ENABLE, 0);
}

static const struct of_device_id lm3642_of_match[] = {
	{ .compatible = "ti,lm3642" },
	{ }
};
MODULE_DEVICE_TABLE(of, lm3642_of_match);

static struct i2c_driver lm3642_i2c_driver = {
	.driver = {
		.name = "lm3642",
		.of_match_table = lm3642_of_match,
	},
	.probe = lm3642_probe,
	.remove = lm3642_remove,
	.shutdown = lm3642_shutdown,
};
module_i2c_driver(lm3642_i2c_driver);

MODULE_DESCRIPTION("Texas Instruments LM3642 flash LED driver");
MODULE_AUTHOR("Daniel Jeong <daniel.jeong@ti.com>");
MODULE_LICENSE("GPL");
