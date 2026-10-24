// SPDX-License-Identifier: GPL-2.0
#include <linux/entry-common.h>
#include <linux/errno.h>
#include <linux/panic.h>
#include <asm/user_context.h>
#include <asm/wasm_imports.h>

void noinstr wasm_set_user_context(bool user)
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
__attribute__((export_name("user_context_v1"))) noinstr int
wasm_user_context_v1(unsigned int mode)
{
	unsigned int previous = current_thread_info()->in_user_context;

	if (mode > 2)
		return -EINVAL;
	if (mode != 2)
		wasm_set_user_context(mode);
	return previous;
}

void noinstr wasm_run_user(void)
{
	wasm_set_user_context(true);
	wasm_user_call();
	wasm_set_user_context(false);
}

int noinstr wasm_run_user_signal(uintptr_t trampoline, uintptr_t fn, u32 sig,
		unsigned long sp, unsigned long base, unsigned long size, u32 flags)
{
	unsigned long irqflags = arch_local_save_flags();
	int result;

	wasm_set_user_context(true);
	result = wasm_user_signal_v1_call_handler(trampoline, fn, sig, sp,
						base, size, flags);
	wasm_set_user_context(false);
	raw_local_irq_restore(irqflags);
	return result;
}

void __init wasm_check_user_context_abi(void)
{
#ifdef CONFIG_CONTEXT_TRACKING_USER
	/*
	 * Missing imports reject old hosts at instantiation, before any boot.
	 * A host which supplies an incompatible version must not enable EQS.
	 */
	if (wasm_user_context_v1_abi() != 1)
		panic("Wasm user context execution ABI mismatch\n");
#endif
}
