#include <linux/entry-common.h>
#include <linux/mman.h>
#include <linux/overflow.h>
#include <linux/rcupdate.h>
#include <linux/syscalls.h>
#ifdef CONFIG_WASM_MMAP_COPY_TEST
#include <linux/slab.h>
#endif
#include <asm/irq.h>
#include <asm/remote_vm.h>
#include <asm/mmap_file.h>

#define sys_sigaltstack sys_wasm_sigaltstack
#undef __SYSCALL
#define __SYSCALL(nr, sym) asmlinkage long sym(const struct pt_regs *regs);
#include <asm/unistd.h>

typedef asmlinkage long (*syscall_handler_t)(const struct pt_regs *regs);

#ifdef CONFIG_WASM_MMAP_COPY_TEST
/* Deliberately absent from UAPI headers and default kernels. */
#define WASM_MMAP_COPY_TEST_NR (__NR_arch_specific_syscall + 9)
asmlinkage long sys_wasm_mmap_copy_test(const struct pt_regs *regs);
asmlinkage long sys_wasm_mmap_file_create_test(const struct pt_regs *regs);
asmlinkage long sys_wasm_mmap_file_read_test(const struct pt_regs *regs);
asmlinkage long sys_wasm_mmap_provenance_test(const struct pt_regs *regs);
asmlinkage long sys_wasm_mmap_stats_test(const struct pt_regs *regs);
#endif

#undef __SYSCALL
#define __SYSCALL(nr, sym) [nr] = (syscall_handler_t)sym,

syscall_handler_t syscall_table[__NR_syscalls] = {
	[0 ... __NR_syscalls - 1] = (syscall_handler_t)sys_ni_syscall,
#include <asm/unistd.h>
#ifdef CONFIG_WASM_MMAP_COPY_TEST
	[WASM_MMAP_COPY_TEST_NR] = sys_wasm_mmap_copy_test,
	[__NR_arch_specific_syscall + 10] = sys_wasm_mmap_file_create_test,
	[__NR_arch_specific_syscall + 11] = sys_wasm_mmap_file_read_test,
	[__NR_arch_specific_syscall + 12] = sys_wasm_mmap_provenance_test,
	[__NR_arch_specific_syscall + 13] = sys_wasm_mmap_stats_test,
#endif
#undef sys_sigaltstack
};

__attribute__((export_name("syscall"))) long
wasm_syscall(long nr, unsigned long arg0, unsigned long arg1,
	     unsigned long arg2, unsigned long arg3, unsigned long arg4,
	     unsigned long arg5)
{
	struct pt_regs *regs = current_pt_regs();

	regs->syscall_args[0] = arg0;
	regs->syscall_args[1] = arg1;
	regs->syscall_args[2] = arg2;
	regs->syscall_args[3] = arg3;
	regs->syscall_args[4] = arg4;
	regs->syscall_args[5] = arg5;

	for (;;) {
		/*
		 * WebAssembly has no asynchronous trap from user mode.  Reaching
		 * this boundary therefore proves that the CPU passed through a
		 * userspace RCU quiescent state.  It also gives the generic entry
		 * code the IRQ-disabled state it expects.
		 */
		local_irq_disable();
		rcu_note_context_switch(false);
		regs->user_mode = 0;
		nr = syscall_enter_from_user_mode(regs, nr);
		regs->syscall_nr = nr;
		regs->restart_syscall_nr = -1;

		/*
		 * Deliver timers armed against a busy task that only enters the
		 * kernel for syscalls; nothing else polls the clockevent deadline
		 * for it.
		 */
		wasm_timer_check();
		wasm_service_remote_request(current);

		if (nr < 0 || nr >= ARRAY_SIZE(syscall_table))
			regs->syscall_return = -ENOSYS;
		else
			regs->syscall_return = syscall_table[nr](regs);

		syscall_exit_to_user_mode(regs);
		regs->user_mode = 1;

		if (regs->restart_syscall_nr < 0)
			return regs->syscall_return;
		nr = regs->restart_syscall_nr;
	}
}

/*
 * The generic implementation queries the unavailable wasm userspace stack
 * pointer and traps. Until alternate signal stacks are implementable, expose
 * the unsupported syscall honestly instead.
 */
SYSCALL_DEFINE0(wasm_sigaltstack)
{
	return -ENOSYS;
}

SYSCALL_DEFINE1(set_thread_area, unsigned long, addr)
{
	struct thread_info *ti = task_thread_info(current);
	ti->tp_value = addr;
	return 0;
}

/*
 * CONFIG_MMU remains disabled.  These wrappers deliberately implement only
 * direct, anonymous mappings: the returned number is an ordinary offset in
 * the caller's WebAssembly.Memory and needs no address translation.  The
 * process-local runtime owns the allocator because it already owns brk and is
 * the only layer that can prevent the two direct allocation paths from
 * overlapping.
 */
