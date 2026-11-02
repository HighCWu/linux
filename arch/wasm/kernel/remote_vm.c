// SPDX-License-Identifier: GPL-2.0

#include <asm/irq.h>
#include <asm/remote_vm.h>
#include <asm/thread_info.h>
#include <asm/wasm_imports.h>
#include <linux/mm.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/limits.h>
#include <linux/kdev_t.h>
#include <linux/rcupdate.h>
#include <linux/sched/signal.h>
#include <linux/time64.h>
#include <linux/uaccess.h>

#define WASM_REMOTE_WAIT_CHUNK_NS	NSEC_PER_SEC
#define WASM_REMOTE_WAIT_TIMEOUT_NS	(5ULL * NSEC_PER_SEC)

static void release_dynamic_files(struct mm_struct *mm);

struct wasm_remote_request {
	struct mm_struct *mm;
	unsigned long user_addr;
	void *kernel_addr;
	unsigned int len;
	bool write;
	int result;
};

/*
 * A registered mm can be copied by the machine agent without interrupting its
 * user Worker. The cooperative mailbox remains available until registration
 * completes. Neither route bypasses the generic caller's mm_access checks.
 */

__attribute__((export_name("current_remote_memory_id")))
int wasm_current_remote_memory_id(void)
{
	struct mm_struct *mm = current->mm;

	if (!mm || READ_ONCE(current_thread_info()->context_mm) != mm)
		return 0;
	return mm->context.remote.host_id;
}

__attribute__((export_name("remote_memory_ready")))
int wasm_remote_memory_ready(int id)
{
	struct mm_struct *mm = current->mm;
	struct wasm_remote_slot *slot;

	if (id <= 0 || wasm_current_remote_memory_id() != id)
		return -EINVAL;
	slot = &mm->context.remote;
	/* The host has already acknowledged retaining this mm's Memory. */
	atomic_set_release(&slot->host_ready, 1);
	__builtin_wasm_memory_atomic_notify(&slot->state.counter, 1);
	return 0;
}

static int wasm_host_remote_copy(struct wasm_remote_slot *slot,
				struct wasm_remote_request *request)
{
	int copied = wasm_remote_memory_v1_copy(slot->host_id,
		request->user_addr, request->kernel_addr, request->len,
		request->write);

	return clamp(copied, 0, (int)request->len);
}

/*
 * Service one request using the user WebAssembly.Memory local to @task's
 * worker. This is also called while a worker owns no logical CPU, so it must
 * not use current, percpu state, allocation, the scheduler, ordinary locks, or
 * anything else that assumes normal task context.
 *
 * The mm-lifetime state word arbitrates request lifetime:
 *
 *   IDLE -> PUBLISHED -> CLAIMED -> DONE -> IDLE
 *                      \
 *                       ` requester cancellation -> IDLE
 *
 * The requester stores the pointer before release-publishing PUBLISHED. A
 * service worker must acquire-claim PUBLISHED before it may load that pointer,
 * so cancellation can safely return with a stack-backed request. The service
 * worker writes the result before release-publishing DONE; the requester reads
 * it only after an acquire load observes DONE.
 *
 * CLAIMED is irrevocable. wasm_user_read() and wasm_user_write() are
 * synchronous host imports: the servicing worker cannot schedule, exec, or
 * exit between claiming the request and publishing DONE. External worker
 * termination only occurs while the whole machine, including the requester,
 * is being torn down.
 */
