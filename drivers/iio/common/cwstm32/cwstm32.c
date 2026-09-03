// SPDX-License-Identifier: GPL-2.0-only
/*
 * CyWee CWSTM32 sensor-hub IIO driver
 *
 * The ZTE Blade S6 wires its accel/gyro/magnetometer/ALS/proximity sensors to
 * an STM32 running CyWee sensor-hub firmware, reachable over I2C at 0x3a. This
 * driver speaks the CyWee register protocol and exposes the five physical
 * sensors as IIO channels. Register map taken from the downstream
 * drivers/misc/zte-sensor/CwMcuSensor.h.
 *
 * The hub clock-stretches/NAKs its motion data registers while a new sample is
 * being latched, so polling accel during movement stalls the I2C bus for
 * seconds. The motion sensors are therefore read only from the hub's
 * data-ready IRQ (when a stable sample is guaranteed) into a cache that
 * read_raw() returns without touching the bus. Light/proximity are low-rate
 * event sensors and stay read-on-demand.
 */

#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/jiffies.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>
#include <linux/workqueue.h>

#include <linux/iio/iio.h>

#define CW_FWVERSION		0x3a	/* read 2 B: major, minor */
#define CW_ENABLE_REG		0x01	/* write enable bitmask, one reg per 8 ids */
#define CW_SENSOR_DELAY_SET	0x06	/* write [id, period_ms] */
#define CW_INTERRUPT_STATUS	0x0f	/* read 6 B: [0,1]=status [2..5]=update */
#define CW_SENSORS_REG_START	0x60	/* per-sensor data window base: + id */

/* CW_SENSORS_ID — the five physical sensors in scope */
#define CW_ACCELERATION		0
#define CW_MAGNETIC		1
#define CW_GYRO			2
#define CW_LIGHT		3
#define CW_PROXIMITY		4
#define CW_MOTION_COUNT		3	/* accel/magn/gyro = ids 0..2 */

/* CW_INTERRUPT_STATUS bits */
#define CW_INT_INIT		BIT(1)	/* hub reset: re-enable sensors */
#define CW_INT_DATAREADY	BIT(6)	/* new sample(s) latched */

#define CW_MOTION_PERIOD_MS	50	/* ~20 Hz motion sampling */
#define CW_FWVERSION_RETRIES	3
#define CW_NODATA		0xff	/* high byte sentinel: no fresh sample */
#define CW_NODATA_RETRIES	8
#define CW_WATCHDOG_MS		250	/* stall-recovery poll interval */
#define CW_STALL_MS		400	/* no data-ready for this long => re-kick */

struct cwstm32 {
	struct i2c_client *client;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *boot_gpio;
	struct gpio_desc *wakeup_gpio;
	struct mutex lock;	/* serialises i2c + enabled_mask + cache */
	u32 enabled_mask;	/* bitmask of enabled CW_SENSORS_ID */

	/* latest motion samples, filled from the data-ready IRQ (ids 0..2) */
	s16 cache[CW_MOTION_COUNT][3];
	bool cache_valid[CW_MOTION_COUNT];

	/* vigorous motion intermittently stalls the hub's data-ready stream and
	 * it does not resume on its own; a watchdog re-kicks it on stall */
	unsigned long last_irq;
	struct delayed_work recover_work;
};

/* The hub sleeps between accesses; strobe the wakeup line and let it settle. */
static void cwstm32_wake(struct cwstm32 *st, bool on)
{
	if (!st->wakeup_gpio)
		return;
	gpiod_set_value_cansleep(st->wakeup_gpio, on);
	if (on)
		usleep_range(2000, 4000);
}

static int cwstm32_read_block(struct cwstm32 *st, u8 reg, u8 *buf, u8 len)
{
	int ret = i2c_smbus_read_i2c_block_data(st->client, reg, len, buf);

	if (ret < 0)
		return ret;
	if (ret != len)
		return -EIO;
	return 0;
}

/* Pulse reset low with boot deasserted to enter application (sensor) mode. */
static void cwstm32_reset(struct cwstm32 *st)
{
	gpiod_set_value_cansleep(st->boot_gpio, 0);
	gpiod_set_value_cansleep(st->reset_gpio, 1);
	msleep(10);
	gpiod_set_value_cansleep(st->reset_gpio, 0);
	msleep(10);
	gpiod_set_value_cansleep(st->reset_gpio, 1);
	msleep(90);
}

