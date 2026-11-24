// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic T21 and T30 SoC CGU driver
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>

#include <dt-bindings/clock/ingenic,t21-cgu.h>
#include <dt-bindings/clock/ingenic,t30-cgu.h>

#include "cgu.h"
#include "pm.h"

/* CGU register offsets */
#define CGU_REG_CPCCR		0x00
#define CGU_REG_CPPCR		0x0c
#define CGU_REG_APLL		0x10
#define CGU_REG_MPLL		0x14
#define CGU_REG_CLKGR0		0x20
#define CGU_REG_OPCR		0x24
#define CGU_REG_CLKGR1		0x28
#define CGU_REG_DDRCDR		0x2c
#define CGU_REG_VPUCDR		0x30
#define CGU_REG_USBPCR		0x3c
#define CGU_REG_USBRDT		0x40
#define CGU_REG_USBVBFIL	0x44
#define CGU_REG_USBPCR1		0x48
#define CGU_REG_EPLL		0x58	/* T30 only */
#define CGU_REG_MACCDR		0x54
#define CGU_REG_I2SCDR		0x60
#define CGU_REG_LPCDR		0x64
#define CGU_REG_MSC0CDR		0x68
#define CGU_REG_SSICDR		0x74
#define CGU_REG_CIMCDR		0x7c
#define CGU_REG_ISPCDR		0x80
#define CGU_REG_MSC1CDR		0xa4
#define CGU_REG_VPLL		0xe0
#define CGU_REG_MACPHYC		0xe8

/* Bits within the I2SCDR register */
#define I2SCDR_PCS_SHIFT	30
#define I2SCDR_PCS_MASK		(0x3 << I2SCDR_PCS_SHIFT)
#define I2SCDR_CE_BIT		29
#define I2SCDR_M_SHIFT		20
#define I2SCDR_M_MASK		(0x1ff << I2SCDR_M_SHIFT)
#define I2SCDR_N_SHIFT		0
#define I2SCDR_N_MASK		(0xfffff << I2SCDR_N_SHIFT)

/* bits within the OPCR register */
#define OPCR_GATE_USBPHYCLK	BIT(23)
#define OPCR_SPENDN0		BIT(7)

/* bits within the USBPCR register */
#define USBPCR_SIDDQ		BIT(21)
#define USBPCR_OTG_DISABLE	BIT(20)

static struct ingenic_cgu *cgu;

static int t21_usb_phy_enable(struct clk_hw *hw)
{
	return 0;
}

static void t21_usb_phy_disable(struct clk_hw *hw)
{
	void __iomem *reg_opcr	= cgu->base + CGU_REG_OPCR;
	void __iomem *reg_usbpcr = cgu->base + CGU_REG_USBPCR;

	writel((readl(reg_opcr) & ~OPCR_SPENDN0) | OPCR_GATE_USBPHYCLK,
	       reg_opcr);
	writel(readl(reg_usbpcr) | USBPCR_OTG_DISABLE | USBPCR_SIDDQ,
	       reg_usbpcr);
}

static int t21_usb_phy_is_enabled(struct clk_hw *hw)
{
	void __iomem *reg_opcr	= cgu->base + CGU_REG_OPCR;
	void __iomem *reg_usbpcr = cgu->base + CGU_REG_USBPCR;

	return (readl(reg_opcr) & OPCR_SPENDN0) &&
	       !(readl(reg_usbpcr) & USBPCR_SIDDQ) &&
	       !(readl(reg_usbpcr) & USBPCR_OTG_DISABLE);
}

static const struct clk_ops t21_otg_phy_ops = {
	.enable		= t21_usb_phy_enable,
	.disable	= t21_usb_phy_disable,
	.is_enabled	= t21_usb_phy_is_enabled,
};

/*
 * I2S CDR fractional divider.
 *
 * The T21 has one I2S clock for both directions, unlike the T31's pair. The
 * CDR register layout is:
 *   bits 31:30 - parent select (mux: SCLKA/MPLL/VPLL)
 *   bit  29    - CE (clock enable, exposed as a separate gate clock)
 *   bits 28:20 - M numerator (9 bits)
 *   bits 19:0  - N denominator (20 bits)
 *
 * Output rate = parent_rate * M / N. We brute-force search for the best
 * (M, N) pair when set_rate is called.
 */

