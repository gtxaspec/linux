// SPDX-License-Identifier: GPL-2.0
/*
 * Ingenic XBurst2 SoC OST timers
 *
 * The global OST is one 64-bit free-running counter that serves as the
 * clocksource. Each core has a 32-bit OST of its own, in a window of its
 * own, that serves as its clockevent and interrupts that core only (CPU
 * interrupt pin 4, through the CCU's per-core OST mask).
 *
 * Copyright (C) 2026 Alfonso Gamboa <gtxent@gmail.com>
 */

#include <linux/clk.h>
#include <linux/clockchips.h>
#include <linux/cpuhotplug.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/percpu.h>
#include <linux/sched_clock.h>

#define OST_CORE_STRIDE		0x100

/* Global OST */
#define GOST_CCR		0x00
#define GOST_ER			0x04
#define GOST_CR			0x08
#define GOST_CNTH		0x0c
#define GOST_CNTL		0x10

/* Core OST */
#define COST_CCR		0x00
#define COST_ER			0x04
#define COST_CR			0x08
#define COST_FR			0x0c
#define COST_MR			0x10
#define COST_DFR		0x14

/* Both count EXCLK undivided */
#define OST_CCR_DIV1		0

#define COST_MIN_DELTA		0xf
#define COST_MAX_DELTA		0xffffffff

struct xb2_ost {
	void __iomem *global;
	void __iomem *core;
	unsigned long rate;
	int irq;
};

static struct xb2_ost xb2_ost;
static DEFINE_PER_CPU(struct clock_event_device, xb2_ost_clkevt);

static void __iomem *xb2_cost_base(unsigned int cpu)
{
	return xb2_ost.core + cpu * OST_CORE_STRIDE;
}

static u64 xb2_gost_read(void)
{
	u32 hi, lo;

	do {
		hi = readl(xb2_ost.global + GOST_CNTH);
		lo = readl(xb2_ost.global + GOST_CNTL);
	} while (hi != readl(xb2_ost.global + GOST_CNTH));

	return (u64)hi << 32 | lo;
}

static u64 xb2_gost_clocksource_read(struct clocksource *cs)
{
	return xb2_gost_read();
}

static struct clocksource xb2_gost_cs = {
	.name	= "ingenic-xburst2-ost",
	.rating	= 400,
	.read	= xb2_gost_clocksource_read,
	.mask	= CLOCKSOURCE_MASK(64),
	.flags	= CLOCK_SOURCE_IS_CONTINUOUS,
};

static int xb2_cost_set_next_event(unsigned long delta,
				   struct clock_event_device *evt)
{
	void __iomem *base = xb2_cost_base(smp_processor_id());

	writel(0, base + COST_ER);
	/* An expiry that is still to be handled will reprogram the timer */
	if (readl(base + COST_FR))
		return 0;

	writel(delta, base + COST_DFR);
	writel(1, base + COST_CR);
	writel(0, base + COST_MR);
	writel(1, base + COST_ER);

	return 0;
}

static int xb2_cost_shutdown(struct clock_event_device *evt)
{
	void __iomem *base = xb2_cost_base(smp_processor_id());

	writel(0, base + COST_ER);
	writel(1, base + COST_MR);

	return 0;
}

static irqreturn_t xb2_cost_irq(int irq, void *dev_id)
{
	struct clock_event_device *evt = this_cpu_ptr(&xb2_ost_clkevt);
	void __iomem *base = xb2_cost_base(smp_processor_id());

	if (!readl(base + COST_FR))
		return IRQ_NONE;

	/* One-shot: the timer stops until the next event is programmed */
	writel(0, base + COST_ER);
	writel(0, base + COST_FR);

	evt->event_handler(evt);

	return IRQ_HANDLED;
}

static int xb2_cost_starting(unsigned int cpu)
{
	struct clock_event_device *evt = this_cpu_ptr(&xb2_ost_clkevt);
	void __iomem *base = xb2_cost_base(cpu);

	writel(0, base + COST_ER);
	writel(OST_CCR_DIV1, base + COST_CCR);
	writel(1, base + COST_CR);
	writel(1, base + COST_MR);

	evt->name = "ingenic-xburst2-ost";
	evt->features = CLOCK_EVT_FEAT_ONESHOT;
	evt->rating = 400;
	evt->cpumask = cpumask_of(cpu);
	evt->irq = xb2_ost.irq;
	evt->set_next_event = xb2_cost_set_next_event;
	evt->set_state_shutdown = xb2_cost_shutdown;
	evt->set_state_oneshot = xb2_cost_shutdown;
	evt->set_state_oneshot_stopped = xb2_cost_shutdown;
	evt->tick_resume = xb2_cost_shutdown;

	clockevents_config_and_register(evt, xb2_ost.rate, COST_MIN_DELTA,
					COST_MAX_DELTA);

	return 0;
}

static int xb2_cost_dying(unsigned int cpu)
{
	xb2_cost_shutdown(NULL);

	return 0;
}

static u64 notrace xb2_ost_sched_clock(void)
{
	return xb2_gost_read();
}

static int __init xb2_ost_init(struct device_node *np)
{
	struct clk *clk;
	int err;

	xb2_ost.global = of_iomap(np, 0);
	xb2_ost.core = of_iomap(np, 1);
	if (!xb2_ost.global || !xb2_ost.core) {
		pr_err("%pOF: failed to map the OST registers\n", np);
		return -ENXIO;
	}

	xb2_ost.irq = irq_of_parse_and_map(np, 0);
	if (!xb2_ost.irq) {
		pr_err("%pOF: failed to get the OST interrupt\n", np);
		return -EINVAL;
	}

	clk = of_clk_get(np, 0);
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	err = clk_prepare_enable(clk);
	if (err)
		return err;

	xb2_ost.rate = clk_get_rate(clk);

	writel(0, xb2_ost.global + GOST_ER);
	writel(OST_CCR_DIV1, xb2_ost.global + GOST_CCR);
	writel(1, xb2_ost.global + GOST_CR);
	writel(1, xb2_ost.global + GOST_ER);

	err = clocksource_register_hz(&xb2_gost_cs, xb2_ost.rate);
	if (err)
		return err;

	sched_clock_register(xb2_ost_sched_clock, 64, xb2_ost.rate);

	/*
	 * The CPU interrupt is enabled on the boot core here and on the other
	 * cores by the code that brings them up.
	 */
	err = request_irq(xb2_ost.irq, xb2_cost_irq,
			  IRQF_TIMER | IRQF_PERCPU | IRQF_NOBALANCING,
			  "ingenic-xburst2-ost", NULL);
	if (err)
		return err;

	return cpuhp_setup_state(CPUHP_AP_INGENIC_XB2_TIMER_STARTING,
				 "clockevents/ingenic-xburst2:starting",
				 xb2_cost_starting, xb2_cost_dying);
}
TIMER_OF_DECLARE(ingenic_t41_ost, "ingenic,t41-ost", xb2_ost_init);
