// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Support for Ingenic SoCs
 *
 * Copyright (C) 2009-2010, Lars-Peter Clausen <lars@metafoo.de>
 * Copyright (C) 2011, Maarten ter Huurne <maarten@treewalker.org>
 * Copyright (C) 2020 Paul Cercueil <paul@crapouillou.net>
 */

#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_fdt.h>
#include <linux/of_reserved_mem.h>
#include <linux/overflow.h>
#include <linux/pm.h>
#include <linux/sizes.h>
#include <linux/soc/ingenic/rmem.h>
#include <linux/suspend.h>
#include <linux/types.h>

#include <asm/bootinfo.h>
#include <asm/io.h>
#include <asm/machine.h>
#include <asm/reboot.h>
#include <asm/sections.h>

/*
 * Thingino's U-Boot places the media stack's carve-outs at boot and passes
 * them as rmem=<size>@<base> (and ispmem= on the T10 and T20); libimp reads
 * them back from /proc/cmdline. The board device tree describes each as a
 * disabled no-map /reserved-memory node of that name; move the node to where
 * the boot loader put the region and enable it, so that the reserved memory
 * code keeps the kernel off it. A size of 0 leaves the region disabled.
 */
static u8 ingenic_fdt_buf[SZ_32K] __initdata __aligned(8);

static int __init ingenic_fdt_writable(void)
{
	void *fdt = initial_boot_params;
	int ret;

	if (fdt == ingenic_fdt_buf)
		return 0;

	ret = fdt_open_into(fdt, ingenic_fdt_buf, sizeof(ingenic_fdt_buf));
	if (ret)
		return ret;

	return early_init_dt_verify(ingenic_fdt_buf, __pa(ingenic_fdt_buf)) ?
	       0 : -EINVAL;
}

static int __init ingenic_carveout_param(const char *name, char *p)
{
	phys_addr_t size, base, end;
	__be32 reg[4], *cell = reg;
	char node_name[32];
	void *fdt;
	int node, ret;

	if (!p)
		return -EINVAL;

	size = memparse(p, &p);
	if (*p != '@')
		return -EINVAL;
	base = memparse(p + 1, &p);

	if (!PAGE_ALIGNED(base) || !PAGE_ALIGNED(size) ||
	    check_add_overflow(base, size, &end) ||
	    (size && dt_root_addr_cells < 2 && upper_32_bits((u64)end - 1))) {
		pr_err("%s=: bad region %pa+%pa\n", name, &base, &size);
		return -EINVAL;
	}

	if (size && base < __pa_symbol(_end) && end > __pa_symbol(_text)) {
		pr_err("%s=: region %pa+%pa overlaps the kernel\n", name, &base, &size);
		return -EINVAL;
	}

	ret = ingenic_fdt_writable();
	if (ret) {
		pr_err("%s=: cannot update the device tree (%d)\n", name, ret);
		return 0;
	}
	fdt = initial_boot_params;

	node = fdt_path_offset(fdt, "/reserved-memory");
	if (node >= 0)
		node = fdt_subnode_offset(fdt, node, name);
	if (node < 0) {
		pr_warn("%s=: no /reserved-memory/%s node, region not reserved\n",
			name, name);
		return 0;
	}

	if (!size) {
		ret = fdt_setprop_string(fdt, node, "status", "disabled");
	} else {
		if (dt_root_addr_cells == 2)
			*cell++ = cpu_to_fdt32(upper_32_bits((u64)base));
		*cell++ = cpu_to_fdt32(lower_32_bits((u64)base));
		if (dt_root_size_cells == 2)
			*cell++ = cpu_to_fdt32(upper_32_bits((u64)size));
		*cell++ = cpu_to_fdt32(lower_32_bits((u64)size));

		snprintf(node_name, sizeof(node_name), "%s@%llx", name, (u64)base);
		ret = fdt_setprop(fdt, node, "reg", reg, (cell - reg) * sizeof(*reg));
		if (!ret)
			ret = fdt_set_name(fdt, node, node_name);
		if (!ret)
			ret = fdt_setprop_string(fdt, node, "status", "okay");
	}

	if (ret)
		pr_err("%s=: cannot update the device tree (%d)\n", name, ret);
	else
		early_init_dt_verify(fdt, __pa(fdt));

	return 0;
}

static int __init ingenic_rmem_param(char *p)
{
	return ingenic_carveout_param("rmem", p);
}
early_param("rmem", ingenic_rmem_param);

static int __init ingenic_ispmem_param(char *p)
{
	return ingenic_carveout_param("ispmem", p);
}
early_param("ispmem", ingenic_ispmem_param);

