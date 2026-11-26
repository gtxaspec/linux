/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __INGENIC_SMP_H__
#define __INGENIC_SMP_H__

#ifndef __ASSEMBLER__
/* What a secondary core needs to reach the kernel; set by the boot core */
struct xb2_smp_ctx {
	unsigned long sp;
	unsigned long gp;
};

extern struct xb2_smp_ctx xb2_smp_ctx;
extern void xb2_secondary_entry(void);
extern char xb2_secondary_entry_end[];
void __noreturn xb2_cpu_park(unsigned long start, unsigned long size,
			     unsigned long linesz);
#endif

#endif
