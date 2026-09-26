// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2016 Linaro Ltd
 */
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/phy/phy-qcom-usb-hs.h>
#include <linux/ulpi/driver.h>
#include <linux/ulpi/regs.h>
#include <linux/clk.h>
#include <linux/regulator/consumer.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/reset.h>
#include <linux/extcon.h>
#include <linux/notifier.h>
#include <linux/workqueue.h>

#define ULPI_PWR_CLK_MNG_REG		0x88
# define ULPI_PWR_OTG_COMP_DISABLE	BIT(0)

#define ULPI_MISC_A			0x96
# define ULPI_MISC_A_VBUSVLDEXTSEL	BIT(1)
# define ULPI_MISC_A_VBUSVLDEXT		BIT(0)

/* SNPS PICO battery charging detection, as used by downstream phy-msm-usb */
#define PICO_CHG_SET			0x85
#define PICO_CHG_CLR			0x86
# define PICO_CHG_CMP_EN		BIT(0)
# define PICO_CHG_SRC_EN		BIT(1)
# define PICO_CHG_SRC_DM		BIT(3)
# define PICO_CHG_DCD_EN		BIT(4)
# define PICO_CHG_ALL			GENMASK(5, 0)
#define PICO_CHG_STATUS			0x87
# define PICO_CHG_STATUS_VOUT		BIT(0)
# define PICO_CHG_STATUS_DCD		BIT(1)
#define PICO_ALT_INT_LATCH_CLR		0x92
#define PICO_ALT_INT_EN_CLR		0x95
# define PICO_ALT_INT_ALL		GENMASK(4, 0)

#define BC12_DCD_TIMEOUT_MS		750
#define BC12_DCD_POLL_MS		50
#define BC12_DET_MS			50
#define BC12_SRC_OFF_MS			20
#define BC12_TRIES			3

struct ulpi_seq {
	u8 addr;
	u8 val;
};

struct qcom_usb_hs_phy {
	struct ulpi *ulpi;
	struct phy *phy;
	struct clk *ref_clk;
	struct clk *sleep_clk;
	struct regulator *v1p8;
	struct regulator *v3p3;
	struct reset_control *reset;
	struct extcon_dev *vbus_edev;
	struct notifier_block vbus_notify;
	struct work_struct vbus_work;
	atomic_t vbus_events;
	struct mutex lock;
	struct ulpi_seq init_seq[];
};

static int qcom_usb_hs_phy_set_mode(struct phy *phy,
				    enum phy_mode mode, int submode)
{
	struct qcom_usb_hs_phy *uphy = phy_get_drvdata(phy);
	u8 addr;
	int ret;

	guard(mutex)(&uphy->lock);

	if (!uphy->vbus_edev) {
		u8 val = 0;

		switch (mode) {
		case PHY_MODE_USB_OTG:
		case PHY_MODE_USB_HOST:
			val |= ULPI_INT_IDGRD;
			fallthrough;
		case PHY_MODE_USB_DEVICE:
			val |= ULPI_INT_SESS_VALID;
			break;
		default:
			break;
		}

		ret = ulpi_write(uphy->ulpi, ULPI_USB_INT_EN_RISE, val);
		if (ret)
			return ret;
		ret = ulpi_write(uphy->ulpi, ULPI_USB_INT_EN_FALL, val);
	} else {
		switch (mode) {
		case PHY_MODE_USB_OTG:
		case PHY_MODE_USB_DEVICE:
			addr = ULPI_SET(ULPI_MISC_A);
			break;
		case PHY_MODE_USB_HOST:
			addr = ULPI_CLR(ULPI_MISC_A);
			break;
		default:
			return -EINVAL;
		}

		ret = ulpi_write(uphy->ulpi, ULPI_SET(ULPI_PWR_CLK_MNG_REG),
				 ULPI_PWR_OTG_COMP_DISABLE);
		if (ret)
			return ret;
		ret = ulpi_write(uphy->ulpi, addr, ULPI_MISC_A_VBUSVLDEXTSEL);
	}

	return ret;
}

static const struct phy_ops qcom_usb_hs_phy_ops;

static int qcom_usb_hs_phy_write_vbus(struct qcom_usb_hs_phy *uphy)
{
	u8 addr;

	if (extcon_get_state(uphy->vbus_edev, EXTCON_USB) > 0)
		addr = ULPI_SET(ULPI_MISC_A);
	else
		addr = ULPI_CLR(ULPI_MISC_A);

	return ulpi_write(uphy->ulpi, addr, ULPI_MISC_A_VBUSVLDEXT);
}

static void qcom_usb_hs_phy_vbus_work(struct work_struct *work)
{
	struct qcom_usb_hs_phy *uphy = container_of(work, struct qcom_usb_hs_phy,
						    vbus_work);

	guard(mutex)(&uphy->lock);
	qcom_usb_hs_phy_write_vbus(uphy);
}

