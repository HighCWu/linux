#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/auxvec.h>
#include <linux/binfmts.h>
#include <linux/coredump.h>
#include <linux/cred.h>
#include <linux/elf.h>
#include <linux/highmem.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/personality.h>
#include <linux/ptrace.h>
#include <linux/random.h>
#include <linux/sched/signal.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <asm/wasm_imports.h>

#ifdef CONFIG_WASM_ELF_CONTAINER
#include "elf_container.h"
#endif

/* Four pages amortize host calls without requiring a large kernel allocation. */
#define WASM_EXEC_MAX_CHUNK_SIZE SZ_256K
#define WASM_ARG_LIMIT SZ_256K
#define WASM32_MAX_MEMORY_PAGES (1U << (32 - PAGE_SHIFT))
#define WASM64_MAX_MEMORY_PAGES (1U << (34 - PAGE_SHIFT))

/*
 * binfmt_wasm hands userland its arguments through a flat blob rather than the
 * ELF stack, so nothing would otherwise supply an auxiliary vector. Unpatched
 * musl still walks for one immediately past envp[]'s NULL terminator, so we lay
 * a real auxv there (see copy_args). AT_RANDOM's 16 bytes live in the blob just
 * after the vector.
 */
#define WASM_AT_RANDOM_SIZE 16

/*
 * The process_args member is the userspace ABI. Everything before it is
 * kernel-private exec state, and process_size is the exact byte range copied
 * to userspace.
 */
struct wasm_process_args {
	int len, envc, argc;
	char **argv, **envp;
	char data[];
};

struct wasm_exec_args {
	size_t process_size;
	size_t arg_start;
	size_t arg_end;
	size_t env_start;
	size_t env_end;
	size_t auxv;
	size_t random;
	size_t execfn;
	int execfd;
	unsigned int secureexec : 1;
	unsigned int preserve_argv0 : 1;
	unsigned int have_execfd : 1;
	struct wasm_process_args process;
};

static int load_wasm_binary(struct linux_binprm *bprm);
#ifdef CONFIG_COREDUMP
static int wasm_core_dump(struct coredump_params *cprm);
#endif

static u32 user_memory_limit_pages(void)
{
	unsigned long limit = rlimit(RLIMIT_AS);
	u32 maximum_pages;

#ifdef CONFIG_64BIT
	maximum_pages = WASM64_MAX_MEMORY_PAGES;
#else
	maximum_pages = WASM32_MAX_MEMORY_PAGES;
#endif

	/* WebAssembly memory limits are an intentional exec-time snapshot. */
	return limit == RLIM_INFINITY ? maximum_pages :
				       min_t(unsigned long, limit / SZ_64K,
					     maximum_pages);
}

static struct linux_binfmt wasm_format = {
	.module = THIS_MODULE,
	.load_binary = load_wasm_binary,
#ifdef CONFIG_COREDUMP
	.core_dump = wasm_core_dump,
#endif
};

static int file_get_size(struct file *f, loff_t *size)
{
	struct kstat stat;
	int ret = vfs_getattr(&f->f_path, &stat, STATX_SIZE,
			      AT_STATX_SYNC_AS_STAT);
	if (ret)
		return ret;
	*size = stat.size;
	return 0;
}

static size_t process_offset(struct wasm_exec_args *exec, const void *ptr)
{
	return (const char *)ptr - (const char *)&exec->process;
}

static int consume_string(char **cursor, const char *end)
{
	size_t available = end - *cursor;
	size_t limit = min_t(size_t, available, MAX_ARG_STRLEN);
	size_t len = strnlen(*cursor, limit);

	if (len == limit)
		return -EINVAL;
	*cursor += len + 1;
	return 0;
}

