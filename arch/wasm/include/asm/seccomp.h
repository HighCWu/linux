/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_WASM_SECCOMP_H
#define _ASM_WASM_SECCOMP_H

/*
 * Signal handlers return through the execution bridge, not a sigreturn
 * syscall. End the generic strict allowlist at this slot instead of
 * admitting an unrelated syscall or inventing a userspace number.
 * The generic mode1 loop stops before comparing a negative terminator.
 */
#define __NR_seccomp_sigreturn (-1)
#include <asm-generic/seccomp.h>

#endif /* _ASM_WASM_SECCOMP_H */
