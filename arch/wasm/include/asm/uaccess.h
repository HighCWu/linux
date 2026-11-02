/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _WASM_UACCESS_H
#define _WASM_UACCESS_H

#include <asm/wasm_imports.h>
#include <linux/unaligned.h>

#define __access_ok __access_ok
static inline int __access_ok(const void __user *ptr, unsigned long size)
{
	unsigned long limit = TASK_SIZE;
	unsigned long addr = (unsigned long)ptr;

	return (size <= limit) && (addr <= (limit - size));
}

#define raw_copy_from_user wasm_user_read
#define raw_copy_to_user wasm_user_write
#define __clear_user wasm_user_write_zeroes

/* WASM_KERNEL_NOFAULT_BEGIN */
/* Kernel addresses belong to this linear memory, unlike user offsets. Wasm
 * traps cannot return through a C exception table. Check the complete typed
 * access before touching it, using current capacity rather than reserved RAM.
 * Compare pages to avoid overflowing a byte limit at a full 4 GiB memory32.
 */
static inline int wasm_kernel_nofault_range(const void *ptr, unsigned long size)
{
	unsigned long first = (unsigned long)ptr;
	unsigned long last = first + size - 1;

	return first && size && last >= first &&
	       (last >> 16) < __builtin_wasm_memory_size(0);
}

#define __get_kernel_nofault(dst, src, type, err_label)			\
do {									\
	const void *__nofault_src = (const void *)(src);		\
	if (!wasm_kernel_nofault_range(__nofault_src, sizeof(type)))	\
		goto err_label;						\
	*((type *)dst) = get_unaligned((const type *)__nofault_src);	\
} while (0)

#define __put_kernel_nofault(dst, src, type, err_label)			\
do {									\
	void *__nofault_dst = (void *)(dst);			\
	if (!wasm_kernel_nofault_range(__nofault_dst, sizeof(type)))	\
		goto err_label;						\
	put_unaligned(*((type *)src), (type *)__nofault_dst);		\
} while (0)
/* WASM_KERNEL_NOFAULT_END */

#define INLINE_COPY_FROM_USER
#define INLINE_COPY_TO_USER

#include <asm-generic/uaccess.h>

#endif