/*
 * The out-of-tree media modules built against this kernel still look the
 * region up through this call; new code finds it by its memory-region.
 */
int ingenic_rmem_region(phys_addr_t *base, phys_addr_t *size)
{
	struct device_node *parent, *np;
	struct reserved_mem *rmem = NULL;

	parent = of_find_node_by_path("/reserved-memory");
	if (!parent)
		return -ENODEV;

	for_each_available_child_of_node(parent, np) {
		if (of_node_name_eq(np, "rmem")) {
			rmem = of_reserved_mem_lookup(np);
			of_node_put(np);
			break;
		}
	}
	of_node_put(parent);

	if (!rmem)
		return -ENODEV;

	*base = rmem->base;
	*size = rmem->size;
	return 0;
}
EXPORT_SYMBOL_GPL(ingenic_rmem_region);

static __init char *ingenic_get_system_type(unsigned long machtype)
{
	switch (machtype) {
	case MACH_INGENIC_A1:
		return "A1";
	case MACH_INGENIC_T10:
		return "T10";
	case MACH_INGENIC_T20:
		return "T20";
	case MACH_INGENIC_T21:
		return "T21";
	case MACH_INGENIC_T23:
		return "T23";
	case MACH_INGENIC_T30:
		return "T30";
	case MACH_INGENIC_T31:
		return "T31";
	case MACH_INGENIC_T32:
		return "T32";
	case MACH_INGENIC_T32PRO:
		return "T32 Pro";
	case MACH_INGENIC_T33:
		return "T33";
	case MACH_INGENIC_T40:
		return "T40";
	case MACH_INGENIC_T41:
		return "T41";
	case MACH_INGENIC_X2100:
		return "X2100";
	case MACH_INGENIC_X2000H:
		return "X2000H";
	case MACH_INGENIC_X2000E:
		return "X2000E";
	case MACH_INGENIC_X2000:
		return "X2000";
	case MACH_INGENIC_X1830:
		return "X1830";
	case MACH_INGENIC_X1000E:
		return "X1000E";
	case MACH_INGENIC_X1000:
		return "X1000";
	case MACH_INGENIC_JZ4780:
		return "JZ4780";
	case MACH_INGENIC_JZ4775:
		return "JZ4775";
	case MACH_INGENIC_JZ4770:
		return "JZ4770";
	case MACH_INGENIC_JZ4760B:
		return "JZ4760B";
	case MACH_INGENIC_JZ4760:
		return "JZ4760";
	case MACH_INGENIC_JZ4755:
		return "JZ4755";
	case MACH_INGENIC_JZ4750:
		return "JZ4750";
	case MACH_INGENIC_JZ4725B:
		return "JZ4725B";
	case MACH_INGENIC_JZ4730:
		return "JZ4730";
	default:
		return "JZ4740";
	}
}

#define INGENIC_CGU_BASE	0x10000000
#define JZ4750_CGU_CPCCR_ECS	BIT(30)
#define JZ4760_CGU_CPCCR_ECS	BIT(31)

static __init void ingenic_force_12M_ext(const void *fdt, unsigned int mask)
{
	const __be32 *prop;
	unsigned int cpccr;
	void __iomem *cgu;
	bool use_div;
	int offset;

	offset = fdt_path_offset(fdt, "/ext");
	if (offset < 0)
		return;

	prop = fdt_getprop(fdt, offset, "clock-frequency", NULL);
	if (!prop)
		return;

	/*
	 * If the external oscillator is 24 MHz, enable the /2 divider to
	 * drive it down to 12 MHz, since this is what the hardware can work
	 * with.
	 * The 16 MHz cutoff value is arbitrary; setting it to 12 MHz would not
	 * work as the crystal frequency (as reported in the Device Tree) might
	 * be slightly above this value.
	 */
	use_div = be32_to_cpup(prop) >= 16000000;

	cgu = ioremap(INGENIC_CGU_BASE, 0x4);
	if (!cgu)
		return;

	cpccr = ioread32(cgu);
	if (use_div)
		cpccr |= mask;
	else
		cpccr &= ~mask;
	iowrite32(cpccr, cgu);

	iounmap(cgu);
}

static __init const void *ingenic_fixup_fdt(const void *fdt, const void *match_data)
{
	/*
	 * Old devicetree files for the qi,lb60 board did not have a /memory
	 * node. Hardcode the memory info here.
	 */
	if (!fdt_node_check_compatible(fdt, 0, "qi,lb60") &&
	    fdt_path_offset(fdt, "/memory") < 0)
		early_init_dt_add_memory_arch(0, SZ_32M);

	mips_machtype = (unsigned long)match_data;
	system_type = ingenic_get_system_type(mips_machtype);

	switch (mips_machtype) {
	case MACH_INGENIC_JZ4750:
	case MACH_INGENIC_JZ4755:
		ingenic_force_12M_ext(fdt, JZ4750_CGU_CPCCR_ECS);
		break;
	case MACH_INGENIC_JZ4760:
		ingenic_force_12M_ext(fdt, JZ4760_CGU_CPCCR_ECS);
		break;
	default:
		break;
	}

	return fdt;
}

