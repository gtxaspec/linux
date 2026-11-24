// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Fast Ethernet PHY inside the Ingenic T21
 *
 * Copyright (c) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/bits.h>
#include <linux/module.h>
#include <linux/phy.h>

/* Set by the MAC glue in the SoC's MACPHYP register */
#define T21_EPHY_ID		0xc8007016

/* Undocumented power control; the BSP sets bit 1 after every PHY reset */
#define T21_EPHY_PWR_CTRL	0x18
#define T21_EPHY_PWR_CTRL_UP	BIT(1)

static int t21_ephy_config_init(struct phy_device *phydev)
{
	return phy_set_bits(phydev, T21_EPHY_PWR_CTRL, T21_EPHY_PWR_CTRL_UP);
}

static struct phy_driver ingenic_ephy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(T21_EPHY_ID),
		.name		= "Ingenic T21 internal PHY",
		/* PHY_BASIC_FEATURES */
		.config_init	= t21_ephy_config_init,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
	},
};
module_phy_driver(ingenic_ephy_driver);

static const struct mdio_device_id __maybe_unused ingenic_ephy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(T21_EPHY_ID) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, ingenic_ephy_tbl);

MODULE_DESCRIPTION("Ingenic T21 internal Ethernet PHY driver");
MODULE_AUTHOR("Alfonso Gamboa <gtxent@gmail.com>");
MODULE_LICENSE("GPL");
