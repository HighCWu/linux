// SPDX-License-Identifier: GPL-2.0
/*
 * NOMMU implementation of mincore(). NOMMU mappings retain their backing
 * storage directly, so residency is described by VMA coverage rather than
 * page-table state.
 */
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>

/*
 * Report the portion of this chunk covered by the VMA containing @addr.
 * The mmap lock is held by the caller, and NOMMU VMAs have resident backing.
 */
static long nommu_do_mincore(unsigned long addr, unsigned long pages,
			     unsigned char *vec)
{
	struct vm_area_struct *vma;
	unsigned long mapped_pages;

	vma = vma_lookup(current->mm, addr);
	if (!vma)
		return -ENOMEM;

	mapped_pages = min(pages, (vma->vm_end - addr) >> PAGE_SHIFT);
	if (!mapped_pages)
		return -ENOMEM;

	memset(vec, 1, mapped_pages);
	return mapped_pages;
}

SYSCALL_DEFINE3(mincore, unsigned long, start, size_t, len,
		unsigned char __user *, vec)
{
	long retval;
	unsigned long pages;
	unsigned char *tmp;

	start = untagged_addr(start);

	/* Keep the generic syscall's argument checks and their order. */
	if (unlikely(start & ~PAGE_MASK))
		return -EINVAL;

	if (!access_ok((void __user *)start, len))
		return -ENOMEM;

	pages = len >> PAGE_SHIFT;
	pages += (offset_in_page(len)) != 0;

	if (!access_ok(vec, pages))
		return -EFAULT;

	tmp = (void *)__get_free_page(GFP_USER);
	if (!tmp)
		return -EAGAIN;

	retval = 0;
	while (pages) {
		/* The temporary vector is one kernel page in size. */
		mmap_read_lock(current->mm);
		retval = nommu_do_mincore(start, min(pages, PAGE_SIZE), tmp);
		mmap_read_unlock(current->mm);

		if (retval <= 0)
			break;
		if (copy_to_user(vec, tmp, retval)) {
			retval = -EFAULT;
			break;
		}

		pages -= retval;
		vec += retval;
		start += retval << PAGE_SHIFT;
		retval = 0;
	}

	free_page((unsigned long)tmp);
	return retval;
}