#define I2S_M_MAX		((1 << 9) - 1)
#define I2S_N_MAX		((1 << 20) - 1)

static void t21_i2s_calc_mn(unsigned long parent_rate, unsigned long target,
			    unsigned int *out_m, unsigned int *out_n)
{
	unsigned long best_err = ULONG_MAX;
	unsigned int best_m = 1, best_n = 1;
	unsigned int m, n;

	if (!target || !parent_rate) {
		*out_m = 1;
		*out_n = 1;
		return;
	}

	/* try each M from large to small; for each, derive nearest N */
	for (m = I2S_M_MAX; m >= 1; m--) {
		u64 numer = (u64)parent_rate * m;
		u64 nq;
		unsigned long got, err;

		nq = div_u64(numer + target / 2, target);
		if (nq < 1 || nq > I2S_N_MAX)
			continue;

		n = (unsigned int)nq;
		got = div_u64((u64)parent_rate * m, n);
		err = (got > target) ? (got - target) : (target - got);

		if (err < best_err) {
			best_err = err;
			best_m = m;
			best_n = n;
			if (err == 0)
				break;
		}
	}

	*out_m = best_m;
	*out_n = best_n;
}

static unsigned long t21_i2s_div_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	u32 reg = readl(cgu->base + CGU_REG_I2SCDR);
	unsigned int m = (reg & I2SCDR_M_MASK) >> I2SCDR_M_SHIFT;
	unsigned int n = (reg & I2SCDR_N_MASK) >> I2SCDR_N_SHIFT;

	if (!n)
		return 0;
	return div_u64((u64)parent_rate * m, n);
}

static int t21_i2s_div_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	unsigned int m, n;

	t21_i2s_calc_mn(req->best_parent_rate, req->rate, &m, &n);
	req->rate = div_u64((u64)req->best_parent_rate * m, n);
	return 0;
}

static int t21_i2s_div_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	unsigned int m, n;
	u32 val;

	t21_i2s_calc_mn(parent_rate, rate, &m, &n);

	val = readl(cgu->base + CGU_REG_I2SCDR);
	val &= ~(I2SCDR_M_MASK | I2SCDR_N_MASK);
	val |= (m << I2SCDR_M_SHIFT) | (n << I2SCDR_N_SHIFT);
	writel(val, cgu->base + CGU_REG_I2SCDR);
	return 0;
}

static u8 t21_i2s_div_get_parent(struct clk_hw *hw)
{
	u32 reg = readl(cgu->base + CGU_REG_I2SCDR);

	return (reg & I2SCDR_PCS_MASK) >> I2SCDR_PCS_SHIFT;
}

static int t21_i2s_div_set_parent(struct clk_hw *hw, u8 index)
{
	u32 val = readl(cgu->base + CGU_REG_I2SCDR);

	val &= ~I2SCDR_PCS_MASK;
	val |= (u32)index << I2SCDR_PCS_SHIFT;
	writel(val, cgu->base + CGU_REG_I2SCDR);
	return 0;
}

static const struct clk_ops t21_i2s_div_ops = {
	.recalc_rate	= t21_i2s_div_recalc_rate,
	.determine_rate	= t21_i2s_div_determine_rate,
	.set_rate	= t21_i2s_div_set_rate,
	.get_parent	= t21_i2s_div_get_parent,
	.set_parent	= t21_i2s_div_set_parent,
};

/*
 * T21 PLL encoding.
 *
 * rate = EXCLK * 2 * (M + 1) / ((N + 1) * 2^OD), M in bits 28:20, N in bits
 * 19:14 and OD in bits 13:11 (vendor clk_pll.c). The model and the vendor
 * U-Boot also have a second divider in bits 10:7 that reads 0 (divide by 1)
 * as the boot loader leaves it; it is not touched here. The loop filter
 * range in bits 7:5 is left as the boot loader set it as well.
 *
 * The post divider is a power of two, so only the dividers 1, 2, 4, ... 64
 * have an encoding. The table is sparse: the entries in between are never
 * selected, as recalc_rate stops at the first match and an OD of 0 sits at
 * index 0.
 */
static const s8 t21_pll_od_encoding[64] = {
	[0] = 0, [1] = 1, [3] = 2, [7] = 3, [15] = 4, [31] = 5, [63] = 6,
};

