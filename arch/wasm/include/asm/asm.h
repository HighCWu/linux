/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_WASM_ASM_H
#define _ASM_WASM_ASM_H

#if __SIZEOF_POINTER__ == 8
#define WASM_PTR_TYPE "i64"
#elif __SIZEOF_POINTER__ == 4
#define WASM_PTR_TYPE "i32"
#else
#error "Unsupported WebAssembly pointer size"
#endif

#endif