static int
qcom_usb_hs_phy_vbus_notifier(struct notifier_block *nb, unsigned long event,
			      void *ptr)
{
	struct qcom_usb_hs_phy *uphy;

	uphy = container_of(nb, struct qcom_usb_hs_phy, vbus_notify);

	atomic_inc(&uphy->vbus_events);
	schedule_work(&uphy->vbus_work);

	return NOTIFY_OK;
}

static void qcom_usb_hs_phy_bc12_write(struct qcom_usb_hs_phy *uphy, u8 addr,
				       u8 val, int *err)
{
	if (!*err)
		*err = ulpi_write(uphy->ulpi, addr, val);
}

static int qcom_usb_hs_phy_bc12_read(struct qcom_usb_hs_phy *uphy, u8 addr,
				     int *err)
{
	int ret;

	if (*err)
		return 0;

	ret = ulpi_read(uphy->ulpi, addr);
	if (ret < 0) {
		*err = ret;
		return 0;
	}
	return ret;
}

/* Sleeps, then fails with -EAGAIN if VBUS changed meanwhile */
static void qcom_usb_hs_phy_bc12_wait(struct qcom_usb_hs_phy *uphy,
				      int events, unsigned int ms, int *err)
{
	if (*err)
		return;

	msleep(ms);
	if (atomic_read(&uphy->vbus_events) != events)
		*err = -EAGAIN;
}

static int qcom_usb_hs_phy_bc12(struct qcom_usb_hs_phy *uphy, int events,
				enum power_supply_usb_type *type)
{
	enum power_supply_usb_type result;
	int func, otg, status, line, err = 0, ret;
	unsigned int ms;
	bool dcd = false;

	func = ulpi_read(uphy->ulpi, ULPI_FUNC_CTRL);
	if (func < 0)
		return func;
	otg = ulpi_read(uphy->ulpi, ULPI_OTG_CTRL);
	if (otg < 0)
		return otg;

	qcom_usb_hs_phy_bc12_write(uphy, ULPI_FUNC_CTRL,
				   (func & ~ULPI_FUNC_CTRL_OPMODE_MASK) |
				   ULPI_FUNC_CTRL_OPMODE_NONDRIVING, &err);
	qcom_usb_hs_phy_bc12_write(uphy, ULPI_CLR(ULPI_OTG_CTRL),
				   ULPI_OTG_CTRL_DP_PULLDOWN |
				   ULPI_OTG_CTRL_DM_PULLDOWN, &err);
	qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_CLR, PICO_CHG_ALL, &err);
	qcom_usb_hs_phy_bc12_write(uphy, PICO_ALT_INT_LATCH_CLR, PICO_ALT_INT_ALL, &err);
	qcom_usb_hs_phy_bc12_write(uphy, PICO_ALT_INT_EN_CLR, PICO_ALT_INT_ALL, &err);
	usleep_range(100, 200);

	/* Data contact detection; a timeout still proceeds to primary detection */
	qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_SET, PICO_CHG_DCD_EN, &err);
	for (ms = 0; !err && !dcd && ms < BC12_DCD_TIMEOUT_MS; ms += BC12_DCD_POLL_MS) {
		qcom_usb_hs_phy_bc12_wait(uphy, events, BC12_DCD_POLL_MS, &err);
		dcd = qcom_usb_hs_phy_bc12_read(uphy, PICO_CHG_STATUS, &err) &
		      PICO_CHG_STATUS_DCD;
	}
	qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_CLR, PICO_CHG_DCD_EN, &err);

	/* Primary detection: source on D+, compare D- */
	qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_SET, PICO_CHG_SRC_EN, &err);
	qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_SET, PICO_CHG_CMP_EN, &err);
	qcom_usb_hs_phy_bc12_wait(uphy, events, BC12_DET_MS, &err);
	status = qcom_usb_hs_phy_bc12_read(uphy, PICO_CHG_STATUS, &err);
	qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_CLR,
				   PICO_CHG_SRC_EN | PICO_CHG_CMP_EN, &err);
	qcom_usb_hs_phy_bc12_wait(uphy, events, BC12_SRC_OFF_MS, &err);
	line = qcom_usb_hs_phy_bc12_read(uphy, ULPI_DEBUG, &err) &
	       (ULPI_DEBUG_LINESTATE0 | ULPI_DEBUG_LINESTATE1);

	if (line) {
		/* D+ or D- above V_LGC: proprietary charger */
		result = POWER_SUPPLY_USB_TYPE_DCP;
	} else if (status & PICO_CHG_STATUS_VOUT) {
		/* Secondary detection: source on D-, compare D+ */
		qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_SET, PICO_CHG_SRC_DM, &err);
		qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_SET, PICO_CHG_SRC_EN, &err);
		qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_SET, PICO_CHG_CMP_EN, &err);
		qcom_usb_hs_phy_bc12_wait(uphy, events, BC12_DET_MS, &err);
		status = qcom_usb_hs_phy_bc12_read(uphy, PICO_CHG_STATUS, &err);
		result = status & PICO_CHG_STATUS_VOUT ? POWER_SUPPLY_USB_TYPE_DCP :
							 POWER_SUPPLY_USB_TYPE_CDP;
	} else {
		result = POWER_SUPPLY_USB_TYPE_SDP;
	}

	dev_dbg(&uphy->ulpi->dev, "BC1.2: dcd %d status %#x line %#x type %d err %d\n",
		dcd, status, line, result, err);

	ret = err;
	err = 0;
	qcom_usb_hs_phy_bc12_write(uphy, PICO_CHG_CLR, PICO_CHG_ALL, &err);
	qcom_usb_hs_phy_bc12_write(uphy, PICO_ALT_INT_LATCH_CLR, PICO_ALT_INT_ALL, &err);
	qcom_usb_hs_phy_bc12_write(uphy, PICO_ALT_INT_EN_CLR, PICO_ALT_INT_ALL, &err);
	qcom_usb_hs_phy_bc12_write(uphy, ULPI_SET(ULPI_OTG_CTRL), otg &
				   (ULPI_OTG_CTRL_DP_PULLDOWN |
				    ULPI_OTG_CTRL_DM_PULLDOWN), &err);
	qcom_usb_hs_phy_bc12_write(uphy, ULPI_FUNC_CTRL, func, &err);

	ret = ret ?: err;
	if (!ret)
		*type = result;
	return ret;
}

