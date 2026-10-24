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
#include <asm/wasm_imports.h>

/* SysV queues/semaphores copy through uaccess and need no shared mapping.
 * Generic NOMMU shmat would instead return a kernel-linear address that is
 * not a mapping in this process's independent user WebAssembly.Memory.
 * Keep the entire SHM family unavailable until real shared backing exists. */
#define sys_shmget sys_wasm_shm_unavailable
#define sys_shmctl sys_wasm_shm_unavailable
#define sys_shmat sys_wasm_shm_unavailable
#define sys_shmdt sys_wasm_shm_unavailable
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
#undef sys_shmget
#undef sys_shmctl
#undef sys_shmat
#undef sys_shmdt
};

__attribute__((export_name("syscall"))) noinstr long
wasm_syscall(long nr, unsigned long arg0, unsigned long arg1,
	     unsigned long arg2, unsigned long arg3, unsigned long arg4,
	     unsigned long arg5)
{
	struct pt_regs *regs = current_pt_regs();
	unsigned long sp = 0;
	int sp_error = 0;
	bool first_entry = true;

	for (;;) {
		/*
		 * Reactivate the kernel context before any instrumentable work,
		 * user-stack import, or RCU operation. A restart follows a real
		 * generic exit transition too, so it must enter again.
		 */
		raw_local_irq_disable();
		enter_from_user_mode(regs);
		instrumentation_begin();
		regs->user_mode = 0;
		/*
		 * With no asynchronous user trap, this entry proves a userspace
		 * quiescent state even under the default tick accounting config.
		 * Report it only after the generic entry has reactivated RCU.
		 */
		rcu_note_context_switch(false);
		local_irq_enable();

		if (first_entry) {
			sp_error = wasm_user_signal_v1_stack_pointer(&sp);
			regs->user_sp = sp;
			regs->syscall_args[0] = arg0;
			regs->syscall_args[1] = arg1;
			regs->syscall_args[2] = arg2;
			regs->syscall_args[3] = arg3;
			regs->syscall_args[4] = arg4;
			regs->syscall_args[5] = arg5;
			first_entry = false;
		}

		/* Generic ptrace entry work reads the current syscall number. */
		regs->syscall_nr = nr;
		/* Entry tracing may skip execution and supply its own result. */
		regs->syscall_return = -ENOSYS;
		nr = syscall_enter_from_user_mode_work(regs, nr);
		regs->syscall_nr = nr;
		regs->restart_syscall_nr = -1;

		/*
		 * Deliver timers armed against a busy task that only enters the
		 * kernel for syscalls; nothing else polls the clockevent deadline
		 * for it.
		 */
		wasm_timer_check();
		wasm_service_remote_request(current);

		if (nr == __NR_sigaltstack && sp_error)
			regs->syscall_return = sp_error;
		else if (nr != -1 && (nr < 0 || nr >= ARRAY_SIZE(syscall_table)))
			regs->syscall_return = -ENOSYS;
		else if (nr >= 0) {
			struct pt_regs entered = *regs;
			long result;

			/* A direct allocator may call back into Linux (for example
			 * while acquiring a userspace mutex). Nested calls use the
			 * same task_pt_regs storage. Preserve the outer entry state,
			 * including any ptrace edits, before reporting its exit. */
			result = syscall_table[nr](regs);
			*regs = entered;
			regs->syscall_return = result;
		}

		instrumentation_end();
		syscall_exit_to_user_mode(regs);
		regs->user_mode = 1;

		if (regs->restart_syscall_nr < 0)
			return regs->syscall_return;
		nr = regs->restart_syscall_nr;
	}
}

SYSCALL_DEFINE0(wasm_shm_unavailable)
{
	return -ENOSYS;
}

/* Private execution boundary: a user allocator can re-enter a live kernel
 * callback. Restore its caller's IRQ state before resuming that callback,
 * not after the outer syscall has already performed post-callback cleanup. */
__attribute__((export_name("callback_irq_save_v1"))) unsigned int
wasm_callback_irq_save_v1(void)
{
	return (unsigned int)arch_local_save_flags();
}

