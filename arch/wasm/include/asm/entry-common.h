#ifndef _WASM_ENTRY_COMMON_H
#define _WASM_ENTRY_COMMON_H

#include <asm/thread_info.h>

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
