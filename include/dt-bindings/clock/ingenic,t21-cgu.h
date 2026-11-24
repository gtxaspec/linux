/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,t21-cgu DT binding.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order they appear in the T21 programmers manual
 *   - gates in order of their bit in the CLKGR* registers
 */

#ifndef __DT_BINDINGS_CLOCK_T21_CGU_H__
#define __DT_BINDINGS_CLOCK_T21_CGU_H__

#define T21_CLK_EXCLK		0
#define T21_CLK_RTCLK		1
#define T21_CLK_APLL		2
#define T21_CLK_MPLL		3
#define T21_CLK_VPLL		4
#define T21_CLK_OTGPHY		5
#define T21_CLK_SCLKA		6
#define T21_CLK_CPUMUX		7
#define T21_CLK_CPU		8
#define T21_CLK_L2CACHE		9
#define T21_CLK_AHB0		10
#define T21_CLK_AHB2		11
#define T21_CLK_PCLK		12
#define T21_CLK_DDR		13
#define T21_CLK_VPU		14
#define T21_CLK_MAC		15
#define T21_CLK_LCD		16
#define T21_CLK_MSCMUX		17
#define T21_CLK_MSC0		18
#define T21_CLK_MSC1		19
#define T21_CLK_SSIPLL		20
#define T21_CLK_ISP		21
#define T21_CLK_CIM		22
#define T21_CLK_NEMC		23
#define T21_CLK_EFUSE		24
#define T21_CLK_OTG		25
#define T21_CLK_SSI0		26
#define T21_CLK_SSI1		27
#define T21_CLK_SMB0		28
#define T21_CLK_SMB1		29
#define T21_CLK_UART0		30
#define T21_CLK_UART1		31
#define T21_CLK_UART2		32
#define T21_CLK_AIC		33
#define T21_CLK_SADC		34
#define T21_CLK_HASH		35
#define T21_CLK_SFC		36
#define T21_CLK_PDMA		37
#define T21_CLK_RISCV		38
#define T21_CLK_DES		39
#define T21_CLK_TCU		40
#define T21_CLK_HELIX		41
#define T21_CLK_DTRNG		42
#define T21_CLK_IPU		43
#define T21_CLK_GMAC		44
#define T21_CLK_AES		45
#define T21_CLK_AHB1		46
#define T21_CLK_OST		47
#define T21_CLK_EXCLK_DIV512	48
#define T21_CLK_RTC		49
#define T21_CLK_USBPHY		50
#define T21_CLK_DIV_I2S		51
#define T21_CLK_CE_I2S		52

#endif /* __DT_BINDINGS_CLOCK_T21_CGU_H__ */
