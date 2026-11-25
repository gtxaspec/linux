// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic T40 SoC CGU driver
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>

#include <dt-bindings/clock/ingenic,t40-cgu.h>

#include "cgu.h"
#include "pm.h"

/* CGU register offsets */
#define CGU_REG_CPCCR		0x00
#define CGU_REG_APLL		0x10
#define CGU_REG_MPLL		0x14
#define CGU_REG_CLKGR0		0x20
#define CGU_REG_CLKGR1		0x28
#define CGU_REG_DDRCDR		0x2c
#define CGU_REG_EL150CDR	0x30
#define CGU_REG_RSACDR		0x4c
#define CGU_REG_MACCDR		0x54
#define CGU_REG_EPLL		0x58
#define CGU_REG_SFCCDR		0x60
#define CGU_REG_LPCDR		0x64
#define CGU_REG_MSC0CDR		0x68
#define CGU_REG_MSC1CDR		0x6c
#define CGU_REG_SSICDR		0x74
#define CGU_REG_ISPCDR		0x80
#define CGU_REG_CIM0CDR		0x90
#define CGU_REG_CIM1CDR		0x94
#define CGU_REG_CIM2CDR		0x98
#define CGU_REG_BSCALERCDR	0xa0
#define CGU_REG_BT0CDR		0xb8
#define CGU_REG_VPLL		0xe0

/*
 * Every peripheral has a CDR of its own: mux in bits 31:30, change enable
 * 29, busy 28, stop 27 and the divider in the low bits. The MSC dividers
 * count in fours, and their muxes take only the APLL (SCLK_A) and the MPLL
 * here: the vendor U-Boot lists the EPLL and EXCLK too, but the T41's MSC
 * mux failed on silicon once the identification clock went through EXCLK,
 * and the EPLL is not running.
 */
#define T40_CDR_CE		29
#define T40_CDR_BUSY		28
#define T40_CDR_STOP		27

/*
 * The PLLs: EXCLK * M / (N * OD0 * OD1), all four fields as they read, with
 * M in bits 31:20, N in 19:14, OD0 in 13:11 and OD1 in 10:8. The boot loader
 * sets them up and the kernel only reads them, see od1_bits.
 */
#define T40_PLL(_reg) {				\
	.reg = _reg,				\
	.rate_multiplier = 1,			\
	.m_shift = 20,				\
	.m_bits = 12,				\
	.n_shift = 14,				\
	.n_bits = 6,				\
	.od_shift = 11,				\
	.od_bits = 3,				\
	.od_max = 8,				\
	.od_encoding = t40_pll_od_encoding,	\
	.od1_shift = 8,				\
	.od1_bits = 3,				\
	.od1_direct = true,			\
	.bypass_bit = -1,			\
	.enable_bit = 0,			\
	.stable_bit = 3,			\
}

#define T40_PLL_SET(_reg) {			\
	.reg = _reg,				\
	.rate_multiplier = 1,			\
	.m_shift = 20,				\
	.m_bits = 12,				\
	.n_shift = 14,				\
	.n_bits = 6,				\
	.od_shift = 11,				\
	.od_bits = 3,				\
	.od_max = 8,				\
	.od_encoding = t40_pll_od_encoding,	\
	.od1_shift = 8,				\
	.od1_bits = 3,				\
	.od1_direct = true,			\
	.bypass_bit = -1,			\
	.enable_bit = 0,			\
	.stable_bit = 3,			\
	.calc_m_n_od = t40_epll_calc,		\
}

/*
 * The EPLL is the only PLL the kernel programs: the sensor drivers set it to
 * 891 MHz (/24 gives the 37.125 MHz an IMX307 wants) and the boot loader
 * leaves it at 384 MHz. The rates are the vendor kernel's table for the
 * EXCLK, M / N and a post divider that is OD0 here (the vendor's od1) with
 * the second divider at 1; a request takes the highest rate not above it.
 */
