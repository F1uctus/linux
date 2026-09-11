// SPDX-License-Identifier: GPL-2.0
/*
 * ROHM BU64291GWZ voice coil motor lens actuator driver
 */

#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/regulator/consumer.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>

#define BU64291_NAME		"bu64291"

#define BU64291_MAX_FOCUS_POS	1023
#define BU64291_FOCUS_STEPS	1

/* Command in the upper bits, position bits 9:8 in the lower two */
#define BU64291_CMD_TARGET	0xf4
#define BU64291_CMD_POINT_A	0x94
#define BU64291_CMD_POINT_B	0x9c
#define BU64291_CMD_SLEW	0xa4
#define BU64291_CMD_MODE	0x8c

/* Fastest slew with 80 Hz resonance suppression */
#define BU64291_SLEW_VALUE	0x84
#define BU64291_MODE_VALUE	0x2b

/* Ramp the lens back to rest this many steps at a time before powering down */
#define BU64291_CTRL_STEPS	16
#define BU64291_CTRL_DELAY_US	1000

struct bu64291_device {
	struct v4l2_ctrl_handler ctrls_vcm;
	struct v4l2_subdev sd;
	u16 current_val;
	struct regulator *vcc;
};

static inline struct bu64291_device *to_bu64291_vcm(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct bu64291_device, ctrls_vcm);
}

static inline struct bu64291_device *sd_to_bu64291_vcm(struct v4l2_subdev *sd)
{
	return container_of(sd, struct bu64291_device, sd);
}

static int bu64291_write(struct i2c_client *client, u8 cmd, u16 data)
{
	u8 buf[2] = { cmd | ((data >> 8) & 0x03), data & 0xff };
	int ret;

	ret = i2c_master_send(client, buf, sizeof(buf));
	if (ret != sizeof(buf)) {
		dev_err(&client->dev, "I2C write fail\n");
		return -EIO;
	}

	return 0;
}

static int bu64291_t_focus_vcm(struct bu64291_device *bu64291_dev, u16 val)
{
	struct i2c_client *client = v4l2_get_subdevdata(&bu64291_dev->sd);

	bu64291_dev->current_val = val;

	return bu64291_write(client, BU64291_CMD_TARGET, val);
}

static int bu64291_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct bu64291_device *dev_vcm = to_bu64291_vcm(ctrl);

	if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE)
		return bu64291_t_focus_vcm(dev_vcm, ctrl->val);

	return -EINVAL;
}

static const struct v4l2_ctrl_ops bu64291_vcm_ctrl_ops = {
	.s_ctrl = bu64291_set_ctrl,
};

static int bu64291_open(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	return pm_runtime_resume_and_get(sd->dev);
}

static int bu64291_close(struct v4l2_subdev *sd, struct v4l2_subdev_fh *fh)
{
	pm_runtime_put(sd->dev);

	return 0;
}

static const struct v4l2_subdev_internal_ops bu64291_int_ops = {
	.open = bu64291_open,
	.close = bu64291_close,
};

