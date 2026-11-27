// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/io_uring.h>
#include <linux/io_uring_types.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <asm/shmparam.h>
#ifdef CONFIG_WASM
#include <asm/wasm_imports.h>
#endif

#include "io_uring.h"
#include "memmap.h"
#include "kbuf.h"
#include "rsrc.h"
#include "zcrx.h"

static bool io_mem_alloc_compound(struct page **pages, int nr_pages,
				  size_t size, gfp_t gfp)
{
	struct page *page;
	int i, order;

	order = get_order(size);
	if (order > MAX_PAGE_ORDER)
		return false;
	else if (order)
		gfp |= __GFP_COMP;

	page = alloc_pages(gfp, order);
	if (!page)
		return false;

	for (i = 0; i < nr_pages; i++)
		pages[i] = page + i;

	return true;
}

struct page **io_pin_pages(unsigned long uaddr, unsigned long len, int *npages)
{
	unsigned long start, end, nr_pages;
	struct page **pages;
	int ret;

	if (check_add_overflow(uaddr, len, &end))
		return ERR_PTR(-EOVERFLOW);
	if (check_add_overflow(end, PAGE_SIZE - 1, &end))
		return ERR_PTR(-EOVERFLOW);

	end = end >> PAGE_SHIFT;
	start = uaddr >> PAGE_SHIFT;
	nr_pages = end - start;
	if (WARN_ON_ONCE(!nr_pages))
		return ERR_PTR(-EINVAL);
	if (WARN_ON_ONCE(nr_pages > INT_MAX))
		return ERR_PTR(-EOVERFLOW);

	pages = kvmalloc_objs(struct page *, nr_pages, GFP_KERNEL_ACCOUNT);
	if (!pages)
		return ERR_PTR(-ENOMEM);

	ret = pin_user_pages_fast(uaddr, nr_pages, FOLL_WRITE | FOLL_LONGTERM,
					pages);
	/* success, mapped all pages */
	if (ret == nr_pages) {
		*npages = nr_pages;
		return pages;
	}

	/* partial map, or didn't map anything */
	if (ret >= 0) {
		/* if we did partial map, release any pages we did get */
		if (ret)
			unpin_user_pages(pages, ret);
		ret = -EFAULT;
	}
	kvfree(pages);
	return ERR_PTR(ret);
}

enum {
	/* memory was vmap'ed for the kernel, freeing the region vunmap's it */
	IO_REGION_F_VMAP			= 1,
	/* memory is provided by user and pinned by the kernel */
	IO_REGION_F_USER_PROVIDED		= 2,
	/* only the first page in the array is ref'ed */
	IO_REGION_F_SINGLE_REF			= 4,
};

void io_free_region(struct user_struct *user, struct io_mapped_region *mr)
{
	if (mr->pages) {
		long nr_refs = mr->nr_pages;

		if (mr->flags & IO_REGION_F_SINGLE_REF)
			nr_refs = 1;

		if (mr->flags & IO_REGION_F_USER_PROVIDED)
			unpin_user_pages(mr->pages, nr_refs);
		else
			release_pages(mr->pages, nr_refs);

		kvfree(mr->pages);
	}
	if ((mr->flags & IO_REGION_F_VMAP) && mr->ptr)
		vunmap(mr->ptr);
	if (mr->nr_pages && user)
		__io_unaccount_mem(user, mr->nr_pages);

	memset(mr, 0, sizeof(*mr));
}

static int io_region_init_ptr(struct io_mapped_region *mr)
{
	struct io_imu_folio_data ifd;
	void *ptr;

	if (io_check_coalesce_buffer(mr->pages, mr->nr_pages, &ifd)) {
		if (ifd.nr_folios == 1 && !PageHighMem(mr->pages[0])) {
			mr->ptr = page_address(mr->pages[0]);
			return 0;
		}
	}
	ptr = vmap(mr->pages, mr->nr_pages, VM_MAP, PAGE_KERNEL);
	if (!ptr)
		return -ENOMEM;

	mr->ptr = ptr;
	mr->flags |= IO_REGION_F_VMAP;
	return 0;
}

static int io_region_pin_pages(struct io_mapped_region *mr,
			       struct io_uring_region_desc *reg)
{
	size_t size = io_region_size(mr);
	struct page **pages;
	int nr_pages;

