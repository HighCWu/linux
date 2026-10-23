#include <linux/ptrace.h>

long arch_ptrace(struct task_struct *child, long request,
		 unsigned long addr, unsigned long data)
{
	/* Generic memory requests use ptrace_access_vm and the Wasm remote-mm
	 * bridge; register requests without an architecture view return EIO. */
	return ptrace_request(child, request, addr, data);
}

void ptrace_disable(struct task_struct *child)
{
	/* No hardware single-step or debug-register state is installed on Wasm.
	 * The common detach path clears syscall work and tracing ownership. */
}

void show_regs(struct pt_regs *fp)
{
	pr_info("syscall=%ld args=%#lx,%#lx,%#lx,%#lx,%#lx,%#lx user=%d\n",
		fp->syscall_nr, fp->syscall_args[0], fp->syscall_args[1],
		fp->syscall_args[2], fp->syscall_args[3], fp->syscall_args[4],
		fp->syscall_args[5], fp->user_mode);
}