static int cwstm32_fw_version(struct cwstm32 *st, u8 *ver)
{
	int ret, i;

	for (i = 0; i < CW_FWVERSION_RETRIES; i++) {
		ret = cwstm32_read_block(st, CW_FWVERSION, ver, 2);
		if (ret == 0)
			return 0;
		msleep(20);
	}
	return ret;
}

/* Enable a sensor and set its sample period. Caller holds st->lock. */
static int cwstm32_enable(struct cwstm32 *st, int id, u8 period_ms)
{
	int part = id / 8;
	u8 delay[2] = { id, period_ms };
	u8 mask;
	int ret;

	st->enabled_mask |= BIT(id);
	mask = st->enabled_mask >> (part * 8);

	ret = i2c_smbus_write_byte_data(st->client, CW_ENABLE_REG + part, mask);
	if (ret < 0)
		goto err;

	ret = i2c_smbus_write_i2c_block_data(st->client, CW_SENSOR_DELAY_SET,
					     sizeof(delay), delay);
	if (ret < 0)
		goto err;

	return 0;
err:
	st->enabled_mask &= ~BIT(id);
	return ret;
}

/* Re-assert every enabled sensor (after a hub INIT/reset). Caller holds lock. */
static void cwstm32_reenable(struct cwstm32 *st)
{
	u32 mask = st->enabled_mask;
	int id;

	st->enabled_mask = 0;
	for (id = 0; mask; id++, mask >>= 1) {
		if (mask & 1)
			cwstm32_enable(st, id, CW_MOTION_PERIOD_MS);
	}
}

/*
 * Data-ready IRQ: the hub has latched fresh sample(s), so the motion data
 * registers are safe to read now (no mid-update stall). Pull every updated
 * motion sensor into the cache; read_raw() serves from there.
 */
static irqreturn_t cwstm32_irq(int irq, void *data)
{
	struct cwstm32 *st = data;
	u8 sbuf[6], blk[6];
	u32 update;
	u16 status;
	int id;

	mutex_lock(&st->lock);
	st->last_irq = jiffies;
	cwstm32_wake(st, true);

	if (cwstm32_read_block(st, CW_INTERRUPT_STATUS, sbuf, 6) < 0)
		goto out;

	status = sbuf[0] | (sbuf[1] << 8);
	update = sbuf[2] | (sbuf[3] << 8) | (sbuf[4] << 16) | (sbuf[5] << 24);

	if (status & CW_INT_INIT)
		cwstm32_reenable(st);

	if (status & CW_INT_DATAREADY) {
		for (id = 0; id < CW_MOTION_COUNT; id++) {
			if (!(update & BIT(id)) || !(st->enabled_mask & BIT(id)))
				continue;
			if (cwstm32_read_block(st, CW_SENSORS_REG_START + id,
					       blk, 6) < 0)
				continue;
			st->cache[id][0] = blk[0] | (blk[1] << 8);
			st->cache[id][1] = blk[2] | (blk[3] << 8);
			st->cache[id][2] = blk[4] | (blk[5] << 8);
			st->cache_valid[id] = true;
		}
	}
out:
	cwstm32_wake(st, false);
	mutex_unlock(&st->lock);
	return IRQ_HANDLED;
}

/*
 * The hub's data-ready stream intermittently stalls during vigorous motion and
 * stays dead until re-kicked. When no IRQ has arrived for CW_STALL_MS, reset
 * into application mode and re-enable the sensors (the same recovery a re-probe
 * does); at rest the IRQ keeps last_irq fresh so this never fires.
 */
static void cwstm32_recover_work(struct work_struct *work)
{
	struct cwstm32 *st = container_of(to_delayed_work(work),
					  struct cwstm32, recover_work);

	mutex_lock(&st->lock);
	if (st->enabled_mask &&
	    time_after(jiffies, st->last_irq + msecs_to_jiffies(CW_STALL_MS))) {
		cwstm32_reset(st);
		cwstm32_wake(st, true);
		cwstm32_reenable(st);
		cwstm32_wake(st, false);
		st->last_irq = jiffies;	/* grace period for the stream to resume */
	}
	mutex_unlock(&st->lock);

	schedule_delayed_work(&st->recover_work,
			      msecs_to_jiffies(CW_WATCHDOG_MS));
}