	pages = io_pin_pages(reg->user_addr, size, &nr_pages);
	if (IS_ERR(pages))
		return PTR_ERR(pages);
	if (WARN_ON_ONCE(nr_pages != mr->nr_pages))
		return -EFAULT;

	mr->pages = pages;
	mr->flags |= IO_REGION_F_USER_PROVIDED;
	return 0;
}

static int io_region_allocate_pages(struct io_mapped_region *mr,
				    struct io_uring_region_desc *reg,
				    unsigned long mmap_offset)
{
	gfp_t gfp = GFP_KERNEL_ACCOUNT | __GFP_ZERO | __GFP_NOWARN;
	size_t size = io_region_size(mr);
	unsigned long nr_allocated;
	struct page **pages;

	pages = kvmalloc_objs(*pages, mr->nr_pages, gfp);
	if (!pages)
		return -ENOMEM;

	if (io_mem_alloc_compound(pages, mr->nr_pages, size, gfp)) {
		mr->flags |= IO_REGION_F_SINGLE_REF;
		goto done;
	}

	nr_allocated = alloc_pages_bulk_node(gfp, NUMA_NO_NODE,
					     mr->nr_pages, pages);
	if (nr_allocated != mr->nr_pages) {
		if (nr_allocated)
			release_pages(pages, nr_allocated);
		kvfree(pages);
		return -ENOMEM;
	}
done:
	reg->mmap_offset = mmap_offset;
	mr->pages = pages;
	return 0;
}

int io_create_region(struct io_ring_ctx *ctx, struct io_mapped_region *mr,
		     struct io_uring_region_desc *reg,
		     unsigned long mmap_offset)
{
	int nr_pages, ret;
	u64 end;

	if (WARN_ON_ONCE(mr->pages || mr->ptr || mr->nr_pages))
		return -EFAULT;
	if (memchr_inv(&reg->__resv, 0, sizeof(reg->__resv)))
		return -EINVAL;
	if (reg->flags & ~IORING_MEM_REGION_TYPE_USER)
		return -EINVAL;
	/* user_addr should be set IFF it's a user memory backed region */
	if ((reg->flags & IORING_MEM_REGION_TYPE_USER) != !!reg->user_addr)
		return -EFAULT;
	if (!reg->size || reg->mmap_offset || reg->id)
		return -EINVAL;
	if ((reg->size >> PAGE_SHIFT) > INT_MAX)
		return -E2BIG;
	if ((reg->user_addr | reg->size) & ~PAGE_MASK)
		return -EINVAL;
	if (check_add_overflow(reg->user_addr, reg->size, &end))
		return -EOVERFLOW;

	nr_pages = reg->size >> PAGE_SHIFT;
	if (ctx->user) {
		ret = __io_account_mem(ctx->user, nr_pages);
		if (ret)
			return ret;
	}
	mr->nr_pages = nr_pages;

	if (reg->flags & IORING_MEM_REGION_TYPE_USER)
		ret = io_region_pin_pages(mr, reg);
	else
		ret = io_region_allocate_pages(mr, reg, mmap_offset);
	if (ret)
		goto out_free;

	ret = io_region_init_ptr(mr);
	if (ret)
		goto out_free;
	return 0;
out_free:
	io_free_region(ctx->user, mr);
	return ret;
}

static struct io_mapped_region *io_mmap_get_region(struct io_ring_ctx *ctx,
						   loff_t pgoff)
{
	loff_t offset = pgoff << PAGE_SHIFT;
	unsigned int id;


	switch (offset & IORING_OFF_MMAP_MASK) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		return &ctx->ring_region;
	case IORING_OFF_SQES:
		return &ctx->sq_region;
	case IORING_OFF_PBUF_RING:
		id = (offset & ~IORING_OFF_MMAP_MASK) >> IORING_OFF_PBUF_SHIFT;
		return io_pbuf_get_region(ctx, id);
	case IORING_MAP_OFF_PARAM_REGION:
		return &ctx->param_region;
	case IORING_MAP_OFF_ZCRX_REGION:
		id = (offset & ~IORING_OFF_MMAP_MASK) >> IORING_OFF_ZCRX_SHIFT;
		return io_zcrx_get_region(ctx, id);
	}
	return NULL;
}

static void *io_region_validate_mmap(struct io_ring_ctx *ctx,
				     struct io_mapped_region *mr)
{
	lockdep_assert_held(&ctx->mmap_lock);