static int copy_args(struct linux_binprm *bprm)
{
	const size_t argument_bytes = MAX_ARG_PAGES * PAGE_SIZE;
	size_t strings_size;
	size_t pointer_count;
	size_t pointer_size;
	size_t auxv_size = sizeof(bprm->mm->saved_auxv);
	size_t data_size;
	size_t process_size;
	struct wasm_exec_args *exec;
	struct wasm_process_args *args;
	char *strings, *cursor, *end;
	int stop = bprm->p >> PAGE_SHIFT;
	int ret;

	if (bprm->p > argument_bytes)
		return -E2BIG;
	strings_size = argument_bytes - bprm->p;

	pointer_count = size_add((size_t)bprm->argc, (size_t)bprm->envc);
	pointer_count = size_add(pointer_count, 2);
	pointer_size = array_size(pointer_count, sizeof(char *));
	data_size = size_add(pointer_size, auxv_size);
	data_size = size_add(data_size, WASM_AT_RANDOM_SIZE);
	data_size = size_add(data_size, strings_size);
	process_size = size_add(sizeof(struct wasm_process_args), data_size);
	if (process_size == SIZE_MAX || process_size > WASM_ARG_LIMIT)
		return -E2BIG;
	if (bprm->exec < bprm->p ||
	    bprm->exec - bprm->p >= strings_size)
		return -E2BIG;

	exec = kzalloc(struct_size(exec, process.data, data_size), GFP_KERNEL);
	if (!exec)
		return -ENOMEM;
	args = &exec->process;

	exec->process_size = process_size;
	exec->auxv = process_offset(exec, args->data + pointer_size);
	exec->random = exec->auxv + auxv_size;
	exec->execfn = exec->random + WASM_AT_RANDOM_SIZE +
		       bprm->exec - bprm->p;
	exec->preserve_argv0 =
		!!(bprm->interp_flags & BINPRM_FLAGS_PRESERVE_ARGV0);

	args->argc = bprm->argc;
	args->envc = bprm->envc;
	args->len = data_size;
	args->argv = (char **)args->data;
	args->envp = args->argv + args->argc + 1;

	strings = (char *)&exec->process + exec->random +
		  WASM_AT_RANDOM_SIZE;
	cursor = strings + strings_size;
	for (int index = MAX_ARG_PAGES - 1; index >= stop; index--) {
		unsigned int offset = index == stop ?
					      bprm->p & ~PAGE_MASK :
					      0;
		size_t size = PAGE_SIZE - offset;
		char *src = kmap_local_page(bprm->page[index]) + offset;

		cursor -= size;
		memcpy(cursor, src, size);
		kunmap_local(src);
	}
	if (WARN_ON_ONCE(cursor != strings)) {
		ret = -EINVAL;
		goto err;
	}

	get_random_bytes((char *)&exec->process + exec->random,
			 WASM_AT_RANDOM_SIZE);

	end = strings + strings_size;
	exec->arg_start = process_offset(exec, cursor);
	for (int i = 0; i < args->argc; i++) {
		args->argv[i] = cursor;
		ret = consume_string(&cursor, end);
		if (ret)
			goto err;
	}
	args->argv[args->argc] = NULL;
	exec->arg_end = process_offset(exec, cursor);

	exec->env_start = process_offset(exec, cursor);
	for (int i = 0; i < args->envc; i++) {
		args->envp[i] = cursor;
		ret = consume_string(&cursor, end);
		if (ret)
			goto err;
	}
	args->envp[args->envc] = NULL;
	exec->env_end = process_offset(exec, cursor);

	if (exec->execfn >= exec->process_size) {
		ret = -EINVAL;
		goto err;
	}

	bprm->mm->context.exec_args = exec;
	return 0;

err:
	kfree(exec);
	return ret;
}

