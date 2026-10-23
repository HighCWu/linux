/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _WASM_MMAP_FILE_H
#define _WASM_MMAP_FILE_H

#include <linux/types.h>

struct file;
struct task_struct;

bool wasm_mmap_snapshot_source(struct file *file);
/* Consumes the caller's file reference on every return/exception path. */
long wasm_mmap_copy_file(struct file *file, unsigned long length,
			u64 offset, loff_t size);
long wasm_mmap_copy_stat(unsigned int selector);
long wasm_mmap_file(unsigned long addr, unsigned long length,
		    unsigned long prot, unsigned long flags, long fd,
		    unsigned long raw_offset);

#ifdef CONFIG_WASM_FILE_COPY_CORE
void wasm_mmap_copy_abort_task(struct task_struct *task);
void wasm_abort_file_mmap_v1(void);
#else
static inline void wasm_mmap_copy_abort_task(struct task_struct *task) { }
#endif

#endif