	if (!io_region_is_set(mr))
		return ERR_PTR(-EINVAL);
	if (mr->flags & IO_REGION_F_USER_PROVIDED)
		return ERR_PTR(-EINVAL);

	return io_region_get_ptr(mr);
}

static void *io_uring_validate_mmap_request(struct file *file, loff_t pgoff)
{
	struct io_ring_ctx *ctx = file->private_data;
	struct io_mapped_region *region;

	region = io_mmap_get_region(ctx, pgoff);
	if (!region)
		return ERR_PTR(-EINVAL);
	return io_region_validate_mmap(ctx, region);
}

#ifdef CONFIG_MMU

static int io_region_mmap(struct io_ring_ctx *ctx,
			  struct io_mapped_region *mr,
			  struct vm_area_struct *vma,
			  unsigned max_pages)
{
	unsigned long nr_pages = min(mr->nr_pages, max_pages);

	vm_flags_set(vma, VM_DONTEXPAND);
	return vm_insert_pages(vma, vma->vm_start, mr->pages, &nr_pages);
}

__cold int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct io_ring_ctx *ctx = file->private_data;
	size_t sz = vma->vm_end - vma->vm_start;
	long offset = vma->vm_pgoff << PAGE_SHIFT;
	unsigned int page_limit = UINT_MAX;
	struct io_mapped_region *region;
	void *ptr;

	guard(mutex)(&ctx->mmap_lock);

	ptr = io_uring_validate_mmap_request(file, vma->vm_pgoff);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	switch (offset & IORING_OFF_MMAP_MASK) {
	case IORING_OFF_SQ_RING:
	case IORING_OFF_CQ_RING:
		page_limit = (sz + PAGE_SIZE - 1) >> PAGE_SHIFT;
		break;
	}

	region = io_mmap_get_region(ctx, vma->vm_pgoff);
	return io_region_mmap(ctx, region, vma, page_limit);
}

unsigned long io_uring_get_unmapped_area(struct file *filp, unsigned long addr,
					 unsigned long len, unsigned long pgoff,
					 unsigned long flags)
{
	struct io_ring_ctx *ctx = filp->private_data;
	void *ptr;

	/*
	 * Do not allow to map to user-provided address to avoid breaking the
	 * aliasing rules. Userspace is not able to guess the offset address of
	 * kernel kmalloc()ed memory area.
	 */
	if (addr)
		return -EINVAL;

	guard(mutex)(&ctx->mmap_lock);

	ptr = io_uring_validate_mmap_request(filp, pgoff);
	if (IS_ERR(ptr))
		return -ENOMEM;

	/*
	 * Some architectures have strong cache aliasing requirements.
	 * For such architectures we need a coherent mapping which aliases
	 * kernel memory *and* userspace memory. To achieve that:
	 * - use a NULL file pointer to reference physical memory, and
	 * - use the kernel virtual address of the shared io_uring context
	 *   (instead of the userspace-provided address, which has to be 0UL
	 *   anyway).
	 * - use the same pgoff which the get_unmapped_area() uses to
	 *   calculate the page colouring.
	 * For architectures without such aliasing requirements, the
	 * architecture will return any suitable mapping because addr is 0.
	 */
	filp = NULL;
	flags |= MAP_SHARED;
	pgoff = 0;	/* has been translated to ptr above */
#ifdef SHM_COLOUR
	addr = (uintptr_t) ptr;
	pgoff = addr >> PAGE_SHIFT;
#else
	addr = 0UL;
#endif
	return mm_get_unmapped_area(filp, addr, len, pgoff, flags);
}

#else /* !CONFIG_MMU */

/*
 * Drop the pages that were initially referenced and added in
 * io_uring_mmap(). We cannot have had a mremap() as that isn't supported,
 * hence the vma should be identical to the one we initially referenced and
 * mapped, and partial unmaps and splitting isn't possible on a file backed
 * mapping.
 */
static void io_uring_nommu_vm_close(struct vm_area_struct *vma)
{
	unsigned long index;

	for (index = vma->vm_start; index < vma->vm_end; index += PAGE_SIZE)
		put_page(virt_to_page((void *) index));
}

static const struct vm_operations_struct io_uring_nommu_vm_ops = {
	.close = io_uring_nommu_vm_close,
};