static void create_wasm_auxv(unsigned long *auxv,
			     struct wasm_exec_args *exec,
			     unsigned long user_base)
{
	const struct cred *cred = current_cred();
	unsigned long flags = 0;

	memset(auxv, 0, sizeof(current->mm->saved_auxv));

#define NEW_AUX_ENT(id, val) \
	do { \
		*auxv++ = (id); \
		*auxv++ = (val); \
	} while (0)
	NEW_AUX_ENT(AT_HWCAP, 0);
	NEW_AUX_ENT(AT_PAGESZ, PAGE_SIZE);
	NEW_AUX_ENT(AT_CLKTCK, CLOCKS_PER_SEC);
	if (exec->preserve_argv0)
		flags |= AT_FLAGS_PRESERVE_ARGV0;
	NEW_AUX_ENT(AT_FLAGS, flags);
	NEW_AUX_ENT(AT_UID, from_kuid_munged(cred->user_ns, cred->uid));
	NEW_AUX_ENT(AT_EUID, from_kuid_munged(cred->user_ns, cred->euid));
	NEW_AUX_ENT(AT_GID, from_kgid_munged(cred->user_ns, cred->gid));
	NEW_AUX_ENT(AT_EGID, from_kgid_munged(cred->user_ns, cred->egid));
	NEW_AUX_ENT(AT_SECURE, exec->secureexec);
	NEW_AUX_ENT(AT_RANDOM, user_base + exec->random);
	NEW_AUX_ENT(AT_EXECFN, user_base + exec->execfn);
	if (exec->have_execfd)
		NEW_AUX_ENT(AT_EXECFD, exec->execfd);
	NEW_AUX_ENT(AT_NULL, 0);
#undef NEW_AUX_ENT
}

static void relocate_args(struct wasm_exec_args *exec,
			  struct wasm_process_args *args,
			  unsigned long user_base)
{
	struct wasm_process_args *template = &exec->process;
	char **argv = (void *)args + process_offset(exec, template->argv);
	char **envp = (void *)args + process_offset(exec, template->envp);

	for (int i = 0; i < template->argc; i++)
		argv[i] = (char *)(user_base +
				   process_offset(exec, template->argv[i]));
	for (int i = 0; i < template->envc; i++)
		envp[i] = (char *)(user_base +
				   process_offset(exec, template->envp[i]));
	args->argv = (char **)(user_base +
			       process_offset(exec, template->argv));
	args->envp = (char **)(user_base +
			       process_offset(exec, template->envp));
}

SYSCALL_DEFINE2(wasm_get_args, void __user *, buf, size_t, len)
{
	struct mm_struct *mm = current->mm;
	struct wasm_exec_args *exec;
	struct wasm_process_args *args;
	unsigned long *auxv;
	unsigned long user_base = (unsigned long)buf;
	int ret;

	exec = xchg(&mm->context.exec_args, NULL);
	if (!exec)
		return -EINVAL;
	if (len < exec->process_size) {
		ret = -E2BIG;
		goto restore;
	}
	if (!access_ok(buf, exec->process_size)) {
		ret = -EFAULT;
		goto restore;
	}

	args = kmemdup(&exec->process, exec->process_size, GFP_KERNEL);
	if (!args) {
		ret = -ENOMEM;
		goto restore;
	}
	auxv = (void *)args + exec->auxv;
	create_wasm_auxv(auxv, exec, user_base);
	relocate_args(exec, args, user_base);
	if (copy_to_user(buf, args, exec->process_size)) {
		ret = -EFAULT;
		goto free_args;
	}

	spin_lock(&mm->arg_lock);
	memcpy(mm->saved_auxv, auxv, sizeof(mm->saved_auxv));
	mm->arg_start = user_base + exec->arg_start;
	mm->arg_end = user_base + exec->arg_end;
	mm->env_start = user_base + exec->env_start;
	mm->env_end = user_base + exec->env_end;
	spin_unlock(&mm->arg_lock);

	kfree(args);
	kfree(exec);
	return 0;

free_args:
	kfree(args);
restore:
	WRITE_ONCE(mm->context.exec_args, exec);
	return ret;
}

static void commit_wasm_exec_args(struct linux_binprm *bprm)
{
	struct wasm_exec_args *exec = current->mm->context.exec_args;

	/* Final credentials and an interpreter execfd are set by begin_new_exec(). */
	exec->secureexec = bprm->secureexec;
	exec->have_execfd = bprm->have_execfd;
	exec->execfd = bprm->execfd;
}

