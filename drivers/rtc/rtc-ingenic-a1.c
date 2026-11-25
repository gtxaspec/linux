// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic A1 RTC driver
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * The RTC is a free running 32-bit seconds counter: nothing sets it and it
 * has no alarm. The wall time is the counter plus an offset the driver keeps,
 * which a reboot loses.
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/rtc.h>

#define A1_RTC_RTCSR		0x04
#define A1_RTC_CLKCNTEN		0x10
#define A1_RTC_CLKCNTINFO	0x18

struct a1_rtc {
	struct rtc_device *rtc;
	void __iomem *base;
	struct mutex lock;	/* the offset and the counter it belongs to */
	time64_t base_time;
	u32 base_count;
};

static u32 a1_rtc_count(struct a1_rtc *a1)
{
	u32 val, prev;
	int tries = 10;

	/* A read during an update can return garbage: take two equal ones */
	val = readl(a1->base + A1_RTC_RTCSR);
	do {
		prev = val;
		val = readl(a1->base + A1_RTC_RTCSR);
	} while (val != prev && --tries);

	return val;
}

static int a1_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct a1_rtc *a1 = dev_get_drvdata(dev);
	u32 count = a1_rtc_count(a1);

	mutex_lock(&a1->lock);
	rtc_time64_to_tm(a1->base_time + (u32)(count - a1->base_count), tm);
	mutex_unlock(&a1->lock);

	return 0;
}

static int a1_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct a1_rtc *a1 = dev_get_drvdata(dev);
	u32 count = a1_rtc_count(a1);

	mutex_lock(&a1->lock);
	a1->base_time = rtc_tm_to_time64(tm);
	a1->base_count = count;
	mutex_unlock(&a1->lock);

	return 0;
}

static const struct rtc_class_ops a1_rtc_ops = {
	.read_time = a1_rtc_read_time,
	.set_time = a1_rtc_set_time,
};

static int a1_rtc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct a1_rtc *a1;
	struct clk *clk;
	u32 info;
	int ret;

	a1 = devm_kzalloc(dev, sizeof(*a1), GFP_KERNEL);
	if (!a1)
		return -ENOMEM;

	a1->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(a1->base))
		return PTR_ERR(a1->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot get the RTC clock\n");

	if (!readl(a1->base + A1_RTC_CLKCNTEN))
		writel(1, a1->base + A1_RTC_CLKCNTEN);

	/* The counter must run, or there is no RTC behind these registers */
	info = readl(a1->base + A1_RTC_CLKCNTINFO);
	usleep_range(1000, 2000);
	if (readl(a1->base + A1_RTC_CLKCNTINFO) == info)
		return dev_err_probe(dev, -ENODEV, "the RTC counter does not run\n");

	mutex_init(&a1->lock);
	a1->base_count = a1_rtc_count(a1);
	platform_set_drvdata(pdev, a1);

	a1->rtc = devm_rtc_allocate_device(dev);
	if (IS_ERR(a1->rtc))
		return PTR_ERR(a1->rtc);

	a1->rtc->ops = &a1_rtc_ops;
	a1->rtc->range_min = RTC_TIMESTAMP_BEGIN_2000;
	a1->rtc->range_max = RTC_TIMESTAMP_END_2099;

	ret = devm_rtc_register_device(a1->rtc);
	if (ret)
		return ret;

	return 0;
}

static const struct of_device_id a1_rtc_of_match[] = {
	{ .compatible = "ingenic,a1-rtc" },
	{ /* sentinel */ },
};
MODULE_DEVICE_TABLE(of, a1_rtc_of_match);

static struct platform_driver a1_rtc_driver = {
	.probe = a1_rtc_probe,
	.driver = {
		.name = "rtc-ingenic-a1",
		.of_match_table = a1_rtc_of_match,
	},
};
module_platform_driver(a1_rtc_driver);

MODULE_DESCRIPTION("Ingenic A1 RTC driver");
MODULE_AUTHOR("Alfonso Gamboa <gtxent@gmail.com>");
MODULE_LICENSE("GPL");