static const struct {
	unsigned int mhz, m, n, od;
} t40_epll_rates[] = {
	{ 1800, 150, 1, 2 }, { 1608, 134, 1, 2 }, { 1500, 125, 1, 2 },
	{ 1404, 117, 1, 2 }, { 1392, 116, 1, 2 }, { 1308, 109, 1, 2 },
	{ 1296, 108, 1, 2 }, { 1200, 100, 1, 2 }, { 1104, 92, 1, 2 },
	{ 1080, 90, 1, 2 }, { 1008, 84, 1, 2 }, { 900, 75, 1, 2 },
	{ 891, 297, 4, 2 }, { 864, 72, 1, 2 }, { 600, 75, 1, 3 },
	{ 384, 32, 1, 2 },
};

static void t40_epll_calc(const struct ingenic_cgu_pll_info *pll_info,
			  unsigned long rate, unsigned long parent_rate,
			  unsigned int *pm, unsigned int *pn, unsigned int *pod)
{
	int i = 0;

	while (i < ARRAY_SIZE(t40_epll_rates) - 1 &&
	       rate < t40_epll_rates[i].mhz * 1000000UL)
		i++;
	*pm = t40_epll_rates[i].m;
	*pn = t40_epll_rates[i].n;
	*pod = t40_epll_rates[i].od;
}

/* OD0 is the divider itself; zero, which no running PLL holds, reads as 8 */
static const s8 t40_pll_od_encoding[8] = {
	1, 2, 3, 4, 5, 6, 7, 0,
};