static int load_wasm_binary(struct linux_binprm *bprm)
{
	loff_t offset = 0;
	u8 *chunk = NULL;
	int ret;
	loff_t filesize;
	loff_t image_offset = 0;
	loff_t image_size;
	bool compiling = false;
	u32 chunk_size;
	bool raw_wasm = !memcmp(bprm->buf, "\0asm\1\0\0\0", 8);

#ifdef CONFIG_WASM_ELF_CONTAINER
	u8 prefix[160];
	size_t prefix_len = 0;
	size_t prefix_size;
	loff_t prefix_offset = 0;
	struct wasm_elf_image_range image;
	unsigned int width;
#endif

#ifndef CONFIG_WASM_ELF_CONTAINER
	if (!raw_wasm)
		return -ENOEXEC;
#endif

	ret = file_get_size(bprm->file, &filesize);
	if (ret < 0)
		return ret;
	if (filesize < 0 || filesize > U32_MAX)
		return -EFBIG;
	image_size = filesize;

	if (!raw_wasm) {
#ifdef CONFIG_WASM_ELF_CONTAINER
		prefix_size = min_t(loff_t, filesize, sizeof(prefix));
		while (prefix_len < prefix_size) {
			ssize_t read = kernel_read(bprm->file, prefix + prefix_len,
						   prefix_size - prefix_len,
						   &prefix_offset);

			if (read < 0)
				return read;
			if (!read)
				return -ENOEXEC;
			prefix_len += read;
		}

#ifdef CONFIG_64BIT
		width = 64;
#else
		width = 32;
#endif
		if (!wasm_elf_container_parse(prefix, prefix_len, filesize, width,
					      &image))
			return -ENOEXEC;
		image_offset = image.offset;
		image_size = image.size;
#endif
	}

	chunk_size = min_t(loff_t, image_size, WASM_EXEC_MAX_CHUNK_SIZE);
	chunk = kmalloc(chunk_size, GFP_KERNEL);
	if (!chunk)
		return -ENOMEM;

	ret = wasm_user_compile_begin((u32)image_size);
	if (ret)
		goto err;
	compiling = true;

	while (offset < image_size) {
		u32 size = min_t(loff_t, image_size - offset, chunk_size);
		loff_t read_offset = image_offset + offset;
		ssize_t read = kernel_read(bprm->file, chunk, size, &read_offset);

		if (read < 0) {
			ret = read;
			goto err;
		}
		if (!read) {
			ret = -EIO;
			goto err;
		}
		ret = wasm_user_compile_write(chunk, (u32)offset,
					      (u32)read);
		if (ret)
			goto err;
		offset += read;
	}

	ret = wasm_user_compile_end(user_memory_limit_pages());
	if (ret)
		goto err;

	kfree(chunk);
	chunk = NULL;

	ret = copy_args(bprm);
	if (ret)
		goto err;

	ret = begin_new_exec(bprm);
	if (ret)
		goto err;
	/* The new mm and credentials are committed at this point. */
	commit_wasm_exec_args(bprm);

#ifdef CONFIG_64BIT
	set_personality(PER_LINUX);
#else
	set_personality(PER_LINUX_32BIT);
#endif
	setup_new_exec(bprm);

	set_binfmt(&wasm_format);

	finalize_exec(bprm);

	wasm_user_instantiate(true);
	/*
	 * begin_new_exec() installs current->mm before the JS worker swaps its
	 * local user memory. Publish readiness only after instantiate returns,
	 * so remote access cannot copy through the old memory during exec.
	 */
	WRITE_ONCE(current_thread_info()->context_mm, current->mm);

	return 0;
err:
	if (compiling)
		wasm_user_compile_abort();
	kfree(chunk);
	return ret;
}

#ifdef CONFIG_COREDUMP
struct wasm_core_note {
	u32 pid;
	u32 tid;
	u32 signo;
	u32 code;
	u64 fault_addr;
	char comm[TASK_COMM_LEN];
	struct pt_regs regs;
};

static int wasm_core_dump_range(struct coredump_params *cprm, void *bounce,
				size_t bounce_size, unsigned long start,
				unsigned long dump_size)
{
	unsigned long offset = 0;

	while (offset < dump_size) {
		size_t chunk = min_t(unsigned long, dump_size - offset,
				     bounce_size);
		unsigned long uncopied;

		if (fatal_signal_pending(current))
			return 0;

		uncopied = copy_from_user(bounce,
					  (const void __user *)(start + offset),
					  chunk);
		if (uncopied) {
			if (uncopied > chunk)
				uncopied = chunk;
			memset((char *)bounce + (chunk - uncopied), 0,
			       uncopied);
		}
		if (!dump_emit(cprm, bounce, (int)chunk))
			return 0;
		offset += chunk;
	}
	return 1;
}

