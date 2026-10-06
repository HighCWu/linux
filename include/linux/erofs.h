/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_EROFS_H
#define _LINUX_EROFS_H
#include <linux/kconfig.h>
#include <linux/types.h>
struct super_block;
#if IS_REACHABLE(CONFIG_EROFS_FS)
bool erofs_is_single_bdev(struct super_block *sb);
#else
static inline bool erofs_is_single_bdev(struct super_block *sb)
{
	return false;
}
#endif
#endif
