// SPDX-License-Identifier: GPL-2.0
/* Private, opt-in fixtures, not Linux UAPI or production file mmap. */
#include <linux/anon_inodes.h>
#include <linux/atomic.h>
#include <linux/completion.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/uio.h>
#include <asm/wasm_imports.h>
#include <asm/mmap_file.h>

/* Kernel-global gauges, never reset by a caller. Only this opt-in fixture's
 * resources are counted, not all kernel allocations or user allocator state.
 */
static atomic_t test_files = ATOMIC_INIT(0);

SYSCALL_DEFINE1(wasm_mmap_stats_test, unsigned int, selector)
{
	switch (selector) {
	case 2:
		return atomic_read(&test_files);
	default:
		return wasm_mmap_copy_stat(selector);
	}
}

struct wasm_test_file {
	size_t size;
	unsigned int mode;
	struct completion entered;
	struct completion proceed;
	u8 data[];
};

/* Query only. Neither this fixture nor the helpers admit standard file mmap. */
SYSCALL_DEFINE1(wasm_mmap_provenance_test, int, fd)
{
	struct file *file = fget(fd);
	long result;

	if (!file)
		return -EBADF;
	result = wasm_mmap_snapshot_source(file);
	fput(file);
	return result;
}

static ssize_t test_read_iter(struct kiocb *iocb, struct iov_iter *iter)
{
	struct wasm_test_file *data = iocb->ki_filp->private_data;
	loff_t pos = iocb->ki_pos;
	size_t count, copied;

	if (pos < 0)
		return -EINVAL;
	if (data->mode == 3)
		return -EINTR;
	if ((data->mode == 4 && !pos) || (data->mode == 5 && pos >= 7)) {
		int error;

		complete_all(&data->entered);
		error = wait_for_completion_interruptible(&data->proceed);
		if (error)
			return error;
		if (data->mode == 5)
			return -EIO;
	}
	if ((data->mode == 1 || data->mode == 2) && pos >= 7)
		return data->mode == 1 ? 0 : -EIO;
	if (pos >= data->size)
		return 0;
	count = min_t(size_t, iov_iter_count(iter), data->size - pos);
	if (data->mode == 1 || data->mode == 2 || data->mode == 5)
		count = min_t(size_t, count, 7 - pos);
	copied = copy_to_iter(data->data + pos, count, iter);
	iocb->ki_pos += copied;
	return copied || !count ? copied : -EFAULT;
}

static loff_t test_llseek(struct file *file, loff_t offset, int whence)
{
	struct wasm_test_file *data = file->private_data;

	return fixed_size_llseek(file, offset, whence, data->size);
}

static int test_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	atomic_dec(&test_files);
	return 0;
}

static int test_open(struct inode *inode, struct file *file)
{
	/* anon_inode_getfile creates the sole initialized file directly; prevent
	 * reopening its inode with a new, uninitialized private_data pointer. */
	return -ENXIO;
}

/* Private barrier controls on the test inode only, with no pointer arguments.
 * The controller keeps a separate descriptor; close/reuse of the reader's fd
 * must not redirect the fget-pinned request to a different file.
 */
static long test_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
	struct wasm_test_file *data = file->private_data;

	if (data->mode < 4)
		return -ENOTTY;
	if (arg)
		return -EINVAL;
	switch (command) {
	case 0x5770: /* WAIT_ENTERED */
		return wait_for_completion_interruptible(&data->entered);
	case 0x5771: /* PROCEED */
		complete_all(&data->proceed);
		return 0;
	default:
		return -ENOTTY;
	}
}

/* Only this fixture owns these bytes; there are no write, truncate or mmap
 * operations. An arbitrary O_RDONLY descriptor is NOT an immutable file. */
static const struct file_operations test_file_ops = {
	.open = test_open,
	.read_iter = test_read_iter,
	.llseek = test_llseek,
	.release = test_release,
	.unlocked_ioctl = test_ioctl,
};

SYSCALL_DEFINE1(wasm_mmap_file_create_test, unsigned int, mode)
{
	const size_t size = 2 * PAGE_SIZE + 7;
	struct wasm_test_file *data;
	struct file *file;
	size_t i;
	int fd;

	if (mode > 5)
		return -EINVAL;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0)
		return fd;
	data = kmalloc(sizeof(*data) + size, GFP_KERNEL);
	if (!data) {
		put_unused_fd(fd);
		return -ENOMEM;
	}
	data->size = size;
	data->mode = mode;
	init_completion(&data->entered);
	init_completion(&data->proceed);
	for (i = 0; i < size; i++)
		data->data[i] = (u8)(i * 37 + 11);
	file = anon_inode_getfile("wasm-mmap-test", &test_file_ops, data, O_RDONLY);
	if (IS_ERR(file)) {
		kfree(data);
		put_unused_fd(fd);
		return PTR_ERR(file);
	}
	atomic_inc(&test_files);
	fd_install(fd, file);
	return fd;
}

SYSCALL_DEFINE4(wasm_mmap_file_read_test, int, fd, unsigned long, length,
		u32, offset_low, u32, offset_high)
{
	u64 offset = ((u64)offset_high << 32) | offset_low;
	struct file *file;
	loff_t size;
	long result;

	if (!length || length > 2 * PAGE_SIZE || offset > LLONG_MAX ||
	    !PAGE_ALIGNED(offset))
		return -EINVAL;
	file = fget(fd);
	if (!file)
		return -EBADF;
	result = -EOPNOTSUPP;
	if (file->f_op == &test_file_ops) {
		struct wasm_test_file *data = file->private_data;

		size = data->size;
	} else if (S_ISREG(file_inode(file)->i_mode) && wasm_mmap_snapshot_source(file)) {
		/* Experimental eager copy only: regular files on the verified single
		 * snapshot bdev. No generic read-only, initramfs or overlay admission. */
		size = i_size_read(file_inode(file));
	} else {
		goto put;
	}
	result = -EBADF;
	if (!(file->f_mode & FMODE_READ))
		goto put;
	result = wasm_mmap_copy_file(file, length, offset, size);
put:
	fput(file);
	return result;
}
