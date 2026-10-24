/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _WASM_USER_CONTEXT_H
#define _WASM_USER_CONTEXT_H

#include <linux/types.h>

/*
 * A phase change does not run pending return work. In particular, a clone
 * callback must initialize its user C stack/TLS before servicing signals.
 */
void wasm_set_user_context(bool user);

#endif
