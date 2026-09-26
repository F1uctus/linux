/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PHY_QCOM_USB_HS_H
#define __PHY_QCOM_USB_HS_H

#include <linux/errno.h>
#include <linux/power_supply.h>

struct phy;

#if IS_REACHABLE(CONFIG_PHY_QCOM_USB_HS)
/*
 * BC1.2 detection on a powered PHY with the D+ pull-up off; sleeps for up
 * to about a second. -ENOTCONN if VBUS is absent.
 */
int qcom_usb_hs_phy_detect_charger(struct phy *phy,
				   enum power_supply_usb_type *type);
#else
static inline int qcom_usb_hs_phy_detect_charger(struct phy *phy,
						 enum power_supply_usb_type *type)
{
	return -EOPNOTSUPP;
}
#endif

#endif
