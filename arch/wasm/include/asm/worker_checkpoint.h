/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _WASM_WORKER_CHECKPOINT_H
#define _WASM_WORKER_CHECKPOINT_H

#include <linux/types.h>

struct task_struct;
struct pt_regs;

bool wasm_worker_checkpoint_update(struct task_struct *task);
void wasm_worker_checkpoint_wait(void);
void wasm_worker_prepare_user_return(struct pt_regs *regs);

#endif
