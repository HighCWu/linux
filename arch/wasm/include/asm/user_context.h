/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _WASM_USER_CONTEXT_H
#define _WASM_USER_CONTEXT_H

#include <linux/types.h>
#include <linux/init.h>

/*
 * A phase change does not run pending return work. In particular, a clone
 * callback must initialize its user C stack/TLS before servicing signals.
 */
void noinstr wasm_set_user_context(bool user);
void noinstr wasm_run_user(void);
int noinstr wasm_run_user_signal(uintptr_t trampoline, uintptr_t fn, u32 sig,
		unsigned long sp, unsigned long base, unsigned long size, u32 flags);
void __init wasm_check_user_context_abi(void);

#endif
