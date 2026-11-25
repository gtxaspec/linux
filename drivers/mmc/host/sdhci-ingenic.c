// SPDX-License-Identifier: GPL-2.0
/*
 * SDHCI host glue for the MSC controllers of the Ingenic T32 and T33
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/mmc/host.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>

#include "sdhci-pltfm.h"

/*
 * The vendor kernel replaces the capability registers with these: a 24 MHz
 * timeout clock, 8-bit bus, ADMA2, SDMA, high speed and 3.3/1.8 V
 * signalling. The base clock they give is not the card clock's source.
 */
#define SDHCI_INGENIC_CAPS		0x276dc898
#define SDHCI_INGENIC_CAPS1		0x00000000

/*
 * The card clock is the CGU's MSC clock, which divides its source by four
 * at least: the PLLs cannot reach the identification rates, so the clock
 * moves to the crystal at those.
 */
#define SDHCI_INGENIC_EXT_MAX_RATE	400000

struct sdhci_ingenic {
	struct clk *clk;
	struct clk *ext;
	struct clk *pll;
	bool no_ext;
};

/*
 * The A1's, T40's and T41's MSC clock is not moved to EXCLK: the T41's mux
 * takes only the PLLs and its index 2 is the VPLL, which stays gated. The CGU
 * gives a card clock of at least this and the SDHCI divider makes the
 * identification clock.
 */
#define SDHCI_INGENIC_T41_MIN_CGU_RATE	12500000

/*
 * The CGU makes the card clock: set it to the rate asked for and leave the
 * SDHCI divider at 1.
 */
static void sdhci_ingenic_set_clock(struct sdhci_host *host, unsigned int clock)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_ingenic *priv = sdhci_pltfm_priv(pltfm_host);
	struct clk *parent;
	int ret;

	host->mmc->actual_clock = 0;
	sdhci_writew(host, 0, SDHCI_CLOCK_CONTROL);

	if (!clock)
		return;

	parent = clock <= SDHCI_INGENIC_EXT_MAX_RATE ? priv->ext : priv->pll;

	clk_disable_unprepare(priv->clk);

	if (priv->no_ext) {
		ret = clk_set_rate(priv->clk, max_t(unsigned int, clock,
						    SDHCI_INGENIC_T41_MIN_CGU_RATE));
		host->max_clk = clk_get_rate(priv->clk);
	} else {
		ret = clk_set_parent(priv->clk, parent);
		if (!ret)
			ret = clk_set_rate(priv->clk, clock);
	}
	if (ret)
		dev_warn(mmc_dev(host->mmc), "cannot set the %u Hz card clock: %d\n",
			 clock, ret);

	ret = clk_prepare_enable(priv->clk);
	if (ret) {
		dev_err(mmc_dev(host->mmc), "cannot enable the card clock: %d\n",
			ret);
		return;
	}

	if (priv->no_ext) {
		sdhci_set_clock(host, clock);
		return;
	}

	host->mmc->actual_clock = clk_get_rate(priv->clk);
	sdhci_enable_clk(host, 0);
}

/* The fastest card clock the MSC clock's present source gives */
static unsigned int sdhci_ingenic_get_max_clock(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_ingenic *priv = sdhci_pltfm_priv(pltfm_host);

	return clk_round_rate(priv->clk, ULONG_MAX);
}

static const struct sdhci_ops sdhci_ingenic_ops = {
	.set_clock = sdhci_ingenic_set_clock,
	.get_max_clock = sdhci_ingenic_get_max_clock,
	.set_bus_width = sdhci_set_bus_width,
	.reset = sdhci_reset,
	.set_uhs_signaling = sdhci_set_uhs_signaling,
};

static const struct sdhci_pltfm_data sdhci_ingenic_pdata = {
	.ops = &sdhci_ingenic_ops,
	.quirks = SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN |
		  SDHCI_QUIRK_INVERTED_WRITE_PROTECT |
		  SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC |
		  SDHCI_QUIRK_NO_HISPD_BIT |
		  SDHCI_QUIRK_32BIT_DMA_ADDR |
		  SDHCI_QUIRK_32BIT_DMA_SIZE,
};

static int sdhci_ingenic_probe(struct platform_device *pdev)
{
	static const u32 caps = SDHCI_INGENIC_CAPS;
	static const u32 caps1 = SDHCI_INGENIC_CAPS1;
	struct device *dev = &pdev->dev;
	struct sdhci_pltfm_host *pltfm_host;
	struct sdhci_ingenic *priv;
	struct sdhci_host *host;
	int ret;

	host = sdhci_pltfm_init(pdev, &sdhci_ingenic_pdata, sizeof(*priv));
	if (IS_ERR(host))
		return PTR_ERR(host);

	pltfm_host = sdhci_priv(host);
	priv = sdhci_pltfm_priv(pltfm_host);

	priv->clk = devm_clk_get_enabled(dev, "mmc");
	if (IS_ERR(priv->clk))
		return dev_err_probe(dev, PTR_ERR(priv->clk),
				     "cannot get the MSC clock\n");

	priv->ext = devm_clk_get(dev, "ext");
	if (IS_ERR(priv->ext))
		return dev_err_probe(dev, PTR_ERR(priv->ext),
				     "cannot get the crystal clock\n");

	/*
	 * Keep the source the boot loader chose for the data rates; from the
	 * crystal the card clock tops out at a quarter of it.
	 */
	priv->pll = clk_get_parent(priv->clk);
	if (!priv->pll || clk_is_match(priv->pll, priv->ext))
		dev_warn(dev, "MSC clock runs from the crystal, card clock limited\n");
	if (!priv->pll)
		priv->pll = priv->ext;

	priv->no_ext = of_device_get_match_data(dev);

	pltfm_host->clk = priv->clk;

	__sdhci_read_caps(host, NULL, &caps, &caps1);

	ret = mmc_of_parse(host->mmc);
	if (ret)
		return ret;

	sdhci_get_property(pdev);

	return sdhci_add_host(host);
}

static const struct of_device_id sdhci_ingenic_of_match[] = {
	{ .compatible = "ingenic,a1-sdhci", .data = (void *)true },
	{ .compatible = "ingenic,t40-sdhci", .data = (void *)true },
	{ .compatible = "ingenic,t41-sdhci", .data = (void *)true },
	{ .compatible = "ingenic,t32-sdhci" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sdhci_ingenic_of_match);

static struct platform_driver sdhci_ingenic_driver = {
	.driver = {
		.name = "sdhci-ingenic",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = sdhci_ingenic_of_match,
		.pm = pm_ptr(&sdhci_pltfm_pmops),
	},
	.probe = sdhci_ingenic_probe,
	.remove = sdhci_pltfm_remove,
};
module_platform_driver(sdhci_ingenic_driver);

MODULE_DESCRIPTION("SDHCI driver for the Ingenic T32 and T33 MSC");
MODULE_AUTHOR("Alfonso Gamboa <gtxent@gmail.com>");
MODULE_LICENSE("GPL");
