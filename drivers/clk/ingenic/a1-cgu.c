// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic A1 SoC CGU driver
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>

#include <dt-bindings/clock/ingenic,a1-cgu.h>

#include "cgu.h"
#include "pm.h"

/* CGU register offsets */
#define CGU_REG_CPCCR		0x00
#define CGU_REG_APLL		0x10
#define CGU_REG_MPLL		0x14
#define CGU_REG_EPLL		0x18
#define CGU_REG_VPLL		0x1c
#define CGU_REG_CLKGR0		0x30
#define CGU_REG_CLKGR1		0x38
#define CGU_REG_DDRCDR		0x3c
#define CGU_REG_AHB1CDR		0x84
#define CGU_REG_SSICDR		0x8c
#define CGU_REG_SFC0CDR		0x90
#define CGU_REG_SFC1CDR		0x94
#define CGU_REG_MSC0CDR		0x98
#define CGU_REG_MSC1CDR		0x9c
#define CGU_REG_I2S0TCDR	0xa0
#define CGU_REG_I2S0RCDR	0xa8
#define CGU_REG_VDEVCDR		0xb0
#define CGU_REG_VDEACDR		0xb4
#define CGU_REG_VDEMCDR		0xb8
#define CGU_REG_IPUMCDR		0xbc
#define CGU_REG_MAC0CDR		0xc0
#define CGU_REG_MAC0TXCDR	0xc4
#define CGU_REG_MACPTPCDR	0xcc
#define CGU_REG_MAC1CDR		0xd0
#define CGU_REG_MAC1TXCDR	0xd4
#define CGU_REG_BT0CDR		0xf8
#define CGU_REG_PWMCDR		0xfc
#define CGU_REG_I2S1TCDR	0x100
#define CGU_REG_I2S1RCDR	0x108

/*
 * Every peripheral has a CDR of its own: mux in bits 31:30, change enable
 * 29, busy 28, stop 27 and the divider in the low bits. The MSC dividers
 * count in fours, and their muxes take only the APLL (SCLK_A), the MPLL and
 * the EPLL: the vendor kernel lists EXCLK as the fourth source, but the
 * vendor boot loader's table has nothing there, and the T41's MSC mux
 * failed on silicon once a table entry that was not a PLL went through it.
 */
#define A1_CDR_CE		29
#define A1_CDR_BUSY		28
#define A1_CDR_STOP		27

/*
 * I2S{0,1}{T,R}CDR, parent * M / N: the source select, a 9 bit M at bit 20,
 * a 20 bit N and the clock enable at bit 29, as the vendor kernel's
 * clk-a1.c has them. The registers 4 bytes above each are left alone.
 */
#define I2SCDR_PCS_SHIFT	30
#define I2SCDR_CE_BIT		29
#define I2SCDR_M_SHIFT		20
#define I2SCDR_M_BITS		9
#define I2SCDR_N_SHIFT		0
#define I2SCDR_N_BITS		20

/*
 * The PLLs: EXCLK * M / (N * OD0 * OD1), all four fields as they read, with
 * M in bits 31:20, N in 19:14, OD0 in 13:11 and OD1 in 10:8. The boot loader
 * sets them up and the kernel only reads them, see od1_bits.
 */
#define A1_PLL(_reg) {				\
	.reg = _reg,				\
	.rate_multiplier = 1,			\
	.m_shift = 20,				\
	.m_bits = 12,				\
	.n_shift = 14,				\
	.n_bits = 6,				\
	.od_shift = 11,				\
	.od_bits = 3,				\
	.od_max = 8,				\
	.od_encoding = a1_pll_od_encoding,	\
	.od1_shift = 8,				\
	.od1_bits = 3,				\
	.od1_direct = true,			\
	.bypass_bit = -1,			\
	.enable_bit = 0,			\
	.stable_bit = 3,			\
}

/* OD0 is the divider itself; zero, which no running PLL holds, reads as 8 */
static const s8 a1_pll_od_encoding[8] = {
	1, 2, 3, 4, 5, 6, 7, 0,
};