/* Motion axis comes from the IRQ-filled cache — never touches the I2C bus. */
static int cwstm32_read_motion(struct cwstm32 *st, int id, int axis, int *val)
{
	int ret = 0;

	mutex_lock(&st->lock);
	if (st->cache_valid[id])
		*val = st->cache[id][axis];
	else
		ret = -EAGAIN;
	mutex_unlock(&st->lock);
	return ret;
}

/*
 * Light and proximity are event-driven: their data register reads CW_NODATA in
 * the high byte until the hub produces a fresh sample, and a cold read can time
 * out. Poll past the sentinel/errors to fetch a real value.
 */
static int cwstm32_read_env(struct cwstm32 *st, int id, int *val)
{
	u8 buf[4];
	int ret = -ENODATA;
	int i;

	mutex_lock(&st->lock);
	cwstm32_wake(st, true);
	if (!(st->enabled_mask & BIT(id))) {
		ret = cwstm32_enable(st, id, CW_MOTION_PERIOD_MS);
		if (ret < 0)
			goto out;
	}
	for (i = 0; i < CW_NODATA_RETRIES; i++) {
		msleep(CW_MOTION_PERIOD_MS);
		ret = cwstm32_read_block(st, CW_SENSORS_REG_START + id, buf, 4);
		if (ret < 0)
			continue;
		if (buf[1] == CW_NODATA) {
			ret = -ENODATA;
			continue;
		}
		*val = buf[0] | (buf[1] << 8);
		break;
	}
out:
	cwstm32_wake(st, false);
	mutex_unlock(&st->lock);
	return ret;
}

static int cwstm32_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct cwstm32 *st = iio_priv(indio_dev);
	int axis, ret;

	if (mask != IIO_CHAN_INFO_RAW)
		return -EINVAL;

	switch (chan->type) {
	case IIO_ACCEL:
	case IIO_ANGL_VEL:
	case IIO_MAGN:
		switch (chan->channel2) {
		case IIO_MOD_X: axis = 0; break;
		case IIO_MOD_Y: axis = 1; break;
		case IIO_MOD_Z: axis = 2; break;
		default: return -EINVAL;
		}
		ret = cwstm32_read_motion(st, chan->address, axis, val);
		if (ret < 0)
			return ret;
		return IIO_VAL_INT;
	case IIO_LIGHT:
	case IIO_PROXIMITY:
		ret = cwstm32_read_env(st, chan->address, val);
		if (ret < 0)
			return ret;
		return IIO_VAL_INT;
	default:
		return -EINVAL;
	}
}

static const struct iio_info cwstm32_info = {
	.read_raw = cwstm32_read_raw,
};

#define CWSTM32_AXIS_CHANNEL(_type, _mod, _id)			\
{								\
	.type = _type,						\
	.modified = 1,						\
	.channel2 = IIO_MOD_##_mod,				\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.address = _id,						\
}

static const struct iio_chan_spec cwstm32_channels[] = {
	CWSTM32_AXIS_CHANNEL(IIO_ACCEL, X, CW_ACCELERATION),
	CWSTM32_AXIS_CHANNEL(IIO_ACCEL, Y, CW_ACCELERATION),
	CWSTM32_AXIS_CHANNEL(IIO_ACCEL, Z, CW_ACCELERATION),
	CWSTM32_AXIS_CHANNEL(IIO_ANGL_VEL, X, CW_GYRO),
	CWSTM32_AXIS_CHANNEL(IIO_ANGL_VEL, Y, CW_GYRO),
	CWSTM32_AXIS_CHANNEL(IIO_ANGL_VEL, Z, CW_GYRO),
	CWSTM32_AXIS_CHANNEL(IIO_MAGN, X, CW_MAGNETIC),
	CWSTM32_AXIS_CHANNEL(IIO_MAGN, Y, CW_MAGNETIC),
	CWSTM32_AXIS_CHANNEL(IIO_MAGN, Z, CW_MAGNETIC),
	{
		.type = IIO_LIGHT,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.address = CW_LIGHT,
	},
	{
		.type = IIO_PROXIMITY,
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
		.address = CW_PROXIMITY,
	},
};