SYSCALL_DEFINE6(wasm_mmap, unsigned long, addr, size_t, len, unsigned long, prot,
		unsigned long, flags, long, fd, unsigned long, pgoff)
{
	const unsigned long supported_flags = MAP_PRIVATE | MAP_ANONYMOUS |
		MAP_FIXED_NOREPLACE;

#ifdef CONFIG_WASM_FILE_MMAP
	if (!(flags & MAP_ANONYMOUS))
		return wasm_mmap_file(addr, len, prot, flags, fd, pgoff);
#endif
	if (!len || pgoff || fd != -1 || (flags & MAP_TYPE) != MAP_PRIVATE ||
	    !(flags & MAP_ANONYMOUS))
		return -EINVAL;
	if (flags & MAP_FIXED)
		return -ENOMEM;
	if ((flags & MAP_FIXED_NOREPLACE) && !addr)
		return -ENOMEM;
	if ((flags & MAP_FIXED_NOREPLACE) && !PAGE_ALIGNED(addr))
		return -EINVAL;
	if ((flags & ~supported_flags) || prot != (PROT_READ | PROT_WRITE))
		return -EINVAL;
	if (len > ULONG_MAX - (PAGE_SIZE - 1))
		return -ENOMEM;

	return wasm_user_v2_mmap(addr, ALIGN(len, PAGE_SIZE), prot, flags, fd,
				 pgoff);
}

/* Direct linear memory has no movable page tables. The process allocator
 * owns reservations shared with brk; it validates ownership and performs any
 * admitted anonymous copy before publishing the replacement record. */
SYSCALL_DEFINE5(wasm_mremap, unsigned long, addr, unsigned long, old_len,
	       unsigned long, new_len, unsigned long, flags,
	       unsigned long, new_addr)
{
	unsigned long old_rounded, new_rounded, end;

	if (!PAGE_ALIGNED(addr) || !new_len || (flags & ~7UL) ||
	    ((flags & (MREMAP_FIXED | MREMAP_DONTUNMAP)) && !(flags & MREMAP_MAYMOVE)))
		return -EINVAL;
	if (flags & MREMAP_FIXED) {
		if (!PAGE_ALIGNED(new_addr))
			return -EINVAL;
		return -EOPNOTSUPP;
	}
	/* Linux's old_len=0 shared alias operation is not a private direct copy. */
	if (!old_len)
		return -EOPNOTSUPP;
	if (check_add_overflow(old_len, PAGE_SIZE - 1, &old_rounded))
		return -EINVAL;
	if (check_add_overflow(new_len, PAGE_SIZE - 1, &new_rounded))
		return -ENOMEM;
	old_rounded &= PAGE_MASK;
	new_rounded &= PAGE_MASK;
	if (flags & MREMAP_DONTUNMAP)
		return old_rounded == new_rounded ? -EOPNOTSUPP : -EINVAL;
	if (check_add_overflow(addr, old_rounded, &end) || end > TASK_SIZE)
		return -EFAULT;
	if (new_rounded > TASK_SIZE)
		return -ENOMEM;
	return wasm_user_v2_mremap(addr, old_rounded, new_rounded, flags);
}

/* No VMA or file writeback is invented here. All supported direct mappings
 * are private; the process allocator must authenticate complete coverage. */
SYSCALL_DEFINE3(wasm_msync, unsigned long, addr, size_t, len, int, flags)
{
	unsigned long rounded, end;

	if (!PAGE_ALIGNED(addr) || (flags & ~(MS_ASYNC | MS_SYNC | MS_INVALIDATE)) ||
	    ((flags & MS_ASYNC) && (flags & MS_SYNC)))
		return -EINVAL;
	if (!len)
		return 0;
	if (check_add_overflow(len, PAGE_SIZE - 1, &rounded))
		return -ENOMEM;
	rounded &= PAGE_MASK;
	if (check_add_overflow(addr, rounded, &end) || end > TASK_SIZE)
		return -ENOMEM;
	return wasm_user_v2_msync(addr, rounded, flags);
}

SYSCALL_DEFINE2(wasm_munmap, unsigned long, addr, size_t, len)
{
	unsigned long rounded;

	if (!PAGE_ALIGNED(addr) || !len ||
	    check_add_overflow(len, PAGE_SIZE - 1, &rounded))
		return -EINVAL;
	rounded &= PAGE_MASK;
	if (check_add_overflow(addr, rounded, &addr))
		return -EINVAL;

	return wasm_user_munmap(addr - rounded, rounded);
}

#ifdef CONFIG_WASM_MMAP_COPY_TEST
/* Bounded, kernel-owned synthetic data; no file read or asynchronous producer.
 * Keep ownership until the allocator's synchronous copy/publication returns. */
SYSCALL_DEFINE1(wasm_mmap_copy_test, unsigned long, length)
{
	const unsigned long rounded = 2 * PAGE_SIZE;
	u8 *staging;
	unsigned long i;
	long result;

	if (length > rounded)
		return -EINVAL;
	staging = kmalloc(rounded, GFP_KERNEL);
	if (!staging)
		return -ENOMEM;
	for (i = 0; i < length; i++)
		staging[i] = (u8)(i * 37 + 11);
	result = wasm_user_mmap_init_v1_map(rounded, staging, length);
	kfree(staging);
	return result;
}
#endif

__attribute__((export_name("get_thread_area"))) unsigned long
wasm_get_thread_area(void)
{
	struct thread_info *ti = task_thread_info(current);
	return ti->tp_value;
}