static const struct v4l2_subdev_core_ops bu64291_core_ops = {
	.log_status = v4l2_ctrl_subdev_log_status,
	.subscribe_event = v4l2_ctrl_subdev_subscribe_event,
	.unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_ops bu64291_ops = {
	.core = &bu64291_core_ops,
};

static int bu64291_init_controls(struct bu64291_device *dev_vcm)
{
	struct v4l2_ctrl_handler *hdl = &dev_vcm->ctrls_vcm;
	const struct v4l2_ctrl_ops *ops = &bu64291_vcm_ctrl_ops;

	v4l2_ctrl_handler_init(hdl, 1);

	v4l2_ctrl_new_std(hdl, ops, V4L2_CID_FOCUS_ABSOLUTE,
			  0, BU64291_MAX_FOCUS_POS, BU64291_FOCUS_STEPS, 0);

	if (hdl->error)
		dev_err(dev_vcm->sd.dev, "%s fail error: 0x%x\n",
			__func__, hdl->error);
	dev_vcm->sd.ctrl_handler = hdl;

	return hdl->error;
}

static int bu64291_hw_init(struct bu64291_device *bu64291_dev)
{
	struct i2c_client *client = v4l2_get_subdevdata(&bu64291_dev->sd);
	int ret;

	ret = bu64291_write(client, BU64291_CMD_POINT_A, 1);
	if (ret)
		return ret;

	ret = bu64291_write(client, BU64291_CMD_POINT_B,
			    BU64291_MAX_FOCUS_POS);
	if (ret)
		return ret;

	ret = bu64291_write(client, BU64291_CMD_SLEW, BU64291_SLEW_VALUE);
	if (ret)
		return ret;

	return bu64291_write(client, BU64291_CMD_MODE, BU64291_MODE_VALUE);
}

static int bu64291_power_up(struct bu64291_device *bu64291_dev)
{
	int ret;

	ret = regulator_enable(bu64291_dev->vcc);
	if (ret)
		return ret;

	usleep_range(12000, 14000);

	return 0;
}

static int bu64291_power_down(struct bu64291_device *bu64291_dev)
{
	return regulator_disable(bu64291_dev->vcc);
}

static void bu64291_subdev_cleanup(struct bu64291_device *bu64291_dev)
{
	v4l2_async_unregister_subdev(&bu64291_dev->sd);
	v4l2_ctrl_handler_free(&bu64291_dev->ctrls_vcm);
	media_entity_cleanup(&bu64291_dev->sd.entity);
}

static int bu64291_probe(struct i2c_client *client)
{
	struct bu64291_device *bu64291_dev;
	int rval;

	bu64291_dev = devm_kzalloc(&client->dev, sizeof(*bu64291_dev),
				   GFP_KERNEL);
	if (!bu64291_dev)
		return -ENOMEM;

	bu64291_dev->vcc = devm_regulator_get(&client->dev, "vcc");
	if (IS_ERR(bu64291_dev->vcc))
		return dev_err_probe(&client->dev, PTR_ERR(bu64291_dev->vcc),
				     "could not get vcc regulator\n");

	rval = bu64291_power_up(bu64291_dev);
	if (rval)
		return dev_err_probe(&client->dev, rval,
				     "failed to power up\n");

	v4l2_i2c_subdev_init(&bu64291_dev->sd, client, &bu64291_ops);
	bu64291_dev->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE |
				 V4L2_SUBDEV_FL_HAS_EVENTS;
	bu64291_dev->sd.internal_ops = &bu64291_int_ops;

	rval = bu64291_hw_init(bu64291_dev);
	if (rval) {
		dev_err_probe(&client->dev, rval, "lens actuator not found\n");
		goto err_power_down;
	}

	rval = bu64291_init_controls(bu64291_dev);
	if (rval)
		goto err_cleanup;

	rval = media_entity_pads_init(&bu64291_dev->sd.entity, 0, NULL);
	if (rval < 0)
		goto err_cleanup;

	bu64291_dev->sd.entity.function = MEDIA_ENT_F_LENS;

	rval = v4l2_async_register_subdev(&bu64291_dev->sd);
	if (rval < 0)
		goto err_cleanup;

	pm_runtime_set_active(&client->dev);
	pm_runtime_enable(&client->dev);
	pm_runtime_idle(&client->dev);

	return 0;

err_cleanup:
	v4l2_ctrl_handler_free(&bu64291_dev->ctrls_vcm);
	media_entity_cleanup(&bu64291_dev->sd.entity);
err_power_down:
	bu64291_power_down(bu64291_dev);

	return rval;
}

static void bu64291_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct bu64291_device *bu64291_dev = sd_to_bu64291_vcm(sd);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		bu64291_power_down(bu64291_dev);
	pm_runtime_set_suspended(&client->dev);
	bu64291_subdev_cleanup(bu64291_dev);
}

static int __maybe_unused bu64291_vcm_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct bu64291_device *bu64291_dev = sd_to_bu64291_vcm(sd);
	int ret, val;

	if (pm_runtime_suspended(&client->dev))
		return 0;

	for (val = bu64291_dev->current_val & ~(BU64291_CTRL_STEPS - 1);
	     val >= 0; val -= BU64291_CTRL_STEPS) {
		ret = bu64291_write(client, BU64291_CMD_TARGET, val);
		if (ret)
			dev_err_once(dev, "%s I2C failure: %d", __func__, ret);
		usleep_range(BU64291_CTRL_DELAY_US,
			     BU64291_CTRL_DELAY_US + 10);
	}

	return bu64291_power_down(bu64291_dev);
}

static int __maybe_unused bu64291_vcm_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct bu64291_device *bu64291_dev = sd_to_bu64291_vcm(sd);
	int ret, val;

	if (pm_runtime_suspended(&client->dev))
		return 0;

	ret = bu64291_power_up(bu64291_dev);
	if (ret)
		return ret;

	ret = bu64291_hw_init(bu64291_dev);
	if (ret)
		return ret;

	for (val = bu64291_dev->current_val % BU64291_CTRL_STEPS;
	     val < bu64291_dev->current_val + BU64291_CTRL_STEPS - 1;
	     val += BU64291_CTRL_STEPS) {
		ret = bu64291_write(client, BU64291_CMD_TARGET, val);
		if (ret)
			dev_err_ratelimited(dev, "%s I2C failure: %d",
					    __func__, ret);
		usleep_range(BU64291_CTRL_DELAY_US,
			     BU64291_CTRL_DELAY_US + 10);
	}

	return 0;
}

static const struct of_device_id bu64291_of_table[] = {
	{ .compatible = "rohm,bu64291" },
	{ }
};
MODULE_DEVICE_TABLE(of, bu64291_of_table);

static const struct dev_pm_ops bu64291_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(bu64291_vcm_suspend, bu64291_vcm_resume)
	SET_RUNTIME_PM_OPS(bu64291_vcm_suspend, bu64291_vcm_resume, NULL)
};

static struct i2c_driver bu64291_i2c_driver = {
	.driver = {
		.name = BU64291_NAME,
		.pm = &bu64291_pm_ops,
		.of_match_table = bu64291_of_table,
	},
	.probe = bu64291_probe,
	.remove = bu64291_remove,
};

module_i2c_driver(bu64291_i2c_driver);

MODULE_DESCRIPTION("ROHM BU64291 VCM driver");
MODULE_LICENSE("GPL");