static int cwstm32_probe(struct i2c_client *client)
{
	static const char * const supplies[] = { "vdd", "vio" };
	struct device *dev = &client->dev;
	struct iio_dev *indio_dev;
	struct cwstm32 *st;
	u8 ver[2];
	int id, ret;

	/* SMBus block/byte ops are emulated over plain I2C transfers */
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -EOPNOTSUPP;

	if (client->irq <= 0)
		return dev_err_probe(dev, -EINVAL, "missing data-ready IRQ\n");

	indio_dev = devm_iio_device_alloc(dev, sizeof(*st));
	if (!indio_dev)
		return -ENOMEM;

	st = iio_priv(indio_dev);
	st->client = client;
	mutex_init(&st->lock);
	i2c_set_clientdata(client, indio_dev);

	ret = devm_regulator_bulk_get_enable(dev, ARRAY_SIZE(supplies), supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable sensor supplies\n");

	st->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(st->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(st->reset_gpio),
				     "failed to get reset gpio\n");

	st->boot_gpio = devm_gpiod_get(dev, "boot", GPIOD_OUT_LOW);
	if (IS_ERR(st->boot_gpio))
		return dev_err_probe(dev, PTR_ERR(st->boot_gpio),
				     "failed to get boot gpio\n");

	st->wakeup_gpio = devm_gpiod_get_optional(dev, "wakeup", GPIOD_OUT_LOW);
	if (IS_ERR(st->wakeup_gpio))
		return dev_err_probe(dev, PTR_ERR(st->wakeup_gpio),
				     "failed to get wakeup gpio\n");

	cwstm32_reset(st);

	cwstm32_wake(st, true);
	ret = cwstm32_fw_version(st, ver);
	if (ret == 0) {
		/* enable the motion sensors so the hub streams data-ready IRQs */
		for (id = 0; id < CW_MOTION_COUNT && ret == 0; id++)
			ret = cwstm32_enable(st, id, CW_MOTION_PERIOD_MS);
	}
	cwstm32_wake(st, false);
	if (ret < 0)
		return dev_err_probe(dev, ret, "STM32 sensor hub not responding\n");
	dev_info(dev, "CyWee STM32 sensor hub firmware %u.%u\n", ver[0], ver[1]);

	st->last_irq = jiffies;
	ret = devm_request_threaded_irq(dev, client->irq, NULL, cwstm32_irq,
					IRQF_ONESHOT, "cwstm32", st);
	if (ret)
		return dev_err_probe(dev, ret, "failed to request IRQ\n");

	INIT_DELAYED_WORK(&st->recover_work, cwstm32_recover_work);
	schedule_delayed_work(&st->recover_work,
			      msecs_to_jiffies(CW_WATCHDOG_MS));

	indio_dev->name = "cwstm32";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &cwstm32_info;
	indio_dev->channels = cwstm32_channels;
	indio_dev->num_channels = ARRAY_SIZE(cwstm32_channels);

	return devm_iio_device_register(dev, indio_dev);
}

static void cwstm32_remove(struct i2c_client *client)
{
	struct iio_dev *indio_dev = i2c_get_clientdata(client);
	struct cwstm32 *st = iio_priv(indio_dev);

	cancel_delayed_work_sync(&st->recover_work);
}

static const struct of_device_id cwstm32_of_match[] = {
	{ .compatible = "cwstm,cwstm32" },
	{ }
};
MODULE_DEVICE_TABLE(of, cwstm32_of_match);

static const struct i2c_device_id cwstm32_id[] = {
	{ "cwstm32" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, cwstm32_id);

static struct i2c_driver cwstm32_driver = {
	.driver = {
		.name = "cwstm32",
		.of_match_table = cwstm32_of_match,
	},
	.probe = cwstm32_probe,
	.remove = cwstm32_remove,
	.id_table = cwstm32_id,
};
module_i2c_driver(cwstm32_driver);

MODULE_DESCRIPTION("CyWee CWSTM32 sensor-hub IIO driver");
MODULE_LICENSE("GPL");
