/* SPDX-License-Identifier: MIT */
static unsigned long raw_offset(unsigned long bytes)
{
#ifdef CONFIG_32BIT
	return bytes / 4096;
#else
	return bytes;
#endif
}

static void clean(void)
{
	assert(!pins && !allocations && file.f_pos == 23);
	assert(wasm_mmap_copy_stat(0) == 0 && wasm_mmap_copy_stat(1) == 0 && wasm_mmap_copy_stat(3) == 0);
}

static void refused(long fd, unsigned long length, unsigned long prot,
		    unsigned long flags, unsigned long offset, long error)
{
	int old_reads = reads, old_copies = copies;
	int old_allocations = allocation_attempts;
	assert(wasm_mmap_file(0, length, prot, flags, fd, offset) == error);
	assert(old_reads == reads && old_copies == copies);
	if (!fail_allocation) assert(old_allocations == allocation_attempts);
	clean();
}

int main(void)
{
	refused(-1, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -EBADF);
	refused(10, 0, PROT_READ, MAP_PRIVATE, 0, -EINVAL);
	refused(10, 2 * PAGE_SIZE + 1, PROT_READ, MAP_PRIVATE, 0, -ENOMEM);
	refused(10, PAGE_SIZE, PROT_NONE, MAP_PRIVATE, 0, -EINVAL);
	refused(10, PAGE_SIZE, PROT_EXEC, MAP_PRIVATE, 0, -EINVAL);
	refused(10, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE, 0, -EINVAL);
	refused(10, PAGE_SIZE, 8, MAP_PRIVATE, 0, -EINVAL);
	refused(10, PAGE_SIZE, PROT_READ, MAP_SHARED, 0, -EINVAL);
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE | MAP_LOCKED, 0, -EINVAL);
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE | MAP_FIXED, 0, -ENOMEM);
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE | MAP_FIXED_NOREPLACE, 0, -ENOMEM);
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, raw_offset(4096), -EINVAL);
#ifndef CONFIG_32BIT
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, ULONG_MAX, -EOVERFLOW);
#endif
	file.f_mode = 0;
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -EACCES);
	refused(10, PAGE_SIZE, PROT_WRITE, MAP_PRIVATE, 0, -EACCES);
	file.f_mode = FMODE_READ;
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -ENODEV);
	file.f_mode |= FMODE_CAN_READ;
	mappable = false;
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -ENODEV);
	mappable = true;
	file.f_op = &explicit_operations;
	file_capabilities = 0;
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -ENODEV);
	file_capabilities = NOMMU_MAP_COPY;
	read_total = 0;
	assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, 0) == 65536);
	clean();
	file.f_op = &operations;
	sb.erofs = false;
	assert(!wasm_mmap_snapshot_source(&file));
	read_total = 0;
	assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, 0) == 65536);
	clean();
	sb.erofs = true;
	sb.s_bdev = (void *)2;
	assert(!wasm_mmap_snapshot_source(&file));
	read_total = 0;
	assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, 0) == 65536);
	clean();
	sb.s_bdev = (void *)1;
	assert(wasm_mmap_snapshot_source(&file));
	inode.i_mode = S_IFDIR;
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -EOPNOTSUPP);
	inode.i_mode = S_IFREG;
	capabilities = 0;
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -EOPNOTSUPP);
	capabilities = 1;
	security_error = -EPERM;
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -EPERM);
	refused(10, PAGE_SIZE, PROT_WRITE, MAP_PRIVATE, 0, -EPERM);
	assert(security_prot == PROT_WRITE);
	security_error = 0;
	read_total = 0;
	assert(wasm_mmap_file(0, 2 * PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, raw_offset(2 * PAGE_SIZE)) == 65536);
	assert(last_length == 7 && last_rounded == 2 * PAGE_SIZE);
	clean();
	loff_t size = inode.size;
	inode.size = 0;
	for (int page = 0; page < 2; page++) {
		read_total = 0;
		assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, raw_offset(page * PAGE_SIZE)) == 65536);
		assert(last_length == 0 && last_rounded == PAGE_SIZE);
		clean();
	}
	/* The private snapshot fixture still rejects empty sources. */
	assert(wasm_mmap_copy_file(fget(10), PAGE_SIZE, 0, i_size_read(&inode)) == -EINVAL);
	clean();
	inode.size = size;
	fail_allocation = 1;
	refused(10, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 0, -ENOMEM);
	fail_allocation = 0;
	/* The post-read callback ownership record can fail independently. */
	fail_allocation = 2;
	read_mode = 0;
	read_total = 0;
	int copies_before_record_failure = copies;
	assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, 0) == -ENOMEM);
	assert(copies == copies_before_record_failure);
	clean();
	fail_allocation = 0;
	for (int mode = 0; mode <= 4; mode++) {
		read_mode = mode;
		read_total = 0;
		int old_copies = copies;
		long result = wasm_mmap_file(123, 2 * PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, raw_offset(PAGE_SIZE));
		assert(result == (mode == 3 ? -EINTR : 65536));
		assert(copies == old_copies + (mode != 3));
		clean();
		if (mode == 0) assert(last_length == PAGE_SIZE + 7 && last_rounded == 2 * PAGE_SIZE);
		if (mode == 1 || mode == 2) assert(last_length == 7);
		if (mode == 4) assert(last_length == 0);
		read_total = 0;
		old_copies = copies;
		result = wasm_mmap_copy_file(fget(10), 2 * PAGE_SIZE, PAGE_SIZE, inode.size);
		assert(result == (mode == 0 ? 65536 : mode == 3 ? -EINTR : -EIO));
		assert(copies == old_copies + (mode == 0));
		clean();
	}
	read_mode = 0;
	read_total = 0;
	copy_error = -ENOMEM;
	assert(wasm_mmap_file(0, 1, PROT_READ, MAP_PRIVATE, 10, raw_offset(2 * PAGE_SIZE)) == -ENOMEM);
	clean();
	copy_error = 0;
	read_total = 0;
	assert(wasm_mmap_file(0, 1, PROT_READ | PROT_WRITE, MAP_PRIVATE, 10, raw_offset(2 * PAGE_SIZE)) == 65536);
	assert(last_length == 7 && last_rounded == PAGE_SIZE);
	clean();
	/* Each hint and their combination preserve eager-copy semantics and
	 * reach the security hook without discarding the caller's flags. */
	const unsigned long hints[] = { MAP_POPULATE, MAP_NONBLOCK, MAP_NORESERVE,
		MAP_STACK, MAP_DENYWRITE, MAP_EXECUTABLE,
		MAP_POPULATE | MAP_NONBLOCK | MAP_NORESERVE | MAP_STACK |
		MAP_DENYWRITE | MAP_EXECUTABLE };
	for (unsigned int i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
		read_total = 0;
		unsigned long flags = MAP_PRIVATE | hints[i];
		assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, flags, 10, 0) == 65536);
		assert(security_flags == flags && last_length == PAGE_SIZE);
		clean();
	}
	/* A readable descriptor is sufficient for a private writable copy.
	 * The actual LSM arguments must not acquire an extra PROT_READ bit. */
	for (unsigned int i = 0; i < 2; i++) {
		unsigned long flags = MAP_PRIVATE | (i ? MAP_POPULATE | MAP_NONBLOCK : 0);
		read_total = 0;
		assert(wasm_mmap_file(0, PAGE_SIZE, PROT_WRITE, flags, 10, 0) == 65536);
		assert(security_prot == PROT_WRITE && security_flags == flags);
		assert(last_length == PAGE_SIZE);
		clean();
	}
	assert(reserve_copy(2 * PAGE_SIZE) && reserve_copy(2 * PAGE_SIZE));
	int old_reads = reads;
	assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, 0) == -ENOMEM);
	assert(reads == old_reads && wasm_mmap_copy_stat(3) == COPY_BUDGET);
	atomic_sub(COPY_BUDGET, &copy_reserved);
	clean();
	/* A visible fatal signal is not permission to release a still-running
	 * producer. The model injects it only before/after synchronous read. */
	for (int phase = 0; phase < 3; phase++) {
		read_mode = phase == 1 ? 1 : 0;
		read_total = 0;
		fatal_pending = phase == 0;
		fatal_after_read = phase != 0;
		int reads_before = reads, copies_before = copies;
		assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, 0) == -EINTR);
		assert(copies == copies_before && reads == reads_before + (phase != 0));
		clean();
		fatal_pending = fatal_after_read = false;
	}
	read_mode = 0;
	read_total = 0;
	assert(wasm_mmap_file(0, PAGE_SIZE, PROT_READ, MAP_PRIVATE, 10, 0) == 65536);
	clean();
#ifdef CONFIG_WASM_MMAP_COPY_TEST
	assert(wasm_mmap_copy_stat(0x100) == copies);
#endif
	return 0;
}
