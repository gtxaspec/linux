// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * JZ4780 EFUSE Memory Support driver
 *
 * Copyright (c) 2017 PrasannaKumar Muralidharan <prasannatsmkumar@gmail.com>
 * Copyright (c) 2020 H. Nikolaus Schaller <hns@goldelico.com>
 */

/*
 * Currently supports JZ4780 efuse which has 8K programmable bit.
 * Efuse is separated into seven segments as below:
 *
 * -----------------------------------------------------------------------
 * | 64 bit | 128 bit | 128 bit | 3520 bit | 8 bit | 2296 bit | 2048 bit |
 * -----------------------------------------------------------------------
 *
 * The rom itself is accessed using a 9 bit address line and an 8 word wide bus
 * which reads/writes based on strobes. The strobe is configured in the config
 * register and is based on number of cycles of the bus clock.
 *
 * Driver supports read only as the writes are done in the Factory.
 */

#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/module.h>
#include <linux/nvmem-provider.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/timer.h>

#define JZ_EFUCTRL		(0x0)	/* Control Register */
#define JZ_EFUCFG		(0x4)	/* Configure Register*/
#define JZ_EFUSTATE		(0x8)	/* Status Register */
#define JZ_EFUDATA(n)		(0xC + (n) * 4)
/* Read-only mirror of the fuse array on the T33 */
#define JZ_EFUWINDOW		0x200
#define JZ_EFUWINDOW_SIZE	0x80

/* We read fixed size chunks to avoid complexity in the driver. */
#define JZ_EFU_READ_SIZE_MAX 32

#define EFUCTRL_ADDR_MASK	0x3FF
#define EFUCTRL_ADDR_SHIFT	21
#define EFUCTRL_LEN_MASK	0x1F
#define EFUCTRL_LEN_SHIFT	16
#define EFUCTRL_PG_EN		BIT(15)
#define EFUCTRL_WR_EN		BIT(1)
#define EFUCTRL_RD_EN		BIT(0)

#define EFUCFG_INT_EN		BIT(31)
#define EFUCFG_RD_ADJ_MASK	0xF
#define EFUCFG_RD_ADJ_SHIFT	20
#define EFUCFG_RD_STR_MASK	0xF
#define EFUCFG_RD_STR_SHIFT	16
#define EFUCFG_WR_ADJ_MASK	0xF
#define EFUCFG_WR_ADJ_SHIFT	12
#define EFUCFG_WR_STR_MASK	0xFFF
#define EFUCFG_WR_STR_SHIFT	0

#define EFUSTATE_WR_DONE	BIT(1)
#define EFUSTATE_RD_DONE	BIT(0)

struct jz4780_efuse_soc_data {
	size_t size;
	size_t read_size;
	bool word_addr;
	unsigned int window;
};

struct jz4780_efuse {
	struct device *dev;
	struct regmap *map;
	struct clk *clk;
	size_t read_size;
	bool word_addr;
	unsigned int window;
};

static const struct jz4780_efuse_soc_data jz4780_efuse_soc_data = {
	.size = 1024,
	.read_size = 32,
};

static const struct jz4780_efuse_soc_data t31_efuse_soc_data = {
	.size = 32,
	.read_size = 32,
};

/* The T10's and T20's: byte addressed, a read returns at most the one data register */
static const struct jz4780_efuse_soc_data t20_efuse_soc_data = {
	.size = 96,
	.read_size = 4,
};

/* The T21's: the control register takes a word index, one word a read */
static const struct jz4780_efuse_soc_data t21_efuse_soc_data = {
	.size = 96,
	.read_size = 4,
	.word_addr = true,
};

/* The T32's: the T21's controller over 48 words */
static const struct jz4780_efuse_soc_data t32_efuse_soc_data = {
	.size = 192,
	.read_size = 4,
	.word_addr = true,
};

/* The T33's: 32 words, read from the mirror rather than the controller */
static const struct jz4780_efuse_soc_data t33_efuse_soc_data = {
	.size = JZ_EFUWINDOW_SIZE,
	.read_size = 4,
	.window = JZ_EFUWINDOW,
};

