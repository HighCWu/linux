/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_NOMMU_MINCORE_H
#define _LINUX_NOMMU_MINCORE_H

#include <linux/compiler_types.h>
#include <linux/types.h>

/* Fill @vec with up to @max_pages mincore status bytes for @start. */
typedef long (*nommu_mincore_query_fn)(unsigned long start,
				       unsigned long max_pages,
				       unsigned char *vec);

/* Shared mincore argument validation, chunking, and user-vector copyout. */
long nommu_mincore(unsigned long start, size_t len,
		   unsigned char __user *vec,
		   nommu_mincore_query_fn query);

#endif /* _LINUX_NOMMU_MINCORE_H */
