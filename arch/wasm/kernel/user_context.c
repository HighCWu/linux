// SPDX-License-Identifier: GPL-2.0
#include <linux/entry-common.h>
#include <linux/errno.h>
#include <asm/user_context.h>

void wasm_set_user_context(bool user)
{
	unsigned long flags;

	raw_local_irq_save(flags);
	if (current_thread_info()->in_user_context != user) {
		if (user)
			exit_to_user_mode();
		else
			enter_from_user_mode(current_pt_regs());
	}
	/*
	 * IRQ handlers may run here, but no pending userspace return work does.
	 * The IRQ bridge preserves the interrupted phase without delivering a
	 * signal before a new clone has initialized its user C stack and TLS.
	 */
	raw_local_irq_restore(flags);
}

/*
 * Private fixed-i32 execution contract: 0 kernel, 1 user, 2 query. The host
 * saves the old phase across callbacks and restores it even on a user SjLj.
 * A dead task must never be re-entered to perform such restoration.
 */
__attribute__((export_name("user_context_v1"))) int
wasm_user_context_v1(unsigned int mode)
{
	unsigned int previous = current_thread_info()->in_user_context;

	if (mode > 2)
		return -EINVAL;
	if (mode != 2)
		wasm_set_user_context(mode);
	return previous;
}
