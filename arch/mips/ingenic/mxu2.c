// SPDX-License-Identifier: GPL-2.0
/*
 * XBurst1 MXU2 SIMD unit (coprocessor 2) support
 *
 * MXU2 has 32 128-bit vector registers and a control/status register
 * (MCSR). User tasks get Status.CU2 on their first MXU2 instruction; the
 * context is saved when such a task is switched out and restored lazily on
 * its next MXU2 instruction, as for the Octeon COP2.
 */

#include <linux/context_tracking.h>
#include <linux/init.h>
#include <linux/bug.h>
#include <linux/export.h>
#include <linux/irqflags.h>
#include <linux/percpu.h>
#include <linux/preempt.h>
#include <linux/notifier.h>
#include <linux/ptrace.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/stddef.h>

#include <asm/cop2.h>
#include <asm/cpu.h>
#include <asm/cpu-info.h>
#include <asm/cpu-type.h>
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

extern asmlinkage void handle_mxu_fpe(void);

/* cfcmxu $1, mcsr and ctcmxu mcsr, $1: the assembler has no MXU2 */
static u32 mcsr_read(void)
{
	u32 v;

	__asm__ __volatile__(".set push\n\t.set noat\n\t"
			     ".word 0x4bc10ffd\n\t"
			     "move %0, $1\n\t"
			     ".set pop"
			     : "=r" (v));

	return v;
}

static void mcsr_write(u32 v)
{
	__asm__ __volatile__(".set push\n\t.set noat\n\t"
			     "move $1, %0\n\t"
			     ".word 0x4bc10ffc\n\t"
			     ".set pop"
			     : : "r" (v));
}

asmlinkage void do_mxu_fpe(struct pt_regs *regs)
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
	status = read_c0_status();
	write_c0_status(status | ST0_CU2);
	mcsr = mcsr_read();
	mcsr_write(mcsr & ~MCSR_CAUSE);
	write_c0_status(status);
	local_irq_enable();

	die_if_kernel("MXU2 FP exception in kernel code", regs);

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
 * that called into the kernel may own live MXU2 registers (the CU2 notifier
 * above loads them lazily and nothing saves them on kernel entry), so the
 * section keeps them in a per-CPU buffer and puts them back at the end.
 * Preemption stays off in between: the buffer belongs to the CPU.
 */
static DEFINE_PER_CPU(struct xburst_cop2_state, kernel_mxu_ctx);
static DEFINE_PER_CPU(unsigned int, kernel_mxu_status);

void kernel_mxu_begin_partial(unsigned int mask)
{
	unsigned int status;

	/* One buffer per CPU: a section must not nest in interrupt context */
	WARN_ON_ONCE(!in_task());
	preempt_disable();
	status = read_c0_status();
	__this_cpu_write(kernel_mxu_status, status);
	write_c0_status(status | ST0_CU2);
	xburst_cop2_save(this_cpu_ptr(&kernel_mxu_ctx));
}
EXPORT_SYMBOL_GPL(kernel_mxu_begin_partial);

void kernel_mxu_end(void)
{
	xburst_cop2_restore(this_cpu_ptr(&kernel_mxu_ctx));
	write_c0_status(__this_cpu_read(kernel_mxu_status));
	preempt_enable();
}
EXPORT_SYMBOL_GPL(kernel_mxu_end);

static int xburst_mxu2_cu2_call(struct notifier_block *nfb,
				unsigned long action, void *data)
{
	struct pt_regs *regs = data;
	unsigned long flags;
	unsigned int status;

	if (action != CU2_EXCEPTION || !user_mode(regs))
		return NOTIFY_OK;

	local_irq_save(flags);
	regs->cp0_status |= ST0_CU2;
	status = read_c0_status();
	write_c0_status(status | ST0_CU2);
	xburst_cop2_restore(&current->thread.cp2);
	write_c0_status(status);
	local_irq_restore(flags);

	return NOTIFY_STOP;
}

static int __init xburst_mxu2_init(void)
{
	unsigned int config1;

	/* the save and restore address $vrN with a 10-bit byte offset */
	BUILD_BUG_ON(offsetof(struct xburst_cop2_state, vpr) + 31 * 16 > 511);

	if (boot_cpu_type() != CPU_XBURST ||
	    (current_cpu_data.processor_id & PRID_IMP_MASK) !=
	    PRID_IMP_XBURST_REV2)
		return 0;

	/*
	 * The T21 and T23 clear Config1.C2: their MXU2 raises Reserved
	 * Instruction even with CU2 set (T21) or CU2 does not stick (T23).
	 * Without the notifier they get SIGILL from the default handler.
	 */
	config1 = read_c0_config1();
	if (!(config1 & MIPS_CONF1_C2)) {
		pr_info("XBurst MXU2 absent (Config1 %08x)\n", config1);
		return 0;
	}

	set_except_vector(XBURST_EXCCODE_MFPE, handle_mxu_fpe);
	cu2_notifier(xburst_mxu2_cu2_call, 0);
	pr_info("XBurst MXU2 enabled (Config1 %08x)\n", config1);

	return 0;
}
early_initcall(xburst_mxu2_init);
