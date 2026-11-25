// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic T41 SoC CGU driver
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/clk-provider.h>
#include <linux/io.h>
#include <linux/of.h>

#include <dt-bindings/clock/ingenic,t41-cgu.h>

#include "cgu.h"
#include "pm.h"

/* CGU register offsets */
#define CGU_REG_CPCCR		0x00
#define CGU_REG_CLKGR0		0x20
#define CGU_REG_CLKGR1		0x28
#define CGU_REG_APLL		0x10
#define CGU_REG_MPLL		0x14
#define CGU_REG_DDRCDR		0x2c
#define CGU_REG_RSACDR		0x4c
#define CGU_REG_SSISLVCDR	0x50
#define CGU_REG_MACCDR		0x54
#define CGU_REG_SFC0CDR		0x60
#define CGU_REG_LPCDR		0x64
#define CGU_REG_MSC0CDR		0x68
#define CGU_REG_MSC1CDR		0x6c
#define CGU_REG_I2STCDR		0x70
#define CGU_REG_SSICDR		0x74
#define CGU_REG_SFC1CDR		0x7c
#define CGU_REG_ISPMCDR		0x80
#define CGU_REG_I2SRCDR		0x84
#define CGU_REG_CIMCDR		0x90
#define CGU_REG_LDCCDR		0xa0
#define CGU_REG_ISPSCDR		0xa8
#define CGU_REG_ISPACDR		0xac
#define CGU_REG_BT0CDR		0xb8
#define CGU_REG_EL200CDR	0xbc
#define CGU_REG_VPLL		0xe0
#define CGU_REG_PWMCDR		0xfc

/*
 * Every peripheral has a CDR of its own: mux in bits 31:30, change enable
 * 29, busy 28, stop 27 and the divider in the low bits. The MSC dividers
 * count in fours, and their mux takes the PLLs only, as the vendor U-Boot's
 * table has it (no EXCLK at index 2).
 */
#define T41_CDR_CE		29
#define T41_CDR_BUSY		28
#define T41_CDR_STOP		27

/*
 * I2S{T,R}CDR, parent * M / N. The vendor kernel's clk-t41.c has the source
 * select, a 9 bit M at bit 20, a 20 bit N and the clock enable at bit 29
 * in both registers, each with its own source select.
 */
#define I2SCDR_PCS_SHIFT	30
#define I2SCDR_CE_BIT		29
#define I2SCDR_M_SHIFT		20
#define I2SCDR_M_BITS		9
#define I2SCDR_N_SHIFT		0
#define I2SCDR_N_BITS		20

/*
 * The PLLs: EXCLK * 2 * (M + 1) / ((N + 1) * 2^OD0 * (OD1 + 1)) with M in
 * bits 28:20, N in 19:14, OD0 in 13:11 and OD1 in 10:7. The boot loader
 * sets them up and the kernel only reads them, see od1_bits.
 */
#define T41_PLL(_reg) {				\
	.reg = _reg,				\
	.rate_multiplier = 2,			\
	.m_shift = 20,				\
	.m_bits = 9,				\
	.m_offset = 1,				\
	.n_shift = 14,				\
	.n_bits = 6,				\
	.n_offset = 1,				\
	.od_shift = 11,				\
	.od_bits = 3,				\
	.od_max = 128,				\
	.od_encoding = t41_pll_od_encoding,	\
	.od1_shift = 7,				\
	.od1_bits = 4,				\
	.bypass_bit = -1,			\
	.enable_bit = 0,			\
	.stable_bit = 3,			\
}

/* OD0 is a power of two: the register holds its logarithm */
static const s8 t41_pll_od_encoding[128] = {
	[0 ... 127] = -1,
	[0] = 0, [1] = 1, [3] = 2, [7] = 3,
	[15] = 4, [31] = 5, [63] = 6, [127] = 7,
};

