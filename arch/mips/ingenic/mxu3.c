// SPDX-License-Identifier: GPL-2.0
/*
 * XBurst2 MXU3 SIMD unit (coprocessor 2) support
 *
 * MXU3 has 32 512-bit vector registers, 4 vector sum registers and a
 * control/status register (MCSR). User tasks get Status.CU2 on their first
 * MXU3 instruction; the context is saved when such a task is switched out
 * and restored lazily on its next MXU3 instruction, as for the XBurst1 MXU2.
 * Nothing stays in the registers across a switch, so a task may move
 * between the cores freely.
 */

#include <linux/bug.h>
#include <linux/cpuhotplug.h>
#include <linux/context_tracking.h>
#include <linux/export.h>
#include <linux/init.h>
#include <linux/irqflags.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/stddef.h>

#include <asm/cop2.h>
#include <asm/cpu.h>
#include <asm/cpu-info.h>
#include <asm/cpu-type.h>
#include <asm/hazards.h>
#include <asm/mipsregs.h>
#include <asm/processor.h>
#include <asm/setup.h>
#include <asm/traps.h>

#define MCSR_CAUSE_E	BIT(17)	/* unimplemented operation, always enabled */
#define MCSR_CAUSE_V	BIT(16)
#define MCSR_CAUSE_Z	BIT(15)
#define MCSR_CAUSE_O	BIT(14)
#define MCSR_CAUSE_U	BIT(13)
#define MCSR_CAUSE_I	BIT(12)
#define MCSR_CAUSE	GENMASK(17, 12)
#define MCSR_ENABLES	GENMASK(11, 7)

extern asmlinkage void handle_mxu3_fpe(void);

bool xburst_mxu3_present;

/* cfcmxu $1, mcsr and ctcmxu mcsr, $1: the assembler has no MXU3 */
static u32 mcsr_read(void)
{
	u32 v;

	__asm__ __volatile__(".set push\n\t.set noat\n\t"
			     ".word 0x7010f843\n\t"
			     "move %0, $1\n\t"
			     ".set pop"
			     : "=r" (v));

	return v;
}

static void mcsr_write(u32 v)
{
	__asm__ __volatile__(".set push\n\t.set noat\n\t"
			     "move $1, %0\n\t"
			     ".word 0x7031f803\n\t"
			     ".set pop"
			     : : "r" (v));
}

static unsigned int mxu3_enable(void)
{
	unsigned int status = read_c0_status();

	write_c0_status(status | ST0_CU2);
	_ehb();

	return status;
}

asmlinkage void do_mxu3_fpe(struct pt_regs *regs)
{
	enum ctx_state prev_state = exception_enter();
	unsigned int status;
	int code = FPE_FLTUNK;
	u32 mcsr, cause;

	/*
	 * Cause must be cleared before anything can switch the task out:
	 * writing MCSR back with a Cause bit whose Enable is set raises the
	 * exception again, in the kernel.
	 */
	status = mxu3_enable();
	mcsr = mcsr_read();
	mcsr_write(mcsr & ~MCSR_CAUSE);
	write_c0_status(status);
	local_irq_enable();

	die_if_kernel("MXU3 FP exception in kernel code", regs);

	cause = mcsr & MCSR_CAUSE & (((mcsr & MCSR_ENABLES) << 5) | MCSR_CAUSE_E);
	if (cause & MCSR_CAUSE_V)
		code = FPE_FLTINV;
	else if (cause & MCSR_CAUSE_Z)
		code = FPE_FLTDIV;
	else if (cause & MCSR_CAUSE_O)
		code = FPE_FLTOVF;
	else if (cause & MCSR_CAUSE_U)
		code = FPE_FLTUND;
	else if (cause & MCSR_CAUSE_I)
		code = FPE_FLTRES;
	force_sig_fault(SIGFPE, code, (void __user *)regs->cp0_epc);

	exception_exit(prev_state);
}

/*
 * Kernel use of the unit by a closed-source module's SIMD routines. The task
 * that called into the kernel may own live MXU3 registers (the CU2 notifier
 * below loads them lazily and nothing saves them on kernel entry), so the
 * section keeps them in a per-CPU buffer and puts them back at the end.
 * Preemption stays off in between: the buffer belongs to the CPU.
 */
static DEFINE_PER_CPU(struct xburst_mxu3_state, kernel_mxu3_ctx);
static DEFINE_PER_CPU(unsigned int, kernel_mxu3_status);

void kernel_mxuv3_begin_partial(unsigned int num_regs)
{
	unsigned int status;

	/* One buffer per CPU: a section must not nest in interrupt context */
	WARN_ON_ONCE(!in_task());
	preempt_disable();
	status = mxu3_enable();
	__this_cpu_write(kernel_mxu3_status, status);
	xburst_mxu3_save(this_cpu_ptr(&kernel_mxu3_ctx));
}
EXPORT_SYMBOL_GPL(kernel_mxuv3_begin_partial);

void kernel_mxuv3_end(void)
{
	xburst_mxu3_restore(this_cpu_ptr(&kernel_mxu3_ctx));
	write_c0_status(__this_cpu_read(kernel_mxu3_status));
	preempt_enable();
}
EXPORT_SYMBOL_GPL(kernel_mxuv3_end);

static int xburst_mxu3_cu2_call(struct notifier_block *nfb,
				unsigned long action, void *data)
{
	struct pt_regs *regs = data;
	unsigned long flags;
	unsigned int status;

	if (action != CU2_EXCEPTION || !user_mode(regs))
		return NOTIFY_OK;

	local_irq_save(flags);
	regs->cp0_status |= ST0_CU2;
	status = mxu3_enable();
	xburst_mxu3_restore(&current->thread.mxu3);
	write_c0_status(status);
	local_irq_restore(flags);

	return NOTIFY_STOP;
}

static int xburst_mxu3_cpu_online(unsigned int cpu)
{
	if (!(read_c0_config1() & MIPS_CONF1_C2))
		pr_warn("XBurst MXU3 absent on CPU%u: tasks using it must not run there\n",
			cpu);

	return 0;
}

static int __init xburst_mxu3_init(void)
{
	unsigned int config1;

	/* the save and restore address one 256-bit half at a time */
	BUILD_BUG_ON(sizeof(((struct xburst_mxu3_state *)0)->vpr[0]) != 64);

	if (boot_cpu_type() != CPU_XBURST ||
	    (current_cpu_data.processor_id & PRID_IMP_MASK) !=
	    PRID_IMP_XBURST2_MXU3)
		return 0;

	config1 = read_c0_config1();
	if (!(config1 & MIPS_CONF1_C2)) {
		pr_info("XBurst MXU3 absent (Config1 %08x)\n", config1);
		return 0;
	}

	set_except_vector(XBURST_EXCCODE_MFPE, handle_mxu3_fpe);
	cu2_notifier(xburst_mxu3_cu2_call, 0);
	xburst_mxu3_present = true;
	pr_info("XBurst MXU3 enabled (Config1 %08x)\n", config1);

	return 0;
}
early_initcall(xburst_mxu3_init);

static int __init xburst_mxu3_hotplug_init(void)
{
	if (!xburst_mxu3_present)
		return 0;

	return cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "mips/ingenic-mxu3:online",
				 xburst_mxu3_cpu_online, NULL);
}
late_initcall(xburst_mxu3_hotplug_init);
