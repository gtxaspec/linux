// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic T-series SAR ADC (SADC)
 *
 * Eight auxiliary channels sharing one 12-bit converter. Setting a
 * channel's enable bit with the converter powered up converts it once:
 * the bit clears and the channel's status bit sets when the sample is in
 * the channel's data register.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>

#define SADC_REG_ENABLE		0x00
#define SADC_REG_CTRL		0x08
#define SADC_REG_STATUS		0x0c
#define SADC_REG_DATA(ch)	(0x10 + 2 * (ch))
#define SADC_REG_CLKDIV		0x20

#define SADC_ENABLE_POWER_DOWN	BIT(15)
#define SADC_CTRL_MASK_ALL	GENMASK(15, 0)
#define SADC_DATA_MASK		GENMASK(11, 0)
#define SADC_CLKDIV_DIV		GENMASK(7, 0)
#define SADC_CLKDIV_US		GENMASK(15, 8)
#define SADC_CLKDIV_MS		GENMASK(31, 16)

#define SADC_NUM_CHANNELS	8
#define SADC_BITS		12
#define SADC_TIMEOUT_US		100000

struct ingenic_sadc_soc_data {
	/* EXTCLK dividers for the A/D clock, the 1 us tick and the 1 ms tick */
	u8 clkdiv;
	u8 clkdiv_us;
	u16 clkdiv_ms;
	/* without a vref-supply: the reference the vendor boards use */
	unsigned int vref_mv;
};

struct ingenic_sadc {
	void __iomem *base;
	struct mutex lock;	/* one conversion at a time */
	unsigned int vref_mv;
};

static int ingenic_sadc_convert(struct ingenic_sadc *adc, unsigned int ch, int *val)
{
	u32 st;
	int ret;

	guard(mutex)(&adc->lock);

	writel(BIT(ch), adc->base + SADC_REG_STATUS);
	writel(readl(adc->base + SADC_REG_ENABLE) | BIT(ch), adc->base + SADC_REG_ENABLE);
	ret = readl_poll_timeout(adc->base + SADC_REG_STATUS, st, st & BIT(ch),
				 10, SADC_TIMEOUT_US);
	if (ret)
		return ret;

	*val = readw(adc->base + SADC_REG_DATA(ch)) & SADC_DATA_MASK;
	writel(BIT(ch), adc->base + SADC_REG_STATUS);

	return 0;
}

static int ingenic_sadc_read_raw(struct iio_dev *indio_dev,
				 struct iio_chan_spec const *chan,
				 int *val, int *val2, long mask)
{
	struct ingenic_sadc *adc = iio_priv(indio_dev);
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = ingenic_sadc_convert(adc, chan->channel, val);
		return ret ? ret : IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = adc->vref_mv;
		*val2 = SADC_BITS;
		return IIO_VAL_FRACTIONAL_LOG2;
	default:
		return -EINVAL;
	}
}

static const struct iio_info ingenic_sadc_info = {
	.read_raw = ingenic_sadc_read_raw,
};

#define INGENIC_SADC_CHANNEL(n) {					\
	.type = IIO_VOLTAGE,						\
	.indexed = 1,							\
	.channel = (n),							\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),			\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),		\
}

static const struct iio_chan_spec ingenic_sadc_channels[] = {
	INGENIC_SADC_CHANNEL(0),
	INGENIC_SADC_CHANNEL(1),
	INGENIC_SADC_CHANNEL(2),
	INGENIC_SADC_CHANNEL(3),
	INGENIC_SADC_CHANNEL(4),
	INGENIC_SADC_CHANNEL(5),
	INGENIC_SADC_CHANNEL(6),
	INGENIC_SADC_CHANNEL(7),
};

static void ingenic_sadc_power_down(void *data)
{
	struct ingenic_sadc *adc = data;

	writel(SADC_ENABLE_POWER_DOWN, adc->base + SADC_REG_ENABLE);
}

