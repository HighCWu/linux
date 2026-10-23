#include "asm/wasm_imports.h"
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/pid.h>

void show_stack(struct task_struct *task, unsigned long *sp, const char *loglvl)
{
	char trace[1024] = { 0 };

	/* The host import can inspect only the calling Worker. Never label its
	 * current engine stack as the stack of another Linux task. */
	if (task && task != current) {
		printk("%sStack for %s (%d) unavailable: separate Wasm worker\n",
		       loglvl, task->comm, task_pid_nr(task));
		return;
	}

	printk("%sStack from %s (%d):\n", loglvl, current->comm,
	       task_pid_nr(current));
	/* Keep diagnostics usable in atomic/OOM paths, including with an older
	 * host which fills the complete buffer without a string terminator. */
	wasm_kernel_get_stacktrace(trace, sizeof(trace));
	trace[sizeof(trace) - 1] = '\0';
	printk("%s%s\n", loglvl, trace);
}