static const struct of_device_id ingenic_of_match[] __initconst = {
	{ .compatible = "ingenic,jz4730", .data = (void *)MACH_INGENIC_JZ4730 },
	{ .compatible = "ingenic,jz4740", .data = (void *)MACH_INGENIC_JZ4740 },
	{ .compatible = "ingenic,jz4725b", .data = (void *)MACH_INGENIC_JZ4725B },
	{ .compatible = "ingenic,jz4750", .data = (void *)MACH_INGENIC_JZ4750 },
	{ .compatible = "ingenic,jz4755", .data = (void *)MACH_INGENIC_JZ4755 },
	{ .compatible = "ingenic,jz4760", .data = (void *)MACH_INGENIC_JZ4760 },
	{ .compatible = "ingenic,jz4760b", .data = (void *)MACH_INGENIC_JZ4760B },
	{ .compatible = "ingenic,jz4770", .data = (void *)MACH_INGENIC_JZ4770 },
	{ .compatible = "ingenic,jz4775", .data = (void *)MACH_INGENIC_JZ4775 },
	{ .compatible = "ingenic,jz4780", .data = (void *)MACH_INGENIC_JZ4780 },
	{ .compatible = "ingenic,x1000", .data = (void *)MACH_INGENIC_X1000 },
	{ .compatible = "ingenic,x1000e", .data = (void *)MACH_INGENIC_X1000E },
	{ .compatible = "ingenic,x1830", .data = (void *)MACH_INGENIC_X1830 },
	{ .compatible = "ingenic,x2000", .data = (void *)MACH_INGENIC_X2000 },
	{ .compatible = "ingenic,x2000e", .data = (void *)MACH_INGENIC_X2000E },
	{ .compatible = "ingenic,x2000h", .data = (void *)MACH_INGENIC_X2000H },
	{ .compatible = "ingenic,x2100", .data = (void *)MACH_INGENIC_X2100 },
	{ .compatible = "ingenic,t10", .data = (void *)MACH_INGENIC_T10 },
	{ .compatible = "ingenic,t20", .data = (void *)MACH_INGENIC_T20 },
	{ .compatible = "ingenic,t21", .data = (void *)MACH_INGENIC_T21 },
	{ .compatible = "ingenic,t23", .data = (void *)MACH_INGENIC_T23 },
	{ .compatible = "ingenic,t30", .data = (void *)MACH_INGENIC_T30 },
	{ .compatible = "ingenic,t31", .data = (void *)MACH_INGENIC_T31 },
	{ .compatible = "ingenic,t32", .data = (void *)MACH_INGENIC_T32 },
	{ .compatible = "ingenic,t32pro", .data = (void *)MACH_INGENIC_T32PRO },
	{ .compatible = "ingenic,t33", .data = (void *)MACH_INGENIC_T33 },
	{ .compatible = "ingenic,t40", .data = (void *)MACH_INGENIC_T40 },
	{ .compatible = "ingenic,t41", .data = (void *)MACH_INGENIC_T41 },
	{ .compatible = "ingenic,a1", .data = (void *)MACH_INGENIC_A1 },
	{}
};

MIPS_MACHINE(ingenic) = {
	.matches = ingenic_of_match,
	.fixup_fdt = ingenic_fixup_fdt,
};

static void ingenic_wait_instr(void)
{
	__asm__(".set push;\n"
		".set mips3;\n"
		"wait;\n"
		".set pop;\n"
	);
}

static void ingenic_halt(void)
{
	for (;;)
		ingenic_wait_instr();
}

static int ingenic_pm_enter(suspend_state_t state)
{
	ingenic_wait_instr();

	return 0;
}

static const struct platform_suspend_ops ingenic_pm_ops = {
	.valid = suspend_valid_only_mem,
	.enter = ingenic_pm_enter,
};

static int __init ingenic_pm_init(void)
{
	if (boot_cpu_type() == CPU_XBURST) {
		if (IS_ENABLED(CONFIG_PM_SLEEP))
			suspend_set_ops(&ingenic_pm_ops);
		_machine_halt = ingenic_halt;
	}

	return 0;

}
late_initcall(ingenic_pm_init);
