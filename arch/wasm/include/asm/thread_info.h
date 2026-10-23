#ifndef _WASM_THREAD_INFO_H
#define _WASM_THREAD_INFO_H

#include <asm/page.h>
#include <linux/types.h>

struct kernel_siginfo;
struct mm_struct;
struct task_bootstrap_args;
struct wasm_signal_frame;
struct wasm_mmap_callback;

/*
 * A remote wake changes the value parked workers wait on, so publishing a
 * request cannot race between their mailbox check and atomic.wait().
 */
#define WASM_CPU_PARKED		(-1)
#define WASM_CPU_REMOTE_WAKE	(-2)
#define WASM_CPU_FORK_ABORT	(-3)

/* THREAD_SIZE is the size of the task_struct + kernel stack
 * This is asserted in setup, but the stack should be 1 page,
 * and a task_struct should be *way* less than a page big. */
#define THREAD_SIZE_ORDER 1
#define THREAD_SIZE (PAGE_SIZE << THREAD_SIZE_ORDER)
#define THREAD_SHIFT (PAGE_SHIFT << THREAD_SIZE_ORDER)

struct thread_info {
	unsigned long flags;
	unsigned long syscall_work; /* SYSCALL_WORK_ flags */
	int preempt_count;
	int cpu; // this is for the kernel
	atomic_t running_cpu; // negative means unscheduled
	/* The mm represented by this worker's local JS user context. */
	struct mm_struct *context_mm;
	unsigned long tp_value;
	const struct kernel_siginfo *active_siginfo;
	struct wasm_signal_frame *active_signal_frame;
	/* Non-NULL only until a newly-created Worker starts or is aborted. */
	struct task_bootstrap_args *bootstrap_args;
	/* Quiescent file copies only, never an in-flight VFS producer. */
	struct wasm_mmap_callback *mmap_callbacks;
};

#ifndef current_thread_info
#include <asm/current.h>
#define current_thread_info() ((struct thread_info *)current)
#endif

#define INIT_THREAD_INFO(tsk)                        \
	{                                            \
		.flags = 0,                          \
		.preempt_count = INIT_PREEMPT_COUNT, \
		.cpu = 0,                            \
		.running_cpu = ATOMIC_INIT(0),       \
		.context_mm = NULL,                  \
		.tp_value = ~0UL,                    \
		.active_siginfo = NULL,              \
		.active_signal_frame = NULL,         \
		.bootstrap_args = NULL,              \
		.mmap_callbacks = NULL,               \
	}

#include <asm-generic/thread_info_tif.h>

#endif
