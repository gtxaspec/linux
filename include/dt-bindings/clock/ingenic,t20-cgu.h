/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,t20-cgu DT binding.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order they appear in the CPM register map
 *   - gates in order of their bit in the CLKGR* registers
 */

#ifndef __DT_BINDINGS_CLOCK_T20_CGU_H__
#define __DT_BINDINGS_CLOCK_T20_CGU_H__

#define T20_CLK_EXCLK		0
#define T20_CLK_RTCLK		1
#define T20_CLK_APLL		2
#define T20_CLK_MPLL		3
#define T20_CLK_VPLL		4
#define T20_CLK_SCLKA		5
#define T20_CLK_CPUMUX		6
#define T20_CLK_CPU		7
#define T20_CLK_L2CACHE		8
#define T20_CLK_AHB0		9
#define T20_CLK_AHB2		10
#define T20_CLK_PCLK		11
#define T20_CLK_DDR		12
#define T20_CLK_VPU		13
#define T20_CLK_MAC		14
#define T20_CLK_LCD		15
#define T20_CLK_MSCMUX		16
#define T20_CLK_MSC0		17
#define T20_CLK_MSC1		18
#define T20_CLK_SSIPLL		19
#define T20_CLK_ISP		20
#define T20_CLK_CIM		21
#define T20_CLK_NEMC		22
#define T20_CLK_EFUSE		23
#define T20_CLK_OTG		24
#define T20_CLK_SSI0		25
#define T20_CLK_SMB0		26
#define T20_CLK_SMB1		27
#define T20_CLK_UART0		28
#define T20_CLK_UART1		29
#define T20_CLK_UART2		30
#define T20_CLK_DMIC		31
#define T20_CLK_AIC		32
#define T20_CLK_SFC		33
#define T20_CLK_PDMA		34
#define T20_CLK_CSI		35
#define T20_CLK_DES		36
#define T20_CLK_TCU		37
#define T20_CLK_SADC		38
#define T20_CLK_IPU		39
#define T20_CLK_GMAC		40
#define T20_CLK_AES		41
#define T20_CLK_OST		42
#define T20_CLK_EXCLK_DIV512	43
#define T20_CLK_RTC		44
#define T20_CLK_DIV_I2S		45
#define T20_CLK_CE_I2S		46

#define T20_CLK_AHB2_MUX	47
#endif /* __DT_BINDINGS_CLOCK_T20_CGU_H__ */