__attribute__((export_name("callback_irq_restore_v1"))) void
wasm_callback_irq_restore_v1(unsigned int flags)
{
	unsigned long irqflags = flags;

	local_irq_restore(irqflags);
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
	/* Resident private backing satisfies population hints. Stack and the
	 * historical executable/deny-write hints do not grant page permissions. */
	const unsigned long advisory_flags = MAP_NORESERVE | MAP_POPULATE |
		MAP_NONBLOCK | MAP_STACK | MAP_DENYWRITE | MAP_EXECUTABLE;
	const unsigned long supported_flags = MAP_PRIVATE | MAP_ANONYMOUS |
		MAP_FIXED | MAP_FIXED_NOREPLACE | advisory_flags;

#ifdef CONFIG_WASM_FILE_MMAP
	if (!(flags & MAP_ANONYMOUS))
		return wasm_mmap_file(addr, len, prot, flags, fd, pgoff);
#endif
	/* mmap uses byte offsets on wasm64; mmap2 uses 4096-byte units on
	 * wasm32 even though a Wasm page is 64 KiB. Preserve the Linux entry
	 * alignment check before ignoring the offset of anonymous backing. */
#ifdef CONFIG_64BIT
	if (!PAGE_ALIGNED(pgoff))
		return -EINVAL;
#else
	if (pgoff & ((PAGE_SIZE >> 12) - 1))
		return -EINVAL;
#endif
	if (!len || (flags & MAP_TYPE) != MAP_PRIVATE ||
	    !(flags & MAP_ANONYMOUS))
		return -EINVAL;
	if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && !addr)
		return -ENOMEM;
	if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && !PAGE_ALIGNED(addr))
		return -EINVAL;
	/* Like NOMMU private copies, a read-only request does not imply enforced
	 * write protection. WRITE implies READ in direct linear memory. Never
	 * pretend to supply guard pages, read isolation or code loading. */
	if ((flags & ~supported_flags) ||
	    !prot || (prot & ~(PROT_READ | PROT_WRITE)))
		return -EINVAL;
	/* Older execution allocators admit READ or READ|WRITE only. */
	if (prot & PROT_WRITE)
		prot |= PROT_READ;
	if (len > ULONG_MAX - (PAGE_SIZE - 1))
		return -ENOMEM;
	if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) &&
	    (addr >= TASK_SIZE || ALIGN(len, PAGE_SIZE) > TASK_SIZE - addr))
		return -ENOMEM;
	if ((flags & MAP_FIXED) && !(flags & MAP_FIXED_NOREPLACE))
		return wasm_user_v2_mmap_fixed(addr, ALIGN(len, PAGE_SIZE));
	/* NOREPLACE takes priority even when the caller also supplies FIXED.
	 * Normalize advisory bits and ignored anonymous fd/offset for old allocators. */
	flags &= ~(MAP_FIXED | advisory_flags);
	fd = -1;
	pgoff = 0;

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
	if ((flags & MREMAP_FIXED) && !PAGE_ALIGNED(new_addr))
		return -EINVAL;
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
	if (flags & MREMAP_FIXED) {
		if (!new_addr || new_addr > TASK_SIZE || new_rounded > TASK_SIZE - new_addr)
			return -ENOMEM;
		if (new_addr < end && addr < new_addr + new_rounded)
			return -EINVAL;
		return wasm_user_v2_mremap_fixed(addr, old_rounded, new_rounded, new_addr);
	}
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

/* These hints apply only to registered direct mappings. DONTNEED discards
 * anonymous bytes, not address reservations or WebAssembly memory capacity. */
SYSCALL_DEFINE3(wasm_madvise, unsigned long, addr, size_t, len, int, advice)
{
	unsigned long rounded, end;

	if (!PAGE_ALIGNED(addr) || advice < MADV_NORMAL || advice > MADV_DONTNEED ||
	    check_add_overflow(len, PAGE_SIZE - 1, &rounded))
		return -EINVAL;
	if (!len)
		return 0;
	rounded &= PAGE_MASK;
	if (check_add_overflow(addr, rounded, &end))
		return -EINVAL;
	if (end > TASK_SIZE)
		return -ENOMEM;
	return wasm_user_v2_madvise(addr, rounded, advice);
}

SYSCALL_DEFINE2(wasm_munmap, unsigned long, addr, size_t, len)
{
	unsigned long rounded;

	if (!PAGE_ALIGNED(addr) || !len ||
	    check_add_overflow(len, PAGE_SIZE - 1, &rounded))
		return -EINVAL;
	rounded &= PAGE_MASK;
	if (check_add_overflow(addr, rounded, &addr) || addr > TASK_SIZE)
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