int qcom_usb_hs_phy_detect_charger(struct phy *phy,
				   enum power_supply_usb_type *type)
{
	struct qcom_usb_hs_phy *uphy;
	int tries = BC12_TRIES, ret;

	if (!phy || phy->ops != &qcom_usb_hs_phy_ops)
		return -EOPNOTSUPP;

	uphy = phy_get_drvdata(phy);
	guard(mutex)(&uphy->lock);

	do {
		if (uphy->vbus_edev &&
		    extcon_get_state(uphy->vbus_edev, EXTCON_USB) <= 0)
			return -ENOTCONN;

		ret = qcom_usb_hs_phy_bc12(uphy, atomic_read(&uphy->vbus_events),
					   type);
	} while (ret == -EAGAIN && --tries);

	return ret;
}
EXPORT_SYMBOL_GPL(qcom_usb_hs_phy_detect_charger);

static int qcom_usb_hs_phy_power_on(struct phy *phy)
{
	struct qcom_usb_hs_phy *uphy = phy_get_drvdata(phy);
	struct ulpi *ulpi = uphy->ulpi;
	const struct ulpi_seq *seq;
	int ret;

	ret = clk_prepare_enable(uphy->ref_clk);
	if (ret)
		return ret;

	ret = clk_prepare_enable(uphy->sleep_clk);
	if (ret)
		goto err_sleep;

	ret = regulator_set_load(uphy->v1p8, 50000);
	if (ret < 0)
		goto err_1p8;

	ret = regulator_enable(uphy->v1p8);
	if (ret)
		goto err_1p8;

	ret = regulator_set_voltage_triplet(uphy->v3p3, 3050000, 3300000,
					    3300000);
	if (ret)
		goto err_3p3;

	ret = regulator_set_load(uphy->v3p3, 50000);
	if (ret < 0)
		goto err_3p3;

	ret = regulator_enable(uphy->v3p3);
	if (ret)
		goto err_3p3;

	for (seq = uphy->init_seq; seq->addr; seq++) {
		ret = ulpi_write(ulpi, ULPI_EXT_VENDOR_SPECIFIC + seq->addr,
				 seq->val);
		if (ret)
			goto err_ulpi;
	}

	if (uphy->reset) {
		ret = reset_control_reset(uphy->reset);
		if (ret)
			goto err_ulpi;
	}

	if (uphy->vbus_edev) {
		ret = extcon_register_notifier(uphy->vbus_edev, EXTCON_USB,
					       &uphy->vbus_notify);
		if (ret)
			goto err_ulpi;

		mutex_lock(&uphy->lock);
		ret = qcom_usb_hs_phy_write_vbus(uphy);
		mutex_unlock(&uphy->lock);
		if (ret) {
			extcon_unregister_notifier(uphy->vbus_edev, EXTCON_USB,
						   &uphy->vbus_notify);
			cancel_work_sync(&uphy->vbus_work);
			goto err_ulpi;
		}
	}

	return 0;
err_ulpi:
	regulator_disable(uphy->v3p3);
err_3p3:
	regulator_disable(uphy->v1p8);
err_1p8:
	clk_disable_unprepare(uphy->sleep_clk);
err_sleep:
	clk_disable_unprepare(uphy->ref_clk);
	return ret;
}

