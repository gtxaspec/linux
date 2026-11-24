// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic T20 and T10 SoC CGU driver
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/of.h>
#include <linux/string.h>

#include <dt-bindings/clock/ingenic,t20-cgu.h>

#include "cgu.h"
#include "pm.h"

/* CGU register offsets */
#define CGU_REG_CPCCR		0x00
#define CGU_REG_APLL		0x10
#define CGU_REG_MPLL		0x14
#define CGU_REG_APLLFRAC	0x18
#define CGU_REG_CLKGR0		0x20
#define CGU_REG_OPCR		0x24
#define CGU_REG_CLKGR1		0x28
#define CGU_REG_DDRCDR		0x2c
#define CGU_REG_VPUCDR		0x30
#define CGU_REG_MACCDR		0x54
#define CGU_REG_I2SCDR		0x60
#define CGU_REG_LPCDR		0x64
#define CGU_REG_MSC0CDR		0x68
#define CGU_REG_SSICDR		0x74
#define CGU_REG_CIMCDR		0x7c
#define CGU_REG_ISPCDR		0x80
#define CGU_REG_MSC1CDR		0xa4
#define CGU_REG_VPLL		0xe0

/* PLL control registers */
#define PLL_EN			BIT(0)
#define PLL_M_SHIFT		20
#define PLL_M_MASK		GENMASK(11, 0)
#define PLL_N_SHIFT		14
#define PLL_N_MASK		GENMASK(5, 0)
#define PLL_OD1_SHIFT		11
#define PLL_OD0_SHIFT		8
#define PLL_OD_MASK		GENMASK(2, 0)
#define PLL_FRAC_MASK		GENMASK(23, 0)

/* I2SCDR: parent select, enable, and M/N */
#define I2SCDR_PCS_SHIFT	30
#define I2SCDR_CE_BIT		29
#define I2SCDR_M_SHIFT		13
#define I2SCDR_M_BITS		9
#define I2SCDR_N_BITS		13

static struct ingenic_cgu *cgu;

/*
 * The PLLs have two output dividers, OD1 and OD0, whose fields hold the
 * divisor itself (zero is invalid), and the APLL adds a fraction of M in
 * units of 2^-24 from its own register. The bootloader sets the rates and
 * nothing here changes them, so only recalc_rate is provided.
 */
static unsigned long t20_pll_recalc_rate(struct clk_hw *hw,
					 unsigned long parent_rate)
{
	const struct ingenic_clk *ingenic_clk = to_ingenic_clk(hw);
	unsigned int reg = ingenic_clk->idx == T20_CLK_APLL ? CGU_REG_APLL :
			   ingenic_clk->idx == T20_CLK_MPLL ? CGU_REG_MPLL :
			   CGU_REG_VPLL;
	u32 ctl = readl(cgu->base + reg);
	u64 m = (ctl >> PLL_M_SHIFT) & PLL_M_MASK;
	u32 n = (ctl >> PLL_N_SHIFT) & PLL_N_MASK;
	u32 od1 = (ctl >> PLL_OD1_SHIFT) & PLL_OD_MASK;
	u32 od0 = (ctl >> PLL_OD0_SHIFT) & PLL_OD_MASK;
	u64 rate;

	if (!(ctl & PLL_EN) || !n || !od1 || !od0)
		return 0;

	m <<= 24;
	if (ingenic_clk->idx == T20_CLK_APLL)
		m |= readl(cgu->base + CGU_REG_APLLFRAC) & PLL_FRAC_MASK;

	rate = div_u64((u64)parent_rate * m, n * od1 * od0);
	return rate >> 24;
}

static const struct clk_ops t20_pll_ops = {
	.recalc_rate	= t20_pll_recalc_rate,
};

