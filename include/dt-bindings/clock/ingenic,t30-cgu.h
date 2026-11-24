/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,t30-cgu DT binding.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order they appear in the T30 programmers manual
 *   - gates in order of their bit in the CLKGR* registers
 */

#ifndef __DT_BINDINGS_CLOCK_T30_CGU_H__
#define __DT_BINDINGS_CLOCK_T30_CGU_H__

#define T30_CLK_EXCLK		0
#define T30_CLK_RTCLK		1
#define T30_CLK_APLL		2
#define T30_CLK_MPLL		3
#define T30_CLK_VPLL		4
#define T30_CLK_EPLL		5
#define T30_CLK_OTGPHY		6
#define T30_CLK_SCLKA		7
#define T30_CLK_CPUMUX		8
#define T30_CLK_CPU		9
#define T30_CLK_L2CACHE		10
#define T30_CLK_AHB0		11
#define T30_CLK_AHB2		12
#define T30_CLK_PCLK		13
#define T30_CLK_DDR		14
#define T30_CLK_VPU		15
#define T30_CLK_MAC		16
#define T30_CLK_LCD		17
#define T30_CLK_MSCMUX		18
#define T30_CLK_MSC0		19
#define T30_CLK_MSC1		20
#define T30_CLK_SSIPLL		21
#define T30_CLK_ISP		22
#define T30_CLK_CIM		23
#define T30_CLK_NEMC		24
#define T30_CLK_EFUSE		25
#define T30_CLK_OTG		26
#define T30_CLK_SSI0		27
#define T30_CLK_SSI1		28
#define T30_CLK_SMB0		29
#define T30_CLK_SMB1		30
#define T30_CLK_SMB2		31
#define T30_CLK_UART0		32
#define T30_CLK_UART1		33
#define T30_CLK_AIC		34
#define T30_CLK_DMIC		35
#define T30_CLK_SADC		36
#define T30_CLK_SFC		37
#define T30_CLK_PDMA		38
#define T30_CLK_CSI		39
#define T30_CLK_DES		40
#define T30_CLK_TCU		41
#define T30_CLK_HELIX		42
#define T30_CLK_IPU		43
#define T30_CLK_NCU		44
#define T30_CLK_GMAC		45
#define T30_CLK_AES		46
#define T30_CLK_AHB1		47
#define T30_CLK_MSCALER		48
#define T30_CLK_LDC		49
#define T30_CLK_OST		50
#define T30_CLK_RADIX		51
#define T30_CLK_EXCLK_DIV512	52
#define T30_CLK_RTC		53
#define T30_CLK_USBPHY		54
#define T30_CLK_DIV_I2S		55
#define T30_CLK_CE_I2S		56

#endif /* __DT_BINDINGS_CLOCK_T30_CGU_H__ */
