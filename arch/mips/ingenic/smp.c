// SPDX-License-Identifier: GPL-2.0
/*
 * SMP support for the Ingenic XBurst2 cores
 *
 * The cores of the CPU are controlled through the CCU: a secondary core is
 * held in software reset, runs from the address in RER when released, and
 * is interrupted through a mailbox register of its own. Interrupts of the
 * interrupt controller and of the core OSTs reach a core only if its bit is
 * set in the CCU's peripheral and OST masks.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/atomic.h>
#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/sched/task_stack.h>
#include <linux/smp.h>
#include <linux/spinlock.h>

#include <asm/mipsregs.h>
#include <asm/smp.h>
#include <asm/smp-ops.h>

#include "smp.h"

/* CCU registers */
#define CCU_CSRR		0x040	/* bit n: core n held in reset */
#define CCU_PIMR		0x120	/* peripheral interrupt, bit n: core n */
#define CCU_MIMR		0x160	/* mailbox interrupt */
#define CCU_OIMR		0x1a0	/* core OST interrupt */
#define CCU_RER			0xf00	/* reset entry, a kernel virtual address */
#define CCU_MBR(cpu)		(0x1000 + (cpu) * 4)

struct xb2_smp_ctx xb2_smp_ctx;

static void __iomem *ccu;
static int ipi_irq;
static atomic_t ipi_action[NR_CPUS];
static DEFINE_RAW_SPINLOCK(ccu_lock);

static void xb2_ccu_set_masks(unsigned int cpu)
{
	guard(raw_spinlock_irqsave)(&ccu_lock);

	writel(readl(ccu + CCU_PIMR) | BIT(cpu), ccu + CCU_PIMR);
	writel(readl(ccu + CCU_MIMR) | BIT(cpu), ccu + CCU_MIMR);
	writel(readl(ccu + CCU_OIMR) | BIT(cpu), ccu + CCU_OIMR);
}

static void xb2_send_ipi_single(int cpu, unsigned int action)
{
	atomic_or(action, &ipi_action[cpu]);
	/* the action must be visible before the mailbox rings */
	smp_mb__after_atomic();
	writel(1, ccu + CCU_MBR(cpu));
}

static void xb2_send_ipi_mask(const struct cpumask *mask, unsigned int action)
{
	unsigned int cpu;

	for_each_cpu(cpu, mask)
		xb2_send_ipi_single(cpu, action);
}

static irqreturn_t xb2_ipi_irq(int irq, void *dev_id)
{
	unsigned int cpu = smp_processor_id();
	unsigned int action;

	writel(0, ccu + CCU_MBR(cpu));
	action = atomic_xchg(&ipi_action[cpu], 0);

	if (action & SMP_RESCHEDULE_YOURSELF)
		scheduler_ipi();
	if (action & SMP_CALL_FUNCTION)
		generic_smp_call_function_interrupt();

	return IRQ_HANDLED;
}

static void xb2_smp_setup(void)
{
	struct device_node *np;
	unsigned int possible = 0;
	u32 hwid;

	for_each_of_cpu_node(np) {
		hwid = of_get_cpu_hwid(np, 0);
		if (hwid >= nr_cpu_ids)
			continue;

		set_cpu_possible(hwid, true);
		set_cpu_present(hwid, true);
		__cpu_number_map[hwid] = hwid;
		__cpu_logical_map[hwid] = hwid;
		possible++;
	}

	pr_info("XBurst2 SMP: %u core(s)\n", possible);

	np = of_find_compatible_node(NULL, NULL, "ingenic,xburst2-ccu");
	if (!np) {
		pr_err("XBurst2 SMP: no CCU in the device tree\n");
		return;
	}

	ccu = of_iomap(np, 0);
	of_node_put(np);
	if (!ccu) {
		pr_err("XBurst2 SMP: cannot map the CCU\n");
		return;
	}

	/* Core 0 runs; the others wait in reset until they are booted */
	writel(readl(ccu + CCU_CSRR) | (GENMASK(possible - 1, 0) & ~1u),
	       ccu + CCU_CSRR);
}

static void xb2_prepare_cpus(unsigned int max_cpus)
{
	struct device_node *np;
	int err;

	/* The CPU interrupt controller is there only from here on */
	np = of_find_compatible_node(NULL, NULL, "ingenic,xburst2-ccu");
	if (np) {
		ipi_irq = irq_of_parse_and_map(np, 0);
		of_node_put(np);
	}

	if (!ccu || !ipi_irq) {
		pr_err("XBurst2 SMP: no mailbox interrupt, staying single core\n");
		init_cpu_present(cpumask_of(0));
		return;
	}

	err = request_irq(ipi_irq, xb2_ipi_irq, IRQF_PERCPU | IRQF_NO_THREAD,
			  "ingenic-ipi", NULL);
	if (err) {
		pr_err("XBurst2 SMP: cannot request the mailbox interrupt: %d\n", err);
		return;
	}

	xb2_ccu_set_masks(0);
}

static void xb2_init_secondary(void)
{
	unsigned int cpu = smp_processor_id();

	clear_c0_cause(CAUSEF_IP);
	clear_c0_status(ST0_IM);

	xb2_ccu_set_masks(cpu);
	/* The interrupt controller, the mailbox and the core OST */
	set_c0_status(STATUSF_IP2 | STATUSF_IP3 | STATUSF_IP4);
}

static void xb2_smp_finish(void)
{
	local_irq_enable();
}

static int xb2_boot_secondary(int cpu, struct task_struct *idle)
{
	unsigned long entry = (unsigned long)xb2_secondary_entry;
	u32 csrr;

	xb2_smp_ctx.sp = __KSTK_TOS(idle);
	xb2_smp_ctx.gp = (unsigned long)task_thread_info(idle);
	/* The core reads this with its caches off */
	dma_cache_wback_inv((unsigned long)&xb2_smp_ctx, sizeof(xb2_smp_ctx));
	dma_cache_wback_inv(entry, (unsigned long)xb2_secondary_entry_end - entry);

	pr_info("XBurst2 SMP: booting CPU%d\n", cpu);

	writel(entry, ccu + CCU_RER);

	csrr = readl(ccu + CCU_CSRR) | BIT(cpu);
	writel(csrr, ccu + CCU_CSRR);
	/* the reset entry and the context must be in place before the release */
	wmb();
	writel(csrr & ~BIT(cpu), ccu + CCU_CSRR);

	return 0;
}

static const struct plat_smp_ops xb2_smp_ops = {
	.send_ipi_single	= xb2_send_ipi_single,
	.send_ipi_mask		= xb2_send_ipi_mask,
	.init_secondary		= xb2_init_secondary,
	.smp_finish		= xb2_smp_finish,
	.boot_secondary		= xb2_boot_secondary,
	.smp_setup		= xb2_smp_setup,
	.prepare_cpus		= xb2_prepare_cpus,
};

int __init register_xburst2_smp_ops(void)
{
	if (boot_cpu_type() != CPU_XBURST ||
	    (read_c0_prid() & PRID_IMP_XBURST2_MASK) != PRID_IMP_XBURST2)
		return -ENODEV;

	register_smp_ops(&xb2_smp_ops);

	return 0;
}