static const struct ingenic_cgu_clk_info t20_cgu_clocks[] = {

	/* External clocks */

	[T20_CLK_EXCLK] = { "ext", CGU_CLK_EXT },
	[T20_CLK_RTCLK] = { "rtc", CGU_CLK_EXT },

	/* PLLs */

	[T20_CLK_APLL] = {
		"apll", CGU_CLK_CUSTOM,
		.parents = { T20_CLK_EXCLK, -1, -1, -1 },
		.custom = { &t20_pll_ops },
	},

	[T20_CLK_MPLL] = {
		"mpll", CGU_CLK_CUSTOM,
		.parents = { T20_CLK_EXCLK, -1, -1, -1 },
		.custom = { &t20_pll_ops },
	},

	[T20_CLK_VPLL] = {
		"vpll", CGU_CLK_CUSTOM,
		.parents = { T20_CLK_EXCLK, -1, -1, -1 },
		.custom = { &t20_pll_ops },
	},

	/* Muxes & dividers */

	[T20_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T20_CLK_EXCLK, T20_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T20_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T20_CLK_SCLKA, T20_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T20_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T20_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},

	[T20_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T20_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T20_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T20_CLK_SCLKA, T20_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	/* The source of both AHB2 and PCLK */
	[T20_CLK_AHB2_MUX] = {
		"ahb2_mux", CGU_CLK_MUX,
		.parents = { -1, T20_CLK_SCLKA, T20_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
	},

	[T20_CLK_AHB2] = {
		"ahb2", CGU_CLK_DIV,
		.parents = { T20_CLK_AHB2_MUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	/* PDIV divides the AHB2 source, not AHB2 */
	[T20_CLK_PCLK] = {
		"pclk", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T20_CLK_AHB2_MUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T20_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T20_CLK_SCLKA, T20_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[T20_CLK_VPU] = {
		"vpu", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T20_CLK_SCLKA, T20_CLK_MPLL,
			     T20_CLK_VPLL, T20_CLK_VPLL },
		.mux = { CGU_REG_VPUCDR, 30, 2 },
		.div = { CGU_REG_VPUCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	[T20_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T20_CLK_SCLKA, T20_CLK_MPLL,
			     T20_CLK_VPLL, T20_CLK_VPLL },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, 29, 28, 27 },
	},

	[T20_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T20_CLK_SCLKA, T20_CLK_MPLL,
			     T20_CLK_VPLL, T20_CLK_VPLL },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, 28, 27, 26 },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	/* The MSC and SSI sources have one select bit: SCLK_A or MPLL */
	[T20_CLK_MSCMUX] = {
		"msc_mux", CGU_CLK_MUX,
		.parents = { T20_CLK_SCLKA, T20_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_MSC0CDR, 31, 1 },
	},

	[T20_CLK_MSC0] = {
		"msc0", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T20_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC0CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T20_CLK_MSC1] = {
		"msc1", CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T20_CLK_MSCMUX, -1, -1, -1 },
		.div = { CGU_REG_MSC1CDR, 0, 2, 8, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T20_CLK_SSIPLL] = {
		"ssi_pll", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T20_CLK_SCLKA, T20_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_SSICDR, 31, 1 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, 29, 28, 27 },
	},

	[T20_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T20_CLK_SCLKA, T20_CLK_MPLL,
			     T20_CLK_VPLL, T20_CLK_VPLL },
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		/* bit 29 = CE, bit 28 = BUSY, bit 27 = STOP */
		.div = { CGU_REG_ISPCDR, 0, 1, 4, 29, 28, 27 },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T20_CLK_CIM] = {
		"cim", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T20_CLK_SCLKA, T20_CLK_MPLL,
			     T20_CLK_VPLL, T20_CLK_VPLL },
		.mux = { CGU_REG_CIMCDR, 30, 2 },
		/*
		 * The STOP bit keeps the MCLK off until the framework clears
		 * it, and CE must be set to apply a new divider.
		 */
		.div = { CGU_REG_CIMCDR, 0, 1, 8, 29, 28, 27 },
	},

	/* Gate-only clocks */

	/*
	 * NEMC and EFUSE are not in the vendor clock tree; their bits follow
	 * the CLKGR0 order every XBurst1 SoC shares.
	 */
	[T20_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T20_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T20_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T20_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T20_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T20_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T20_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T20_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T20_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 8 },
	},

	[T20_CLK_AIC] = {
		"aic", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},

	[T20_CLK_DMIC] = {
		"dmic", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 12 },
	},

	[T20_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T20_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T20_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T20_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T20_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	/* Not in the vendor clock tree; the bit is the next after UART1 */
	[T20_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { T20_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 16 },
	},

	/* The SFC driver reads its rate from this gate: ssi_pll sets it */
	[T20_CLK_SFC] = {
		"sfc", CGU_CLK_GATE,
		.parents = { T20_CLK_SSIPLL, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T20_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T20_CLK_CSI] = {
		"csi", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 25 },
	},

	[T20_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T20_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T20_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T20_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T20_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T20_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T20_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T20_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T20_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T20_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T20_CLK_EXCLK_DIV512] = {
		"exclk_div512", CGU_CLK_FIXDIV,
		.parents = { T20_CLK_EXCLK },
		.fixdiv = { 512 },
	},

	[T20_CLK_RTC] = {
		"rtc_ercs", CGU_CLK_MUX | CGU_CLK_GATE,
		.parents = { T20_CLK_EXCLK_DIV512, T20_CLK_RTCLK, -1, -1 },
		.mux = { CGU_REG_OPCR, 2, 1 },
		.gate = { CGU_REG_CLKGR0, 29 },
	},

	/* I2S fractional divider, M/N from SCLK_A, MPLL or VPLL */

	[T20_CLK_DIV_I2S] = {
		"div_i2s", CGU_CLK_MUX | CGU_CLK_FRAC,
		.parents = { T20_CLK_SCLKA, T20_CLK_MPLL,
			     T20_CLK_VPLL, T20_CLK_VPLL },
		.mux = { CGU_REG_I2SCDR, I2SCDR_PCS_SHIFT, 2 },
		.frac = { CGU_REG_I2SCDR, I2SCDR_M_SHIFT, I2SCDR_M_BITS,
			  0, I2SCDR_N_BITS },
	},

	/* CE (clock enable) gate inside I2SCDR, set to enable */

	[T20_CLK_CE_I2S] = {
		"ce_i2s", CGU_CLK_GATE,
		.parents = { T20_CLK_DIV_I2S, -1, -1, -1 },
		.gate = { CGU_REG_I2SCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},
};

/*
 * The T10 is the T20 without the VPLL, the LCD gate, the CSI gate and
 * UART2, and its VPU, MAC, CIM, ISP and I2S sources are SCLK_A or MPLL
 * (bit 31), like the MSC and SSI ones. It keeps the T20 clock IDs.
 */
static struct ingenic_cgu_clk_info t10_cgu_clocks[ARRAY_SIZE(t20_cgu_clocks)];

static void __init t10_cgu_clocks_init(void)
{
	static const unsigned int one_bit_mux[] = {
		T20_CLK_VPU, T20_CLK_MAC, T20_CLK_ISP, T20_CLK_CIM,
		T20_CLK_DIV_I2S,
	};
	struct ingenic_cgu_clk_info *info;
	unsigned int i;

	memcpy(t10_cgu_clocks, t20_cgu_clocks, sizeof(t10_cgu_clocks));

	memset(&t10_cgu_clocks[T20_CLK_VPLL], 0, sizeof(*info));
	memset(&t10_cgu_clocks[T20_CLK_LCD], 0, sizeof(*info));
	memset(&t10_cgu_clocks[T20_CLK_CSI], 0, sizeof(*info));
	memset(&t10_cgu_clocks[T20_CLK_UART2], 0, sizeof(*info));

	for (i = 0; i < ARRAY_SIZE(one_bit_mux); i++) {
		info = &t10_cgu_clocks[one_bit_mux[i]];
		info->parents[2] = -1;
		info->parents[3] = -1;
		if (info->type & CGU_CLK_MUX) {
			info->mux.shift = 31;
			info->mux.bits = 1;
		}
	}
}

static void __init t20_cgu_register(struct device_node *np,
				    const struct ingenic_cgu_clk_info *clocks,
				    unsigned int num)
{
	int retval;

	cgu = ingenic_cgu_new(clocks, num, np);
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

static void __init t20_cgu_init(struct device_node *np)
{
	t20_cgu_register(np, t20_cgu_clocks, ARRAY_SIZE(t20_cgu_clocks));
}

static void __init t10_cgu_init(struct device_node *np)
{
	t10_cgu_clocks_init();
	t20_cgu_register(np, t10_cgu_clocks, ARRAY_SIZE(t10_cgu_clocks));
}

/*
 * CGU has some children devices, this is useful for probing children devices
 * in the case where the device node is compatible with "simple-mfd".
 */
CLK_OF_DECLARE_DRIVER(t20_cgu, "ingenic,t20-cgu", t20_cgu_init);
CLK_OF_DECLARE_DRIVER(t10_cgu, "ingenic,t10-cgu", t10_cgu_init);
