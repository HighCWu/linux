/* SPDX-License-Identifier: MIT */
/* Host substitutes only: no Linux atomic, VFS, LSM or Wasm execution proof. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>

typedef uint64_t u64;
typedef unsigned char u8;
typedef _Atomic int atomic_t;
#define ATOMIC_INIT(value) (value)
#define atomic_read(pointer) atomic_load(pointer)
#define atomic_inc(pointer) ((void)atomic_fetch_add(pointer, 1))
#define atomic_dec(pointer) ((void)atomic_fetch_sub(pointer, 1))
#define atomic_sub(value, pointer) ((void)atomic_fetch_sub(pointer, value))
#define atomic_try_cmpxchg(pointer, old, value) atomic_compare_exchange_weak(pointer, old, value)
#define PAGE_SIZE 65536UL
#define PAGE_ALIGNED(value) (!((value) & (PAGE_SIZE - 1)))
#define ALIGN(value, alignment) (((value) + (alignment) - 1) & ~((alignment) - 1))
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))
#define GFP_KERNEL 0
#define FMODE_READ 1
#define FMODE_CAN_READ 2
#define NOMMU_MAP_COPY 1
#ifndef MAP_TYPE
#define MAP_TYPE 0x0f
#endif

struct super_block { bool erofs; void *s_bdev; };
struct inode { unsigned int i_mode; struct super_block *i_sb; loff_t size; };
struct file;
struct file_operations { unsigned (*mmap_capabilities)(struct file *); };
static const struct file_operations operations;
struct file { unsigned int f_mode; struct inode *inode; loff_t f_pos;
	const struct file_operations *f_op; };
static struct super_block sb = { true, (void *)1 };
static struct inode inode = { S_IFREG, &sb, 2 * PAGE_SIZE + 7 };
static struct file file = { FMODE_READ | FMODE_CAN_READ, &inode, 23, &operations };
static bool mappable = true;
static bool can_mmap_file(struct file *f) { assert(f == &file); return mappable; }
static unsigned file_capabilities = NOMMU_MAP_COPY;
static unsigned mmap_capabilities(struct file *f) { assert(f == &file); return file_capabilities; }
static const struct file_operations explicit_operations = { mmap_capabilities };
static int pins, allocations, reads, copies, capabilities = 1;
static int allocation_attempts;
static int read_mode, fail_allocation, security_error, copy_error;
static unsigned long security_flags, security_prot;
static size_t read_total, last_length, last_rounded;
static u64 first_offset;
static bool fatal_pending, fatal_after_read;
struct wasm_mmap_callback;
struct thread_info { struct wasm_mmap_callback *mmap_callbacks; };
struct task_struct { struct thread_info info; };
static struct task_struct task;
#define current (&task)
#define task_thread_info(t) (&(t)->info)
static bool fatal_signal_pending(void *task) { (void)task; return fatal_pending; }

static struct inode *file_inode(struct file *f) { return f->inode; }
static loff_t i_size_read(struct inode *node) { return node->size; }
static bool erofs_is_single_bdev(struct super_block *s) { return s->erofs; }
static bool wasm_bdev_has_snapshot_source(void *bdev) { return bdev == (void *)1; }
static struct file *fget(long fd) {
	if (fd != 10) return NULL;
	pins++;
	return &file;
}
static void fput(struct file *f) { assert(f == &file && pins == 1); pins--; }
static unsigned int wasm_user_mmap_init_v1_capabilities(void) { return capabilities; }
static int security_mmap_file(struct file *f, unsigned long prot, unsigned long flags) {
	assert(f == &file && prot && !(prot & ~(PROT_READ | PROT_WRITE)));
	security_flags = flags;
	security_prot = prot;
	return security_error;
}
static void *kmalloc(size_t length, int flags) {
	(void)flags;
	allocation_attempts++;
	if (fail_allocation == 1 || (fail_allocation == 2 && length < 256)) return NULL;
	void *buffer = malloc(length);
	assert(buffer);
	allocations++;
	return buffer;
}
static void kfree(void *buffer) { assert(allocations > 0); allocations--; free(buffer); }
static ssize_t kernel_read(struct file *f, void *buffer, size_t length, loff_t *position) {
	assert(f == &file && pins == 1 && length > 0);
	reads++;
	if (!read_total) first_offset = *position;
	if (read_mode == 3) return -EINTR;
	if (read_mode == 4) return 0;
	if ((read_mode == 1 || read_mode == 2) && read_total) return read_mode == 1 ? 0 : -EIO;
	if (*position >= f->inode->size) return 0;
	if (length > (size_t)(f->inode->size - *position)) length = f->inode->size - *position;
	if (read_mode) length = 7;
	for (size_t i = 0; i < length; i++) ((u8 *)buffer)[i] = (u8)((*position + i) * 37 + 11);
	*position += length;
	read_total += length;
	if (fatal_after_read) fatal_pending = true;
	return length;
}
static long wasm_user_mmap_init_v1_map(unsigned long rounded, const void *source, unsigned long length) {
	assert(pins == 1 && allocations == 2 && length <= rounded);
	for (size_t i = 0; i < length; i++) assert(((const u8 *)source)[i] == (u8)((first_offset + i) * 37 + 11));
	copies++;
	last_length = length;
	last_rounded = rounded;
	return copy_error ? copy_error : 65536;
}