static int jz4780_efuse_read_window(struct jz4780_efuse *efuse,
				    unsigned int offset, void *val,
				    size_t bytes)
{
	while (bytes > 0) {
		unsigned int start = offset & ~3;
		size_t chunk = min_t(size_t, bytes, start + 4 - offset);
		__le32 word;
		unsigned int tmp;
		int ret;

		ret = regmap_read(efuse->map, efuse->window + start, &tmp);
		if (ret < 0)
			return ret;

		word = cpu_to_le32(tmp);
		memcpy(val, (u8 *)&word + offset - start, chunk);

		val += chunk;
		offset += chunk;
		bytes -= chunk;
	}

	return 0;
}

/* main entry point */
static int jz4780_efuse_read(void *context, unsigned int offset,
			     void *val, size_t bytes)
{
	struct jz4780_efuse *efuse = context;

	if (efuse->window)
		return jz4780_efuse_read_window(efuse, offset, val, bytes);

	while (bytes > 0) {
		size_t start = offset & ~(efuse->read_size - 1);
		size_t chunk = min(bytes, (start + efuse->read_size)
				    - offset);
		char buf[JZ_EFU_READ_SIZE_MAX];
		unsigned int tmp;
		u32 ctrl;
		int ret;

		ctrl = ((efuse->word_addr ? start / 4 : start) << EFUCTRL_ADDR_SHIFT)
			| ((efuse->read_size - 1) << EFUCTRL_LEN_SHIFT)
			| EFUCTRL_RD_EN;

		regmap_update_bits(efuse->map, JZ_EFUCTRL,
				   (EFUCTRL_ADDR_MASK << EFUCTRL_ADDR_SHIFT) |
				   (EFUCTRL_LEN_MASK << EFUCTRL_LEN_SHIFT) |
				   EFUCTRL_PG_EN | EFUCTRL_WR_EN |
				   EFUCTRL_RD_EN,
				   ctrl);

		ret = regmap_read_poll_timeout(efuse->map, JZ_EFUSTATE,
					       tmp, tmp & EFUSTATE_RD_DONE,
					       1 * MSEC_PER_SEC,
					       50 * MSEC_PER_SEC);
		if (ret < 0) {
			dev_err(efuse->dev, "Time out while reading efuse data");
			return ret;
		}

		ret = regmap_bulk_read(efuse->map, JZ_EFUDATA(0),
				       buf, efuse->read_size / sizeof(u32));
		if (ret < 0)
			return ret;

		memcpy(val, &buf[offset - start], chunk);

		val += chunk;
		offset += chunk;
		bytes -= chunk;
	}

	return 0;
}

static struct nvmem_config jz4780_efuse_nvmem_config = {
	.name = "jz4780-efuse",
	.size = 1024,
	.word_size = 1,
	.stride = 1,
	.owner = THIS_MODULE,
	.reg_read = jz4780_efuse_read,
};

static const struct regmap_config jz4780_efuse_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = JZ_EFUDATA(7),
};

static const struct regmap_config jz4780_efuse_window_regmap_config = {
	.reg_bits = 32,
	.val_bits = 32,
	.reg_stride = 4,
	.max_register = JZ_EFUWINDOW + JZ_EFUWINDOW_SIZE - 4,
};

static void clk_disable_unprepare_helper(void *clock)
{
	clk_disable_unprepare(clock);
}

static int jz4780_efuse_set_timing(struct jz4780_efuse *efuse,
				   unsigned long clk_rate)
{
	unsigned long rd_adj, rd_strobe;

	/*
	 * rd_adj and rd_strobe are 4 bit values
	 * conditions:
	 *   bus clk_period * (rd_adj + 1) > 6.5ns
	 *   bus clk_period * (rd_adj + 5 + rd_strobe) > 35ns
	 *   i.e. rd_adj >= 6.5ns / clk_period
	 *   i.e. rd_strobe >= 35 ns / clk_period - 5 - rd_adj + 1
	 * constants:
	 *   1 / 6.5ns == 153846154 Hz
	 *   1 / 35ns == 28571429 Hz
	 */

	rd_adj = clk_rate / 153846154;
	rd_strobe = clk_rate / 28571429 - 5 - rd_adj + 1;

	if (rd_adj > EFUCFG_RD_ADJ_MASK ||
	    rd_strobe > EFUCFG_RD_STR_MASK) {
		dev_err(efuse->dev, "Cannot set clock configuration\n");
		return -EINVAL;
	}

	regmap_update_bits(efuse->map, JZ_EFUCFG,
			   (EFUCFG_RD_ADJ_MASK << EFUCFG_RD_ADJ_SHIFT) |
			   (EFUCFG_RD_STR_MASK << EFUCFG_RD_STR_SHIFT),
			   (rd_adj << EFUCFG_RD_ADJ_SHIFT) |
			   (rd_strobe << EFUCFG_RD_STR_SHIFT));

	return 0;
}

