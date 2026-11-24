// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic T31, T23, T32 and T33 SoC CGU driver
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>

#include <dt-bindings/clock/ingenic,t31-cgu.h>
#include <dt-bindings/clock/ingenic,t23-cgu.h>
#include <dt-bindings/clock/ingenic,t32-cgu.h>

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
#define CGU_REG_EL150CDR	0x30
#define CGU_REG_USBPCR		0x3c
#define CGU_REG_USBRDT		0x40
#define CGU_REG_USBVBFIL	0x44
#define CGU_REG_USBPCR1		0x48
#define CGU_REG_RSACDR		0x4c
#define CGU_REG_MACCDR		0x54
#define CGU_REG_LPCDR		0x64
#define CGU_REG_MSC0CDR		0x68
#define CGU_REG_SSICDR		0x74
#define CGU_REG_T23_SSICDR	0x60
#define CGU_REG_SFCCDR		0x74
#define CGU_REG_VPUCDR		0x30
#define CGU_REG_CIMCDR		0x7c
#define CGU_REG_ISPCDR		0x80
#define CGU_REG_MSC1CDR		0xa4
#define CGU_REG_I2STCDR		0x70
#define CGU_REG_I2SRCDR		0x84
#define CGU_REG_VPLL		0xe0
#define CGU_REG_T32_SFC0CDR	0x58
#define CGU_REG_T32_PWMCDR	0x5c
#define CGU_REG_T32_SSISLVCDR	0x60
#define CGU_REG_T32_LDCCDR	0x6c
#define CGU_REG_T32_ISPSCDR	0x90
#define CGU_REG_T32_ISPACDR	0x94
#define CGU_REG_T32_TNPUCDR	0x98
#define CGU_REG_T32_SFC1CDR	0xac
#define CGU_REG_MACPHYC		0xe8

/* Bits within the I2S{T,R}CDR registers */
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

static int t31_usb_phy_enable(struct clk_hw *hw)
{
	return 0;
}

static void t31_usb_phy_disable(struct clk_hw *hw)
{
	void __iomem *reg_opcr	= cgu->base + CGU_REG_OPCR;
	void __iomem *reg_usbpcr = cgu->base + CGU_REG_USBPCR;

	writel((readl(reg_opcr) & ~OPCR_SPENDN0) | OPCR_GATE_USBPHYCLK,
	       reg_opcr);
	writel(readl(reg_usbpcr) | USBPCR_OTG_DISABLE | USBPCR_SIDDQ,
	       reg_usbpcr);
}

static int t31_usb_phy_is_enabled(struct clk_hw *hw)
{
	void __iomem *reg_opcr	= cgu->base + CGU_REG_OPCR;
	void __iomem *reg_usbpcr = cgu->base + CGU_REG_USBPCR;

	return (readl(reg_opcr) & OPCR_SPENDN0) &&
	       !(readl(reg_usbpcr) & USBPCR_SIDDQ) &&
	       !(readl(reg_usbpcr) & USBPCR_OTG_DISABLE);
}

static const struct clk_ops t31_otg_phy_ops = {
	.enable		= t31_usb_phy_enable,
	.disable	= t31_usb_phy_disable,
	.is_enabled	= t31_usb_phy_is_enabled,
};

/*
 * I2S CDR fractional divider.
 *
 * The CDR register layout is:
 *   bits 31:30 - parent select (mux: SCLKA/MPLL/VPLL)
 *   bit  29    - CE (clock enable, exposed as a separate gate clock)
 *   bits 28:20 - M numerator (9 bits)
 *   bits 19:0  - N denominator (20 bits)
 *
 * Output rate = parent_rate * M / N. We brute-force search for the best
 * (M, N) pair when set_rate is called.
 *
 * The i2s_parents indices below match each entry's `parents` table in the
 * t31_cgu_clocks[] array.
 */

#define I2S_M_MAX		((1 << 9) - 1)
#define I2S_N_MAX		((1 << 20) - 1)

struct t31_i2s_div {
	u32 reg;
	u8 num_parents;
	u8 parent_shift;
	u8 parent_bits;
};

static const struct t31_i2s_div t31_i2s_div_t = {
	.reg = CGU_REG_I2STCDR, .num_parents = 3,
	.parent_shift = I2SCDR_PCS_SHIFT, .parent_bits = 2,
};

static const struct t31_i2s_div t31_i2s_div_r = {
	.reg = CGU_REG_I2SRCDR, .num_parents = 3,
	.parent_shift = I2SCDR_PCS_SHIFT, .parent_bits = 2,
};

