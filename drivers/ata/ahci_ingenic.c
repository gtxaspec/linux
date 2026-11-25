// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Ingenic A1 AHCI SATA driver
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * A two-port AHCI controller with an Innophy PHY behind each port. The
 * controller is reset through the CPM and its ports are set up through a PHY
 * control register of their own before the generic host comes up.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/libata.h>
#include <linux/ahci_platform.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include "ahci.h"

#define DRV_NAME "ahci-ingenic"

#define A1_NUM_PORTS		2

/* CPM soft reset and bus control register */
#define CPM_SRBC_SATA_SR	BIT(18)

/* The port PHY control registers, 0x80 apart */
#define A1_AHCI_P0PHYCR		0x178
#define A1_AHCI_PHYCR_STRIDE	0x80
#define PHYCR_PHY_RESET_N	BIT(4)
#define PHYCR_SPDSEL		BIT(8)
#define PHYCR_HOST_SEL		BIT(12)
#define PHYCR_POWER_RESET_N	BIT(28)

/* Innophy registers: receiver squelch and offsets, eye and drive strength */
#define PHY_DRV_STRENGTH	0x028
#define PHY_RX_OFFSET_C		0x020
#define PHY_RX_SQUELCH		0x030
#define PHY_RX_OFFSET_B		0x044
#define PHY_RX_CTLE_EQN_BYPS	0x674
#define PHY_RX_CTLE_EQN		0x714
#define PHY_RX_OFFSET_A		0x870
#define PHY_TX_MAIN_LEG_BYPS	0x1c38
#define PHY_TX_MAIN_LEG_VAL	0x1c3c

#define PHY_DRV_STRENGTH_VAL	0xb0002c92
#define PHY_RX_SQUELCH_VAL	0x1284
#define PHY_TX_MAIN_LEG_BYPS_VAL	0x3
#define PHY_TX_MAIN_LEG_VAL_VAL	0xfc
#define PHY_RX_CTLE_EQN_BYPS_VAL	0x800
#define PHY_RX_CTLE_EQN_VAL	0x1cf3f73c

struct ahci_ingenic {
	void __iomem *phy[A1_NUM_PORTS];
};

static void ahci_ingenic_rmw(void __iomem *reg, u32 clear, u32 set)
{
	writel((readl(reg) & ~clear) | set, reg);
}

static void ahci_ingenic_phy_tune(void __iomem *phy)
{
	writel(PHY_RX_SQUELCH_VAL, phy + PHY_RX_SQUELCH);
	ahci_ingenic_rmw(phy + PHY_RX_OFFSET_A, 0xff, 0);
	ahci_ingenic_rmw(phy + PHY_RX_OFFSET_B, BIT(16), BIT(17));
	ahci_ingenic_rmw(phy + PHY_RX_OFFSET_C, GENMASK(31, 30), BIT(29));

	/* Eye diagram correction and the drive strength */
	writel(PHY_TX_MAIN_LEG_BYPS_VAL, phy + PHY_TX_MAIN_LEG_BYPS);
	writel(PHY_TX_MAIN_LEG_VAL_VAL, phy + PHY_TX_MAIN_LEG_VAL);
	writel(PHY_RX_CTLE_EQN_BYPS_VAL, phy + PHY_RX_CTLE_EQN_BYPS);
	writel(PHY_RX_CTLE_EQN_VAL, phy + PHY_RX_CTLE_EQN);
	writel(PHY_DRV_STRENGTH_VAL, phy + PHY_DRV_STRENGTH);
}

static void ahci_ingenic_port_up(void __iomem *mmio, unsigned int port)
{
	void __iomem *phycr = mmio + A1_AHCI_P0PHYCR + port * A1_AHCI_PHYCR_STRIDE;
	u32 val = readl(phycr);

	/* Host mode at 3 Gbit/s, then the PHY out of reset, power first */
	val |= PHYCR_HOST_SEL | PHYCR_SPDSEL;
	writel(val, phycr);
	udelay(2);
	val |= PHYCR_POWER_RESET_N;
	writel(val, phycr);
	udelay(2);
	val |= PHYCR_PHY_RESET_N;
	writel(val, phycr);
}