void wasm_service_remote_request(struct task_struct *task)
{
	struct thread_info *ti = task_thread_info(task);
	struct wasm_remote_request *request;
	struct wasm_remote_slot *slot;
	struct mm_struct *mm;
	unsigned long not_copied;

	mm = READ_ONCE(task->mm);
	if (!mm)
		return;

	slot = &mm->context.remote;
	/* The normal syscall-entry path is one unordered mailbox-state load. */
	if (atomic_read(&slot->state) != WASM_REMOTE_PUBLISHED)
		return;
	if (READ_ONCE(ti->context_mm) != mm)
		return;
	if (atomic_cmpxchg_acquire(&slot->state, WASM_REMOTE_PUBLISHED,
				   WASM_REMOTE_CLAIMED) !=
	    WASM_REMOTE_PUBLISHED)
		return;

	request = READ_ONCE(slot->request);
	BUG_ON(!request || request->mm != mm);

	if (request->write)
		not_copied = wasm_user_write(
			(void __user *)request->user_addr,
			request->kernel_addr, request->len);
	else
		not_copied = wasm_user_read(
			request->kernel_addr,
			(const void __user *)request->user_addr, request->len);

	WRITE_ONCE(request->result,
		   request->len - min_t(unsigned long, not_copied,
					request->len));

	atomic_set_release(&slot->state, WASM_REMOTE_DONE);
	__builtin_wasm_memory_atomic_notify(&slot->state.counter, 1);
}

/*
 * Wake every user task whose worker could own this mm's local user context.
 * thread_info is embedded in the RCU-protected task_struct on wasm.
 */
static bool wasm_kick_remote_mm(struct mm_struct *mm)
{
	struct task_struct *group, *task;
	bool found = false;

	rcu_read_lock();
	for_each_process_thread(group, task) {
		struct thread_info *ti;
		int running_cpu;

		if (task->flags & PF_KTHREAD || READ_ONCE(task->mm) != mm)
			continue;

		found = true;
		ti = task_thread_info(task);
		running_cpu = atomic_cmpxchg(&ti->running_cpu,
					    WASM_CPU_PARKED,
					    WASM_CPU_REMOTE_WAKE);
		if (running_cpu == WASM_CPU_PARKED)
			__builtin_wasm_memory_atomic_notify(
				&ti->running_cpu.counter, 1);
	}
	rcu_read_unlock();

	return found;
}

static bool wasm_cancel_remote_request(struct wasm_remote_slot *slot)
{
	if (atomic_cmpxchg(&slot->state, WASM_REMOTE_PUBLISHED,
			   WASM_REMOTE_IDLE) != WASM_REMOTE_PUBLISHED)
		return false;

	/*
	 * No service worker may load the pointer without first winning the
	 * PUBLISHED -> CLAIMED CAS. The mutex prevents a new publisher until
	 * after this pointer has been cleared.
	 */
	WRITE_ONCE(slot->request, NULL);
	return true;
}

