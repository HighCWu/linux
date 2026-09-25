#ifndef _WASM_ELF_H
#define _WASM_ELF_H

#ifdef CONFIG_64BIT
#define ELF_CLASS ELFCLASS64
#else
#define ELF_CLASS ELFCLASS32
#endif
typedef struct {
} elf_gregset_t;
typedef struct {
} elf_fpregset_t;

#endif