static void t31_i2s_calc_mn(unsigned long parent_rate, unsigned long target,
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

static unsigned long t31_i2s_div_recalc_rate(struct clk_hw *hw,
					     unsigned long parent_rate)
{
	const struct t31_i2s_div *info;
	u32 reg;
	unsigned int m, n;
	const char *name = clk_hw_get_name(hw);

	info = strstr(name, "div_i2sr") ? &t31_i2s_div_r : &t31_i2s_div_t;
	reg = readl(cgu->base + info->reg);
	m = (reg & I2SCDR_M_MASK) >> I2SCDR_M_SHIFT;
	n = (reg & I2SCDR_N_MASK) >> I2SCDR_N_SHIFT;
	if (!n)
		return 0;
	return div_u64((u64)parent_rate * m, n);
}

static int t31_i2s_div_determine_rate(struct clk_hw *hw,
				      struct clk_rate_request *req)
{
	unsigned int m, n;

	t31_i2s_calc_mn(req->best_parent_rate, req->rate, &m, &n);
	req->rate = div_u64((u64)req->best_parent_rate * m, n);
	return 0;
}

static int t31_i2s_div_set_rate(struct clk_hw *hw, unsigned long rate,
				unsigned long parent_rate)
{
	const struct t31_i2s_div *info;
	const char *name = clk_hw_get_name(hw);
	unsigned int m, n;
	u32 val;

	info = strstr(name, "div_i2sr") ? &t31_i2s_div_r : &t31_i2s_div_t;
	t31_i2s_calc_mn(parent_rate, rate, &m, &n);

	pr_info("t31-cgu: %s set_rate target=%lu parent=%lu -> M=%u N=%u rate=%llu\n",
	         name, rate, parent_rate, m, n,
	         div_u64((u64)parent_rate * m, n));

	val = readl(cgu->base + info->reg);
	val &= ~(I2SCDR_M_MASK | I2SCDR_N_MASK);
	val |= (m << I2SCDR_M_SHIFT) | (n << I2SCDR_N_SHIFT);
	writel(val, cgu->base + info->reg);
	return 0;
}

static u8 t31_i2s_div_get_parent(struct clk_hw *hw)
{
	const struct t31_i2s_div *info;
	const char *name = clk_hw_get_name(hw);
	u32 reg;

	info = strstr(name, "div_i2sr") ? &t31_i2s_div_r : &t31_i2s_div_t;
	reg = readl(cgu->base + info->reg);
	return (reg & I2SCDR_PCS_MASK) >> I2SCDR_PCS_SHIFT;
}

static int t31_i2s_div_set_parent(struct clk_hw *hw, u8 index)
{
	const struct t31_i2s_div *info;
	const char *name = clk_hw_get_name(hw);
	u32 val;

	info = strstr(name, "div_i2sr") ? &t31_i2s_div_r : &t31_i2s_div_t;
	val = readl(cgu->base + info->reg);
	val &= ~I2SCDR_PCS_MASK;
	val |= (u32)index << I2SCDR_PCS_SHIFT;
	writel(val, cgu->base + info->reg);
	return 0;
}

static const struct clk_ops t31_i2s_div_ops = {
	.recalc_rate	= t31_i2s_div_recalc_rate,
	.determine_rate	= t31_i2s_div_determine_rate,
	.set_rate	= t31_i2s_div_set_rate,
	.get_parent	= t31_i2s_div_get_parent,
	.set_parent	= t31_i2s_div_set_parent,
};

/*
 * T31 PLL OD encoding.
 *
 * T31 PLLs have two output divider fields: OD1 (bits 13:11) and OD0
 * (bits 10:8). The effective output divider is OD1 * OD0. In practice,
 * the bootloader always sets OD0 = 1, so we model only OD1 here.
 *
 * The OD register value maps directly to the divider value: reg=N
 * means divider=N (for N >= 1). The CGU framework looks up the
 * register value in this table to find an index, then adds 1 to get
 * the effective divider. So encoding[i] = i+1 yields divider = i+1
 * for register value i+1.
 */
static const s8 t31_pll_od_encoding[7] = {
	0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7,
};

static const struct ingenic_cgu_clk_info t31_cgu_clocks[] = {

	/* External clocks */

	[T31_CLK_EXCLK] = { "ext", CGU_CLK_EXT },
	[T31_CLK_RTCLK] = { "rtc", CGU_CLK_EXT },

	/* PLLs */

	[T31_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.pll = {
			.reg = CGU_REG_APLL,
			.rate_multiplier = 1,
			.m_shift = 20,
			.m_bits = 12,
			.m_offset = 0,
			.n_shift = 14,
			.n_bits = 6,
			.n_offset = 0,
			.od_shift = 11,
			.od_bits = 3,
			.od_max = 7,
			.od_encoding = t31_pll_od_encoding,
			.bypass_reg = CGU_REG_APLL,
			.bypass_bit = -1,
			.enable_bit = 0,
			.stable_bit = 3,
		},
	},

	[T31_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.pll = {
			.reg = CGU_REG_MPLL,
			.rate_multiplier = 1,
			.m_shift = 20,
			.m_bits = 12,
			.m_offset = 0,
			.n_shift = 14,
			.n_bits = 6,
			.n_offset = 0,
			.od_shift = 11,
			.od_bits = 3,
			.od_max = 7,
			.od_encoding = t31_pll_od_encoding,
			.bypass_reg = CGU_REG_MPLL,
			.bypass_bit = -1,
			.enable_bit = 0,
			.stable_bit = 3,
		},
	},

	[T31_CLK_VPLL] = {
		"vpll", CGU_CLK_PLL,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.pll = {
			.reg = CGU_REG_VPLL,
			.rate_multiplier = 1,
			.m_shift = 20,
			.m_bits = 12,
			.m_offset = 0,
			.n_shift = 14,
			.n_bits = 6,
			.n_offset = 0,
			.od_shift = 11,
			.od_bits = 3,
			.od_max = 7,
			.od_encoding = t31_pll_od_encoding,
			.bypass_reg = CGU_REG_VPLL,
			.bypass_bit = -1,
			.enable_bit = 0,
			.stable_bit = 3,
		},
	},

	/* Custom (SoC-specific) OTG PHY */

	[T31_CLK_OTGPHY] = {
		"otg_phy", CGU_CLK_CUSTOM,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.custom = { &t31_otg_phy_ops },
	},

	/* Muxes & dividers */

	[T31_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T31_CLK_EXCLK, T31_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T31_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T31_CLK_SCLKA, T31_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T31_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T31_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},

	[T31_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T31_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T31_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T31_CLK_SCLKA, T31_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[T31_CLK_AHB2] = {
		"ahb2", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { -1, T31_CLK_SCLKA, T31_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	/* PDIV divides the AHB2 mux source, not AHB2 */
	[T31_CLK_PCLK] = {
		"pclk", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { -1, T31_CLK_SCLKA, T31_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T31_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T31_CLK_SCLKA, T31_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[T31_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL,
			     T31_CLK_VPLL, -1 },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, 29, 28, 27 },
	},

	[T31_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL,
			     T31_CLK_VPLL, -1 },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, 28, 27, 26 },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	[T31_CLK_MSCMUX] = {
		"msc_mux", CGU_CLK_MUX,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL,
			     T31_CLK_VPLL, -1 },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
	},

	[T31_CLK_MSC0] = {
		"msc0", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T31_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC0CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T31_CLK_MSC1] = {
		"msc1", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T31_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC1CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T31_CLK_SSIPLL] = {
		"ssi_pll", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL,
			     T31_CLK_VPLL, -1 },
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, 28, 27, 26 },
	},

	[T31_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL,
			     T31_CLK_VPLL, -1 },
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		/* ISPCDR uses the same control bit layout as CIMCDR:
		 * bit 29=CE, bit 28=BUSY, bit 27=STOP.  Field order is
		 * ce_bit, busy_bit, stop_bit. */
		.div = { CGU_REG_ISPCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T31_CLK_CIM] = {
		"cim", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL,
			     T31_CLK_VPLL, -1 },
		.mux = { CGU_REG_CIMCDR, 30, 2 },
		/* CIMCDR control bits per BSP DIV() macro:
		 *   bit 29 = CE (write 1 to apply new divider)
		 *   bit 28 = BUSY (read-only, set while divider updates)
		 *   bit 27 = STOP (1 = clock gated off internally)
		 * Without these the framework never clears STOP, so the
		 * clock stays off in hardware even when enable_count > 0.
		 * Mainline cgu_div_info field order is ce_bit, busy_bit,
		 * stop_bit. */
		.div = { CGU_REG_CIMCDR, 0, 1, 8, 29, 28, 27 },
	},

	[T31_CLK_RSA] = {
		"rsa", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL,
			     T31_CLK_VPLL, -1 },
		.mux = { CGU_REG_RSACDR, 30, 2 },
		.div = { CGU_REG_RSACDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 27 },
	},

	[T31_CLK_EL150] = {
		"el150", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL,
			     T31_CLK_VPLL, -1 },
		.mux = { CGU_REG_EL150CDR, 30, 2 },
		.div = { CGU_REG_EL150CDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	/* Gate-only clocks */

	[T31_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T31_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T31_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T31_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T31_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T31_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T31_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T31_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T31_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T31_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T31_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 8 },
	},

	[T31_CLK_AIC] = {
		"aic", CGU_CLK_GATE,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},

	[T31_CLK_DMIC] = {
		"dmic", CGU_CLK_GATE,
		.parents = { T31_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 12 },
	},

	[T31_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T31_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T31_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T31_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[T31_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 16 },
	},

	[T31_CLK_SLV] = {
		"slv", CGU_CLK_GATE,
		.parents = { T31_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 17 },
	},

	[T31_CLK_HASH] = {
		"hash", CGU_CLK_GATE,
		.parents = { T31_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 18 },
	},

	[T31_CLK_SSI1] = {
		"ssi1", CGU_CLK_GATE,
		.parents = { T31_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 19 },
	},

	[T31_CLK_SFC] = {
		"sfc", CGU_CLK_GATE,
		.parents = { T31_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T31_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T31_CLK_MIPI_CSI] = {
		"mipi_csi", CGU_CLK_GATE,
		.parents = { T31_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 25 },
	},

	[T31_CLK_RISCV] = {
		"riscv", CGU_CLK_GATE,
		.parents = { T31_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 26 },
	},

	[T31_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T31_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T31_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T31_CLK_DTRNG] = {
		"dtrng", CGU_CLK_GATE,
		.parents = { T31_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 1 },
	},

	[T31_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T31_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T31_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T31_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T31_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T31_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T31_CLK_AHB1] = {
		"ahb1", CGU_CLK_GATE,
		.parents = { T31_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 6 },
	},

	[T31_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T31_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T31_CLK_EXCLK_DIV512] = {
		"exclk_div512", CGU_CLK_FIXDIV,
		.parents = { T31_CLK_EXCLK },
		.fixdiv = { 512 },
	},

	[T31_CLK_RTC] = {
		"rtc_ercs", CGU_CLK_MUX | CGU_CLK_GATE,
		.parents = { T31_CLK_EXCLK_DIV512, T31_CLK_RTCLK, -1, -1 },
		.mux = { CGU_REG_OPCR, 2, 1 },
		.gate = { CGU_REG_CLKGR0, 29 },
	},

	[T31_CLK_USBPHY] = {
		"usb_phy", CGU_CLK_GATE,
		.parents = { T31_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_OPCR, 23, .clear_to_gate = true },
	},

	/* I2S TX/RX fractional dividers (custom: M/N ratio) */

	[T31_CLK_DIV_I2ST] = {
		"div_i2st", CGU_CLK_CUSTOM,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL, T31_CLK_VPLL, -1 },
		.custom = { &t31_i2s_div_ops },
	},

	[T31_CLK_DIV_I2SR] = {
		"div_i2sr", CGU_CLK_CUSTOM,
		.parents = { T31_CLK_SCLKA, T31_CLK_MPLL, T31_CLK_VPLL, -1 },
		.custom = { &t31_i2s_div_ops },
	},

	/* CE (clock-enable) gates inside the CDR registers, bit 29 */

	[T31_CLK_CE_I2ST] = {
		"ce_i2st", CGU_CLK_GATE,
		.parents = { T31_CLK_DIV_I2ST, -1, -1, -1 },
		/* CE bit 29 is "set to enable", opposite of CLKGR semantics */
		.gate = { CGU_REG_I2STCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

	[T31_CLK_CE_I2SR] = {
		"ce_i2sr", CGU_CLK_GATE,
		.parents = { T31_CLK_DIV_I2SR, -1, -1, -1 },
		.gate = { CGU_REG_I2SRCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},
};

/*
 * T23: the T31's CPCCR, PLL and CDR layouts (vendor clk-t23.c). Against the
 * T31 it has no VPLL, so the CDR muxes pick SCLK_A or MPLL only. The SSI
 * (CDR at 0x60) and the SFC (0x74) have their own dividers, the VPU/IVDC
 * clock has a CDR at 0x30 where the T31 has the EL150's, and it has a third
 * I2C but no DMIC, SLV, SSI1 or AHB1 gate. SSI and SFC dividers keep the
 * T31's SSICDR control bits (28/27/26), as the vendor's DIV_SPECIAL does.
 */
static const struct ingenic_cgu_clk_info t23_cgu_clocks[] = {

	/* External clocks */

	[T23_CLK_EXCLK] = { "ext", CGU_CLK_EXT },
	[T23_CLK_RTCLK] = { "rtc", CGU_CLK_EXT },

	/* PLLs */

	[T23_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.pll = {
			.reg = CGU_REG_APLL,
			.rate_multiplier = 1,
			.m_shift = 20,
			.m_bits = 12,
			.m_offset = 0,
			.n_shift = 14,
			.n_bits = 6,
			.n_offset = 0,
			.od_shift = 11,
			.od_bits = 3,
			.od_max = 7,
			.od_encoding = t31_pll_od_encoding,
			.bypass_reg = CGU_REG_APLL,
			.bypass_bit = -1,
			.enable_bit = 0,
			.stable_bit = 3,
		},
	},

	[T23_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.pll = {
			.reg = CGU_REG_MPLL,
			.rate_multiplier = 1,
			.m_shift = 20,
			.m_bits = 12,
			.m_offset = 0,
			.n_shift = 14,
			.n_bits = 6,
			.n_offset = 0,
			.od_shift = 11,
			.od_bits = 3,
			.od_max = 7,
			.od_encoding = t31_pll_od_encoding,
			.bypass_reg = CGU_REG_MPLL,
			.bypass_bit = -1,
			.enable_bit = 0,
			.stable_bit = 3,
		},
	},

	/* Custom (SoC-specific) OTG PHY */

	[T23_CLK_OTGPHY] = {
		"otg_phy", CGU_CLK_CUSTOM,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.custom = { &t31_otg_phy_ops },
	},

	/* Muxes & dividers */

	[T23_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T23_CLK_EXCLK, T23_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T23_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T23_CLK_SCLKA, T23_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T23_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T23_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},

	[T23_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T23_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T23_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T23_CLK_SCLKA, T23_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[T23_CLK_AHB2] = {
		"ahb2", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { -1, T23_CLK_SCLKA, T23_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	/* PDIV divides the AHB2 mux source, not AHB2 */
	[T23_CLK_PCLK] = {
		"pclk", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { -1, T23_CLK_SCLKA, T23_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T23_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T23_CLK_SCLKA, T23_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	/* The VPU's and the IVDC's clock; each has its own gate below */
	[T23_CLK_VPU] = {
		"vpu", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_VPUCDR, 30, 2 },
		.div = { CGU_REG_VPUCDR, 0, 1, 4, 29, 28, 27 },
	},

	[T23_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, 29, 28, 27 },
	},

	[T23_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, 28, 27, 26 },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	/* Vendor: MSC1's mux register is MSC0CDR's too */
	[T23_CLK_MSCMUX] = {
		"msc_mux", CGU_CLK_MUX,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
	},

	[T23_CLK_MSC0] = {
		"msc0", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T23_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC0CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T23_CLK_MSC1] = {
		"msc1", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T23_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC1CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T23_CLK_SSI] = {
		"ssi_div", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_T23_SSICDR, 30, 2 },
		.div = { CGU_REG_T23_SSICDR, 0, 1, 8, 28, 27, 26 },
	},

	[T23_CLK_SFCDIV] = {
		"sfc_div", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_SFCCDR, 30, 2 },
		.div = { CGU_REG_SFCCDR, 0, 1, 8, 28, 27, 26 },
	},

	[T23_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		.div = { CGU_REG_ISPCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T23_CLK_CIM] = {
		"cim", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_CIMCDR, 30, 2 },
		.div = { CGU_REG_CIMCDR, 0, 1, 8, 29, 28, 27 },
	},

	[T23_CLK_RSA] = {
		"rsa", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_RSACDR, 30, 2 },
		.div = { CGU_REG_RSACDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 27 },
	},

	/* Gate-only clocks */

	[T23_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T23_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T23_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T23_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T23_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T23_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T23_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T23_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T23_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 8 },
	},

	[T23_CLK_SMB2] = {
		"smb2", CGU_CLK_GATE,
		.parents = { T23_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 9 },
	},

	[T23_CLK_AIC] = {
		"aic", CGU_CLK_GATE,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},

	[T23_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T23_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T23_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T23_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[T23_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 16 },
	},

	[T23_CLK_HASH] = {
		"hash", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 18 },
	},

	[T23_CLK_SFC] = {
		"sfc", CGU_CLK_GATE,
		.parents = { T23_CLK_SFCDIV, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T23_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T23_CLK_MIPI_CSI] = {
		"mipi_csi", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 25 },
	},

	[T23_CLK_RISCV] = {
		"riscv", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 26 },
	},

	[T23_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T23_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T23_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T23_CLK_VPUGATE] = {
		"vpu_gate", CGU_CLK_GATE,
		.parents = { T23_CLK_VPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	[T23_CLK_DTRNG] = {
		"dtrng", CGU_CLK_GATE,
		.parents = { T23_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 1 },
	},

	[T23_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T23_CLK_IVDC] = {
		"ivdc", CGU_CLK_GATE,
		.parents = { T23_CLK_VPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 3 },
	},

	[T23_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T23_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T23_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T23_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T23_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T23_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T23_CLK_USBPHY] = {
		"usb_phy", CGU_CLK_GATE,
		.parents = { T23_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_OPCR, 23, .clear_to_gate = true },
	},

	/* I2S TX/RX fractional dividers (custom: M/N ratio) */

	[T23_CLK_DIV_I2ST] = {
		"div_i2st", CGU_CLK_CUSTOM,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.custom = { &t31_i2s_div_ops },
	},

	[T23_CLK_DIV_I2SR] = {
		"div_i2sr", CGU_CLK_CUSTOM,
		.parents = { T23_CLK_SCLKA, T23_CLK_MPLL, -1, -1 },
		.custom = { &t31_i2s_div_ops },
	},

	/* CE (clock-enable) gates inside the CDR registers, bit 29 */

	[T23_CLK_CE_I2ST] = {
		"ce_i2st", CGU_CLK_GATE,
		.parents = { T23_CLK_DIV_I2ST, -1, -1, -1 },
		.gate = { CGU_REG_I2STCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

	[T23_CLK_CE_I2SR] = {
		"ce_i2sr", CGU_CLK_GATE,
		.parents = { T23_CLK_DIV_I2SR, -1, -1, -1 },
		.gate = { CGU_REG_I2SRCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},
};

/*
 * T32 and T33: the T31's CPCCR and PLL layouts (vendor clk-PRJ007.c and
 * clk-PRJ008.c), but each peripheral has a CDR of its own (mux in bits
 * 31:30, CE 29, BUSY 28, STOP 27) and the CLKGR bits are all reassigned.
 * The SFC dividers have a power-of-two predivider in bits 9:8 below the
 * 8-bit divider; the MSC dividers count in fours and can also take EXCLK
 * (mux index 3). The T33 has no VPLL, so its other CDR muxes pick SCLK_A
 * or MPLL only.
 */
#define T32_CDR_CE		29
#define T32_CDR_BUSY		28
#define T32_CDR_STOP		27
#define T32_MSC0CDR_EXCLK_EN	BIT(21)

#define T32_PLL(_reg) {				\
	.reg = _reg,				\
	.rate_multiplier = 1,			\
	.m_shift = 20,				\
	.m_bits = 12,				\
	.m_offset = 0,				\
	.n_shift = 14,				\
	.n_bits = 6,				\
	.n_offset = 0,				\
	.od_shift = 11,				\
	.od_bits = 3,				\
	.od_max = 7,				\
	.od_encoding = t31_pll_od_encoding,	\
	.bypass_bit = -1,			\
	.enable_bit = 0,			\
	.stable_bit = 3,			\
}

static const u8 t32_sfc_pre_div_table[] = { 1, 2, 4, 8 };

static const struct ingenic_cgu_clk_info t32_cgu_clocks[] = {
	/* External clocks */

	[T32_CLK_EXCLK] = { "ext", CGU_CLK_EXT },
	[T32_CLK_RTCLK] = { "rtc", CGU_CLK_EXT },

	/* PLLs */

	[T32_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { T32_CLK_EXCLK, -1, -1, -1 },
		.pll = T32_PLL(CGU_REG_APLL),
	},

	[T32_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { T32_CLK_EXCLK, -1, -1, -1 },
		.pll = T32_PLL(CGU_REG_MPLL),
	},

	[T32_CLK_VPLL] = {
		"vpll", CGU_CLK_PLL,
		.parents = { T32_CLK_EXCLK, -1, -1, -1 },
		.pll = T32_PLL(CGU_REG_VPLL),
	},

	/* Muxes & dividers */

	[T32_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T32_CLK_EXCLK, T32_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T32_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T32_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T32_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 12 },
	},

	[T32_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T32_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T32_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 6 },
	},

	[T32_CLK_AHB2] = {
		"ahb2", CGU_CLK_MUX | CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	/* PDIV divides the AHB2 mux source, not AHB2 */
	[T32_CLK_PCLK] = {
		"pclk", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[T32_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 27 },
	},

	[T32_CLK_VPU] = {
		"vpu", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_VPUCDR, 30, 2 },
		.div = { CGU_REG_VPUCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T32_CLK_RSA] = {
		"rsa", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_RSACDR, 30, 2 },
		.div = { CGU_REG_RSACDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T32_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_SFC0_PRE] = {
		"sfc0_pre", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_T32_SFC0CDR, 30, 2 },
		.div = { CGU_REG_T32_SFC0CDR, 8, 1, 2, -1, -1, -1, 0, t32_sfc_pre_div_table },
	},

	[T32_CLK_SFC0] = {
		"sfc0", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SFC0_PRE, -1, -1, -1 },
		.div = { CGU_REG_T32_SFC0CDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 17 },
	},

	[T32_CLK_PWM] = {
		"pwm", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_T32_PWMCDR, 30, 2 },
		.div = { CGU_REG_T32_PWMCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 9 },
	},

	[T32_CLK_SSISLV] = {
		"ssi_slv", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_T32_SSISLVCDR, 30, 2 },
		.div = { CGU_REG_T32_SSISLVCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T32_CLK_MSC0] = {
		"msc0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, T32_CLK_EXCLK },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
		.div = { CGU_REG_MSC0CDR, 0, 4, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T32_CLK_LDC] = {
		"ldc", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_T32_LDCCDR, 30, 2 },
		.div = { CGU_REG_T32_LDCCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[T32_CLK_SSI] = {
		"ssi", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_CIM] = {
		"cim", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_CIMCDR, 30, 2 },
		.div = { CGU_REG_CIMCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		.div = { CGU_REG_ISPCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 19 },
	},

	[T32_CLK_ISPS] = {
		"isps", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_T32_ISPSCDR, 30, 2 },
		.div = { CGU_REG_T32_ISPSCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_ISPA] = {
		"ispa", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_T32_ISPACDR, 30, 2 },
		.div = { CGU_REG_T32_ISPACDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_TNPU] = {
		"tnpu", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_T32_TNPUCDR, 30, 2 },
		.div = { CGU_REG_T32_TNPUCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T32_CLK_MSC1] = {
		"msc1", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, T32_CLK_EXCLK },
		.mux = { CGU_REG_MSC1CDR, 30, 2 },
		.div = { CGU_REG_MSC1CDR, 0, 4, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T32_CLK_SFC1_PRE] = {
		"sfc1_pre", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, T32_CLK_VPLL, -1 },
		.mux = { CGU_REG_T32_SFC1CDR, 30, 2 },
		.div = { CGU_REG_T32_SFC1CDR, 8, 1, 2, -1, -1, -1, 0, t32_sfc_pre_div_table },
	},

	[T32_CLK_SFC1] = {
		"sfc1", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SFC1_PRE, -1, -1, -1 },
		.div = { CGU_REG_T32_SFC1CDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 13 },
	},

	/* Gate-only clocks */

	[T32_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T32_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T32_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 2 },
	},

	[T32_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T32_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T32_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T32_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T32_CLK_DMIC] = {
		"dmic", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 9 },
	},

	[T32_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 10 },
	},

	[T32_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},

	[T32_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 12 },
	},

	[T32_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T32_CLK_SMB2] = {
		"smb2", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T32_CLK_HASH] = {
		"hash", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[T32_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 18 },
	},

	[T32_CLK_MIPI_CSI] = {
		"mipi_csi", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T32_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	[T32_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 26 },
	},

	[T32_CLK_DTRNG] = {
		"dtrng", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 29 },
	},

	[T32_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T32_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T32_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	[T32_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 1 },
	},

	[T32_CLK_AHB1] = {
		"ahb1", CGU_CLK_GATE,
		.parents = { T32_CLK_ISPA, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T32_CLK_IVDC] = {
		"ivdc", CGU_CLK_GATE,
		.parents = { T32_CLK_ISPA, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 3 },
	},

	[T32_CLK_LZMA0] = {
		"lzma0", CGU_CLK_GATE,
		.parents = { T32_CLK_TNPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T32_CLK_DVP] = {
		"dvp", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T32_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T32_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 7 },
	},

	[T32_CLK_I2D] = {
		"i2d", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 8 },
	},

	[T32_CLK_SLV] = {
		"slv", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T32_CLK_LZMA1] = {
		"lzma1", CGU_CLK_GATE,
		.parents = { T32_CLK_TNPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},
};

