/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
/*
 * This header provides clock numbers for the ingenic,a1-cgu DT binding.
 *
 * They are roughly ordered as:
 *   - external clocks
 *   - PLLs
 *   - muxes/dividers in the order of their registers
 *   - gates that stand alone
 */

#ifndef __DT_BINDINGS_CLOCK_A1_CGU_H__
#define __DT_BINDINGS_CLOCK_A1_CGU_H__

#define A1_CLK_EXCLK	0
#define A1_CLK_RTCLK	1
#define A1_CLK_APLL	2
#define A1_CLK_MPLL	3
#define A1_CLK_VPLL	4
#define A1_CLK_EPLL	5
#define A1_CLK_SCLKA	6
#define A1_CLK_CPUMUX	7
#define A1_CLK_CPU	8
#define A1_CLK_L2CACHE	9
#define A1_CLK_AHB0	10
#define A1_CLK_AHB2_MUX	11
#define A1_CLK_AHB2	12
#define A1_CLK_PCLK	13
#define A1_CLK_AHB1	14
#define A1_CLK_DDR	15
#define A1_CLK_SSI	16
#define A1_CLK_SFC0	17
#define A1_CLK_SFC1	18
#define A1_CLK_MSC0	19
#define A1_CLK_MSC1	20
#define A1_CLK_MAC0	21
#define A1_CLK_MAC0TX	22
#define A1_CLK_MACPTP	23
#define A1_CLK_MAC1	24
#define A1_CLK_MAC1TX	25
#define A1_CLK_BT0	26
#define A1_CLK_PWM	27
#define A1_CLK_TCU	28
#define A1_CLK_OST	29
#define A1_CLK_EFUSE	30
#define A1_CLK_NEMC	31
#define A1_CLK_UART0	32
#define A1_CLK_UART1	33
#define A1_CLK_UART2	34
#define A1_CLK_OTG0	35
#define A1_CLK_OTG1	36
#define A1_CLK_OTG2	37
#define A1_CLK_SSI0	38
#define A1_CLK_SSI1	39
#define A1_CLK_I2C0	40
#define A1_CLK_I2C1	41
#define A1_CLK_RTC	42
#define A1_CLK_PDMA	43
#define A1_CLK_HASH	44
#define A1_CLK_DES	45
#define A1_CLK_DTRNG	46
#define A1_CLK_GMAC0	47
#define A1_CLK_GMAC1	48
#define A1_CLK_AES	49
#define A1_CLK_SATA	50
#define A1_CLK_MONITOR	51
#define A1_CLK_JPEG	52
#define A1_CLK_VC8000D	53
#define A1_CLK_VDEV	54
#define A1_CLK_VDEA	55
#define A1_CLK_VDEM	56
#define A1_CLK_IPU	57
#define A1_CLK_VDE	58
#define A1_CLK_HDMI	59
#define A1_CLK_VGA	60
#define A1_CLK_IPU_GATE	61
#define A1_CLK_AIP	62

#endif /* __DT_BINDINGS_CLOCK_A1_CGU_H__ */
