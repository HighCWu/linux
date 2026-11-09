// SPDX-License-Identifier: MIT
#include "elf_container.h"

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#define ELF32_EHDR_SIZE 52U
#define ELF64_EHDR_SIZE 64U
#define ELF32_PHDR_SIZE 32U
#define ELF64_PHDR_SIZE 56U
#define ELF32_PAYLOAD_OFFSET 84U
#define ELF64_PAYLOAD_OFFSET 120U
#define ELF32_MODULE_OFFSET 116U
#define ELF64_MODULE_OFFSET 152U
#define ELF_OSSEG_WASM 0x60005741U

static unsigned int read_le16(const unsigned char *bytes)
{
	return (unsigned int)bytes[0] | (unsigned int)bytes[1] << 8;
}

static unsigned int read_le32(const unsigned char *bytes)
{
	return (unsigned int)bytes[0] |
	       (unsigned int)bytes[1] << 8 |
	       (unsigned int)bytes[2] << 16 |
	       (unsigned int)bytes[3] << 24;
}

static unsigned long long read_le64(const unsigned char *bytes)
{
	return (unsigned long long)read_le32(bytes) |
	       (unsigned long long)read_le32(bytes + 4) << 32;
}

static bool bytes_are_zero(const unsigned char *bytes, size_t size)
{
	for (size_t index = 0; index < size; index++) {
		if (bytes[index])
			return false;
	}
	return true;
}

bool wasm_elf_container_parse(const unsigned char *prefix, size_t prefix_len,
			      unsigned long long file_size, unsigned int width,
			      struct wasm_elf_image_range *out)
{
	struct wasm_elf_image_range image;
	const unsigned char *ident;
	const unsigned char *header;
	const unsigned char *program;
	const unsigned char *payload;
	unsigned int elf_class;
	unsigned int program_offset;
	unsigned int payload_offset;
	unsigned int module_offset;
	unsigned long long segment_offset;
	unsigned long long segment_size;
	unsigned long long module_size;
	unsigned int class_width;

	if (!prefix || !out || (width != 32 && width != 64))
		return false;
	if (file_size > 0xffffffffULL || file_size < ELF32_MODULE_OFFSET + 8U)
		return false;
	if (prefix_len < 16)
		return false;

	ident = prefix;
	if (ident[0] != 0x7f || ident[1] != 'E' || ident[2] != 'L' ||
	    ident[3] != 'F' || ident[5] != 1 || ident[6] != 1 ||
	    ident[7] != 3 || ident[8] != 0 || !bytes_are_zero(ident + 9, 7))
		return false;

	elf_class = ident[4];
	if (elf_class == 1) {
		class_width = 32;
		program_offset = ELF32_EHDR_SIZE;
		payload_offset = ELF32_PAYLOAD_OFFSET;
		module_offset = ELF32_MODULE_OFFSET;
	} else if (elf_class == 2) {
		class_width = 64;
		program_offset = ELF64_EHDR_SIZE;
		payload_offset = ELF64_PAYLOAD_OFFSET;
		module_offset = ELF64_MODULE_OFFSET;
	} else {
		return false;
	}
	if (class_width != width || file_size < (unsigned long long)module_offset + 8)
		return false;
	if (prefix_len < (size_t)module_offset + 8)
		return false;

	header = prefix + 16;
	if (read_le16(header) != 2 || read_le16(header + 2) != 0 ||
	    read_le32(header + 4) != 1)
		return false;
	if (elf_class == 1) {
		if (read_le32(header + 8) != 0 ||
		    read_le32(header + 12) != ELF32_EHDR_SIZE ||
		    read_le32(header + 16) != 0 || read_le32(header + 20) != 0 ||
		    read_le16(header + 24) != ELF32_EHDR_SIZE ||
		    read_le16(header + 26) != ELF32_PHDR_SIZE ||
		    read_le16(header + 28) != 1 || read_le16(header + 30) != 0 ||
		    read_le16(header + 32) != 0 || read_le16(header + 34) != 0)
			return false;
	} else {
		if (read_le64(header + 8) != 0 ||
		    read_le64(header + 16) != ELF64_EHDR_SIZE ||
		    read_le64(header + 24) != 0 || read_le32(header + 32) != 0 ||
		    read_le16(header + 36) != ELF64_EHDR_SIZE ||
		    read_le16(header + 38) != ELF64_PHDR_SIZE ||
		    read_le16(header + 40) != 1 || read_le16(header + 42) != 0 ||
		    read_le16(header + 44) != 0 || read_le16(header + 46) != 0)
			return false;
	}

	program = prefix + program_offset;
	if (read_le32(program) != ELF_OSSEG_WASM)
		return false;
	if (elf_class == 1) {
		segment_offset = read_le32(program + 4);
		segment_size = read_le32(program + 16);
		if (read_le32(program + 8) != 0 || read_le32(program + 12) != 0 ||
		    read_le32(program + 20) != 0 || read_le32(program + 24) != 4 ||
		    read_le32(program + 28) != 4)
			return false;
	} else {
		if (read_le32(program + 4) != 4)
			return false;
		segment_offset = read_le64(program + 8);
		if (read_le64(program + 16) != 0 || read_le64(program + 24) != 0)
			return false;
		segment_size = read_le64(program + 32);
		if (read_le64(program + 40) != 0 || read_le64(program + 48) != 4)
			return false;
	}
	if (segment_offset != payload_offset || segment_size < 40 ||
	    segment_offset > file_size || segment_size != file_size - segment_offset)
		return false;

	payload = prefix + payload_offset;
	if (memcmp(payload, "LINUX.WASM\0\0", 12) ||
	    read_le32(payload + 12) != 1 || read_le32(payload + 16) != width ||
	    read_le32(payload + 20) != 0)
		return false;

	module_size = read_le64(payload + 24);
	if (module_size < 8 || module_size > 0xffffffffULL ||
	    module_size != file_size - module_offset)
		return false;
	if (memcmp(prefix + module_offset, "\0asm\1\0\0\0", 8))
		return false;
	if (segment_size != 32 + module_size ||
	    (unsigned long long)module_offset + module_size != file_size)
		return false;

	image.offset = module_offset;
	image.size = module_size;
	*out = image;
	return true;
}
