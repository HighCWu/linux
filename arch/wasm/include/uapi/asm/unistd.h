/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#define _WASM_UNISTD_H
#ifdef _WASM_UNISTD_H

#include <asm/bitsperlong.h>

#define __ARCH_WANT_RENAMEAT
#if __BITS_PER_LONG == 32
#define __ARCH_WANT_STAT64
#define __ARCH_WANT_TIME32_SYSCALLS
#define __ARCH_WANT_SYNC_FILE_RANGE2
#endif
#define __ARCH_WANT_SET_GET_RLIMIT
#define __ARCH_WANT_NO_MEMORY_SYSCALLS
#define __ARCH_WANT_NO_RT_SIGRETURN

#include <asm-generic/unistd.h>

/*
 * wasm keeps the generic NOMMU memory syscalls disabled: their VMA and file
 * mapping model cannot describe a directly-dereferenceable WebAssembly linear
 * memory address.  Anonymous mmap and munmap nevertheless have their standard
 * asm-generic numbers and are implemented by arch-specific direct-memory
 * wrappers.
 */
#define __NR_munmap 215
__SYSCALL(__NR_munmap, sys_wasm_munmap)

#define __NR3264_mmap 222
__SYSCALL(__NR3264_mmap, sys_wasm_mmap)
#if __BITS_PER_LONG == 64
#define __NR_mmap __NR3264_mmap
#else
#define __NR_mmap2 __NR3264_mmap
#endif

#define __NR_set_thread_area (__NR_arch_specific_syscall + 0)
__SYSCALL(__NR_set_thread_area, sys_set_thread_area)

#define __NR_wasm_get_args (__NR_arch_specific_syscall + 1)
__SYSCALL(__NR_wasm_get_args, sys_wasm_get_args)

#define __NR_wasm_sem_open (__NR_arch_specific_syscall + 2)
__SYSCALL(__NR_wasm_sem_open, sys_wasm_sem_open)
#define __NR_wasm_sem_unlink (__NR_arch_specific_syscall + 3)
__SYSCALL(__NR_wasm_sem_unlink, sys_wasm_sem_unlink)
#define __NR_wasm_sem_wait (__NR_arch_specific_syscall + 4)
__SYSCALL(__NR_wasm_sem_wait, sys_wasm_sem_wait)
#define __NR_wasm_sem_trywait (__NR_arch_specific_syscall + 5)
__SYSCALL(__NR_wasm_sem_trywait, sys_wasm_sem_trywait)
#define __NR_wasm_sem_timedwait (__NR_arch_specific_syscall + 6)
__SYSCALL(__NR_wasm_sem_timedwait, sys_wasm_sem_timedwait)
#define __NR_wasm_sem_post (__NR_arch_specific_syscall + 7)
__SYSCALL(__NR_wasm_sem_post, sys_wasm_sem_post)
#define __NR_wasm_sem_getvalue (__NR_arch_specific_syscall + 8)
__SYSCALL(__NR_wasm_sem_getvalue, sys_wasm_sem_getvalue)

#endif
