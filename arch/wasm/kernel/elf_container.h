/* SPDX-License-Identifier: MIT */
#ifndef _WASM_ELF_CONTAINER_H
#define _WASM_ELF_CONTAINER_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stddef.h>
#endif

struct wasm_elf_image_range {
	unsigned long long offset;
	unsigned long long size;
};

bool wasm_elf_container_parse(const unsigned char *prefix, size_t prefix_len,
			      unsigned long long file_size, unsigned int width,
			      struct wasm_elf_image_range *out);

#endif /* _WASM_ELF_CONTAINER_H */
