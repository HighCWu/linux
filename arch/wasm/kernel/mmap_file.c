// SPDX-License-Identifier: GPL-2.0
/* Bounded eager private copies, not an MMU, shared mappings or page protection. */
#include <linux/atomic.h>
#include <linux/erofs.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/security.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <asm/mmap_file.h>
#include <asm/virtio_snapshot.h>
#include <asm/wasm_imports.h>
#include <asm/thread_info.h>

#define COPY_BUDGET (8 * PAGE_SIZE)
static atomic_t copy_reserved = ATOMIC_INIT(0);
static atomic_t copy_requests = ATOMIC_INIT(0);
static atomic_t copy_staging = ATOMIC_INIT(0);

/* Heap-owned because a user SjLj/trap can discard the original C frame.
 * Only publish after kernel_read has returned: exit must never free a
 * buffer still owned by an asynchronous producer. Each task owns its list. */
struct wasm_mmap_callback {
	struct wasm_mmap_callback *next;
	struct file *file;
	void *staging;
	unsigned long rounded;
};

static void release_callback(struct wasm_mmap_callback *callback)
{
	kfree(callback->staging);
	atomic_dec(&copy_staging);
	atomic_sub(2 * callback->rounded, &copy_reserved);
	atomic_dec(&copy_requests);
	fput(callback->file);
	kfree(callback);
}

void wasm_mmap_copy_abort_task(struct task_struct *task)
{
	struct thread_info *info = task_thread_info(task);
	struct wasm_mmap_callback *callback;

	while ((callback = info->mmap_callbacks)) {
		info->mmap_callbacks = callback->next;
		release_callback(callback);
	}
}

/* Internal execution export, never a Linux syscall or user module import.
 * Called before the bridge rethrows a callback exception, while this task
 * still owns its CPU. A callback's exit instead cleans in exit_thread(). */
#ifdef __wasm__
__attribute__((export_name("abort_file_mmap_v1")))
#endif
void wasm_abort_file_mmap_v1(void)
{
	struct thread_info *info = task_thread_info(current);
	struct wasm_mmap_callback *callback = info->mmap_callbacks;

	if (callback) {
		info->mmap_callbacks = callback->next;
		release_callback(callback);
	}
}
#ifdef CONFIG_WASM_MMAP_COPY_TEST
static atomic_t copy_callback_attempts = ATOMIC_INIT(0);
#endif

static bool reserve_copy(unsigned long rounded)
{
	int charge = 2 * rounded;
	int used = atomic_read(&copy_reserved);

	do {
		if (used > (int)COPY_BUDGET - charge)
			return false;
	} while (!atomic_try_cmpxchg(&copy_reserved, &used, used + charge));
	return true;
}

/* Internal gauges, exposed only by the opt-in private test syscall. */
long wasm_mmap_copy_stat(unsigned int selector)
{
	switch (selector) {
	case 0:
		return atomic_read(&copy_requests);
	case 1:
		return atomic_read(&copy_staging);
	case 3:
		return atomic_read(&copy_reserved);
#ifdef CONFIG_WASM_MMAP_COPY_TEST
	case 0x100:
		return atomic_read(&copy_callback_attempts);
#endif
	default:
		return -EINVAL;
	}
}

bool wasm_mmap_snapshot_source(struct file *file)
{
	struct super_block *sb = file_inode(file)->i_sb;

	return erofs_is_single_bdev(sb) &&
		wasm_bdev_has_snapshot_source(sb->s_bdev);
}

/* Consumes the caller's file reference and uses its authenticated size/source. No userspace or
 * host-selected pathname is read. kernel_read owns its producer until return;
 * the synchronous initialized-copy bridge never publishes partial data. */
