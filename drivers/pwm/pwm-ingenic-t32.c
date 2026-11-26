// SPDX-License-Identifier: GPL-2.0-only
/*
 * Ingenic T32, T33 and T41 PWM controller
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 *
 * The dedicated PWM block, apart from the TCU: four channels on the T32 and
 * T33, eight on the T41. A channel counts its clock divided by 2^PRESCALE
 * (4 bits a channel in CCFG) and runs periods of HIGH plus LOW ticks (16 bits
 * each, in WCFG): first the channel's init level (PWMLVL bits 15:0) for that
 * level's count, then the other level.
 *
 * Limitations:
 * - A running channel takes a new WCFG, PWMLVL and CCFG only at the end of
 *   its current period, after PWMUPDATE.
 * - Disabling a channel lets it finish its period, then it rests at its
 *   idle level, low.
 * - With either count 0 a channel holds its init level, so a duty of 0 is
 *   a constant low and a full duty a constant high.
 * - The offset of the high part is 0, or the whole low part (inverted
 *   polarity). The DMA mode is not supported.
 */

#include <linux/bitfield.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/pwm.h>
#include <linux/time.h>

#define T32_PWM_CCFG		0x00
#define T32_PWM_ENS		0x10
#define T32_PWM_ENC		0x14
#define T32_PWM_EN		0x18
#define T32_PWM_UPDATE		0x20
#define T32_PWM_LVL		0x30
#define T32_PWM_WCFG(n)		(0xb0 + 4 * (n))
#define T32_PWM_OEN		0x300

#define T32_PWM_CCFG_SHIFT(n)	(4 * (n))
#define T32_PWM_CCFG_PRESCALE	GENMASK(3, 0)
#define T32_PWM_WCFG_HIGH	GENMASK(31, 16)
#define T32_PWM_WCFG_LOW	GENMASK(15, 0)

#define T32_PWM_MAX_PRESCALE	15
#define T32_PWM_MAX_COUNT	0xffff

struct t32_pwm {
	void __iomem *base;
	unsigned long rate;
};

struct t32_pwm_wfhw {
	bool enabled;
	u8 prescale;
	bool init_high;
	u16 high;
	u16 low;
};

static struct t32_pwm *t32_pwm_from_chip(struct pwm_chip *chip)
{
	return pwmchip_get_drvdata(chip);
}

static u64 t32_pwm_ticks(const struct t32_pwm *pwm, u64 ns, unsigned int prescale)
{
	return mul_u64_u64_div_u64(ns, pwm->rate, (u64)NSEC_PER_SEC << prescale);
}

static u64 t32_pwm_ns(const struct t32_pwm *pwm, u64 ticks, unsigned int prescale)
{
	return DIV64_U64_ROUND_UP(ticks * NSEC_PER_SEC << prescale, pwm->rate);
}

static int t32_pwm_round_waveform_tohw(struct pwm_chip *chip,
				       struct pwm_device *pwmd,
				       const struct pwm_waveform *wf,
				       void *_wfhw)
{
	struct t32_pwm *pwm = t32_pwm_from_chip(chip);
	struct t32_pwm_wfhw *wfhw = _wfhw;
	u64 period, duty, low, offset = 0;
	unsigned int prescale;
	int ret = 0;

	if (!wf->period_length_ns) {
		*wfhw = (struct t32_pwm_wfhw){ };
		return 0;
	}

	/* The finest tick that holds both parts of the period in 16 bits */
	for (prescale = 0; ; prescale++) {
		period = t32_pwm_ticks(pwm, wf->period_length_ns, prescale);
		duty = min(t32_pwm_ticks(pwm, wf->duty_length_ns, prescale), period);
		low = period - duty;

		if ((duty <= T32_PWM_MAX_COUNT && low <= T32_PWM_MAX_COUNT) ||
		    prescale == T32_PWM_MAX_PRESCALE)
			break;
	}

	if (!period) {
		/* Shorter than a tick: the shortest period, rounded up */
		period = 1;
		duty = 0;
		low = 1;
		ret = 1;
	}

	duty = min_t(u64, duty, T32_PWM_MAX_COUNT);
	low = min_t(u64, low, T32_PWM_MAX_COUNT);

	if (wf->duty_offset_ns)
		offset = t32_pwm_ticks(pwm, wf->duty_offset_ns, prescale);

	wfhw->enabled = true;
	wfhw->prescale = prescale;
	wfhw->high = duty;
	wfhw->low = low;
	if (!duty)
		wfhw->init_high = false;
	else if (!low)
		wfhw->init_high = true;
	else
		wfhw->init_high = offset < low;

	return ret;
}

static int t32_pwm_round_waveform_fromhw(struct pwm_chip *chip,
					 struct pwm_device *pwmd,
					 const void *_wfhw,
					 struct pwm_waveform *wf)
{
	struct t32_pwm *pwm = t32_pwm_from_chip(chip);
	const struct t32_pwm_wfhw *wfhw = _wfhw;

	if (!wfhw->enabled || (!wfhw->high && !wfhw->low)) {
		*wf = (struct pwm_waveform){ };
		return 0;
	}

