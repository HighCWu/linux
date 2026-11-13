// SPDX-License-Identifier: GPL-2.0
#include <linux/irq-entry-common.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <asm/wasm_imports.h>
#include <asm/worker_checkpoint.h>

/*
 * A parked Worker may update its own module metadata without owning a CPU.
 * It must not wait for another participant, run user code or reenter Linux.
 * A separate return barrier keeps provisional participants out of userspace.
 */
bool wasm_worker_checkpoint_update(struct task_struct *task)
{
	int state = wasm_kernel_worker_checkpoint_v2();

	BUG_ON(state < 0 || state > 2);
	if (state == 2)
		set_tsk_thread_flag(task, TIF_WASM_WORKER_BARRIER);
	else
		clear_tsk_thread_flag(task, TIF_WASM_WORKER_BARRIER);
	return state != 0;
}

/* Called only with a logical CPU at a schedulable kernel boundary. */
void wasm_worker_checkpoint_wait(void)
{
	bool irq_was_disabled = irqs_disabled();

	if (irq_was_disabled)
		local_irq_enable();
	for (;;) {
		wasm_worker_checkpoint_update(current);
		if (!test_thread_flag(TIF_WASM_WORKER_BARRIER))
			break;
		/* Fatal signals terminate the task without executing user code. */
		if (fatal_signal_pending(current))
			do_group_exit(SIGKILL);
		schedule_timeout_uninterruptible(1);
	}
	if (irq_was_disabled)
		local_irq_disable();
}

/* The generic prepare hook runs after ordinary return work, with IRQs off. */
void wasm_worker_prepare_user_return(struct pt_regs *regs)
{
	unsigned long work;

	lockdep_assert_irqs_disabled();
	wasm_worker_checkpoint_wait();
	/*
	 * Waiting may queue ordinary reschedule or signal return work. The
	 * generic loop also handles a new barrier recorded during a schedule.
	 */
	work = read_thread_flags();
	if (work & EXIT_TO_USER_MODE_WORK)
		exit_to_user_mode_loop(regs, work);
}