int io_uring_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct io_ring_ctx *ctx = file->private_data;
	struct io_mapped_region *region;
	unsigned long i;

	if (!is_nommu_shared_mapping(vma->vm_flags))
		return -EINVAL;

	guard(mutex)(&ctx->mmap_lock);
	region = io_mmap_get_region(ctx, vma->vm_pgoff);
	if (!region || !io_region_is_set(region))
		return -EINVAL;

	if ((vma->vm_end - vma->vm_start) !=
	    (unsigned long) region->nr_pages << PAGE_SHIFT)
		return -EINVAL;

	/*
	 * Pin the pages so io_free_region()'s release_pages() does not
	 * drop the last reference while this VMA exists. delete_vma()
	 * in mm/nommu.c calls vma_close() which runs ->close above.
	 */
	for (i = 0; i < region->nr_pages; i++)
		get_page(region->pages[i]);

	vma->vm_ops = &io_uring_nommu_vm_ops;
	return 0;
}

unsigned int io_uring_nommu_mmap_capabilities(struct file *file)
{
	return NOMMU_MAP_DIRECT | NOMMU_MAP_READ | NOMMU_MAP_WRITE;
}

unsigned long io_uring_get_unmapped_area(struct file *file, unsigned long addr,
					 unsigned long len, unsigned long pgoff,
					 unsigned long flags)
{
	struct io_ring_ctx *ctx = file->private_data;
	void *ptr;

	guard(mutex)(&ctx->mmap_lock);

	ptr = io_uring_validate_mmap_request(file, pgoff);
	if (IS_ERR(ptr))
		return PTR_ERR(ptr);

	return (unsigned long) ptr;
}

#ifdef CONFIG_WASM
static LIST_HEAD(io_wasm_shadow_list);
static DEFINE_SPINLOCK(io_wasm_shadow_list_lock);

static bool io_wasm_ranges_overlap(unsigned long a_start, unsigned long a_len,
				   unsigned long b_start, unsigned long b_len)
{
	return a_len && b_len && a_start < b_start + b_len &&
	       b_start < a_start + a_len;
}

