// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic XBurst2 SoC interrupt controller
 *
 * Every core has a register window of its own, each with 2 banks of 32
 * sources. A source is delivered to the core in whose window it is unmasked
 * and reaches that core's CPU interrupt pin 2 through the CCU's per-core
 * peripheral mask. The raw source state is shared between the windows.
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/cpumask.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/spinlock.h>

#define INTC_CORE_STRIDE	0x100
#define INTC_BANK_STRIDE	0x20
#define INTC_NUM_BANKS		2
#define INTC_NUM_IRQS		(INTC_NUM_BANKS * 32)

#define INTC_REG_IMSR		0x08
#define INTC_REG_IMCR		0x0c
#define INTC_REG_IPR		0x10

struct xb2_intc {
	void __iomem *base;
	struct irq_domain *domain;
	raw_spinlock_t lock;
	int parent_irq;
	/* the core each source is unmasked on */
	u8 route[INTC_NUM_IRQS];
};

static struct xb2_intc *xb2_intc;

static void __iomem *xb2_intc_reg(struct xb2_intc *intc, unsigned int cpu,
				  irq_hw_number_t hwirq, unsigned int reg)
{
	return intc->base + cpu * INTC_CORE_STRIDE +
	       (hwirq / 32) * INTC_BANK_STRIDE + reg;
}

static void xb2_intc_mask(struct irq_data *d)
{
	struct xb2_intc *intc = irq_data_get_irq_chip_data(d);

	guard(raw_spinlock)(&intc->lock);
	writel(BIT(d->hwirq % 32),
	       xb2_intc_reg(intc, intc->route[d->hwirq], d->hwirq, INTC_REG_IMSR));
}

static void xb2_intc_unmask(struct irq_data *d)
{
	struct xb2_intc *intc = irq_data_get_irq_chip_data(d);

	guard(raw_spinlock)(&intc->lock);
	writel(BIT(d->hwirq % 32),
	       xb2_intc_reg(intc, intc->route[d->hwirq], d->hwirq, INTC_REG_IMCR));
}

static int xb2_intc_set_affinity(struct irq_data *d,
				 const struct cpumask *dest, bool force)
{
	struct xb2_intc *intc = irq_data_get_irq_chip_data(d);
	unsigned int cpu;

	if (force)
		cpu = cpumask_first(dest);
	else
		cpu = cpumask_any_and(dest, cpu_online_mask);
	if (cpu >= nr_cpu_ids)
		return -EINVAL;

	guard(raw_spinlock)(&intc->lock);

	if (cpu == intc->route[d->hwirq]) {
		irq_data_update_effective_affinity(d, cpumask_of(cpu));
		return IRQ_SET_MASK_OK;
	}

	/* The old window keeps the source masked, the new one takes it over */
	writel(BIT(d->hwirq % 32),
	       xb2_intc_reg(intc, intc->route[d->hwirq], d->hwirq, INTC_REG_IMSR));
	intc->route[d->hwirq] = cpu;
	if (!irqd_irq_masked(d))
		writel(BIT(d->hwirq % 32),
		       xb2_intc_reg(intc, cpu, d->hwirq, INTC_REG_IMCR));
	irq_data_update_effective_affinity(d, cpumask_of(cpu));

	return IRQ_SET_MASK_OK;
}

static struct irq_chip xb2_intc_chip = {
	.name			= "xburst2-intc",
	.irq_mask		= xb2_intc_mask,
	.irq_mask_ack		= xb2_intc_mask,
	.irq_unmask		= xb2_intc_unmask,
	.irq_set_affinity	= xb2_intc_set_affinity,
	.flags			= IRQCHIP_SKIP_SET_WAKE,
};

static int xb2_intc_domain_map(struct irq_domain *d, unsigned int irq,
			       irq_hw_number_t hw)
{
	irq_set_chip_data(irq, d->host_data);
	irq_set_chip_and_handler(irq, &xb2_intc_chip, handle_level_irq);
	irq_set_noprobe(irq);

	return 0;
}

static const struct irq_domain_ops xb2_intc_domain_ops = {
	.map	= xb2_intc_domain_map,
	.xlate	= irq_domain_xlate_onecell,
};

static irqreturn_t xb2_intc_irq(int irq, void *dev_id)
{
	struct xb2_intc *intc = xb2_intc;
	unsigned int cpu = smp_processor_id();
	irqreturn_t ret = IRQ_NONE;
	unsigned int bank;
	unsigned long pending;
	int bit;

	for (bank = 0; bank < INTC_NUM_BANKS; bank++) {
		pending = readl(xb2_intc_reg(intc, cpu, bank * 32, INTC_REG_IPR));
		for_each_set_bit(bit, &pending, 32) {
			generic_handle_domain_irq(intc->domain, bank * 32 + bit);
			ret = IRQ_HANDLED;
		}
	}

	return ret;
}

static int __init xb2_intc_of_init(struct device_node *node,
				   struct device_node *parent)
{
	struct xb2_intc *intc;
	unsigned int cpu, bank;
	int err;

	intc = kzalloc_obj(*intc);
	if (!intc)
		return -ENOMEM;

	raw_spin_lock_init(&intc->lock);

	intc->base = of_iomap(node, 0);
	if (!intc->base) {
		err = -ENODEV;
		goto err_free;
	}

	intc->parent_irq = irq_of_parse_and_map(node, 0);
	if (!intc->parent_irq) {
		err = -EINVAL;
		goto err_unmap;
	}

	/* Mask everything in every core's window; unmasking picks the core */
	for_each_possible_cpu(cpu)
		for (bank = 0; bank < INTC_NUM_BANKS; bank++)
			writel(~0, xb2_intc_reg(intc, cpu, bank * 32, INTC_REG_IMSR));

	intc->domain = irq_domain_create_linear(of_fwnode_handle(node),
						INTC_NUM_IRQS,
						&xb2_intc_domain_ops, intc);
	if (!intc->domain) {
		err = -ENOMEM;
		goto err_unmap;
	}

	xb2_intc = intc;

	/*
	 * The CPU interrupt is enabled on the boot core here and on the other
	 * cores by the code that brings them up.
	 */
	err = request_irq(intc->parent_irq, xb2_intc_irq,
			  IRQF_PERCPU | IRQF_NO_THREAD, "xburst2-intc", intc);
	if (err)
		goto err_domain;

	return 0;

err_domain:
	irq_domain_remove(intc->domain);
err_unmap:
	iounmap(intc->base);
err_free:
	kfree(intc);
	xb2_intc = NULL;

	return err;
}
IRQCHIP_DECLARE(ingenic_t41_intc, "ingenic,t41-intc", xb2_intc_of_init);
