#include <linux/init.h>
#include <linux/limits.h>
#include <linux/memblock.h>
#include <linux/mm.h>
#include <linux/slab.h>

static atomic_t remote_mm_ids = ATOMIC_INIT(0);

static int alloc_remote_mm_id(void)
{
	int old = atomic_read(&remote_mm_ids);

	/* Saturate rather than binding a stale queued message to another mm. */
	while (old < INT_MAX) {
		int found = atomic_cmpxchg(&remote_mm_ids, old, old + 1);

		if (found == old)
			return old + 1;
		old = found;
	}
	return 0;
}

int wasm_init_new_context(struct task_struct *tsk, struct mm_struct *mm)
{
	/*
	 * dup_mm() copied these from the parent, but a fork gets a distinct
	 * WebAssembly.Memory and never inherits pending exec or request state.
	 */
	mutex_init(&mm->context.remote.mutex);
	atomic_set(&mm->context.remote.state, WASM_REMOTE_IDLE);
	mm->context.remote.host_id = alloc_remote_mm_id();
	atomic_set(&mm->context.remote.host_ready, 0);
	mm->context.remote.request = NULL;
	mm->context.remote.accepting = true;
	mm->context.exec_args = NULL;
	/* Dynamic module publication/fork is not supported by the execution ABI. */
	mm->context.dynamic_files = NULL;
	mm->context.dynamic_file_count = 0;
	mm->context.dynamic_file_next = 0;
	return 0;
}

void wasm_destroy_context(struct mm_struct *mm)
{
	kfree(mm->context.exec_args);
	WARN_ON_ONCE(mm->context.remote.accepting);
	WARN_ON_ONCE(atomic_read(&mm->context.remote.state) !=
		     WASM_REMOTE_IDLE);
	WARN_ON_ONCE(mm->context.remote.request);
	WARN_ON_ONCE(mm->context.dynamic_files);
}

void __init arch_zone_limits_init(unsigned long *max_zone_pfn)
{
	max_low_pfn = PHYS_PFN(memblock_end_of_DRAM());
	max_pfn = max_low_pfn;
	max_mapnr = max_pfn;

	max_zone_pfn[ZONE_NORMAL] = max_low_pfn;
#ifdef CONFIG_HIGHMEM
	max_zone_pfn[ZONE_HIGHMEM] = max_pfn;
#endif
}

void __init mem_init(void)
{
}
