#ifndef _WASM_WASM_IMPORTS_H
#define _WASM_WASM_IMPORTS_H

#include <linux/types.h>

/* Maximum time a parked worker waits before servicing cooperative work. */
#define WASM_WORKER_CHECKPOINT_TIMEOUT_NS (50ULL * 1000 * 1000)

#define wasm_import(ns, name)                                   \
	__attribute__((import_module(#ns), import_name(#name))) \
	wasm_##ns##_##name

size_t wasm_import(boot, get_devicetree)(char *buf, size_t size);
int wasm_import(boot, get_initramfs)(char *buf, size_t size);

void wasm_import(kernel, breakpoint)(void);
void wasm_import(kernel, halt_worker)(void);
/* Returns 1 when the runtime has cooperative work sources, otherwise 0. */
int wasm_import(kernel, worker_checkpoint_v1)(void);
/* Bounded metadata step: 0 no source, 1 resumable, 2 user return blocked. */
int wasm_import(kernel, worker_checkpoint_v2)(void);

/* Whole-machine lifecycle event. Values are part of the guest/host ABI. */
enum wasm_machine_termination_reason {
	WASM_MACHINE_TERMINATION_CLEAN = 0,
	WASM_MACHINE_TERMINATION_PANIC = 1,
};

void wasm_import(kernel, terminate_machine)(
	enum wasm_machine_termination_reason reason);

void wasm_import(kernel, boot_console_write)(const char *msg, size_t len);
void wasm_import(kernel, boot_console_close)(void);

void *wasm_import(kernel, return_address)(int level);

/* Unix time in nanoseconds, monotonically advancing during the host session. */
unsigned long long wasm_import(kernel, get_now_nsec)(void);

void wasm_import(kernel, get_stacktrace)(char *buf, size_t size);

enum wasm_user_memory {
	WASM_USER_MEMORY_NONE = 0,
	WASM_USER_MEMORY_SHARE = 1,
	WASM_USER_MEMORY_COPY = 2,
};

/*
 * WASM_USER_MEMORY_COPY is synchronous: success means the destination worker
 * has completed its private snapshot; allocation failure returns -ENOMEM.
 */
int wasm_import(kernel, spawn_worker)(void (*fn)(void *), void *arg,
				      char *name, size_t name_len,
				      enum wasm_user_memory user_memory);

void wasm_import(kernel, run_on_main)(void (*fn)(void *), void *arg);

/* Private foreign-memory byte copy after Linux mm_access permission checks.
 * Returns copied bytes, never a user pointer or an address translation. */
int wasm_import(remote_memory_v1, copy)(int id, unsigned long user_addr,
		void *kernel_addr, unsigned int len, int write);
void wasm_import(remote_memory_v1, release)(int id);

int wasm_import(user, compile_begin)(u32 len);
int wasm_import(user, compile_write)(u8 *bytes, u32 offset, u32 len);
int wasm_import(user, compile_end)(u32 maximum_memory_pages);
void wasm_import(user, compile_abort)(void);
void wasm_import(user, instantiate)(bool fresh_memory);
void wasm_import(user, call)(void);
void wasm_import(user, switch_entry)(uintptr_t fn, uintptr_t arg);
/* Private execution contract: zero unless a suspended clone3 is in progress. */
uintptr_t wasm_import(user_fork_v1, entry)(void);
void wasm_import(user, call_signal_handler)(uintptr_t fn, u32 sig);
int wasm_import(user, call_siginfo_handler)(uintptr_t trampoline,
					    uintptr_t fn, u32 sig);
void wasm_import(user, halt_signal_handler)(void);

#ifdef CONFIG_CONTEXT_TRACKING_USER
/* Kernel/host contract only: user callbacks must bracket their execution. */
int wasm_import(user_context_v1, abi)(void);
#endif

/* Versioned execution ABI, not a Linux application interface. */
int wasm_import(user_signal_v1, stack_pointer)(unsigned long *sp);
int wasm_import(user_signal_v1, call_handler)(uintptr_t trampoline,
		uintptr_t fn, u32 sig, unsigned long sp, unsigned long base,
		unsigned long size, u32 flags);

int wasm_import(user, read)(void *to, const void __user *from, unsigned long n);
int wasm_import(user, write)(void __user *to, const void *from, unsigned long n);
int wasm_import(user, write_zeroes)(void __user *to, unsigned long n);
int wasm_import(user, futex_atomic_op)(int *oldval, u32 __user *uaddr,
				       int op, int oparg);
int wasm_import(user, futex_atomic_cmpxchg)(u32 *oldval, u32 __user *uaddr,
					    u32 expected, u32 replacement);

long wasm_import(user_v2, mremap)(unsigned long addr, unsigned long old_len,
				unsigned long new_len, unsigned long flags);
/* Private direct-memory allocator operation for anonymous DONTUNMAP remaps. */
long wasm_import(user_v2, mremap_dontunmap)(unsigned long addr,
		unsigned long old_len, unsigned long new_len, unsigned long flags,
		unsigned long new_addr);
/* Fixed u64 layout slots and process offsets, never shared user/kernel pointers. */
int wasm_import(user_brk_v1, layout)(u64 fields[3]);
unsigned long wasm_import(user_brk_v1, update)(unsigned long requested);
long wasm_import(user_v2, msync)(unsigned long addr, unsigned long len, int flags);
long wasm_import(user_v2, madvise)(unsigned long addr, unsigned long len, int advice);
long wasm_import(user_mmap_query_v1, span)(unsigned long address,
					    unsigned long pages);
long wasm_import(user_v2, mmap_fixed)(unsigned long addr, unsigned long len);
long wasm_import(user_v2, mremap_fixed)(unsigned long addr, unsigned long old_len,
		unsigned long new_len, unsigned long new_addr);

/*
 * Direct anonymous mappings live in the process WebAssembly.Memory.  The
 * owner worker calls the user runtime synchronously so allocation metadata is
 * copied naturally by fork together with the rest of that memory.
 */
long wasm_import(user, mmap)(unsigned long len);
long wasm_import(user, munmap)(unsigned long addr, unsigned long len);

/*
 * Versioned mapping requests retain the complete validated syscall shape so
 * later direct allocators can implement hints and fixed placement without
 * another incompatible function-signature change.
 */
long wasm_import(user_v2, mmap)(unsigned long addr, unsigned long len,
				unsigned int prot, unsigned int flags, int fd,
				unsigned long pgoff);

/* Caller owns a quiescent staging buffer until this synchronous call returns.
 * This experimental execution primitive does not admit Linux file mappings. */
long wasm_import(user_mmap_init_v1, map)(unsigned long rounded,
				       const void *source, unsigned long length);
/* Bit 0: current user module exports the initialized-copy v1 allocator.
 * A capability never authorizes a file source or promises page protection. */
unsigned int wasm_import(user_mmap_init_v1, capabilities)(void);

#ifdef CONFIG_WASM_FILE_MMAP_STREAM
/* Kernel/host only. The existing user initializer still grants one copy. */
unsigned int wasm_import(user_mmap_stream_v1, capabilities)(void);
long wasm_import(user_mmap_stream_v1, map)(unsigned long rounded);
#endif

#ifdef CONFIG_VIRTIO_WASM
void wasm_import(virtio, set_features)(u32 id, u64 features);

void wasm_import(virtio, setup)(u32 id, u32 config_irq, u8 *config,
				u32 config_len);
void wasm_import(virtio, reset)(u32 id);

void wasm_import(virtio, enable_vring)(u32 id, u32 index, u32 size,
				       dma_addr_t desc, u32 irq);
void wasm_import(virtio, disable_vring)(u32 id, u32 index);

void wasm_import(virtio, notify)(u32 id, u32 index);
#endif

#undef wasm_import

#endif