struct wasm_core_seg {
	unsigned long start;
	unsigned long size;
};

#define WASM_CORE_MAX_SEGS 1024

static u64 wasm_core_resident_bytes(void)
{
	u32 low = 0;
	u32 high = (u32)min_t(u64, user_memory_limit_pages(),
			      ((u64)TASK_SIZE + 1) / SZ_64K);

	while (low < high) {
		u32 mid = low + (high - low + 1) / 2;
		unsigned long probe_addr = (unsigned long)(mid - 1) * SZ_64K;
		u8 byte;

		if (probe_addr >= TASK_SIZE) {
			high = mid - 1;
			continue;
		}
		if (copy_from_user(&byte, (const void __user *)probe_addr,
				   sizeof(byte)) == 0)
			low = mid;
		else
			high = mid - 1;
	}

	return (u64)low * SZ_64K;
}

static int wasm_core_collect_segs(struct wasm_core_seg *segs, int max_segs)
{
	u64 resident_bytes = wasm_core_resident_bytes();
	u64 cursor = 0;
	int count = 0;

	while (cursor < resident_bytes && count < max_segs) {
		unsigned long rem_pages =
			(unsigned long)((resident_bytes - cursor) >> PAGE_SHIFT);
		long span;

		if (!rem_pages)
			break;
		span = wasm_user_mmap_query_v1_span((unsigned long)cursor,
						    rem_pages);
		if (span < 0) {
			count = 0;
			break;
		}
		if (span == 0) {
			cursor += PAGE_SIZE;
			continue;
		}
		if ((unsigned long)span > rem_pages)
			span = (long)rem_pages;
		segs[count].start = (unsigned long)cursor;
		segs[count].size = (unsigned long)span * PAGE_SIZE;
		count++;
		cursor += (u64)span * PAGE_SIZE;
	}

	if (count == 0 && resident_bytes > 0 && max_segs > 0) {
		segs[0].start = 0;
		segs[0].size = (unsigned long)min_t(u64, resident_bytes,
						    TASK_SIZE);
		count = 1;
	}

	return count;
}