long wasm_io_uring_mmap(unsigned long addr, size_t len, unsigned long prot,
			unsigned long flags, long fd, unsigned long pgoff)
{
	const unsigned long advisory_flags = MAP_NORESERVE | MAP_POPULATE |
		MAP_NONBLOCK;
	struct file *file;
	struct io_ring_ctx *ctx;
	struct io_mapped_region *region;
	unsigned long *slot_addr = NULL, *slot_size = NULL;
	unsigned long old_addr = 0, old_size = 0;
	unsigned long aligned_len;
	unsigned long uaddr;
	loff_t offset;
	long ret;

	if (fd < 0 || fd > INT_MAX)
		return -EBADF;
	file = fget((unsigned int)fd);
	if (!file)
		return -EBADF;
	if (!io_is_uring_fops(file)) {
		fput(file);
		return -ENOIOCTLCMD;
	}

	if (addr || !len) {
		ret = -EINVAL;
		goto out_fput;
	}
	if ((flags & MAP_TYPE) != MAP_SHARED ||
	    (flags & ~(MAP_SHARED | advisory_flags))) {
		ret = -EINVAL;
		goto out_fput;
	}
	if (!prot || (prot & ~(PROT_READ | PROT_WRITE))) {
		ret = -EINVAL;
		goto out_fput;
	}
#ifdef CONFIG_64BIT
	if (!PAGE_ALIGNED(pgoff)) {
		ret = -EINVAL;
		goto out_fput;
	}
	offset = (loff_t)pgoff;
#else
	if (pgoff & ((PAGE_SIZE >> 12) - 1)) {
		ret = -EINVAL;
		goto out_fput;
	}
	offset = (loff_t)pgoff << 12;
#endif
	if (len > ULONG_MAX - (PAGE_SIZE - 1)) {
		ret = -ENOMEM;
		goto out_fput;
	}
	aligned_len = PAGE_ALIGN(len);

	ctx = file->private_data;
	mutex_lock(&ctx->mmap_lock);
	switch (offset & IORING_OFF_MMAP_MASK) {
	case IORING_OFF_SQ_RING:
		region = &ctx->ring_region;
		slot_addr = &ctx->wasm_sq_user_addr;
		slot_size = &ctx->wasm_sq_user_size;
		break;
	case IORING_OFF_CQ_RING:
		region = &ctx->ring_region;
		slot_addr = &ctx->wasm_cq_user_addr;
		slot_size = &ctx->wasm_cq_user_size;
		break;
	case IORING_OFF_SQES:
		region = &ctx->sq_region;
		slot_addr = &ctx->wasm_sqes_user_addr;
		slot_size = &ctx->wasm_sqes_user_size;
		break;
	default:
		mutex_unlock(&ctx->mmap_lock);
		ret = -EINVAL;
		goto out_fput;
	}

	if (!io_region_is_set(region) ||
	    (region->flags & IO_REGION_F_USER_PROVIDED) ||
	    aligned_len > io_region_size(region)) {
		mutex_unlock(&ctx->mmap_lock);
		ret = -EINVAL;
		goto out_fput;
	}

	ret = wasm_user_v2_mmap(0, aligned_len, PROT_READ | PROT_WRITE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (IS_ERR_VALUE((unsigned long)ret)) {
		mutex_unlock(&ctx->mmap_lock);
		goto out_fput;
	}
	uaddr = (unsigned long)ret;
	if (!PAGE_ALIGNED(uaddr) ||
	    !access_ok((void __user *)uaddr, aligned_len)) {
		wasm_user_munmap(uaddr, aligned_len);
		mutex_unlock(&ctx->mmap_lock);
		ret = -ENOMEM;
		goto out_fput;
	}

	if (wasm_user_write((void __user *)uaddr, region->ptr, aligned_len)) {
		wasm_user_munmap(uaddr, aligned_len);
		mutex_unlock(&ctx->mmap_lock);
		ret = -EFAULT;
		goto out_fput;
	}

	spin_lock(&ctx->wasm_shadow_lock);
	if (!ctx->wasm_mm)
		ctx->wasm_mm = current->mm;
	old_addr = *slot_addr;
	old_size = *slot_size;
	*slot_addr = uaddr;
	*slot_size = aligned_len;
	spin_unlock(&ctx->wasm_shadow_lock);

	spin_lock(&io_wasm_shadow_list_lock);
	if (list_empty(&ctx->wasm_shadow_node))
		list_add(&ctx->wasm_shadow_node, &io_wasm_shadow_list);
	spin_unlock(&io_wasm_shadow_list_lock);

	mutex_unlock(&ctx->mmap_lock);

	if (old_size)
		wasm_user_munmap(old_addr, old_size);

	fput(file);
	return (long)uaddr;

out_fput:
	fput(file);
	return ret;
}

void wasm_io_uring_munmap(struct mm_struct *mm, unsigned long addr,
			  unsigned long len)
{
	struct io_ring_ctx *ctx;

	if (!mm || !len || list_empty(&io_wasm_shadow_list))
		return;

	spin_lock(&io_wasm_shadow_list_lock);
	list_for_each_entry(ctx, &io_wasm_shadow_list, wasm_shadow_node) {
		if (ctx->wasm_mm != mm)
			continue;
		spin_lock(&ctx->wasm_shadow_lock);
		if (io_wasm_ranges_overlap(ctx->wasm_sq_user_addr,
					   ctx->wasm_sq_user_size, addr, len)) {
			ctx->wasm_sq_user_addr = 0;
			ctx->wasm_sq_user_size = 0;
		}
		if (io_wasm_ranges_overlap(ctx->wasm_cq_user_addr,
					   ctx->wasm_cq_user_size, addr, len)) {
			ctx->wasm_cq_user_addr = 0;
			ctx->wasm_cq_user_size = 0;
		}
		if (io_wasm_ranges_overlap(ctx->wasm_sqes_user_addr,
					   ctx->wasm_sqes_user_size, addr, len)) {
			ctx->wasm_sqes_user_addr = 0;
			ctx->wasm_sqes_user_size = 0;
		}
		spin_unlock(&ctx->wasm_shadow_lock);
	}
	spin_unlock(&io_wasm_shadow_list_lock);
}

void io_wasm_release_shadow(struct io_ring_ctx *ctx)
{
	unsigned long sq_addr, sq_size, cq_addr, cq_size, sqes_addr, sqes_size;
	bool can_unmap;

	spin_lock(&io_wasm_shadow_list_lock);
	if (!list_empty(&ctx->wasm_shadow_node))
		list_del_init(&ctx->wasm_shadow_node);
	spin_unlock(&io_wasm_shadow_list_lock);

	spin_lock(&ctx->wasm_shadow_lock);
	sq_addr = ctx->wasm_sq_user_addr;
	sq_size = ctx->wasm_sq_user_size;
	cq_addr = ctx->wasm_cq_user_addr;
	cq_size = ctx->wasm_cq_user_size;
	sqes_addr = ctx->wasm_sqes_user_addr;
	sqes_size = ctx->wasm_sqes_user_size;
	ctx->wasm_sq_user_addr = 0;
	ctx->wasm_sq_user_size = 0;
	ctx->wasm_cq_user_addr = 0;
	ctx->wasm_cq_user_size = 0;
	ctx->wasm_sqes_user_addr = 0;
	ctx->wasm_sqes_user_size = 0;
	can_unmap = current->mm && current->mm == ctx->wasm_mm &&
		    !(current->flags & PF_EXITING);
	spin_unlock(&ctx->wasm_shadow_lock);

	if (can_unmap) {
		if (sq_size)
			wasm_user_munmap(sq_addr, sq_size);
		if (cq_size)
			wasm_user_munmap(cq_addr, cq_size);
		if (sqes_size)
			wasm_user_munmap(sqes_addr, sqes_size);
	}
}

void io_wasm_sync_from_user(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;
	unsigned long sq_addr, sq_size, cq_addr, cq_size, sqes_addr, sqes_size;
	unsigned long array_off;
	size_t sqe_size;
	u32 sq_tail, cq_head, head, count, i, mask;

	if (!rings || !current->mm || current->mm != ctx->wasm_mm)
		return;

	spin_lock(&ctx->wasm_shadow_lock);
	sq_addr = ctx->wasm_sq_user_addr;
	sq_size = ctx->wasm_sq_user_size;
	cq_addr = ctx->wasm_cq_user_addr;
	cq_size = ctx->wasm_cq_user_size;
	sqes_addr = ctx->wasm_sqes_user_addr;
	sqes_size = ctx->wasm_sqes_user_size;

	if (sq_addr && sq_size >= sizeof(*rings)) {
		if (!wasm_user_read(&sq_tail,
				    (const void __user *)(sq_addr + offsetof(struct io_rings, sq.tail)),
				    sizeof(sq_tail))) {
			WRITE_ONCE(rings->sq.tail, sq_tail);
			head = ctx->cached_sq_head;
			count = min_t(u32, sq_tail - head, ctx->sq_entries);
			mask = ctx->sq_entries - 1;
			sqe_size = (ctx->flags & IORING_SETUP_SQE128) ?
				   2 * sizeof(struct io_uring_sqe) :
				   sizeof(struct io_uring_sqe);

			if (!(ctx->flags & IORING_SETUP_NO_SQARRAY) && ctx->sq_array) {
				array_off = (unsigned long)((char *)ctx->sq_array - (char *)rings);
				for (i = 0; i < count; i++) {
					u32 ring_idx = (head + i) & mask;
					u32 sqe_idx = 0;
					unsigned long elem_off = array_off + ring_idx * sizeof(u32);

					if (elem_off + sizeof(u32) > sq_size)
						break;
					if (wasm_user_read(&sqe_idx,
							   (const void __user *)(sq_addr + elem_off),
							   sizeof(sqe_idx)))
						break;
					ctx->sq_array[ring_idx] = sqe_idx;
					if (sqes_addr && sqe_idx < ctx->sq_entries &&
					    (unsigned long)(sqe_idx + 1) * sqe_size <= sqes_size) {
						unsigned long dst_idx = (ctx->flags & IORING_SETUP_SQE128) ?
									(sqe_idx << 1) : sqe_idx;

						(void)wasm_user_read(&ctx->sq_sqes[dst_idx],
								     (const void __user *)(sqes_addr + sqe_idx * sqe_size),
								     sqe_size);
					}
				}
			} else if (sqes_addr) {
				for (i = 0; i < count; i++) {
					u32 sqe_idx = (head + i) & mask;
					unsigned long dst_idx = (ctx->flags & IORING_SETUP_SQE128) ?
								(sqe_idx << 1) : sqe_idx;

					if ((unsigned long)(sqe_idx + 1) * sqe_size > sqes_size)
						break;
					(void)wasm_user_read(&ctx->sq_sqes[dst_idx],
							     (const void __user *)(sqes_addr + sqe_idx * sqe_size),
							     sqe_size);
				}
			}
		}
	}

	if (!cq_addr) {
		cq_addr = sq_addr;
		cq_size = sq_size;
	}
	if (cq_addr && cq_size >= sizeof(*rings)) {
		if (!wasm_user_read(&cq_head,
				    (const void __user *)(cq_addr + offsetof(struct io_rings, cq.head)),
				    sizeof(cq_head)))
			WRITE_ONCE(rings->cq.head, cq_head);
	}
	spin_unlock(&ctx->wasm_shadow_lock);
	smp_rmb();
}

void io_wasm_publish_sq(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;
	u32 sq_head, sq_dropped, sq_flags;

	if (!rings || !current->mm || current->mm != ctx->wasm_mm)
		return;

	spin_lock(&ctx->wasm_shadow_lock);
	if (ctx->wasm_sq_user_addr && ctx->wasm_sq_user_size >= sizeof(*rings)) {
		sq_head = READ_ONCE(rings->sq.head);
		sq_dropped = READ_ONCE(rings->sq_dropped);
		sq_flags = (u32)atomic_read(&rings->sq_flags);
		(void)wasm_user_write((void __user *)(ctx->wasm_sq_user_addr +
						      offsetof(struct io_rings, sq.head)),
				      &sq_head, sizeof(sq_head));
		(void)wasm_user_write((void __user *)(ctx->wasm_sq_user_addr +
						      offsetof(struct io_rings, sq_dropped)),
				      &sq_dropped, sizeof(sq_dropped));
		(void)wasm_user_write((void __user *)(ctx->wasm_sq_user_addr +
						      offsetof(struct io_rings, sq_flags)),
				      &sq_flags, sizeof(sq_flags));
	}
	spin_unlock(&ctx->wasm_shadow_lock);
}

void io_wasm_publish_cq(struct io_ring_ctx *ctx)
{
	struct io_rings *rings = ctx->rings;
	unsigned long cq_addr, cq_size;
	size_t cqe_size;
	u32 new_tail, old_tail, count, i, mask;

	if (!rings || !current->mm || current->mm != ctx->wasm_mm)
		return;

	spin_lock(&ctx->wasm_shadow_lock);
	cq_addr = ctx->wasm_cq_user_addr ? ctx->wasm_cq_user_addr : ctx->wasm_sq_user_addr;
	cq_size = ctx->wasm_cq_user_addr ? ctx->wasm_cq_user_size : ctx->wasm_sq_user_size;
	if (!cq_addr || cq_size < sizeof(*rings))
		goto out_unlock;

	new_tail = ctx->cached_cq_tail;
	old_tail = ctx->wasm_published_cq_tail;
	if (old_tail == new_tail)
		goto out_unlock;

	count = min_t(u32, new_tail - old_tail, ctx->cq_entries);
	old_tail = new_tail - count;
	mask = ctx->cq_entries - 1;
	cqe_size = (ctx->flags & IORING_SETUP_CQE32) ?
		   2 * sizeof(struct io_uring_cqe) :
		   sizeof(struct io_uring_cqe);

	for (i = 0; i < count; i++) {
		u32 off = (old_tail + i) & mask;
		u32 src_off = (ctx->flags & IORING_SETUP_CQE32) ? (off << 1) : off;
		unsigned long cqe_off = offsetof(struct io_rings, cqes) +
					(unsigned long)off * cqe_size;

		if (cqe_off + cqe_size > cq_size)
			break;
		if (wasm_user_write((void __user *)(cq_addr + cqe_off),
				    &rings->cqes[src_off], cqe_size))
			goto out_unlock;
	}

	/*
	 * Ensure newly written CQEs are visible in user linear memory before
	 * advancing user cq.tail.
	 */
	smp_wmb();

	if (!wasm_user_write((void __user *)(cq_addr + offsetof(struct io_rings, cq.tail)),
			     &new_tail, sizeof(new_tail)))
		ctx->wasm_published_cq_tail = new_tail;

out_unlock:
	spin_unlock(&ctx->wasm_shadow_lock);
}
#endif

#endif /* !CONFIG_MMU */
