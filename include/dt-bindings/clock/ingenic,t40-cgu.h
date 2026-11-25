/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,t40-cgu DT binding.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order of their registers
 *   - gates that stand alone
 */

#ifndef __DT_BINDINGS_CLOCK_T40_CGU_H__
#define __DT_BINDINGS_CLOCK_T40_CGU_H__

#define T40_CLK_EXCLK        0
#define T40_CLK_RTCLK        1
#define T40_CLK_APLL         2
#define T40_CLK_MPLL         3
#define T40_CLK_VPLL         4
#define T40_CLK_EPLL         5
#define T40_CLK_SCLKA        6
#define T40_CLK_CPUMUX       7
#define T40_CLK_CPU          8
#define T40_CLK_L2CACHE      9
#define T40_CLK_AHB0         10
#define T40_CLK_AHB2_MUX     11
#define T40_CLK_AHB2         12
#define T40_CLK_PCLK         13
#define T40_CLK_DDR          14
#define T40_CLK_EL150        15
#define T40_CLK_RSA          16
#define T40_CLK_MAC          17
#define T40_CLK_SFC          18
#define T40_CLK_LCD          19
#define T40_CLK_MSC0         20
#define T40_CLK_MSC1         21
#define T40_CLK_SSI          22
#define T40_CLK_ISP          23
#define T40_CLK_CIM0         24
#define T40_CLK_CIM1         25
#define T40_CLK_CIM2         26
#define T40_CLK_BSCALER      27
#define T40_CLK_BT0          28
#define T40_CLK_TCU          29
#define T40_CLK_DES          30
#define T40_CLK_MIPI_CSI     31
#define T40_CLK_PDMA         32
#define T40_CLK_SSI1         33
#define T40_CLK_HASH         34
#define T40_CLK_SLV          35
#define T40_CLK_UART3        36
#define T40_CLK_UART2        37
#define T40_CLK_UART1        38
#define T40_CLK_UART0        39
#define T40_CLK_SADC         40
#define T40_CLK_DMIC         41
#define T40_CLK_SMB3         42
#define T40_CLK_SMB2         43
#define T40_CLK_SMB1         44
#define T40_CLK_SMB0         45
#define T40_CLK_SSI0         46
#define T40_CLK_OTG          47
#define T40_CLK_EFUSE        48
#define T40_CLK_NEMC         49
#define T40_CLK_APB0         50
#define T40_CLK_OST          51
#define T40_CLK_MONITOR      52
#define T40_CLK_I2D          53
#define T40_CLK_MIPI_DSI     54
#define T40_CLK_DRAWBOX      55
#define T40_CLK_AES          56
#define T40_CLK_GMAC         57
#define T40_CLK_AHB1         58
#define T40_CLK_IPU          59
#define T40_CLK_DTRNG        60

#endif /* __DT_BINDINGS_CLOCK_T40_CGU_H__ */