static const struct ingenic_cgu_clk_info t33_cgu_clocks[] = {
	/* External clocks */

	[T32_CLK_EXCLK] = { "ext", CGU_CLK_EXT },
	[T32_CLK_RTCLK] = { "rtc", CGU_CLK_EXT },

	/* PLLs */

	[T32_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { T32_CLK_EXCLK, -1, -1, -1 },
		.pll = T32_PLL(CGU_REG_APLL),
	},

	[T32_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { T32_CLK_EXCLK, -1, -1, -1 },
		.pll = T32_PLL(CGU_REG_MPLL),
	},

	/* Muxes & dividers */

	[T32_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T32_CLK_EXCLK, T32_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T32_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T32_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T32_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 12 },
	},

	[T32_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T32_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T32_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 6 },
	},

	[T32_CLK_AHB2] = {
		"ahb2", CGU_CLK_MUX | CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	/* PDIV divides the AHB2 mux source, not AHB2 */
	[T32_CLK_PCLK] = {
		"pclk", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[T32_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T32_CLK_SCLKA, T32_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 27 },
	},

	[T32_CLK_VPU] = {
		"vpu", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_VPUCDR, 30, 2 },
		.div = { CGU_REG_VPUCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T32_CLK_RSA] = {
		"rsa", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_RSACDR, 30, 2 },
		.div = { CGU_REG_RSACDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T32_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_SFC0_PRE] = {
		"sfc0_pre", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_T32_SFC0CDR, 30, 2 },
		.div = { CGU_REG_T32_SFC0CDR, 8, 1, 2, -1, -1, -1, 0, t32_sfc_pre_div_table },
	},

	[T32_CLK_SFC0] = {
		"sfc0", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SFC0_PRE, -1, -1, -1 },
		.div = { CGU_REG_T32_SFC0CDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 17 },
	},

	[T32_CLK_PWM] = {
		"pwm", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_T32_PWMCDR, 30, 2 },
		.div = { CGU_REG_T32_PWMCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 9 },
	},

	[T32_CLK_SSISLV] = {
		"ssi_slv", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_T32_SSISLVCDR, 30, 2 },
		.div = { CGU_REG_T32_SSISLVCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T32_CLK_MSC0] = {
		"msc0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, T32_CLK_EXCLK },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
		.div = { CGU_REG_MSC0CDR, 0, 4, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T32_CLK_LDC] = {
		"ldc", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_T32_LDCCDR, 30, 2 },
		.div = { CGU_REG_T32_LDCCDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[T32_CLK_SSI] = {
		"ssi", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		.div = { CGU_REG_ISPCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 19 },
	},

	[T32_CLK_ISPA] = {
		"ispa", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_T32_ISPACDR, 30, 2 },
		.div = { CGU_REG_T32_ISPACDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
	},

	[T32_CLK_TNPU] = {
		"tnpu", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_T32_TNPUCDR, 30, 2 },
		.div = { CGU_REG_T32_TNPUCDR, 0, 1, 4, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T32_CLK_MSC1] = {
		"msc1", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, T32_CLK_EXCLK },
		.mux = { CGU_REG_MSC1CDR, 30, 2 },
		.div = { CGU_REG_MSC1CDR, 0, 4, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T32_CLK_SFC1_PRE] = {
		"sfc1_pre", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T32_CLK_SCLKA, T32_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_T32_SFC1CDR, 30, 2 },
		.div = { CGU_REG_T32_SFC1CDR, 8, 1, 2, -1, -1, -1, 0, t32_sfc_pre_div_table },
	},

	[T32_CLK_SFC1] = {
		"sfc1", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T32_CLK_SFC1_PRE, -1, -1, -1 },
		.div = { CGU_REG_T32_SFC1CDR, 0, 1, 8, T32_CDR_CE, T32_CDR_BUSY, T32_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 13 },
	},

	/* Gate-only clocks */

	[T32_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T32_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T32_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 2 },
	},

	[T32_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T32_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T32_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T32_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T32_CLK_DMIC] = {
		"dmic", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 9 },
	},

	[T32_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 10 },
	},

	[T32_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},

	[T32_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 12 },
	},

	[T32_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T32_CLK_SMB2] = {
		"smb2", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T32_CLK_HASH] = {
		"hash", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[T32_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 18 },
	},

	[T32_CLK_MIPI_CSI] = {
		"mipi_csi", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T32_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	[T32_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 26 },
	},

	[T32_CLK_DTRNG] = {
		"dtrng", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 29 },
	},

	[T32_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T32_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T32_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	[T32_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 1 },
	},

	[T32_CLK_AHB1] = {
		"ahb1", CGU_CLK_GATE,
		.parents = { T32_CLK_ISPA, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T32_CLK_IVDC] = {
		"ivdc", CGU_CLK_GATE,
		.parents = { T32_CLK_ISPA, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 3 },
	},

	[T32_CLK_LZMA0] = {
		"lzma0", CGU_CLK_GATE,
		.parents = { T32_CLK_TNPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T32_CLK_DVP] = {
		"dvp", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T32_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T32_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 7 },
	},

	[T32_CLK_I2D] = {
		"i2d", CGU_CLK_GATE,
		.parents = { T32_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 8 },
	},

	[T32_CLK_SLV] = {
		"slv", CGU_CLK_GATE,
		.parents = { T32_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T32_CLK_LZMA1] = {
		"lzma1", CGU_CLK_GATE,
		.parents = { T32_CLK_TNPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},
};

static void __init t31_cgu_setup(struct device_node *np,
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

static void __init t31_cgu_init(struct device_node *np)
{
	t31_cgu_setup(np, t31_cgu_clocks, ARRAY_SIZE(t31_cgu_clocks));
}

static void __init t23_cgu_init(struct device_node *np)
{
	t31_cgu_setup(np, t23_cgu_clocks, ARRAY_SIZE(t23_cgu_clocks));
}

/*
 * The MSC muxes switch to EXCLK only while MSC0CDR's EXCLK_EN is set; the
 * vendor kernel sets it at boot and leaves it set.
 */
static void __init t32_cgu_enable_msc_exclk(void)
{
	unsigned long flags;

	if (!cgu)
		return;

	spin_lock_irqsave(&cgu->lock, flags);
	writel(readl(cgu->base + CGU_REG_MSC0CDR) | T32_MSC0CDR_EXCLK_EN,
	       cgu->base + CGU_REG_MSC0CDR);
	spin_unlock_irqrestore(&cgu->lock, flags);
}

static void __init t32_cgu_init(struct device_node *np)
{
	t31_cgu_setup(np, t32_cgu_clocks, ARRAY_SIZE(t32_cgu_clocks));
	t32_cgu_enable_msc_exclk();
}

static void __init t33_cgu_init(struct device_node *np)
{
	t31_cgu_setup(np, t33_cgu_clocks, ARRAY_SIZE(t33_cgu_clocks));
	t32_cgu_enable_msc_exclk();
}

/*
 * CGU has some children devices, this is useful for probing children devices
 * in the case where the device node is compatible with "simple-mfd".
 */
CLK_OF_DECLARE_DRIVER(t31_cgu, "ingenic,t31-cgu", t31_cgu_init);
CLK_OF_DECLARE_DRIVER(t23_cgu, "ingenic,t23-cgu", t23_cgu_init);
CLK_OF_DECLARE_DRIVER(t32_cgu, "ingenic,t32-cgu", t32_cgu_init);
CLK_OF_DECLARE_DRIVER(t33_cgu, "ingenic,t33-cgu", t33_cgu_init);