#define T21_PLL_VCO_MIN		1500000000UL
#define T21_PLL_VCO_MAX		3000000000UL

/*
 * The VCO, EXCLK * 2 * (M + 1) / (N + 1), has to stay between 1.5 and 3 GHz.
 * Take the combination closest to the requested rate.
 */
static void t21_pll_calc_m_n_od(const struct ingenic_cgu_pll_info *pll_info,
				unsigned long rate, unsigned long parent_rate,
				unsigned int *pm, unsigned int *pn,
				unsigned int *pod)
{
	unsigned long best_err = ULONG_MAX;
	unsigned int best_m = 2, best_n = 1, best_od = 1;
	unsigned int n, od, m;

	for (od = 1; od <= 8; od <<= 1) {
		for (n = 1; n <= 4; n++) {
			unsigned long vco, got, err;

			/* m is M + 1 */
			m = DIV_ROUND_CLOSEST_ULL((u64)rate * od * n,
						  2 * parent_rate);
			if (m < 1 || m > 512)
				continue;

			vco = div_u64((u64)parent_rate * 2 * m, n);
			if (vco < T21_PLL_VCO_MIN || vco > T21_PLL_VCO_MAX)
				continue;

			got = div_u64((u64)vco, od);
			err = got > rate ? got - rate : rate - got;
			if (err < best_err) {
				best_err = err;
				best_m = m;
				best_n = n;
				best_od = od;
			}
		}
	}

	*pm = best_m;
	*pn = best_n;
	*pod = best_od;
}

#define T21_PLL(_reg)							\
	{								\
		.reg = _reg,						\
		.rate_multiplier = 2,					\
		.m_shift = 20,						\
		.m_bits = 9,						\
		.m_offset = 1,						\
		.n_shift = 14,						\
		.n_bits = 6,						\
		.n_offset = 1,						\
		.od_shift = 11,						\
		.od_bits = 3,						\
		.od_max = 64,						\
		.od_encoding = t21_pll_od_encoding,			\
		.bypass_reg = _reg,					\
		.bypass_bit = -1,					\
		.enable_bit = 0,					\
		.stable_bit = 3,					\
		.calc_m_n_od = t21_pll_calc_m_n_od,			\
	}