int wasm_access_remote_vm(struct mm_struct *mm, unsigned long addr, void *buf,
			  int len, unsigned int gup_flags)
{
	struct wasm_remote_request request;
	struct wasm_remote_slot *slot = &mm->context.remote;
	u64 deadline;
	int result = 0;

	if (len <= 0 || addr >= TASK_SIZE)
		return 0;
	/* Bound the mailbox payload, not the address's offset in a Wasm page.
	 * Adjacent linear-memory bytes need no separate page pin. In particular
	 * ptrace PEEK/POKE must transfer an unaligned word across that boundary.
	 * Preserve a valid prefix at the architecture's user-address limit.
	 */
	len = min_t(unsigned long, len, PAGE_SIZE);
	len = min_t(unsigned long, len, TASK_SIZE - addr);
	if (!access_ok((void __user *)addr, len))
		return 0;

	request = (struct wasm_remote_request) {
		.mm = mm,
		.user_addr = addr,
		.kernel_addr = buf,
		.len = len,
		.write = gup_flags & FOLL_WRITE,
		.result = 0,
	};

	/*
	 * Queueing requesters sleep schedulably on the mutex. Only the single
	 * in-flight requester holds its logical CPU in a wasm atomic wait.
	 */
	mutex_lock(&slot->mutex);
	if (!slot->accepting) {
		mutex_unlock(&slot->mutex);
		return 0;
	}
	BUG_ON(atomic_read(&slot->state) != WASM_REMOTE_IDLE);
	if (atomic_read_acquire(&slot->host_ready)) {
		result = wasm_host_remote_copy(slot, &request);
		goto out;
	}
	WRITE_ONCE(slot->request, &request);
	atomic_set_release(&slot->state, WASM_REMOTE_PUBLISHED);
	deadline = wasm_kernel_get_now_nsec() + WASM_REMOTE_WAIT_TIMEOUT_NS;

	for (;;) {
		u64 timer_deadline;
		u64 now;
		s64 wait_ns;
		int state;

		/* Reciprocal A -> B and B -> A reads must make progress. */
		wasm_service_remote_request(current);

		state = atomic_read_acquire(&slot->state);
		if (state == WASM_REMOTE_DONE) {
			result = READ_ONCE(request.result);
			WRITE_ONCE(slot->request, NULL);
			atomic_set_release(&slot->state, WASM_REMOTE_IDLE);
			break;
		}

		now = wasm_kernel_get_now_nsec();
		if (state == WASM_REMOTE_PUBLISHED) {
			/* Registration may finish after mailbox publication. Revoke
			 * that request before copying, or join its existing claimant.
			 */
			if (atomic_read_acquire(&slot->host_ready)) {
				if (wasm_cancel_remote_request(slot)) {
					result = wasm_host_remote_copy(slot, &request);
					break;
				}
				continue;
			}
			/*
			 * A task is visible before its asynchronously-created
			 * worker is ready, and exec temporarily makes task->mm
			 * lead context_mm. Both are live and worth retrying.
			 */
			if (!wasm_kick_remote_mm(mm) ||
			    (s64)(now - deadline) >= 0) {
				if (wasm_cancel_remote_request(slot))
					break;
				continue;
			}
			wait_ns = min_t(u64, WASM_REMOTE_WAIT_CHUNK_NS,
					deadline - now);
		} else {
			/*
			 * Once claimed, the requester must retain its stack and
			 * destination buffer until the synchronous host import
			 * publishes DONE. Claims copy at most PAGE_SIZE through
			 * a checked, non-throwing host primitive.
			 */
			BUG_ON(state != WASM_REMOTE_CLAIMED);
			wait_ns = WASM_REMOTE_WAIT_CHUNK_NS;
		}

		/*
		 * The requester retains this logical CPU while blocked. Do not
		 * let a remote copy defer an hrtimer already armed on the CPU.
		 */
		timer_deadline = wasm_get_timer_deadline();
		if (timer_deadline) {
			s64 timer_wait_ns = (s64)(timer_deadline - now);

			wait_ns = min_t(s64, wait_ns,
					max_t(s64, timer_wait_ns, 0));
		}

		__builtin_wasm_memory_atomic_wait32(&slot->state.counter,
						    state, wait_ns);
		/*
		 * This worker still owns a logical CPU while waiting. Poll its
		 * clockevent between bounded waits so timers cannot remain
		 * stalled for the duration of the remote access.
		 */
		wasm_timer_check();
	}

out:
	mutex_unlock(&slot->mutex);
	return result;
}

void wasm_remote_mm_shutdown(struct mm_struct *mm)
{
	struct wasm_remote_slot *slot = &mm->context.remote;

	/*
	 * access_remote_vm() callers hold an mm reference. Taking the admission
	 * mutex after mm_users reaches zero therefore finds an idle slot, but
	 * also makes that lifetime rule explicit and drains any caller that
	 * reached this path with a weaker reference.
	 */
	mutex_lock(&slot->mutex);
	WRITE_ONCE(slot->accepting, false);
	WARN_ON_ONCE(atomic_read(&slot->state) != WASM_REMOTE_IDLE);
	WARN_ON_ONCE(READ_ONCE(slot->request));
	/* mm_users has drained, including mm_access users. Also release an
	 * acknowledged registration whose Worker failed before publishing ready.
	 */
	if (slot->host_id)
		wasm_remote_memory_v1_release(slot->host_id);
	mutex_unlock(&slot->mutex);
	release_dynamic_files(mm);
}