static const struct ingenic_cgu_clk_info t40_cgu_clocks[] = {
	[T40_CLK_EXCLK] = {
		"ext", CGU_CLK_EXT,
	},

	[T40_CLK_RTCLK] = {
		"rtc", CGU_CLK_EXT,
	},

	[T40_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { T40_CLK_EXCLK, -1, -1, -1 },
		.pll = T40_PLL(CGU_REG_APLL),
	},

	[T40_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { T40_CLK_EXCLK, -1, -1, -1 },
		.pll = T40_PLL(CGU_REG_MPLL),
	},

	[T40_CLK_VPLL] = {
		"vpll", CGU_CLK_PLL,
		.parents = { T40_CLK_EXCLK, -1, -1, -1 },
		.pll = T40_PLL(CGU_REG_VPLL),
	},

	[T40_CLK_EPLL] = {
		"epll", CGU_CLK_PLL,
		.parents = { T40_CLK_EXCLK, -1, -1, -1 },
		.pll = T40_PLL_SET(CGU_REG_EPLL),
	},

	[T40_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T40_CLK_EXCLK, T40_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T40_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T40_CLK_SCLKA, T40_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T40_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T40_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},

	[T40_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T40_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T40_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T40_CLK_SCLKA, T40_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[T40_CLK_AHB2_MUX] = {
		"ahb2_mux", CGU_CLK_MUX,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T40_CLK_SCLKA, T40_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
	},

	[T40_CLK_AHB2] = {
		"ahb2", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T40_CLK_AHB2_MUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	[T40_CLK_PCLK] = {
		"pclk", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T40_CLK_AHB2_MUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
	},

	[T40_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T40_CLK_SCLKA, T40_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[T40_CLK_EL150] = {
		"el150", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_EL150CDR, 30, 2 },
		.div = { CGU_REG_EL150CDR, 0, 1, 4, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	[T40_CLK_RSA] = {
		"rsa", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_RSACDR, 30, 2 },
		.div = { CGU_REG_RSACDR, 0, 1, 4, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 27 },
	},

	[T40_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
	},

	[T40_CLK_SFC] = {
		"sfc", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_SFCCDR, 30, 2 },
		.div = { CGU_REG_SFCCDR, 0, 1, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T40_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	[T40_CLK_MSC0] = {
		"msc0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
		.div = { CGU_REG_MSC0CDR, 0, 4, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T40_CLK_MSC1] = {
		"msc1", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, -1, -1 },
		.mux = { CGU_REG_MSC1CDR, 30, 2 },
		.div = { CGU_REG_MSC1CDR, 0, 4, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T40_CLK_SSI] = {
		"ssi", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
	},

	[T40_CLK_ISP] = {
		"isp", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_ISPCDR, 30, 2 },
		.div = { CGU_REG_ISPCDR, 0, 1, 4, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T40_CLK_CIM0] = {
		"cim0", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_CIM0CDR, 30, 2 },
		.div = { CGU_REG_CIM0CDR, 0, 1, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
	},

	[T40_CLK_CIM1] = {
		"cim1", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_CIM1CDR, 30, 2 },
		.div = { CGU_REG_CIM1CDR, 0, 1, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
	},

	[T40_CLK_CIM2] = {
		"cim2", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_CIM2CDR, 30, 2 },
		.div = { CGU_REG_CIM2CDR, 0, 1, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
	},

	[T40_CLK_BSCALER] = {
		"bscaler", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_BSCALERCDR, 30, 2 },
		.div = { CGU_REG_BSCALERCDR, 0, 1, 4, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 2 },
	},

	[T40_CLK_BT0] = {
		"bt0", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T40_CLK_SCLKA, T40_CLK_MPLL, T40_CLK_VPLL, T40_CLK_EPLL },
		.mux = { CGU_REG_BT0CDR, 30, 2 },
		.div = { CGU_REG_BT0CDR, 0, 1, 8, T40_CDR_CE, T40_CDR_BUSY, T40_CDR_STOP },
	},

	[T40_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T40_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T40_CLK_MIPI_CSI] = {
		"mipi_csi", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 25 },
	},

	[T40_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 22 },
	},

	[T40_CLK_SSI1] = {
		"ssi1", CGU_CLK_GATE,
		.parents = { T40_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T40_CLK_HASH] = {
		"hash", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 19 },
	},

	[T40_CLK_SLV] = {
		"slv", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 18 },
	},

	[T40_CLK_UART3] = {
		"uart3", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 17 },
	},

	[T40_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 16 },
	},

	[T40_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[T40_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T40_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T40_CLK_DMIC] = {
		"dmic", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 12 },
	},

	[T40_CLK_SMB3] = {
		"smb3", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 10 },
	},

	[T40_CLK_SMB2] = {
		"smb2", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 9 },
	},

	[T40_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 8 },
	},

	[T40_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T40_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T40_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T40_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T40_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T40_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T40_CLK_APB0] = {
		"apb0", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T40_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T40_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T40_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T40_CLK_MONITOR] = {
		"monitor", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 9 },
	},

	[T40_CLK_I2D] = {
		"i2d", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 8 },
	},

	[T40_CLK_MIPI_DSI] = {
		"mipi_dsi", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 7 },
	},

	[T40_CLK_DRAWBOX] = {
		"drawbox", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 6 },
	},

	[T40_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T40_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T40_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T40_CLK_AHB1] = {
		"ahb1", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T40_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 3 },
	},

	[T40_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T40_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T40_CLK_DTRNG] = {
		"dtrng", CGU_CLK_GATE,
		.parents = { T40_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 1 },
	},
};

static struct ingenic_cgu *cgu;

static void __init t40_cgu_init(struct device_node *np)
{
	int retval;

	cgu = ingenic_cgu_new(t40_cgu_clocks, ARRAY_SIZE(t40_cgu_clocks), np);
	if (!cgu) {
		pr_err("%s: failed to initialise CGU\n", __func__);
		return;
	}

	/* Divider outputs stay off while CE is set */
	cgu->clear_ce = true;

	retval = ingenic_cgu_register_clocks(cgu);
	if (retval) {
		pr_err("%s: failed to register CGU clocks\n", __func__);
		return;
	}

	ingenic_cgu_register_syscore(cgu);
}

/*
 * CGU has some children devices, this is useful for probing children devices
 * in the case where the device node is compatible with "simple-mfd".
 */
CLK_OF_DECLARE_DRIVER(t40_cgu, "ingenic,t40-cgu", t40_cgu_init);
