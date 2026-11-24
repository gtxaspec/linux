/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XBurst1 MXU2 context save and restore
 *
 * The MXU2 instructions are hand-encoded: the kernel is not assembled for
 * MXU2. su1q/lu1q $vrN, N*16(a0) = SPECIAL2 | a0 << 21 | byte offset << 11
 * | N << 6 | funct (0x1c su1q, 0x14 lu1q); cfcmxu/ctcmxu move MCSR through
 * $1. Callers have Status.CU2 set.
 */

#include <asm/asm.h>
#include <asm/asm-offsets.h>
#include <asm/regdef.h>

#define SU1Q_A0		0x7080001c
#define LU1Q_A0		0x70800014
#define CFCMXU_AT_MCSR	0x4bc10ffd
#define CTCMXU_MCSR_AT	0x4bc10ffc
#define MCSR_CAUSE	0x0003f000

	.set	noreorder
	.set	noat

/* insn $vrN, N*16(a0) for N = 0..31 */
	.macro	vpr_all insn
	.set	vr, 0
	.rept	32
	.word	\insn | ((XBURST_CP2_VPR + vr * 16) << 11) | (vr << 6)
	.set	vr, vr + 1
	.endr
	.endm

/* void xburst_cop2_save(struct xburst_cop2_state *a0) */
LEAF(xburst_cop2_save)
	.word	CFCMXU_AT_MCSR
	sw	$1, XBURST_CP2_MCSR(a0)
	vpr_all	SU1Q_A0
	jr	ra
	 nop
	END(xburst_cop2_save)

/* void xburst_cop2_restore(struct xburst_cop2_state *a0) */
LEAF(xburst_cop2_restore)
	/*
	 * Writing a Cause bit whose Enable is set raises MXU2 FP exception
	 * on the ctcmxu itself, here in the kernel. Cause only reports the
	 * last operation, so drop it.
	 */
	lw	$1, XBURST_CP2_MCSR(a0)
	li	t0, ~MCSR_CAUSE
	and	$1, $1, t0
	.word	CTCMXU_MCSR_AT
	vpr_all	LU1Q_A0
	jr	ra
	 nop
	END(xburst_cop2_restore)
