/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,t41-cgu DT binding.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order of their registers
 *   - gates that stand alone
 */

#ifndef __DT_BINDINGS_CLOCK_T41_CGU_H__
#define __DT_BINDINGS_CLOCK_T41_CGU_H__

#define T41_CLK_EXCLK        0
#define T41_CLK_RTCLK        1
#define T41_CLK_APLL         2
#define T41_CLK_MPLL         3
#define T41_CLK_VPLL         4
#define T41_CLK_SCLKA        5
#define T41_CLK_CPUMUX       6
#define T41_CLK_CPU          7
#define T41_CLK_L2CACHE      8
#define T41_CLK_AHB0         9
#define T41_CLK_AHB2_MUX     10
#define T41_CLK_AHB2         11
#define T41_CLK_PCLK         12
#define T41_CLK_DDR          13
#define T41_CLK_RSA          14
#define T41_CLK_SSISLV       15
#define T41_CLK_MAC          16
#define T41_CLK_SFC0         17
#define T41_CLK_LCD          18
#define T41_CLK_MSC0         19
#define T41_CLK_MSC1         20
#define T41_CLK_SSI          21
#define T41_CLK_SFC1         22
#define T41_CLK_ISPM         23
#define T41_CLK_CIM          24
#define T41_CLK_LDC          25
#define T41_CLK_ISPS         26
#define T41_CLK_ISPA         27
#define T41_CLK_BT0          28
#define T41_CLK_EL200        29
#define T41_CLK_PWMDIV       30
#define T41_CLK_VO           31
#define T41_CLK_SLV          32
#define T41_CLK_PWM          33
#define T41_CLK_TCU          34
#define T41_CLK_RTC          35
#define T41_CLK_DES          36
#define T41_CLK_MIPI_CSI     37
#define T41_CLK_PDMA         38
#define T41_CLK_SSI1         39
#define T41_CLK_UART5        40
#define T41_CLK_UART4        41
#define T41_CLK_UART3        42
#define T41_CLK_UART2        43
#define T41_CLK_UART1        44
#define T41_CLK_UART0        45
#define T41_CLK_SADC         46
#define T41_CLK_DMIC         47
#define T41_CLK_SMB2         48
#define T41_CLK_SMB1         49
#define T41_CLK_SMB0         50
#define T41_CLK_SSI0         51
#define T41_CLK_OTG          52
#define T41_CLK_HASH         53
#define T41_CLK_EFUSE        54
#define T41_CLK_NEMC         55
#define T41_CLK_IVDC         56
#define T41_CLK_APB0         57
#define T41_CLK_RJPEG        58
#define T41_CLK_OST          59
#define T41_CLK_MONITOR      60
#define T41_CLK_I2D          61
#define T41_CLK_DRAWBOX      62
#define T41_CLK_AES          63
#define T41_CLK_GMAC         64
#define T41_CLK_LZMA         65
#define T41_CLK_IPU          66
#define T41_CLK_DTRNG        67
#define T41_CLK_AIC          68
#define T41_CLK_I2S_MUX      69
#define T41_CLK_DIV_I2ST     70
#define T41_CLK_DIV_I2SR     71
#define T41_CLK_CE_I2ST      72
#define T41_CLK_CE_I2SR      73
#define T41_CLK_I2SR_MUX     74

#endif /* __DT_BINDINGS_CLOCK_T41_CGU_H__ */
