/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _WASM_REMOTE_VM_H
#define _WASM_REMOTE_VM_H

#include <linux/types.h>

struct task_struct;

void wasm_service_remote_request(struct task_struct *task);
int wasm_current_remote_memory_id(void);
int wasm_remote_memory_ready(int id);
int wasm_dynamic_file_pin_abi_v1(void);
int wasm_dynamic_file_pin_v1(int fd, u64 device, u64 inode);
int wasm_dynamic_file_unpin_v1(int id);

#endif