#define A1_PLL_PARENTS	{ A1_CLK_SCLKA, A1_CLK_MPLL, A1_CLK_VPLL, A1_CLK_EPLL }

#define A1_CDR_DIV(_reg, _bits) \
	{ _reg, 0, 1, _bits, A1_CDR_CE, A1_CDR_BUSY, A1_CDR_STOP }

static const struct ingenic_cgu_clk_info a1_cgu_clocks[] = {
	[A1_CLK_EXCLK] = {
		"ext", CGU_CLK_EXT,
	},

	[A1_CLK_RTCLK] = {
		"rtc", CGU_CLK_EXT,
	},

	[A1_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { A1_CLK_EXCLK, -1, -1, -1 },
		.pll = A1_PLL(CGU_REG_APLL),
	},

	[A1_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { A1_CLK_EXCLK, -1, -1, -1 },
		.pll = A1_PLL(CGU_REG_MPLL),
	},

	[A1_CLK_VPLL] = {
		"vpll", CGU_CLK_PLL,
		.parents = { A1_CLK_EXCLK, -1, -1, -1 },
		.pll = A1_PLL(CGU_REG_VPLL),
	},

	[A1_CLK_EPLL] = {
		"epll", CGU_CLK_PLL,
		.parents = { A1_CLK_EXCLK, -1, -1, -1 },
		.pll = A1_PLL(CGU_REG_EPLL),
	},

	[A1_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, A1_CLK_EXCLK, A1_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[A1_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, A1_CLK_SCLKA, A1_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[A1_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { A1_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[A1_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { A1_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[A1_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, A1_CLK_SCLKA, A1_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 2 },
	},

	[A1_CLK_AHB2_MUX] = {
		"ahb2_mux", CGU_CLK_MUX,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, A1_CLK_SCLKA, A1_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
	},

	[A1_CLK_AHB2] = {
		"ahb2", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { A1_CLK_AHB2_MUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	[A1_CLK_PCLK] = {
		"pclk", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { A1_CLK_AHB2_MUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[A1_CLK_AHB1] = {
		"ahb1", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_AHB1CDR, 0, 2 },
		.div = { CGU_REG_AHB1CDR, 4, 1, 4, 8, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 19 },
	},

	[A1_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, A1_CLK_SCLKA, A1_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_DDRCDR, 4),
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[A1_CLK_SSI] = {
		"ssi", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_SSICDR, 8),
	},

	[A1_CLK_SFC0] = {
		"sfc0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_SFC0CDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_SFC0CDR, 8),
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	[A1_CLK_SFC1] = {
		"sfc1", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_SFC1CDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_SFC1CDR, 8),
		.gate = { CGU_REG_CLKGR0, 25 },
	},

	[A1_CLK_MSC0] = {
		"msc0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { A1_CLK_SCLKA, A1_CLK_MPLL, A1_CLK_EPLL, -1 },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
		.div = { CGU_REG_MSC0CDR, 0, 4, 8, A1_CDR_CE, A1_CDR_BUSY, A1_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[A1_CLK_MSC1] = {
		"msc1", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { A1_CLK_SCLKA, A1_CLK_MPLL, A1_CLK_EPLL, -1 },
		.mux = { CGU_REG_MSC1CDR, 30, 2 },
		.div = { CGU_REG_MSC1CDR, 0, 4, 8, A1_CDR_CE, A1_CDR_BUSY, A1_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[A1_CLK_MAC0] = {
		"mac0", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_MAC0CDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_MAC0CDR, 8),
	},

	[A1_CLK_MAC0TX] = {
		"mac0_tx", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_MAC0TXCDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_MAC0TXCDR, 8),
	},

	[A1_CLK_MACPTP] = {
		"mac_ptp", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_MACPTPCDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_MACPTPCDR, 8),
	},

	[A1_CLK_MAC1] = {
		"mac1", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_MAC1CDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_MAC1CDR, 8),
	},

	[A1_CLK_MAC1TX] = {
		"mac1_tx", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_MAC1TXCDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_MAC1TXCDR, 8),
	},

	[A1_CLK_BT0] = {
		"bt0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_BT0CDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_BT0CDR, 8),
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[A1_CLK_PWM] = {
		"pwm", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_PWMCDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_PWMCDR, 8),
		.gate = { CGU_REG_CLKGR1, 15 },
	},

	[A1_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[A1_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { A1_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[A1_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[A1_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[A1_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 8 },
	},

	[A1_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 9 },
	},

	[A1_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 10 },
	},

	[A1_CLK_OTG0] = {
		"otg0", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},

	[A1_CLK_OTG1] = {
		"otg1", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 12 },
	},

	[A1_CLK_OTG2] = {
		"otg2", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[A1_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { A1_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 16 },
	},

	[A1_CLK_SSI1] = {
		"ssi1", CGU_CLK_GATE,
		.parents = { A1_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 17 },
	},

	[A1_CLK_I2C0] = {
		"i2c0", CGU_CLK_GATE,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[A1_CLK_I2C1] = {
		"i2c1", CGU_CLK_GATE,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[A1_CLK_RTC] = {
		"rtc_gate", CGU_CLK_GATE,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 22 },
	},

	[A1_CLK_HASH] = {
		"hash", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[A1_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 3 },
	},

	[A1_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[A1_CLK_VC8000D] = {
		"vc8000d", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB1, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 6 },
	},

	[A1_CLK_DTRNG] = {
		"dtrng", CGU_CLK_GATE,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 7 },
	},

	[A1_CLK_GMAC0] = {
		"gmac0", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 8 },
	},

	[A1_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 9 },
	},

	[A1_CLK_GMAC1] = {
		"gmac1", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[A1_CLK_SATA] = {
		"sata", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[A1_CLK_MONITOR] = {
		"monitor", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 12 },
	},

	[A1_CLK_JPEG] = {
		"jpeg", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB1, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 13 },
	},

	[A1_CLK_VDEV] = {
		"vdev", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_VDEVCDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_VDEVCDR, 8),
	},

	[A1_CLK_VDEA] = {
		"vdea", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_VDEACDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_VDEACDR, 8),
	},

	[A1_CLK_VDEM] = {
		"vdem", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_VDEMCDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_VDEMCDR, 8),
	},

	[A1_CLK_IPU] = {
		"ipu", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_IPUMCDR, 30, 2 },
		.div = A1_CDR_DIV(CGU_REG_IPUMCDR, 8),
	},

	/*
	 * HDMI gate shut with the VDE gate open hangs the bus on a read, so
	 * the unused-clock sweep must not pick them off one by one.
	 */
	[A1_CLK_VDE] = {
		"gate_vde", CGU_CLK_GATE,
		.flags = CLK_IGNORE_UNUSED,
		.parents = { A1_CLK_VDEA, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[A1_CLK_HDMI] = {
		"gate_hdmi", CGU_CLK_GATE,
		.flags = CLK_IGNORE_UNUSED,
		.parents = { A1_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 27 },
	},

	[A1_CLK_VGA] = {
		"gate_vga", CGU_CLK_GATE,
		.flags = CLK_IGNORE_UNUSED,
		.parents = { A1_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 26 },
	},

	[A1_CLK_IPU_GATE] = {
		"gate_ipu", CGU_CLK_GATE,
		.parents = { A1_CLK_IPU, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 29 },
	},

	[A1_CLK_AIP] = {
		"gate_aip", CGU_CLK_GATE,
		.parents = { A1_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 18 },
	},

	/* the I2S clocks: AIC0 (inner codec) on I2S0, AIC1 (HDMI) on I2S1 */

	[A1_CLK_I2S0T_MUX] = {
		"i2s0t_mux", CGU_CLK_MUX,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_I2S0TCDR, I2SCDR_PCS_SHIFT, 2 },
	},

	[A1_CLK_DIV_I2S0T] = {
		"div_i2s0t", CGU_CLK_FRAC,
		.parents = { A1_CLK_I2S0T_MUX, -1, -1, -1 },
		.frac = { CGU_REG_I2S0TCDR, I2SCDR_M_SHIFT, I2SCDR_M_BITS,
			  I2SCDR_N_SHIFT, I2SCDR_N_BITS },
	},

	[A1_CLK_CE_I2S0T] = {
		"ce_i2s0t", CGU_CLK_GATE,
		.parents = { A1_CLK_DIV_I2S0T, -1, -1, -1 },
		.gate = { CGU_REG_I2S0TCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

	[A1_CLK_I2S0R_MUX] = {
		"i2s0r_mux", CGU_CLK_MUX,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_I2S0RCDR, I2SCDR_PCS_SHIFT, 2 },
	},

	[A1_CLK_DIV_I2S0R] = {
		"div_i2s0r", CGU_CLK_FRAC,
		.parents = { A1_CLK_I2S0R_MUX, -1, -1, -1 },
		.frac = { CGU_REG_I2S0RCDR, I2SCDR_M_SHIFT, I2SCDR_M_BITS,
			  I2SCDR_N_SHIFT, I2SCDR_N_BITS },
	},

	[A1_CLK_CE_I2S0R] = {
		"ce_i2s0r", CGU_CLK_GATE,
		.parents = { A1_CLK_DIV_I2S0R, -1, -1, -1 },
		.gate = { CGU_REG_I2S0RCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

	[A1_CLK_I2S1T_MUX] = {
		"i2s1t_mux", CGU_CLK_MUX,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_I2S1TCDR, I2SCDR_PCS_SHIFT, 2 },
	},

	[A1_CLK_DIV_I2S1T] = {
		"div_i2s1t", CGU_CLK_FRAC,
		.parents = { A1_CLK_I2S1T_MUX, -1, -1, -1 },
		.frac = { CGU_REG_I2S1TCDR, I2SCDR_M_SHIFT, I2SCDR_M_BITS,
			  I2SCDR_N_SHIFT, I2SCDR_N_BITS },
	},

	[A1_CLK_CE_I2S1T] = {
		"ce_i2s1t", CGU_CLK_GATE,
		.parents = { A1_CLK_DIV_I2S1T, -1, -1, -1 },
		.gate = { CGU_REG_I2S1TCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

	[A1_CLK_I2S1R_MUX] = {
		"i2s1r_mux", CGU_CLK_MUX,
		.parents = A1_PLL_PARENTS,
		.mux = { CGU_REG_I2S1RCDR, I2SCDR_PCS_SHIFT, 2 },
	},

	[A1_CLK_DIV_I2S1R] = {
		"div_i2s1r", CGU_CLK_FRAC,
		.parents = { A1_CLK_I2S1R_MUX, -1, -1, -1 },
		.frac = { CGU_REG_I2S1RCDR, I2SCDR_M_SHIFT, I2SCDR_M_BITS,
			  I2SCDR_N_SHIFT, I2SCDR_N_BITS },
	},

	[A1_CLK_CE_I2S1R] = {
		"ce_i2s1r", CGU_CLK_GATE,
		.parents = { A1_CLK_DIV_I2S1R, -1, -1, -1 },
		.gate = { CGU_REG_I2S1RCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

	[A1_CLK_AIC0] = {
		"gate_aic0", CGU_CLK_GATE,
		.parents = { A1_CLK_DIV_I2S0T, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[A1_CLK_AIC1] = {
		"gate_aic1", CGU_CLK_GATE,
		.parents = { A1_CLK_DIV_I2S1T, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 23 },
	},
};

static struct ingenic_cgu *cgu;

static void __init a1_cgu_init(struct device_node *np)
{
	int retval;

	cgu = ingenic_cgu_new(a1_cgu_clocks, ARRAY_SIZE(a1_cgu_clocks), np);
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
CLK_OF_DECLARE_DRIVER(a1_cgu, "ingenic,a1-cgu", a1_cgu_init);
