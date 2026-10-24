#include <asm/wasm_imports.h>
#include <asm/user_context.h>
#include <asm/unistd.h>
#include <linux/entry-common.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/syscalls.h>
#include <linux/overflow.h>
#include <linux/uaccess.h>

struct wasm_signal_frame {
	stack_t stack;
	sigset_t mask;
	struct wasm_signal_frame *previous;
	const kernel_siginfo_t *previous_info;
};

static void syscall_restart(struct pt_regs *regs, bool handler,
			    const struct k_sigaction *ka)
{
	/* Recompute the decision for each pass through the signal-exit loop. */
	regs->restart_syscall_nr = -1;

	switch (regs->syscall_return) {
	case -ERESTARTNOHAND:
		if (handler)
			regs->syscall_return = -EINTR;
		else
			regs->restart_syscall_nr = regs->syscall_nr;
		break;
	case -ERESTARTSYS:
		if (handler && !(ka->sa.sa_flags & SA_RESTART))
			regs->syscall_return = -EINTR;
		else
			regs->restart_syscall_nr = regs->syscall_nr;
		break;
	case -ERESTARTNOINTR:
		regs->restart_syscall_nr = regs->syscall_nr;
		break;
	case -ERESTART_RESTARTBLOCK:
		if (handler)
			regs->syscall_return = -EINTR;
		else
			regs->restart_syscall_nr = __NR_restart_syscall;
		break;
	}
}

static void call_signal_handler(struct ksignal *ksig, struct pt_regs *regs)
{
	const struct sigaction *sa = &ksig->ka.sa;
	struct thread_info *ti = current_thread_info();
	struct pt_regs saved_regs = *regs;
	/* ppoll/pselect/sigsuspend may have installed a temporary mask. Save
	 * the return mask before signal_setup_done clears restore_sigmask,
	 * just as a native signal frame would do for rt_sigreturn. */
	sigset_t saved_mask = *sigmask_to_save();
	struct wasm_signal_frame frame;
	struct wasm_signal_frame *previous_frame = ti->active_signal_frame;
	const kernel_siginfo_t *previous_info = ti->active_siginfo;
	unsigned long sp = 0, end;
	u32 callback_flags = (sa->sa_flags & SA_SIGINFO) ? 1 : 0;
	int failed = 0;
	int sp_error = wasm_user_signal_v1_stack_pointer(&sp);

	/* The public stack object has padding on LP64. Never disclose it. */
	memset(&frame, 0, sizeof(frame));
	frame.stack.ss_sp = (void __user *)current->sas_ss_sp;
	frame.stack.ss_size = current->sas_ss_size;
	frame.stack.ss_flags = current->sas_ss_flags;
	frame.mask = saved_mask;
	frame.previous = previous_frame;
	frame.previous_info = previous_info;

	/* Old modules may still receive ordinary callbacks without a C stack
	 * export. A registered alternative stack, however, requires the v1 ABI. */
	if (sp_error && current->sas_ss_size) {
		force_sigsegv(ksig->sig);
		return;
	}
	if ((sa->sa_flags & SA_ONSTACK) && !sas_ss_flags(sp)) {
		if (check_add_overflow(current->sas_ss_sp, current->sas_ss_size, &end)) {
			force_sigsegv(ksig->sig);
			return;
		}
		sp = end & ~15UL;
		callback_flags |= 2;
	}

	/*
	 * wasm invokes the userspace handler synchronously. Mirror the mask setup
	 * normally performed before returning through a userspace signal frame,
	 * then restore it in place of rt_sigreturn when the callback returns.
	 */
	signal_setup_done(0, ksig, 0);
	ti->active_signal_frame = &frame;
	ti->active_siginfo = (sa->sa_flags & SA_SIGINFO) ? &ksig->info : NULL;

	/*
	 * The callback runs userspace code before the outer syscall exit has
	 * completed its context-tracking transition. Make that transition here so
	 * syscalls made by a handler enter from a real userspace context.
	 */
	regs->user_mode = 1;

	failed = wasm_run_user_signal(
		(uintptr_t)sa->sa_restorer, (uintptr_t)sa->sa_handler, ksig->sig,
		sp, (unsigned long)frame.stack.ss_sp, frame.stack.ss_size,
		callback_flags);

	/*
	 * A synchronous handler can make nested syscalls, which use the same
	 * task_pt_regs storage. Preserve the interrupted syscall's result and
	 * restart decision across those calls.
	 */
	*regs = saved_regs;
	ti->active_signal_frame = previous_frame;
	ti->active_siginfo = previous_info;
	/* Like rt_sigreturn, normal callback completion restores the saved/edited
	 * registration, including one temporarily disabled by SS_AUTODISARM.
	 * A SjLj exit skips this restoration and keeps the disarmed state. */
	/* As restore_altstack() does, ignore invalid settings from a signal
	 * context while retaining the generic on-stack and size checks. Use the
	 * delivery SP: a non-autodisarmed active stack remains protected until
	 * the callback has returned to the interrupted C frame. */
	(void)do_sigaltstack(&frame.stack, NULL, sp, MINSIGSTKSZ);
	sigdelsetmask(&frame.mask, sigmask(SIGKILL) | sigmask(SIGSTOP));
	set_current_blocked(&frame.mask);
	if (failed)
		force_sigsegv(ksig->sig);
}

__attribute__((export_name("copy_siginfo"))) int
wasm_copy_active_siginfo(siginfo_t __user *to)
{
	const kernel_siginfo_t *info = current_thread_info()->active_siginfo;

	return info ? copy_siginfo_to_user(to, info) : -EINVAL;
}

__attribute__((export_name("clear_siginfo"))) void
wasm_clear_active_siginfo(void)
{
	struct thread_info *ti = current_thread_info();
	struct wasm_signal_frame *frame = ti->active_signal_frame;

	/* A SjLj may land inside an outer handler which is still live. Its
	 * frame must remain available to the restorer, not be cleared with
	 * the escaped inner frame. Each JS callback unwinds exactly one level. */
	ti->active_siginfo = frame ? frame->previous_info : NULL;
	ti->active_signal_frame = frame ? frame->previous : NULL;
}

__attribute__((export_name("copy_signal_frame"))) int
wasm_copy_active_signal_frame(stack_t __user *stack, sigset_t __user *mask)
{
	const struct wasm_signal_frame *frame = current_thread_info()->active_signal_frame;

	if (!frame)
		return -EINVAL;
	return copy_to_user(stack, &frame->stack, sizeof(frame->stack)) ||
	       copy_to_user(mask, &frame->mask, sizeof(frame->mask)) ? -EFAULT : 0;
}

__attribute__((export_name("finish_signal_frame"))) int
wasm_finish_active_signal_frame(const stack_t __user *stack,
			       const sigset_t __user *mask)
{
	struct wasm_signal_frame *frame = current_thread_info()->active_signal_frame;
	stack_t updated_stack;
	sigset_t updated_mask;

	if (!frame)
		return -EINVAL;
	if (copy_from_user(&updated_stack, stack, sizeof(updated_stack)) ||
	    copy_from_user(&updated_mask, mask, sizeof(updated_mask)))
		return -EFAULT;
	frame->stack = updated_stack;
	frame->mask = updated_mask;
	return 0;
}

void arch_do_signal_or_restart(struct pt_regs *regs)
{
	struct ksignal ksig;

	if (get_signal(&ksig)) {
		syscall_restart(regs, true, &ksig.ka);
		call_signal_handler(&ksig, regs);
		return;
	}

	syscall_restart(regs, false, NULL);
	restore_saved_sigmask();
}
