#ifndef _WASM_ENTRY_COMMON_H
#define _WASM_ENTRY_COMMON_H

#include <asm/thread_info.h>
#include <asm/worker_checkpoint.h>

#define ARCH_EXIT_TO_USER_MODE_WORK _TIF_WASM_WORKER_BARRIER

#define arch_exit_to_user_mode_work arch_exit_to_user_mode_work
static inline void arch_exit_to_user_mode_work(struct pt_regs *regs,
					       unsigned long ti_work)
{
	if (ti_work & _TIF_WASM_WORKER_BARRIER)
		wasm_worker_checkpoint_wait();
}

#define arch_exit_to_user_mode_prepare arch_exit_to_user_mode_prepare
static inline void arch_exit_to_user_mode_prepare(struct pt_regs *regs,
						  unsigned long ti_work)
{
	wasm_worker_prepare_user_return(regs);
}

#define arch_enter_from_user_mode arch_enter_from_user_mode
static __always_inline void arch_enter_from_user_mode(struct pt_regs *regs)
{
	current_thread_info()->in_user_context = false;
}

#define arch_exit_to_user_mode arch_exit_to_user_mode
static __always_inline void arch_exit_to_user_mode(void)
{
	current_thread_info()->in_user_context = true;
}

#endif