static const struct ingenic_cgu_clk_info t21_cgu_clocks[] = {

	/* External clocks */

	[T21_CLK_EXCLK] = { "ext", CGU_CLK_EXT },
	[T21_CLK_RTCLK] = { "rtc", CGU_CLK_EXT },

	/* PLLs */

	[T21_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.pll = T21_PLL(CGU_REG_APLL),
	},

	[T21_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.pll = T21_PLL(CGU_REG_MPLL),
	},

	[T21_CLK_VPLL] = {
		"vpll", CGU_CLK_PLL,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.pll = T21_PLL(CGU_REG_VPLL),
	},

	/* Custom (SoC-specific) OTG PHY */

	[T21_CLK_OTGPHY] = {
		"otg_phy", CGU_CLK_CUSTOM,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.custom = { &t21_otg_phy_ops },
	},

	/* Muxes & dividers */

	[T21_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T21_CLK_EXCLK, T21_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T21_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T21_CLK_SCLKA, T21_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T21_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T21_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},

	[T21_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T21_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T21_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T21_CLK_SCLKA, T21_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[T21_CLK_AHB2] = {
		"ahb2", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { -1, T21_CLK_SCLKA, T21_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	/* PDIV divides the AHB2 mux source, not AHB2 */
	[T21_CLK_PCLK] = {
		"pclk", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { -1, T21_CLK_SCLKA, T21_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T21_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T21_CLK_SCLKA, T21_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[T21_CLK_VPU] = {
		"vpu", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T21_CLK_SCLKA, T21_CLK_MPLL,
			     T21_CLK_VPLL, -1 },
		.mux = { CGU_REG_VPUCDR, 30, 2 },
		.div = { CGU_REG_VPUCDR, 0, 1, 4, 29, 28, 27 },
	},

	[T21_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T21_CLK_SCLKA, T21_CLK_MPLL,
			     T21_CLK_VPLL, -1 },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, 29, 28, 27 },
	},

	[T21_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T21_CLK_SCLKA, T21_CLK_MPLL,
			     T21_CLK_VPLL, -1 },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, 28, 27, 26 },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	[T21_CLK_MSCMUX] = {
		"msc_mux", CGU_CLK_MUX,
		.parents = { T21_CLK_SCLKA, T21_CLK_MPLL,
			     T21_CLK_VPLL, -1 },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
	},

	[T21_CLK_MSC0] = {
		"msc0", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T21_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC0CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T21_CLK_MSC1] = {
		"msc1", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T21_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC1CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T21_CLK_SSIPLL] = {
		"ssi_pll", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T21_CLK_SCLKA, T21_CLK_MPLL,
			     T21_CLK_VPLL, -1 },
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, 28, 27, 26 },
	},

	[T21_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T21_CLK_SCLKA, T21_CLK_MPLL,
			     T21_CLK_VPLL, -1 },
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		/* CE 29, BUSY 28 and STOP 27, as CIMCDR */
		.div = { CGU_REG_ISPCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T21_CLK_CIM] = {
		"cim", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T21_CLK_SCLKA, T21_CLK_MPLL,
			     T21_CLK_VPLL, -1 },
		.mux = { CGU_REG_CIMCDR, 30, 2 },
		/* CE 29, BUSY 28 and STOP 27; the STOP bit must be cleared */
		.div = { CGU_REG_CIMCDR, 0, 1, 8, 29, 28, 27 },
	},

	/* Gate-only clocks */

	[T21_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T21_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T21_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T21_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T21_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T21_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T21_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T21_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T21_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T21_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T21_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 8 },
	},

	[T21_CLK_AIC] = {
		"aic", CGU_CLK_GATE,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},

	[T21_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T21_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T21_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T21_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[T21_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 16 },
	},

	[T21_CLK_HASH] = {
		"hash", CGU_CLK_GATE,
		.parents = { T21_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 18 },
	},

	[T21_CLK_SSI1] = {
		"ssi1", CGU_CLK_GATE,
		.parents = { T21_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 19 },
	},

	[T21_CLK_SFC] = {
		"sfc", CGU_CLK_GATE,
		.parents = { T21_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T21_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T21_CLK_RISCV] = {
		"riscv", CGU_CLK_GATE,
		.parents = { T21_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 26 },
	},

	[T21_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T21_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T21_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T21_CLK_HELIX] = {
		"helix", CGU_CLK_GATE,
		.parents = { T21_CLK_VPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	[T21_CLK_DTRNG] = {
		"dtrng", CGU_CLK_GATE,
		.parents = { T21_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 1 },
	},

	[T21_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T21_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T21_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T21_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T21_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T21_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T21_CLK_AHB1] = {
		"ahb1", CGU_CLK_GATE,
		.parents = { T21_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 6 },
	},

	[T21_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T21_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T21_CLK_EXCLK_DIV512] = {
		"exclk_div512", CGU_CLK_FIXDIV,
		.parents = { T21_CLK_EXCLK },
		.fixdiv = { 512 },
	},

	[T21_CLK_RTC] = {
		"rtc_ercs", CGU_CLK_MUX | CGU_CLK_GATE,
		.parents = { T21_CLK_EXCLK_DIV512, T21_CLK_RTCLK, -1, -1 },
		.mux = { CGU_REG_OPCR, 2, 1 },
		.gate = { CGU_REG_CLKGR0, 29 },
	},

	[T21_CLK_USBPHY] = {
		"usb_phy", CGU_CLK_GATE,
		.parents = { T21_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_OPCR, 23, .clear_to_gate = true },
	},

	/* I2S fractional divider (custom: M/N ratio) */

	[T21_CLK_DIV_I2S] = {
		"div_i2s", CGU_CLK_CUSTOM,
		.parents = { T21_CLK_SCLKA, T21_CLK_MPLL, T21_CLK_VPLL, -1 },
		.custom = { &t21_i2s_div_ops },
	},

	/* CE (clock-enable) gate inside the CDR register, bit 29 */

	[T21_CLK_CE_I2S] = {
		"ce_i2s", CGU_CLK_GATE,
		.parents = { T21_CLK_DIV_I2S, -1, -1, -1 },
		/* CE bit 29 is "set to enable", opposite of CLKGR semantics */
		.gate = { CGU_REG_I2SCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},
};

/*
 * T30: the T21's CPCCR, CDR and gate layout and PLL encoding (the vendor
 * clk_cpccr.c, clk_cgu.c and clk_pll.c are the same files), plus an EPLL
 * as fourth source of the CDR muxes, and a different set of gates.
 */
static const struct ingenic_cgu_clk_info t30_cgu_clocks[] = {
	/* External clocks */

	[T30_CLK_EXCLK] = { "ext", CGU_CLK_EXT },
	[T30_CLK_RTCLK] = { "rtc", CGU_CLK_EXT },

	/* PLLs */

	[T30_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.pll = T21_PLL(CGU_REG_APLL),
	},

	[T30_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.pll = T21_PLL(CGU_REG_MPLL),
	},

	[T30_CLK_VPLL] = {
		"vpll", CGU_CLK_PLL,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.pll = T21_PLL(CGU_REG_VPLL),
	},

	[T30_CLK_EPLL] = {
		"epll", CGU_CLK_PLL,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.pll = T21_PLL(CGU_REG_EPLL),
	},

	/* Custom (SoC-specific) OTG PHY */

	[T30_CLK_OTGPHY] = {
		"otg_phy", CGU_CLK_CUSTOM,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.custom = { &t21_otg_phy_ops },
	},

	/* Muxes & dividers */

	[T30_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T30_CLK_EXCLK, T30_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T30_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T30_CLK_SCLKA, T30_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T30_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T30_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},

	[T30_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T30_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T30_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T30_CLK_SCLKA, T30_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[T30_CLK_AHB2] = {
		"ahb2", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { -1, T30_CLK_SCLKA, T30_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	/* PDIV divides the AHB2 mux source, not AHB2 */
	[T30_CLK_PCLK] = {
		"pclk", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { -1, T30_CLK_SCLKA, T30_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T30_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T30_CLK_SCLKA, T30_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[T30_CLK_VPU] = {
		"vpu", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T30_CLK_SCLKA, T30_CLK_MPLL,
			     T30_CLK_VPLL, T30_CLK_EPLL },
		.mux = { CGU_REG_VPUCDR, 30, 2 },
		.div = { CGU_REG_VPUCDR, 0, 1, 4, 29, 28, 27 },
	},

	[T30_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T30_CLK_SCLKA, T30_CLK_MPLL,
			     T30_CLK_VPLL, T30_CLK_EPLL },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, 29, 28, 27 },
	},

	[T30_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T30_CLK_SCLKA, T30_CLK_MPLL,
			     T30_CLK_VPLL, T30_CLK_EPLL },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, 28, 27, 26 },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	[T30_CLK_MSCMUX] = {
		"msc_mux", CGU_CLK_MUX,
		.parents = { T30_CLK_SCLKA, T30_CLK_MPLL,
			     T30_CLK_VPLL, -1 },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
	},

	[T30_CLK_MSC0] = {
		"msc0", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T30_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC0CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T30_CLK_MSC1] = {
		"msc1", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T30_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC1CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T30_CLK_SSIPLL] = {
		"ssi_pll", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T30_CLK_SCLKA, T30_CLK_MPLL,
			     T30_CLK_VPLL, T30_CLK_EPLL },
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, 28, 27, 26 },
	},

	[T30_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T30_CLK_SCLKA, T30_CLK_MPLL,
			     T30_CLK_VPLL, T30_CLK_EPLL },
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		/* CE 29, BUSY 28 and STOP 27, as CIMCDR */
		.div = { CGU_REG_ISPCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T30_CLK_CIM] = {
		"cim", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T30_CLK_SCLKA, T30_CLK_MPLL,
			     T30_CLK_VPLL, T30_CLK_EPLL },
		.mux = { CGU_REG_CIMCDR, 30, 2 },
		/* CE 29, BUSY 28 and STOP 27; the STOP bit must be cleared */
		.div = { CGU_REG_CIMCDR, 0, 1, 8, 29, 28, 27 },
	},

	/* Gate-only clocks */

	[T30_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T30_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T30_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T30_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T30_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T30_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T30_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T30_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T30_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T30_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T30_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 8 },
	},

	[T30_CLK_SMB2] = {
		"smb2", CGU_CLK_GATE,
		.parents = { T30_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 9 },
	},

	[T30_CLK_AIC] = {
		"aic", CGU_CLK_GATE,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},

	[T30_CLK_DMIC] = {
		"dmic", CGU_CLK_GATE,
		.parents = { T30_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 12 },
	},

	[T30_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T30_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T30_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T30_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[T30_CLK_SSI1] = {
		"ssi1", CGU_CLK_GATE,
		.parents = { T30_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 19 },
	},

	[T30_CLK_SFC] = {
		"sfc", CGU_CLK_GATE,
		.parents = { T30_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T30_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T30_CLK_CSI] = {
		"csi", CGU_CLK_GATE,
		.parents = { T30_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 25 },
	},

	[T30_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T30_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T30_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T30_CLK_HELIX] = {
		"helix", CGU_CLK_GATE,
		.parents = { T30_CLK_VPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	[T30_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T30_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T30_CLK_NCU] = {
		"ncu", CGU_CLK_GATE,
		.parents = { T30_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 3 },
	},

	[T30_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T30_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T30_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T30_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T30_CLK_AHB1] = {
		"ahb1", CGU_CLK_GATE,
		.parents = { T30_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 6 },
	},

	[T30_CLK_MSCALER] = {
		"mscaler", CGU_CLK_GATE,
		.parents = { T30_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 8 },
	},

	[T30_CLK_LDC] = {
		"ldc", CGU_CLK_GATE,
		.parents = { T30_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 9 },
	},

	[T30_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T30_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T30_CLK_RADIX] = {
		"radix", CGU_CLK_GATE,
		.parents = { T30_CLK_VPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 12 },
	},

	[T30_CLK_EXCLK_DIV512] = {
		"exclk_div512", CGU_CLK_FIXDIV,
		.parents = { T30_CLK_EXCLK },
		.fixdiv = { 512 },
	},

	[T30_CLK_RTC] = {
		"rtc_ercs", CGU_CLK_MUX | CGU_CLK_GATE,
		.parents = { T30_CLK_EXCLK_DIV512, T30_CLK_RTCLK, -1, -1 },
		.mux = { CGU_REG_OPCR, 2, 1 },
		.gate = { CGU_REG_CLKGR0, 29 },
	},

	[T30_CLK_USBPHY] = {
		"usb_phy", CGU_CLK_GATE,
		.parents = { T30_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_OPCR, 23, .clear_to_gate = true },
	},

	/* I2S fractional divider (custom: M/N ratio) */

	[T30_CLK_DIV_I2S] = {
		"div_i2s", CGU_CLK_CUSTOM,
		.parents = { T30_CLK_SCLKA, T30_CLK_MPLL,
		     T30_CLK_VPLL, T30_CLK_EPLL },
		.custom = { &t21_i2s_div_ops },
	},

	/* CE (clock-enable) gate inside the CDR register, bit 29 */

	[T30_CLK_CE_I2S] = {
		"ce_i2s", CGU_CLK_GATE,
		.parents = { T30_CLK_DIV_I2S, -1, -1, -1 },
		/* CE bit 29 is "set to enable", opposite of CLKGR semantics */
		.gate = { CGU_REG_I2SCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

};

static void __init t21_cgu_setup(struct device_node *np,
				 const struct ingenic_cgu_clk_info *clocks,
				 unsigned int num_clocks)
{
	int retval;

	cgu = ingenic_cgu_new(clocks, num_clocks, np);
	if (!cgu) {
		pr_err("%s: failed to initialise CGU\n", __func__);
		return;
	}

	/* Divider outputs (CIM MCLK at least) stay off while CE is set */
	cgu->clear_ce = true;

	retval = ingenic_cgu_register_clocks(cgu);
	if (retval) {
		pr_err("%s: failed to register CGU clocks\n", __func__);
		return;
	}

	ingenic_cgu_register_syscore(cgu);
}

static void __init t21_cgu_init(struct device_node *np)
{
	t21_cgu_setup(np, t21_cgu_clocks, ARRAY_SIZE(t21_cgu_clocks));
}

static void __init t30_cgu_init(struct device_node *np)
{
	t21_cgu_setup(np, t30_cgu_clocks, ARRAY_SIZE(t30_cgu_clocks));
}

/*
 * CGU has some children devices, this is useful for probing children devices
 * in the case where the device node is compatible with "simple-mfd".
 */
CLK_OF_DECLARE_DRIVER(t21_cgu, "ingenic,t21-cgu", t21_cgu_init);
CLK_OF_DECLARE_DRIVER(t30_cgu, "ingenic,t30-cgu", t30_cgu_init);
