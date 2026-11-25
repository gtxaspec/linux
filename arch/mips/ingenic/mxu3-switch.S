/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XBurst2 MXU3 context save and restore
 *
 * The MXU3 instructions are hand-encoded: the kernel is not assembled for
 * MXU3. A 512-bit register moves as two 256-bit halves, sao/lao $vrN[n],
 * off(a1) with the byte offset in units of 32 and n the half. The sums are
 * reached through $vr0 (mfsum/mtsum), and the control register through $1
 * (cfcmxu/ctcmxu register 31). Callers have Status.CU2 set.
 */

#include <asm/asm.h>
#include <asm/asm-offsets.h>
#include <asm/regdef.h>

#define SAO_A1		0x70a000d5	/* sao $vrp[n], off(a1) */
#define LAO_A1		0x70a01811	/* lao $vrd[n], off(a1) */
#define MFSUM_VR0	0x4a60000f	/* mfsum $vr0, $vsN */
#define MTSUM_VR0	0x4a60001d	/* mtsum $vsN, $vr0 */
#define CFCMXU_AT_MCSR	0x7010f843
#define CTCMXU_MCSR_AT	0x7031f803
#define MCSR_CAUSE	0x0003f000

	.set	noreorder
	.set	noat

/* both halves of $vr<r> to and from a1 */
	.macro	save_vr r
	.word	SAO_A1 | (0 << 16) | (\r << 11) | (0 << 9)
	.word	SAO_A1 | (1 << 16) | (\r << 11) | (1 << 9)
	.endm

	.macro	load_vr r
	.word	LAO_A1 | (0 << 16) | (0 << 14) | (\r << 6)
	.word	LAO_A1 | (1 << 16) | (1 << 14) | (\r << 6)
	.endm

/* void xburst_mxu3_save(struct xburst_mxu3_state *a0) */
LEAF(xburst_mxu3_save)
	.word	CFCMXU_AT_MCSR
	sw	$1, XBURST_MXU3_MCSR(a0)
	PTR_ADDIU a1, a0, XBURST_MXU3_VPR
	.set	vr, 0
	.rept	32
	save_vr	vr
	PTR_ADDIU a1, a1, 64
	.set	vr, vr + 1
	.endr
	/* the sums go through $vr0, which is saved by now */
	PTR_ADDIU a1, a0, XBURST_MXU3_VSR
	.set	vs, 0
	.rept	4
	.word	MFSUM_VR0 | (vs << 11)
	save_vr	0
	PTR_ADDIU a1, a1, 64
	.set	vs, vs + 1
	.endr
	/* mfsum took $vr0: put it back, a fork keeps running on the registers */
	PTR_ADDIU a1, a0, XBURST_MXU3_VPR
	load_vr	0
	jr	ra
	 nop
	END(xburst_mxu3_save)

/* void xburst_mxu3_restore(struct xburst_mxu3_state *a0) */
LEAF(xburst_mxu3_restore)
	/*
	 * Writing a Cause bit whose Enable is set raises the MXU3 FP
	 * exception on the ctcmxu itself, here in the kernel. Cause only
	 * reports the last operation, so drop it.
	 */
	lw	$1, XBURST_MXU3_MCSR(a0)
	li	t0, ~MCSR_CAUSE
	and	$1, $1, t0
	.word	CTCMXU_MCSR_AT
	/* the sums go through $vr0, which is loaded again below */
	PTR_ADDIU a1, a0, XBURST_MXU3_VSR
	.set	vs, 0
	.rept	4
	load_vr	0
	.word	MTSUM_VR0 | (vs << 6)
	PTR_ADDIU a1, a1, 64
	.set	vs, vs + 1
	.endr
	PTR_ADDIU a1, a0, XBURST_MXU3_VPR
	.set	vr, 0
	.rept	32
	load_vr	vr
	PTR_ADDIU a1, a1, 64
	.set	vr, vr + 1
	.endr
	jr	ra
	 nop
	END(xburst_mxu3_restore)