static const struct ingenic_cgu_clk_info t41_cgu_clocks[] = {
	[T41_CLK_EXCLK] = {
		"ext", CGU_CLK_EXT,
	},

	[T41_CLK_RTCLK] = {
		"rtc", CGU_CLK_EXT,
	},

	[T41_CLK_APLL] = {
		"apll", CGU_CLK_PLL,
		.parents = { T41_CLK_EXCLK, -1, -1, -1 },
		.pll = T41_PLL(CGU_REG_APLL),
	},

	[T41_CLK_MPLL] = {
		"mpll", CGU_CLK_PLL,
		.parents = { T41_CLK_EXCLK, -1, -1, -1 },
		.pll = T41_PLL(CGU_REG_MPLL),
	},

	[T41_CLK_VPLL] = {
		"vpll", CGU_CLK_PLL,
		.parents = { T41_CLK_EXCLK, -1, -1, -1 },
		.pll = T41_PLL(CGU_REG_VPLL),
	},

	[T41_CLK_SCLKA] = {
		"sclk_a", CGU_CLK_MUX,
		.parents = { -1, T41_CLK_EXCLK, T41_CLK_APLL, -1 },
		.mux = { CGU_REG_CPCCR, 30, 2 },
	},

	[T41_CLK_CPUMUX] = {
		"cpu_mux", CGU_CLK_MUX,
		.parents = { -1, T41_CLK_SCLKA, T41_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 28, 2 },
	},

	[T41_CLK_CPU] = {
		"cpu", CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T41_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 0, 1, 4, 22, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 16 },
	},

	[T41_CLK_L2CACHE] = {
		"l2cache", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T41_CLK_CPUMUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 4, 1, 4, 22, -1, -1 },
	},

	[T41_CLK_AHB0] = {
		"ahb0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T41_CLK_SCLKA, T41_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 26, 2 },
		.div = { CGU_REG_CPCCR, 8, 1, 4, 21, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 10 },
	},

	[T41_CLK_AHB2_MUX] = {
		"ahb2_mux", CGU_CLK_MUX,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T41_CLK_SCLKA, T41_CLK_MPLL, -1 },
		.mux = { CGU_REG_CPCCR, 24, 2 },
	},

	[T41_CLK_AHB2] = {
		"ahb2", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T41_CLK_AHB2_MUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 12, 1, 4, 20, -1, -1 },
	},

	[T41_CLK_PCLK] = {
		"pclk", CGU_CLK_DIV,
		.flags = CLK_IS_CRITICAL,
		.parents = { T41_CLK_AHB2_MUX, -1, -1, -1 },
		.div = { CGU_REG_CPCCR, 16, 1, 4, 20, -1, -1 },
	},

	[T41_CLK_DDR] = {
		"ddr", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { -1, T41_CLK_SCLKA, T41_CLK_MPLL, -1 },
		.mux = { CGU_REG_DDRCDR, 30, 2 },
		.div = { CGU_REG_DDRCDR, 0, 1, 4, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 31 },
	},

	[T41_CLK_RSA] = {
		"rsa", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_RSACDR, 30, 2 },
		.div = { CGU_REG_RSACDR, 0, 1, 4, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 27 },
	},

	[T41_CLK_SSISLV] = {
		"ssislv", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_SSISLVCDR, 30, 2 },
		.div = { CGU_REG_SSISLVCDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
	},

	[T41_CLK_MAC] = {
		"mac", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_MACCDR, 30, 2 },
		.div = { CGU_REG_MACCDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
	},

	[T41_CLK_SFC0] = {
		"sfc0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_SFC0CDR, 30, 2 },
		.div = { CGU_REG_SFC0CDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 21 },
	},

	[T41_CLK_LCD] = {
		"lcd", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_LPCDR, 30, 2 },
		.div = { CGU_REG_LPCDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 24 },
	},

	[T41_CLK_MSC0] = {
		"msc0", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_MSC0CDR, 30, 2 },
		.div = { CGU_REG_MSC0CDR, 0, 4, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 4 },
	},

	[T41_CLK_MSC1] = {
		"msc1", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_MSC1CDR, 30, 2 },
		.div = { CGU_REG_MSC1CDR, 0, 4, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 5 },
	},

	[T41_CLK_SSI] = {
		"ssi", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_SSICDR, 30, 2 },
		.div = { CGU_REG_SSICDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
	},

	[T41_CLK_SFC1] = {
		"sfc1", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_SFC1CDR, 30, 2 },
		.div = { CGU_REG_SFC1CDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 12 },
	},

	[T41_CLK_ISPM] = {
		"ispm", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_ISPMCDR, 30, 2 },
		.div = { CGU_REG_ISPMCDR, 0, 1, 4, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR0, 23 },
	},

	[T41_CLK_CIM] = {
		"cim", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_CIMCDR, 30, 2 },
		.div = { CGU_REG_CIMCDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
	},

	[T41_CLK_LDC] = {
		"ldc", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_LDCCDR, 30, 2 },
		.div = { CGU_REG_LDCCDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 13 },
	},

	[T41_CLK_ISPS] = {
		"isps", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_ISPSCDR, 30, 2 },
		.div = { CGU_REG_ISPSCDR, 0, 1, 4, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
	},

	[T41_CLK_ISPA] = {
		"ispa", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_ISPACDR, 30, 2 },
		.div = { CGU_REG_ISPACDR, 0, 1, 4, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
	},

	[T41_CLK_BT0] = {
		"bt0", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_BT0CDR, 30, 2 },
		.div = { CGU_REG_BT0CDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
	},

	[T41_CLK_EL200] = {
		"el200", CGU_CLK_MUX | CGU_CLK_DIV | CGU_CLK_GATE,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_EL200CDR, 30, 2 },
		.div = { CGU_REG_EL200CDR, 0, 1, 4, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
		.gate = { CGU_REG_CLKGR1, 0 },
	},

	[T41_CLK_PWMDIV] = {
		"pwm_div", CGU_CLK_MUX | CGU_CLK_DIV,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_PWMCDR, 30, 2 },
		.div = { CGU_REG_PWMCDR, 0, 1, 8, T41_CDR_CE, T41_CDR_BUSY, T41_CDR_STOP },
	},

	[T41_CLK_VO] = {
		"vo", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 26 },
	},

	[T41_CLK_SLV] = {
		"slv", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 10 },
	},

	[T41_CLK_PWM] = {
		"pwm", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 7 },
	},

	[T41_CLK_TCU] = {
		"tcu_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 30 },
	},

	[T41_CLK_RTC] = {
		"rtc_gate", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 29 },
	},

	[T41_CLK_DES] = {
		"des", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 28 },
	},

	[T41_CLK_MIPI_CSI] = {
		"mipi_csi", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 25 },
	},

	[T41_CLK_PDMA] = {
		"pdma", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 22 },
	},

	[T41_CLK_SSI1] = {
		"ssi1", CGU_CLK_GATE,
		.parents = { T41_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 20 },
	},

	[T41_CLK_UART5] = {
		"uart5", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 19 },
	},

	[T41_CLK_UART4] = {
		"uart4", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 18 },
	},

	[T41_CLK_UART3] = {
		"uart3", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 17 },
	},

	[T41_CLK_UART2] = {
		"uart2", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 16 },
	},

	[T41_CLK_UART1] = {
		"uart1", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 15 },
	},

	[T41_CLK_UART0] = {
		"uart0", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 14 },
	},

	[T41_CLK_SADC] = {
		"sadc", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 13 },
	},

	[T41_CLK_DMIC] = {
		"dmic", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 12 },
	},

	[T41_CLK_SMB2] = {
		"smb2", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 9 },
	},

	[T41_CLK_SMB1] = {
		"smb1", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 8 },
	},

	[T41_CLK_SMB0] = {
		"smb0", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 7 },
	},

	[T41_CLK_SSI0] = {
		"ssi0", CGU_CLK_GATE,
		.parents = { T41_CLK_SSI, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 6 },
	},

	[T41_CLK_OTG] = {
		"otg", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 3 },
	},

	[T41_CLK_HASH] = {
		"hash", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 2 },
	},

	[T41_CLK_EFUSE] = {
		"efuse", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 1 },
	},

	[T41_CLK_NEMC] = {
		"nemc", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 0 },
	},

	[T41_CLK_IVDC] = {
		"ivdc", CGU_CLK_GATE,
		.parents = { T41_CLK_ISPA, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 17 },
	},

	[T41_CLK_APB0] = {
		"apb0", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T41_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 15 },
	},

	[T41_CLK_RJPEG] = {
		"rjpeg", CGU_CLK_GATE,
		.parents = { T41_CLK_ISPA, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 14 },
	},

	[T41_CLK_OST] = {
		"ost_gate", CGU_CLK_GATE,
		.flags = CLK_IS_CRITICAL,
		.parents = { T41_CLK_EXCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 11 },
	},

	[T41_CLK_MONITOR] = {
		"monitor", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 9 },
	},

	[T41_CLK_I2D] = {
		"i2d", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 8 },
	},

	[T41_CLK_DRAWBOX] = {
		"drawbox", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 6 },
	},

	[T41_CLK_AES] = {
		"aes", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB2, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 5 },
	},

	[T41_CLK_GMAC] = {
		"gmac", CGU_CLK_GATE,
		.parents = { T41_CLK_MAC, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 4 },
	},

	[T41_CLK_LZMA] = {
		"lzma", CGU_CLK_GATE,
		.parents = { T41_CLK_ISPA, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 3 },
	},

	[T41_CLK_IPU] = {
		"ipu", CGU_CLK_GATE,
		.parents = { T41_CLK_AHB0, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 2 },
	},

	[T41_CLK_DTRNG] = {
		"dtrng", CGU_CLK_GATE,
		.parents = { T41_CLK_PCLK, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR1, 1 },
	},

	/* the I2S sources: playback in I2STCDR, capture in I2SRCDR */

	[T41_CLK_I2S_MUX] = {
		"i2s_mux", CGU_CLK_MUX,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_I2STCDR, I2SCDR_PCS_SHIFT, 2 },
	},

	[T41_CLK_I2SR_MUX] = {
		"i2sr_mux", CGU_CLK_MUX,
		.parents = { T41_CLK_SCLKA, T41_CLK_MPLL, T41_CLK_VPLL, -1 },
		.mux = { CGU_REG_I2SRCDR, I2SCDR_PCS_SHIFT, 2 },
	},

	[T41_CLK_DIV_I2ST] = {
		"div_i2st", CGU_CLK_FRAC,
		.parents = { T41_CLK_I2S_MUX, -1, -1, -1 },
		.frac = { CGU_REG_I2STCDR, I2SCDR_M_SHIFT, I2SCDR_M_BITS,
			  I2SCDR_N_SHIFT, I2SCDR_N_BITS },
	},

	[T41_CLK_DIV_I2SR] = {
		"div_i2sr", CGU_CLK_FRAC,
		.parents = { T41_CLK_I2SR_MUX, -1, -1, -1 },
		.frac = { CGU_REG_I2SRCDR, I2SCDR_M_SHIFT, I2SCDR_M_BITS,
			  I2SCDR_N_SHIFT, I2SCDR_N_BITS },
	},

	/* the clock enables inside the CDRs, set to enable */

	[T41_CLK_CE_I2ST] = {
		"ce_i2st", CGU_CLK_GATE,
		.parents = { T41_CLK_DIV_I2ST, -1, -1, -1 },
		.gate = { CGU_REG_I2STCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

	[T41_CLK_CE_I2SR] = {
		"ce_i2sr", CGU_CLK_GATE,
		.parents = { T41_CLK_DIV_I2SR, -1, -1, -1 },
		.gate = { CGU_REG_I2SRCDR, I2SCDR_CE_BIT, .clear_to_gate = true },
	},

	[T41_CLK_AIC] = {
		"aic", CGU_CLK_GATE,
		.parents = { T41_CLK_DIV_I2ST, -1, -1, -1 },
		.gate = { CGU_REG_CLKGR0, 11 },
	},
};

static struct ingenic_cgu *cgu;

static void __init t41_cgu_init(struct device_node *np)
{
	int retval;

	cgu = ingenic_cgu_new(t41_cgu_clocks, ARRAY_SIZE(t41_cgu_clocks), np);
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
CLK_OF_DECLARE_DRIVER(t41_cgu, "ingenic,t41-cgu", t41_cgu_init);