static int qcom_usb_hs_phy_power_off(struct phy *phy)
{
	struct qcom_usb_hs_phy *uphy = phy_get_drvdata(phy);

	if (uphy->vbus_edev) {
		extcon_unregister_notifier(uphy->vbus_edev, EXTCON_USB,
					   &uphy->vbus_notify);
		cancel_work_sync(&uphy->vbus_work);
	}
	regulator_disable(uphy->v3p3);
	regulator_disable(uphy->v1p8);
	clk_disable_unprepare(uphy->sleep_clk);
	clk_disable_unprepare(uphy->ref_clk);

	return 0;
}

static const struct phy_ops qcom_usb_hs_phy_ops = {
	.power_on = qcom_usb_hs_phy_power_on,
	.power_off = qcom_usb_hs_phy_power_off,
	.set_mode = qcom_usb_hs_phy_set_mode,
	.owner = THIS_MODULE,
};

static int qcom_usb_hs_phy_probe(struct ulpi *ulpi)
{
	struct qcom_usb_hs_phy *uphy;
	struct phy_provider *p;
	struct clk *clk;
	struct regulator *reg;
	struct reset_control *reset;
	int size;
	int ret;

	size = of_property_count_u8_elems(ulpi->dev.of_node, "qcom,init-seq");
	if (size < 0)
		size = 0;

	uphy = devm_kzalloc(&ulpi->dev, struct_size(uphy, init_seq, (size / 2) + 1), GFP_KERNEL);
	if (!uphy)
		return -ENOMEM;
	ulpi_set_drvdata(ulpi, uphy);
	uphy->ulpi = ulpi;
	mutex_init(&uphy->lock);

	ret = of_property_read_u8_array(ulpi->dev.of_node, "qcom,init-seq",
					(u8 *)uphy->init_seq, size);
	if (ret && size)
		return ret;
	/* NUL terminate */
	uphy->init_seq[size / 2].addr = uphy->init_seq[size / 2].val = 0;

	uphy->ref_clk = clk = devm_clk_get(&ulpi->dev, "ref");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	uphy->sleep_clk = clk = devm_clk_get(&ulpi->dev, "sleep");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	uphy->v1p8 = reg = devm_regulator_get(&ulpi->dev, "v1p8");
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	uphy->v3p3 = reg = devm_regulator_get(&ulpi->dev, "v3p3");
	if (IS_ERR(reg))
		return PTR_ERR(reg);

	uphy->reset = reset = devm_reset_control_get(&ulpi->dev, "por");
	if (IS_ERR(reset)) {
		if (PTR_ERR(reset) == -EPROBE_DEFER)
			return PTR_ERR(reset);
		uphy->reset = NULL;
	}

	uphy->phy = devm_phy_create(&ulpi->dev, ulpi->dev.of_node,
				    &qcom_usb_hs_phy_ops);
	if (IS_ERR(uphy->phy))
		return PTR_ERR(uphy->phy);

	uphy->vbus_edev = extcon_get_edev_by_phandle(&ulpi->dev, 0);
	if (IS_ERR(uphy->vbus_edev)) {
		if (PTR_ERR(uphy->vbus_edev) != -ENODEV)
			return PTR_ERR(uphy->vbus_edev);
		uphy->vbus_edev = NULL;
	}

	uphy->vbus_notify.notifier_call = qcom_usb_hs_phy_vbus_notifier;
	INIT_WORK(&uphy->vbus_work, qcom_usb_hs_phy_vbus_work);
	phy_set_drvdata(uphy->phy, uphy);

	p = devm_of_phy_provider_register(&ulpi->dev, of_phy_simple_xlate);
	return PTR_ERR_OR_ZERO(p);
}

static const struct of_device_id qcom_usb_hs_phy_match[] = {
	{ .compatible = "qcom,usb-hs-phy", },
	{ }
};
MODULE_DEVICE_TABLE(of, qcom_usb_hs_phy_match);

static struct ulpi_driver qcom_usb_hs_phy_driver = {
	.probe = qcom_usb_hs_phy_probe,
	.driver = {
		.name = "qcom_usb_hs_phy",
		.of_match_table = qcom_usb_hs_phy_match,
	},
};
module_ulpi_driver(qcom_usb_hs_phy_driver);

MODULE_DESCRIPTION("Qualcomm USB HS phy");
MODULE_LICENSE("GPL v2");