long wasm_mmap_copy_file(struct file *file, unsigned long length,
			u64 offset, loff_t size)
{
	unsigned long rounded;
	loff_t position;
	size_t count, done = 0;
	u8 *staging;
	struct wasm_mmap_callback *callback;
	struct thread_info *info;
	long result = -EINVAL;

	if (!length || length > 2 * PAGE_SIZE || offset > LLONG_MAX ||
	    !PAGE_ALIGNED(offset))
		goto put;
	rounded = ALIGN(length, PAGE_SIZE);
	atomic_inc(&copy_requests);
	/* Permit a final partial page, never a page wholly beyond EOF. */
	if (size <= 0 || offset >= size || rounded - PAGE_SIZE >= size - offset)
		goto out;
	count = min_t(u64, rounded, size - offset);
	result = -ENOMEM;
	if (!reserve_copy(rounded))
		goto out;
	staging = kmalloc(count, GFP_KERNEL);
	if (!staging)
		goto unreserve;
	atomic_inc(&copy_staging);
	position = offset;
	while (done < count) {
		ssize_t read;

		if (fatal_signal_pending(current)) {
			result = -EINTR;
			goto free;
		}
		read = kernel_read(file, staging + done, count - done, &position);

		if (read <= 0) {
			result = read ? read : -EIO;
			goto free;
		}
		done += read;
	}
	/* kernel_read has returned, so no producer can still write staging.
	 * A fatal signal observed here must not start a new user allocator
	 * callback. Keep ownership until this point, then take ordinary cleanup. */
	if (fatal_signal_pending(current)) {
		result = -EINTR;
		goto free;
	}
	callback = kmalloc(sizeof(*callback), GFP_KERNEL);
	if (!callback) {
		result = -ENOMEM;
		goto free;
	}
	info = task_thread_info(current);
	callback->file = file;
	callback->staging = staging;
	callback->rounded = rounded;
	callback->next = info->mmap_callbacks;
	info->mmap_callbacks = callback;
#ifdef CONFIG_WASM_MMAP_COPY_TEST
	atomic_inc(&copy_callback_attempts);
#endif
	result = wasm_user_mmap_init_v1_map(rounded, staging, count);
	info->mmap_callbacks = callback->next;
	release_callback(callback);
	return result;
free:
	kfree(staging);
	atomic_dec(&copy_staging);
unreserve:
	atomic_sub(2 * rounded, &copy_reserved);
out:
	atomic_dec(&copy_requests);
put:
	fput(file);
	return result;
}

#ifdef CONFIG_WASM_FILE_MMAP
/* Standard Linux mmap/mmap2 shape; the versioned bridge carries only a bounded
 * copy, never fd/offset/provenance. NOMMU PROT_READ is not enforced write
 * protection. Each request receives independent backing, even when read-only. */
long wasm_mmap_file(unsigned long addr, unsigned long length,
		    unsigned long prot, unsigned long flags, long fd,
		    unsigned long raw_offset)
{
	u64 offset = raw_offset;
	struct file *file;
	long result;

	(void)addr; /* A non-fixed placement hint may be ignored. */
	if (!length || (flags & MAP_TYPE) != MAP_PRIVATE)
		return -EINVAL;
	if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE))
		return -ENOMEM;
	if (flags != MAP_PRIVATE ||
	    (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE)))
		return -EINVAL;
	if (length > 2 * PAGE_SIZE)
		return -ENOMEM;
#ifdef CONFIG_32BIT
	/* mmap2 uses 4096-byte units, not the architecture's 64-KiB page. */
	offset <<= 12;
#endif
	if (offset > LLONG_MAX - ALIGN(length, PAGE_SIZE))
		return -EOVERFLOW;
	if (!PAGE_ALIGNED(offset))
		return -EINVAL;
	file = fget(fd);
	if (!file)
		return -EBADF;
	result = -EACCES;
	if (!(file->f_mode & FMODE_READ))
		goto put;
	result = -EOPNOTSUPP;
	if (!S_ISREG(file_inode(file)->i_mode) || !wasm_mmap_snapshot_source(file))
		goto put;
	/* Old modules must fail before staging allocation or any VFS read.
	 * Bit 0 describes initialized private-copy execution, not file admission. */
	if (!(wasm_user_mmap_init_v1_capabilities() & 1))
		goto put;
	result = security_mmap_file(file, prot, flags);
	if (result)
		goto put;
	return wasm_mmap_copy_file(file, length, offset,
				   i_size_read(file_inode(file)));
put:
	fput(file);
	return result;
}
#endif