	wf->period_length_ns = t32_pwm_ns(pwm, wfhw->high + wfhw->low,
					  wfhw->prescale);
	wf->duty_length_ns = t32_pwm_ns(pwm, wfhw->high, wfhw->prescale);
	if (wfhw->init_high || !wfhw->high || !wfhw->low)
		wf->duty_offset_ns = 0;
	else
		wf->duty_offset_ns = t32_pwm_ns(pwm, wfhw->low, wfhw->prescale);

	return 0;
}

static int t32_pwm_read_waveform(struct pwm_chip *chip, struct pwm_device *pwmd,
				 void *_wfhw)
{
	struct t32_pwm *pwm = t32_pwm_from_chip(chip);
	struct t32_pwm_wfhw *wfhw = _wfhw;
	unsigned int n = pwmd->hwpwm;
	u32 wcfg;

	wcfg = readl(pwm->base + T32_PWM_WCFG(n));

	wfhw->enabled = readl(pwm->base + T32_PWM_EN) & BIT(n);
	wfhw->prescale = FIELD_GET(T32_PWM_CCFG_PRESCALE,
				   readl(pwm->base + T32_PWM_CCFG) >>
				   T32_PWM_CCFG_SHIFT(n));
	wfhw->init_high = readl(pwm->base + T32_PWM_LVL) & BIT(n);
	wfhw->high = FIELD_GET(T32_PWM_WCFG_HIGH, wcfg);
	wfhw->low = FIELD_GET(T32_PWM_WCFG_LOW, wcfg);

	return 0;
}

static int t32_pwm_write_waveform(struct pwm_chip *chip, struct pwm_device *pwmd,
				  const void *_wfhw)
{
	struct t32_pwm *pwm = t32_pwm_from_chip(chip);
	const struct t32_pwm_wfhw *wfhw = _wfhw;
	unsigned int n = pwmd->hwpwm;
	u32 val;

	if (!wfhw->enabled) {
		writel(BIT(n), pwm->base + T32_PWM_ENC);
		val = readl(pwm->base + T32_PWM_OEN);
		writel(val & ~BIT(n), pwm->base + T32_PWM_OEN);
		return 0;
	}

	val = readl(pwm->base + T32_PWM_CCFG);
	val &= ~(T32_PWM_CCFG_PRESCALE << T32_PWM_CCFG_SHIFT(n));
	val |= wfhw->prescale << T32_PWM_CCFG_SHIFT(n);
	writel(val, pwm->base + T32_PWM_CCFG);

	val = readl(pwm->base + T32_PWM_LVL);
	val &= ~BIT(n);
	if (wfhw->init_high)
		val |= BIT(n);
	writel(val, pwm->base + T32_PWM_LVL);

	writel(FIELD_PREP(T32_PWM_WCFG_HIGH, wfhw->high) |
	       FIELD_PREP(T32_PWM_WCFG_LOW, wfhw->low),
	       pwm->base + T32_PWM_WCFG(n));

	val = readl(pwm->base + T32_PWM_OEN);
	writel(val | BIT(n), pwm->base + T32_PWM_OEN);

	/*
	 * A channel disabled a moment ago may still be finishing its last
	 * period, which this restart would otherwise keep running.
	 */
	writel(BIT(n), pwm->base + T32_PWM_ENS);
	writel(BIT(n), pwm->base + T32_PWM_UPDATE);

	return 0;
}

static const struct pwm_ops t32_pwm_ops = {
	.sizeof_wfhw = sizeof(struct t32_pwm_wfhw),
	.round_waveform_tohw = t32_pwm_round_waveform_tohw,
	.round_waveform_fromhw = t32_pwm_round_waveform_fromhw,
	.read_waveform = t32_pwm_read_waveform,
	.write_waveform = t32_pwm_write_waveform,
};

static int t32_pwm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct pwm_chip *chip;
	struct t32_pwm *pwm;
	struct clk *clk;
	int ret;

	chip = devm_pwmchip_alloc(dev, (uintptr_t)device_get_match_data(dev),
				  sizeof(*pwm));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	pwm = t32_pwm_from_chip(chip);

	pwm->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pwm->base))
		return PTR_ERR(pwm->base);

	clk = devm_clk_get_enabled(dev, NULL);
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "cannot get the clock\n");

	ret = devm_clk_rate_exclusive_get(dev, clk);
	if (ret)
		return dev_err_probe(dev, ret, "cannot hold the clock rate\n");

	pwm->rate = clk_get_rate(clk);
	if (!pwm->rate || pwm->rate > NSEC_PER_SEC)
		return dev_err_probe(dev, -EINVAL, "bad clock rate %lu\n", pwm->rate);

	chip->ops = &t32_pwm_ops;

	return devm_pwmchip_add(dev, chip);
}

static const struct of_device_id t32_pwm_of_match[] = {
	{ .compatible = "ingenic,t32-pwm", .data = (void *)4 },
	{ .compatible = "ingenic,t41-pwm", .data = (void *)8 },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, t32_pwm_of_match);

static struct platform_driver t32_pwm_driver = {
	.driver = {
		.name = "pwm-ingenic-t32",
		.of_match_table = t32_pwm_of_match,
	},
	.probe = t32_pwm_probe,
};
module_platform_driver(t32_pwm_driver);

MODULE_AUTHOR("Alfonso Gamboa <gtxent@gmail.com>");
MODULE_DESCRIPTION("Ingenic T32, T33 and T41 PWM driver");
MODULE_LICENSE("GPL");
