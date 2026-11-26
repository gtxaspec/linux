/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Where a released XBurst2 secondary core starts: it runs with the caches
 * and the CP0 in their reset state, from the address in the CCU's RER.
 * And where a core taken offline stops before it is put back into reset.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <asm/asm.h>
#include <asm/cacheops.h>
#include <asm/mipsregs.h>
#include <asm/regdef.h>

#define IFU_SMALL_BUFFER_OFF	(7 << 3)	/* CP0 Config7 bits 5:3 */

	.text
	.set	noreorder
	.align	5
LEAF(xb2_secondary_entry)
	mfc0	v0, CP0_CONFIG, 7
	ori	v0, v0, IFU_SMALL_BUFFER_OFF
	mtc0	v0, CP0_CONFIG, 7

	/* KSEG0 cacheable, then the same as a core that was never reset */
	mfc0	v0, CP0_CONFIG
	li	v1, ~CONF_CM_CMASK
	and	v0, v0, v1
	ori	v0, v0, CONF_CM_CACHABLE_NONCOHERENT
	mtc0	v0, CP0_CONFIG

	li	v0, CAUSEF_DC
	mtc0	v0, CP0_CAUSE
	mtc0	zero, CP0_COUNT
	li	v0, ST0_CU0
	mtc0	v0, CP0_STATUS

	PTR_LA	t0, xb2_smp_ctx
	LONG_L	sp, 0(t0)
	LONG_L	gp, LONGSIZE(t0)

	PTR_LA	ra, smp_bootstrap
	jr	ra
	 nop
	END(xb2_secondary_entry)
	.globl	xb2_secondary_entry_end
xb2_secondary_entry_end:

/*
 * xb2_cpu_park(start, size, linesz): write the L1 data cache back by index
 * and sleep in WAIT for good. Nothing is stored to memory from here on, so
 * the reset that follows loses no data.
 */
LEAF(xb2_cpu_park)
	PTR_ADDU	a1, a0, a1
1:	cache	Index_Writeback_Inv_D, 0(a0)
	PTR_ADDU	a0, a0, a2
	bne	a0, a1, 1b
	 nop
	sync
2:	wait
	b	2b
	 nop
	END(xb2_cpu_park)