static int ingenic_sadc_probe(struct platform_device *pdev)
{
	const struct ingenic_sadc_soc_data *soc;
	struct device *dev = &pdev->dev;
	struct iio_dev *indio_dev;
	struct ingenic_sadc *adc;
	struct clk *clk;
	int ret;

	soc = device_get_match_data(dev);
	if (!soc)
		return -EINVAL;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*adc));
	if (!indio_dev)
		return -ENOMEM;

	adc = iio_priv(indio_dev);
	ret = devm_mutex_init(dev, &adc->lock);
	if (ret)
		return ret;

	adc->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(adc->base))
		return PTR_ERR(adc->base);

	clk = devm_clk_get_enabled(dev, "adc");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "Unable to get clock\n");

	ret = devm_regulator_get_enable_read_voltage(dev, "vref");
	if (ret < 0 && ret != -ENODEV)
		return dev_err_probe(dev, ret, "Unable to read the reference\n");
	adc->vref_mv = ret > 0 ? ret / 1000 : soc->vref_mv;

	/*
	 * A channel started while the converter is powered down never
	 * finishes, and nothing but a reset clears it: power up first.
	 */
	writel(SADC_CTRL_MASK_ALL, adc->base + SADC_REG_CTRL);
	writel(SADC_ENABLE_POWER_DOWN, adc->base + SADC_REG_ENABLE);
	writel(FIELD_PREP(SADC_CLKDIV_DIV, soc->clkdiv - 1) |
	       FIELD_PREP(SADC_CLKDIV_US, soc->clkdiv_us - 1) |
	       FIELD_PREP(SADC_CLKDIV_MS, soc->clkdiv_ms - 1),
	       adc->base + SADC_REG_CLKDIV);
	writel(0, adc->base + SADC_REG_ENABLE);
	ret = devm_add_action_or_reset(dev, ingenic_sadc_power_down, adc);
	if (ret)
		return ret;
	usleep_range(5000, 6000);

	indio_dev->name = "ingenic-sadc";
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->info = &ingenic_sadc_info;
	indio_dev->channels = ingenic_sadc_channels;
	indio_dev->num_channels = ARRAY_SIZE(ingenic_sadc_channels);

	return devm_iio_device_register(dev, indio_dev);
}

/* T10, T20: the vendor boards' reference is 3.06 V */
static const struct ingenic_sadc_soc_data t20_sadc_soc_data = {
	.clkdiv = 120,
	.clkdiv_us = 2,
	.clkdiv_ms = 200,
	.vref_mv = 3060,
};

static const struct ingenic_sadc_soc_data t21_sadc_soc_data = {
	.clkdiv = 120,
	.clkdiv_us = 2,
	.clkdiv_ms = 200,
	.vref_mv = 1800,
};

static const struct ingenic_sadc_soc_data t31_sadc_soc_data = {
	.clkdiv = 12,
	.clkdiv_us = 2,
	.clkdiv_ms = 200,
	.vref_mv = 1800,
};

static const struct ingenic_sadc_soc_data t32_sadc_soc_data = {
	.clkdiv = 120,
	.clkdiv_us = 2,
	.clkdiv_ms = 300,
	.vref_mv = 1800,
};

static const struct of_device_id ingenic_sadc_of_match[] = {
	{ .compatible = "ingenic,t20-sadc", .data = &t20_sadc_soc_data },
	{ .compatible = "ingenic,t21-sadc", .data = &t21_sadc_soc_data },
	{ .compatible = "ingenic,t23-sadc", .data = &t31_sadc_soc_data },
	{ .compatible = "ingenic,t30-sadc", .data = &t21_sadc_soc_data },
	{ .compatible = "ingenic,t31-sadc", .data = &t31_sadc_soc_data },
	{ .compatible = "ingenic,t32-sadc", .data = &t32_sadc_soc_data },
	{ .compatible = "ingenic,t40-sadc", .data = &t21_sadc_soc_data },
	{ }
};
MODULE_DEVICE_TABLE(of, ingenic_sadc_of_match);

static struct platform_driver ingenic_sadc_driver = {
	.driver = {
		.name = "ingenic-sadc",
		.of_match_table = ingenic_sadc_of_match,
	},
	.probe = ingenic_sadc_probe,
};
module_platform_driver(ingenic_sadc_driver);

MODULE_DESCRIPTION("Ingenic T-series SAR ADC driver");
MODULE_LICENSE("GPL");
