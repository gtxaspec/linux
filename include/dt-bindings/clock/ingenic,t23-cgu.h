/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,t23-cgu DT binding.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order they appear in the T23 programmers manual
 *   - gates in order of their bit in the CLKGR* registers
 */

#ifndef __DT_BINDINGS_CLOCK_T23_CGU_H__
#define __DT_BINDINGS_CLOCK_T23_CGU_H__

#define T23_CLK_EXCLK		0
#define T23_CLK_RTCLK		1
#define T23_CLK_APLL		2
#define T23_CLK_MPLL		3
#define T23_CLK_OTGPHY		4
#define T23_CLK_SCLKA		5
#define T23_CLK_CPUMUX		6
#define T23_CLK_CPU		7
#define T23_CLK_L2CACHE		8
#define T23_CLK_AHB0		9
#define T23_CLK_AHB2		10
#define T23_CLK_PCLK		11
#define T23_CLK_DDR		12
#define T23_CLK_VPU		13
#define T23_CLK_MAC		14
#define T23_CLK_LCD		15
#define T23_CLK_MSCMUX		16
#define T23_CLK_MSC0		17
#define T23_CLK_MSC1		18
#define T23_CLK_SSI		19
#define T23_CLK_SFCDIV		20
#define T23_CLK_ISP		21
#define T23_CLK_CIM		22
#define T23_CLK_RSA		23
#define T23_CLK_NEMC		24
#define T23_CLK_EFUSE		25
#define T23_CLK_OTG		26
#define T23_CLK_SSI0		27
#define T23_CLK_SMB0		28
#define T23_CLK_SMB1		29
#define T23_CLK_SMB2		30
#define T23_CLK_AIC		31
#define T23_CLK_SADC		32
#define T23_CLK_UART0		33
#define T23_CLK_UART1		34
#define T23_CLK_UART2		35
#define T23_CLK_HASH		36
#define T23_CLK_SFC		37
#define T23_CLK_PDMA		38
#define T23_CLK_MIPI_CSI	39
#define T23_CLK_RISCV		40
#define T23_CLK_DES		41
#define T23_CLK_TCU		42
#define T23_CLK_VPUGATE		43
#define T23_CLK_DTRNG		44
#define T23_CLK_IPU		45
#define T23_CLK_IVDC		46
#define T23_CLK_GMAC		47
#define T23_CLK_AES		48
#define T23_CLK_OST		49
#define T23_CLK_USBPHY		50
#define T23_CLK_DIV_I2ST	51
#define T23_CLK_DIV_I2SR	52
#define T23_CLK_CE_I2ST		53
#define T23_CLK_CE_I2SR		54

#define T23_CLK_AHB2_MUX	55
#define T23_CLK_I2S_MUX		56
#endif /* __DT_BINDINGS_CLOCK_T23_CGU_H__ */