/* WASM_DYNAMIC_FILE_REFS_BEGIN
 * A compiled Wasm library has no executable VMA. Keep its source file alive so
 * device/inode identities cannot be recycled underneath resident loader state.
 * These are mm-owned fget references, never visible or closeable as user fds.
 * The host supplies an fd obtained by the process's normal Linux open/read path.
 */
struct wasm_dynamic_file {
	struct wasm_dynamic_file *next;
	struct file *file;
	int id;
};

#ifdef __wasm__
#define DYNAMIC_EXPORT(name) __attribute__((export_name(name)))
#else
#define DYNAMIC_EXPORT(name)
#endif

DYNAMIC_EXPORT("dynamic_file_pin_abi_v1")
int wasm_dynamic_file_pin_abi_v1(void)
{
	return 1;
}

DYNAMIC_EXPORT("dynamic_file_pin_v1")
int wasm_dynamic_file_pin_v1(int fd, u64 device, u64 inode)
{
	struct mm_struct *mm = current->mm;
	struct wasm_dynamic_file *pin;
	struct file *file;
	int result;

	if (!mm || READ_ONCE(current_thread_info()->context_mm) != mm)
		return -EAGAIN;
	file = fget(fd);
	if (!file)
		return -EBADF;
	if (!(file->f_mode & FMODE_READ) || !S_ISREG(file_inode(file)->i_mode)) {
		fput(file);
		return -EACCES;
	}
	if (huge_encode_dev(file_inode(file)->i_sb->s_dev) != device ||
	    file_inode(file)->i_ino != inode) {
		fput(file);
		return -ESTALE;
	}
	pin = kmalloc(sizeof(*pin), GFP_KERNEL);
	if (!pin) {
		fput(file);
		return -ENOMEM;
	}
	mutex_lock(&mm->context.remote.mutex);
	if (!mm->context.remote.accepting) {
		result = -ESRCH;
	} else if (mm->context.dynamic_file_count >= 1024 ||
		   mm->context.dynamic_file_next == INT_MAX) {
		result = -ENOSPC;
	} else {
		pin->file = file;
		pin->id = ++mm->context.dynamic_file_next;
		pin->next = mm->context.dynamic_files;
		mm->context.dynamic_files = pin;
		mm->context.dynamic_file_count++;
		result = pin->id;
	}
	mutex_unlock(&mm->context.remote.mutex);
	if (result < 0) {
		fput(file);
		kfree(pin);
	}
	return result;
}

DYNAMIC_EXPORT("dynamic_file_unpin_v1")
int wasm_dynamic_file_unpin_v1(int id)
{
	struct mm_struct *mm = current->mm;
	struct wasm_dynamic_file **link, *pin = NULL;

	if (!mm || READ_ONCE(current_thread_info()->context_mm) != mm)
		return -EAGAIN;
	mutex_lock(&mm->context.remote.mutex);
	for (link = &mm->context.dynamic_files; *link; link = &(*link)->next) {
		if ((*link)->id != id)
			continue;
		pin = *link;
		*link = pin->next;
		mm->context.dynamic_file_count--;
		break;
	}
	mutex_unlock(&mm->context.remote.mutex);
	if (!pin)
		return -ENOENT;
	fput(pin->file);
	kfree(pin);
	return 0;
}

static void release_dynamic_files(struct mm_struct *mm)
{
	struct wasm_dynamic_file *pin;

	mutex_lock(&mm->context.remote.mutex);
	pin = mm->context.dynamic_files;
	mm->context.dynamic_files = NULL;
	mm->context.dynamic_file_count = 0;
	mutex_unlock(&mm->context.remote.mutex);
	while (pin) {
		struct wasm_dynamic_file *next = pin->next;

		fput(pin->file);
		kfree(pin);
		pin = next;
	}
}
#undef DYNAMIC_EXPORT
/* WASM_DYNAMIC_FILE_REFS_END */
