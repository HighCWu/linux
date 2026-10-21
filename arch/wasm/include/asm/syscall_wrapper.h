#ifndef _WASM_SYSCALL_WRAPPER_H
#define _WASM_SYSCALL_WRAPPER_H

#include <asm/ptrace.h>
#include <asm/sigcontext.h>
#include <linux/errno.h>

#ifdef CONFIG_64BIT
#define SC_WASM_DECODE_ARGS(x, ...)
#define SC_WASM_REGS_TO_ARGS(x, ...)                                          \
	__MAP(x, __SC_ARGS, , regs->syscall_args[0], , regs->syscall_args[1], \
	      , regs->syscall_args[2], , regs->syscall_args[3], ,             \
	      regs->syscall_args[4], , regs->syscall_args[5])
#else
/* wasm32's libc passes a 64-bit scalar as consecutive low/high words, without
 * an even-register alignment hole. Decode in declaration order before calling
 * __se_sys: widening just one register loses high bits and shifts later args.
 * Explicit split-word syscalls (e.g. preadv) still consist of ordinary longs.
 */
static inline int wasm_decode_syscall_args(const struct pt_regs *regs,
		const unsigned char *words, unsigned long long *args,
		unsigned int count)
{
	unsigned int slot = 0, i;

	for (i = 0; i < count; i++) {
		if (slot + words[i] > 6)
			return -ENOSYS;
		args[i] = regs->syscall_args[slot++];
		if (words[i] == 2)
			args[i] |= (unsigned long long)regs->syscall_args[slot++] << 32;
	}
	return 0;
}

#define __SC_WASM_WORDS(t, a) (1 + __TYPE_IS_LL(t))
#define SC_WASM_DECODE_ARGS(x, ...)                                         \
	const unsigned char __wasm_words[] = { __MAP(x, __SC_WASM_WORDS, __VA_ARGS__) }; \
	unsigned long long __wasm_args[x];                                  \
	int __wasm_error = wasm_decode_syscall_args(regs, __wasm_words,       \
						   __wasm_args, x);        \
	if (__wasm_error)                                                  \
		return __wasm_error
#define SC_WASM_REGS_TO_ARGS(x, ...)                                       \
	__MAP(x, __SC_ARGS, , __wasm_args[0], , __wasm_args[1], ,            \
	      __wasm_args[2], , __wasm_args[3], , __wasm_args[4], , __wasm_args[5])
#endif

#define __SYSCALL_DEFINEx(x, name, ...)                                      \
	asmlinkage long sys##name(const struct pt_regs *regs);               \
	ALLOW_ERROR_INJECTION(sys##name, ERRNO);                             \
	static long __se_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__));        \
	static inline long __do_sys##name(__MAP(x, __SC_DECL, __VA_ARGS__)); \
	asmlinkage long sys##name(const struct pt_regs *regs)                \
	{                                                                    \
		SC_WASM_DECODE_ARGS(x, __VA_ARGS__);                        \
		return __se_sys##name(SC_WASM_REGS_TO_ARGS(x, __VA_ARGS__)); \
	}                                                                    \
	static long __se_sys##name(__MAP(x, __SC_LONG, __VA_ARGS__))         \
	{                                                                    \
		long ret = __do_sys##name(__MAP(x, __SC_CAST, __VA_ARGS__)); \
		__MAP(x, __SC_TEST, __VA_ARGS__);                            \
		__PROTECT(x, ret, __MAP(x, __SC_ARGS, __VA_ARGS__));         \
		return ret;                                                  \
	}                                                                    \
	static inline long __do_sys##name(__MAP(x, __SC_DECL, __VA_ARGS__))

#define SYSCALL_DEFINE0(sname)                                       \
	SYSCALL_METADATA(_##sname, 0);                               \
	asmlinkage long sys_##sname(const struct pt_regs *__unused); \
	ALLOW_ERROR_INJECTION(sys_##sname, ERRNO);                   \
	asmlinkage long sys_##sname(const struct pt_regs *__unused)

#define SYS_NI(name) SYSCALL_ALIAS(sys_##name, sys_ni_posix_timers);

#endif
