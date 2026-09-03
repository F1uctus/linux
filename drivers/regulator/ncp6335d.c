// SPDX-License-Identifier: GPL-2.0-only

#include <linux/i2c.h>
#include <linux/minmax.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/of_regulator.h>

#define NCP6335D_REG_PID	0x03
#define NCP6335D_REG_VSEL1	0x10
#define NCP6335D_REG_VSEL0	0x11
#define NCP6335D_REG_TIMING	0x13

#define NCP6335D_VSEL_MASK	GENMASK(6, 0)
#define NCP6335D_ENABLE		BIT(7)
#define NCP6335D_SLEW_MASK	GENMASK(4, 3)
#define NCP6335D_SLEW_SLOW	(0x3 << 3)

#define NCP6335D_MIN_UV		600000
#define NCP6335D_STEP_UV	6250
#define NCP6335D_N_VOLTAGES	128

static int ncp6335d_write_both(struct regmap *regmap, unsigned int mask,
			       unsigned int val)
{
	int ret;

	ret = regmap_update_bits(regmap, NCP6335D_REG_VSEL0, mask, val);
	if (ret)
		return ret;

	return regmap_update_bits(regmap, NCP6335D_REG_VSEL1, mask, val);
}

static int ncp6335d_set_voltage_sel(struct regulator_dev *rdev, unsigned int sel)
{
	return ncp6335d_write_both(rdev->regmap, NCP6335D_VSEL_MASK, sel);
}

static int ncp6335d_get_voltage_sel(struct regulator_dev *rdev)
{
	unsigned int val;
	int ret;

	ret = regmap_read(rdev->regmap, NCP6335D_REG_VSEL0, &val);
	if (ret)
		return ret;

	return val & NCP6335D_VSEL_MASK;
}

static int ncp6335d_enable(struct regulator_dev *rdev)
{
	return ncp6335d_write_both(rdev->regmap, NCP6335D_ENABLE,
				   NCP6335D_ENABLE);
}

static int ncp6335d_disable(struct regulator_dev *rdev)
{
	return ncp6335d_write_both(rdev->regmap, NCP6335D_ENABLE, 0);
}

static int ncp6335d_is_enabled(struct regulator_dev *rdev)
{
	unsigned int val;
	int ret;

	ret = regmap_read(rdev->regmap, NCP6335D_REG_VSEL0, &val);
	if (ret)
		return ret;

	return !!(val & NCP6335D_ENABLE);
}

static const struct regulator_ops ncp6335d_ops = {
	.list_voltage		= regulator_list_voltage_linear,
	.map_voltage		= regulator_map_voltage_linear,
	.set_voltage_sel	= ncp6335d_set_voltage_sel,
	.get_voltage_sel	= ncp6335d_get_voltage_sel,
	.set_voltage_time_sel	= regulator_set_voltage_time_sel,
	.enable			= ncp6335d_enable,
	.disable		= ncp6335d_disable,
	.is_enabled		= ncp6335d_is_enabled,
};

static const struct regulator_desc ncp6335d_desc = {
	.name		= "ncp6335d",
	.owner		= THIS_MODULE,
	.type		= REGULATOR_VOLTAGE,
	.ops		= &ncp6335d_ops,
	.min_uV		= NCP6335D_MIN_UV,
	.uV_step	= NCP6335D_STEP_UV,
	.n_voltages	= NCP6335D_N_VOLTAGES,

	.ramp_delay	= 1000,
};

static const struct regmap_config ncp6335d_regmap_config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= 0x15,
};

static int ncp6335d_presync(struct regmap *regmap)
{
	unsigned int v0, v1, sel;
	int ret;

	ret = regmap_read(regmap, NCP6335D_REG_VSEL0, &v0);
	if (ret)
		return ret;

	ret = regmap_read(regmap, NCP6335D_REG_VSEL1, &v1);
	if (ret)
		return ret;

	ret = regmap_update_bits(regmap, NCP6335D_REG_TIMING,
				 NCP6335D_SLEW_MASK, NCP6335D_SLEW_SLOW);
	if (ret)
		return ret;

	sel = max(v0 & NCP6335D_VSEL_MASK, v1 & NCP6335D_VSEL_MASK);

	return ncp6335d_write_both(regmap, NCP6335D_VSEL_MASK | NCP6335D_ENABLE,
				   sel | NCP6335D_ENABLE);
}

static int ncp6335d_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct regulator_config config = { };
	struct regulator_dev *rdev;
	struct regmap *regmap;
	unsigned int pid;
	int ret;

	regmap = devm_regmap_init_i2c(client, &ncp6335d_regmap_config);
	if (IS_ERR(regmap))
		return dev_err_probe(dev, PTR_ERR(regmap),
				     "failed to init regmap\n");

	ret = regmap_read(regmap, NCP6335D_REG_PID, &pid);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read chip ID\n");

	ret = ncp6335d_presync(regmap);
	if (ret)
		return dev_err_probe(dev, ret, "failed to sync VSEL presets\n");

	config.dev = dev;
	config.of_node = dev->of_node;
	config.regmap = regmap;
	config.init_data = of_get_regulator_init_data(dev, dev->of_node,
						      &ncp6335d_desc);

	rdev = devm_regulator_register(dev, &ncp6335d_desc, &config);
	if (IS_ERR(rdev))
		return dev_err_probe(dev, PTR_ERR(rdev),
				     "failed to register regulator\n");

	dev_info(dev, "NCP6335D VDD_APC regulator (PID 0x%02x)\n", pid);

	return 0;
}

static const struct of_device_id ncp6335d_of_match[] = {
	{ .compatible = "onnn,ncp6335d" },
	{ }
};
MODULE_DEVICE_TABLE(of, ncp6335d_of_match);

static const struct i2c_device_id ncp6335d_id[] = {
	{ "ncp6335d" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ncp6335d_id);

static struct i2c_driver ncp6335d_driver = {
	.driver = {
		.name = "ncp6335d",
		.of_match_table = ncp6335d_of_match,
	},
	.probe = ncp6335d_probe,
	.id_table = ncp6335d_id,
};
module_i2c_driver(ncp6335d_driver);

MODULE_DESCRIPTION("ON Semiconductor NCP6335D regulator driver");
MODULE_LICENSE("GPL");