static int jz4780_efuse_probe(struct platform_device *pdev)
{
	struct nvmem_device *nvmem;
	const struct jz4780_efuse_soc_data *soc_data;
	struct jz4780_efuse *efuse;
	struct nvmem_config cfg;
	struct device *dev = &pdev->dev;
	void __iomem *regs;
	int ret;

	efuse = devm_kzalloc_obj(dev, *efuse);
	if (!efuse)
		return -ENOMEM;

	regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(regs))
		return PTR_ERR(regs);

	soc_data = of_device_get_match_data(dev);

	efuse->map = devm_regmap_init_mmio(dev, regs, soc_data->window ?
					   &jz4780_efuse_window_regmap_config :
					   &jz4780_efuse_regmap_config);
	if (IS_ERR(efuse->map))
		return PTR_ERR(efuse->map);

	efuse->clk = devm_clk_get(&pdev->dev, NULL);
	if (IS_ERR(efuse->clk))
		return PTR_ERR(efuse->clk);

	ret = clk_prepare_enable(efuse->clk);
	if (ret < 0)
		return ret;

	ret = devm_add_action_or_reset(&pdev->dev,
				       clk_disable_unprepare_helper,
				       efuse->clk);
	if (ret < 0)
		return ret;

	efuse->dev = dev;
	efuse->read_size = soc_data->read_size;
	efuse->word_addr = soc_data->word_addr;
	efuse->window = soc_data->window;

	if (!efuse->window) {
		ret = jz4780_efuse_set_timing(efuse, clk_get_rate(efuse->clk));
		if (ret)
			return ret;
	}

	cfg = jz4780_efuse_nvmem_config;
	cfg.dev = &pdev->dev;
	cfg.priv = efuse;
	cfg.size = soc_data->size;

	nvmem = devm_nvmem_register(dev, &cfg);

	return PTR_ERR_OR_ZERO(nvmem);
}

static const struct of_device_id jz4780_efuse_match[] = {
	{ .compatible = "ingenic,jz4780-efuse", .data = &jz4780_efuse_soc_data },
	{ .compatible = "ingenic,t10-efuse",    .data = &t20_efuse_soc_data    },
	{ .compatible = "ingenic,t20-efuse",    .data = &t20_efuse_soc_data    },
	{ .compatible = "ingenic,t21-efuse",    .data = &t21_efuse_soc_data    },
	{ .compatible = "ingenic,t23-efuse",    .data = &t21_efuse_soc_data    },
	{ .compatible = "ingenic,t30-efuse",    .data = &t21_efuse_soc_data    },
	{ .compatible = "ingenic,t31-efuse",    .data = &t31_efuse_soc_data    },
	{ .compatible = "ingenic,t32-efuse",    .data = &t32_efuse_soc_data    },
	{ .compatible = "ingenic,t33-efuse",    .data = &t33_efuse_soc_data    },
	{ .compatible = "ingenic,t40-efuse",    .data = &t33_efuse_soc_data    },
	{ .compatible = "ingenic,t41-efuse",    .data = &t33_efuse_soc_data    },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, jz4780_efuse_match);

static struct platform_driver jz4780_efuse_driver = {
	.probe  = jz4780_efuse_probe,
	.driver = {
		.name = "jz4780-efuse",
		.of_match_table = jz4780_efuse_match,
	},
};
module_platform_driver(jz4780_efuse_driver);

MODULE_AUTHOR("PrasannaKumar Muralidharan <prasannatsmkumar@gmail.com>");
MODULE_AUTHOR("H. Nikolaus Schaller <hns@goldelico.com>");
MODULE_AUTHOR("Paul Cercueil <paul@crapouillou.net>");
MODULE_DESCRIPTION("Ingenic JZ4780 efuse driver");
MODULE_LICENSE("GPL v2");