static int wasm_core_dump(struct coredump_params *cprm)
{
	static const char note_name[8] = "WASM\0\0\0";
	struct elfhdr elf = { 0 };
	struct elf_note nhdr = { 0 };
	struct wasm_core_note note = { 0 };
	struct elf_phdr *phdrs = NULL;
	struct wasm_core_seg *linear_segs = NULL;
	void *bounce = NULL;
	size_t bounce_size = SZ_16K;
	int vma_count = min_t(int, cprm->vma_count, 65534);
	int linear_count = 0;
	int seg_count;
	int phnum;
	loff_t offset;
	size_t note_size;
	int i;
	int has_dumped = 0;

	if (vma_count == 0) {
		linear_segs = kvcalloc(WASM_CORE_MAX_SEGS,
				       sizeof(*linear_segs), GFP_KERNEL);
		if (!linear_segs)
			goto out;
		linear_count = wasm_core_collect_segs(linear_segs,
						      WASM_CORE_MAX_SEGS);
	}
	seg_count = vma_count > 0 ? vma_count : linear_count;
	phnum = 1 + seg_count;

	memcpy(elf.e_ident, ELFMAG, SELFMAG);
	elf.e_ident[EI_CLASS] = ELF_CLASS;
	elf.e_ident[EI_DATA] = ELFDATA2LSB;
	elf.e_ident[EI_VERSION] = EV_CURRENT;
	elf.e_ident[EI_OSABI] = ELFOSABI_LINUX;
	elf.e_type = ET_CORE;
	elf.e_machine = EM_NONE;
	elf.e_version = EV_CURRENT;
	elf.e_phoff = sizeof(elf);
	elf.e_ehsize = sizeof(elf);
	elf.e_phentsize = sizeof(*phdrs);
	elf.e_phnum = (Elf_Half)phnum;

	note.pid = (u32)task_tgid_vnr(current);
	note.tid = (u32)task_pid_vnr(current);
	if (cprm->siginfo) {
		note.signo = (u32)cprm->siginfo->si_signo;
		note.code = (u32)cprm->siginfo->si_code;
		note.fault_addr = (u64)(uintptr_t)cprm->siginfo->si_addr;
	}
	memcpy(note.comm, current->comm, sizeof(note.comm));
	note.regs = *task_pt_regs(current);

	nhdr.n_namesz = 5;
	nhdr.n_descsz = sizeof(note);
	nhdr.n_type = NT_PRSTATUS;
	note_size = sizeof(nhdr) + sizeof(note_name) + round_up(sizeof(note), 4);

	phdrs = kvcalloc(phnum, sizeof(*phdrs), GFP_KERNEL);
	if (!phdrs)
		goto out;

	bounce = kmalloc(bounce_size, GFP_KERNEL);
	if (!bounce)
		goto out;

	offset = sizeof(elf) + (loff_t)phnum * sizeof(*phdrs);
	phdrs[0].p_type = PT_NOTE;
	phdrs[0].p_offset = offset;
	phdrs[0].p_filesz = note_size;
	phdrs[0].p_align = 4;
	offset += (loff_t)note_size;

	if (vma_count > 0) {
		for (i = 0; i < vma_count; i++) {
			const struct core_vma_metadata *m = cprm->vma_meta + i;
			struct elf_phdr *ph = &phdrs[1 + i];

			ph->p_type = PT_LOAD;
			ph->p_flags = ((m->flags & VM_READ) ? PF_R : 0) |
				      ((m->flags & VM_WRITE) ? PF_W : 0) |
				      ((m->flags & VM_EXEC) ? PF_X : 0);
			ph->p_offset = offset;
			ph->p_vaddr = m->start;
			ph->p_paddr = 0;
			ph->p_filesz = m->dump_size;
			ph->p_memsz = m->end - m->start;
			ph->p_align = PAGE_SIZE;
			offset += (loff_t)m->dump_size;
		}
	} else {
		for (i = 0; i < linear_count; i++) {
			const struct wasm_core_seg *s = &linear_segs[i];
			struct elf_phdr *ph = &phdrs[1 + i];

			ph->p_type = PT_LOAD;
			ph->p_flags = PF_R | PF_W;
			ph->p_offset = offset;
			ph->p_vaddr = s->start;
			ph->p_paddr = 0;
			ph->p_filesz = s->size;
			ph->p_memsz = s->size;
			ph->p_align = PAGE_SIZE;
			offset += (loff_t)s->size;
		}
	}

	if (!dump_emit(cprm, &elf, sizeof(elf)))
		goto out;
	if (!dump_emit(cprm, phdrs, (int)((size_t)phnum * sizeof(*phdrs))))
		goto out;
	if (!dump_emit(cprm, &nhdr, sizeof(nhdr)))
		goto out;
	if (!dump_emit(cprm, note_name, sizeof(note_name)))
		goto out;
	if (!dump_emit(cprm, &note, sizeof(note)))
		goto out;
	if (round_up(sizeof(note), 4) > sizeof(note)) {
		static const char pad[4] = { 0 };
		size_t pad_len = round_up(sizeof(note), 4) - sizeof(note);

		if (!dump_emit(cprm, pad, (int)pad_len))
			goto out;
	}

	if (vma_count > 0) {
		for (i = 0; i < vma_count; i++) {
			const struct core_vma_metadata *m = cprm->vma_meta + i;

			if (!m->dump_size)
				continue;
			if (!wasm_core_dump_range(cprm, bounce, bounce_size,
						  m->start, m->dump_size))
				goto out;
		}
	} else {
		for (i = 0; i < linear_count; i++) {
			const struct wasm_core_seg *s = &linear_segs[i];

			if (!s->size)
				continue;
			if (!wasm_core_dump_range(cprm, bounce, bounce_size,
						  s->start, s->size))
				goto out;
		}
	}

	has_dumped = 1;
out:
	kfree(bounce);
	kvfree(phdrs);
	kvfree(linear_segs);
	return has_dumped;
}
#endif

static int __init init_wasm_binfmt(void)
{
	register_binfmt(&wasm_format);
	return 0;
}
core_initcall(init_wasm_binfmt);