static int ahci_ingenic_phy_init(struct platform_device *pdev,
				 struct ahci_host_priv *hpriv)
{
	struct ahci_ingenic *priv = hpriv->plat_data;
	struct regmap *srbc;
	unsigned int i;
	int ret;

	srbc = syscon_regmap_lookup_by_phandle(pdev->dev.of_node, "ingenic,srbc");
	if (IS_ERR(srbc))
		return dev_err_probe(&pdev->dev, PTR_ERR(srbc),
				     "cannot get the soft reset register\n");

	ret = regmap_set_bits(srbc, 0, CPM_SRBC_SATA_SR);
	if (ret)
		return ret;
	ret = regmap_clear_bits(srbc, 0, CPM_SRBC_SATA_SR);
	if (ret)
		return ret;

	for (i = 0; i < A1_NUM_PORTS; i++) {
		ahci_ingenic_phy_tune(priv->phy[i]);
		ahci_ingenic_port_up(hpriv->mmio, i);
	}

	return 0;
}

static const struct ata_port_info ahci_ingenic_port_info = {
	.flags		= AHCI_FLAG_COMMON | ATA_FLAG_NCQ,
	.pio_mask	= ATA_PIO4,
	.udma_mask	= ATA_UDMA6,
	.port_ops	= &ahci_platform_ops,
};

static const struct scsi_host_template ahci_ingenic_sht = {
	AHCI_SHT(DRV_NAME),
};

static int ahci_ingenic_probe(struct platform_device *pdev)
{
	static const char * const phy_names[A1_NUM_PORTS] = { "phy0", "phy1" };
	struct device *dev = &pdev->dev;
	struct ahci_host_priv *hpriv;
	struct ahci_ingenic *priv;
	unsigned int i;
	int rc;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	for (i = 0; i < A1_NUM_PORTS; i++) {
		priv->phy[i] = devm_platform_ioremap_resource_byname(pdev, phy_names[i]);
		if (IS_ERR(priv->phy[i]))
			return PTR_ERR(priv->phy[i]);
	}

	hpriv = ahci_platform_get_resources(pdev, 0);
	if (IS_ERR(hpriv))
		return PTR_ERR(hpriv);

	hpriv->plat_data = priv;
	hpriv->flags |= AHCI_HFLAG_YES_FBS | AHCI_HFLAG_YES_NCQ;

	rc = ahci_platform_enable_resources(hpriv);
	if (rc)
		return rc;

	rc = ahci_ingenic_phy_init(pdev, hpriv);
	if (rc)
		goto disable_resources;

	rc = ahci_platform_init_host(pdev, hpriv, &ahci_ingenic_port_info,
				     &ahci_ingenic_sht);
	if (rc)
		goto disable_resources;

	return 0;

disable_resources:
	ahci_platform_disable_resources(hpriv);
	return rc;
}

static DEFINE_SIMPLE_DEV_PM_OPS(ahci_ingenic_pm_ops, ahci_platform_suspend,
				ahci_platform_resume);

static const struct of_device_id ahci_ingenic_of_match[] = {
	{ .compatible = "ingenic,a1-ahci" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, ahci_ingenic_of_match);

static struct platform_driver ahci_ingenic_driver = {
	.probe = ahci_ingenic_probe,
	.remove = ata_platform_remove_one,
	.shutdown = ahci_platform_shutdown,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = ahci_ingenic_of_match,
		.pm = &ahci_ingenic_pm_ops,
	},
};
module_platform_driver(ahci_ingenic_driver);

MODULE_DESCRIPTION("Ingenic A1 AHCI SATA driver");
MODULE_AUTHOR("Alfonso Gamboa <gtxent@gmail.com>");
MODULE_LICENSE("GPL");
