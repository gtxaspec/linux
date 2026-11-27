/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,t32-cgu,
 * ingenic,t32pro-cgu and ingenic,t33-cgu DT bindings.
 *
 * The T33 uses the T32's numbers. It has no VPLL and no ISPS divider; its
 * CIM divider is a fractional one the binding does not describe yet.
 * The I2S source mux feeds both I2S dividers; on the T33 it picks SCLK_A or
 * MPLL only.
 *
 * The T32 Pro uses the T32's numbers too. It has the T32's VPLL and three-way
 * muxes, but a RISC-V divider (T33_CLK_RISCV) where the T32 has its ISPS one.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order of their CDR registers
 *   - gates in order of their bit in the CLKGR* registers
 */

#ifndef __DT_BINDINGS_CLOCK_T32_CGU_H__
#define __DT_BINDINGS_CLOCK_T32_CGU_H__

#define T32_CLK_EXCLK		0
#define T32_CLK_RTCLK		1
#define T32_CLK_APLL		2
#define T32_CLK_MPLL		3
#define T32_CLK_VPLL		4
#define T32_CLK_SCLKA		5
#define T32_CLK_CPUMUX		6
#define T32_CLK_CPU		7
#define T32_CLK_L2CACHE		8
#define T32_CLK_AHB0		9
#define T32_CLK_AHB2		10
#define T32_CLK_PCLK		11
#define T32_CLK_DDR		12
#define T32_CLK_VPU		13
#define T32_CLK_RSA		14
#define T32_CLK_MAC		15
#define T32_CLK_SFC0_PRE	16
#define T32_CLK_SFC0		17
#define T32_CLK_PWM		18
#define T32_CLK_SSISLV		19
#define T32_CLK_LCD		20
#define T32_CLK_MSC0		21
#define T32_CLK_LDC		22
#define T32_CLK_SSI		23
#define T32_CLK_CIM		24
#define T32_CLK_ISP		25
#define T32_CLK_ISPS		26
#define T32_CLK_ISPA		27
#define T32_CLK_TNPU		28
#define T32_CLK_MSC1		29
#define T32_CLK_SFC1_PRE	30
#define T32_CLK_SFC1		31
#define T32_CLK_NEMC		32
#define T32_CLK_EFUSE		33
#define T32_CLK_OTG		34
#define T32_CLK_SSI0		35
#define T32_CLK_SMB0		36
#define T32_CLK_SMB1		37
#define T32_CLK_DMIC		38
#define T32_CLK_SADC		39
#define T32_CLK_UART0		40
#define T32_CLK_UART1		41
#define T32_CLK_UART2		42
#define T32_CLK_SMB2		43
#define T32_CLK_HASH		44
#define T32_CLK_PDMA		45
#define T32_CLK_MIPI_CSI	46
#define T32_CLK_DES		47
#define T32_CLK_TCU		48
#define T32_CLK_DTRNG		49
#define T32_CLK_IPU		50
#define T32_CLK_GMAC		51
#define T32_CLK_AES		52
#define T32_CLK_AHB1		53
#define T32_CLK_IVDC		54
#define T32_CLK_LZMA0		55
#define T32_CLK_DVP		56
#define T32_CLK_OST		57
#define T32_CLK_I2D		58
#define T32_CLK_SLV		59
#define T32_CLK_LZMA1		60

#define T32_CLK_AHB2_MUX	61
#define T32_CLK_AIC		62
#define T32_CLK_I2S_MUX		63
#define T32_CLK_DIV_I2ST	64
#define T32_CLK_DIV_I2SR	65
#define T32_CLK_CE_I2ST		66
#define T32_CLK_CE_I2SR		67

/* T32 Pro and T33 */
#define T33_CLK_RISCV		68

/* T32 and T33 */
#define T32_CLK_OTGPHY		69
#endif /* __DT_BINDINGS_CLOCK_T32_CGU_H__ */
