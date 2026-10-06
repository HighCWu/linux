/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _WASM_VIRTIO_SNAPSHOT_H
#define _WASM_VIRTIO_SNAPSHOT_H
#include <linux/kconfig.h>
#include <linux/types.h>
struct block_device;
#if IS_REACHABLE(CONFIG_VIRTIO_WASM)
bool wasm_bdev_has_snapshot_source(struct block_device *bdev);
#else
static inline bool wasm_bdev_has_snapshot_source(struct block_device *bdev)
{
	return false;
}
#endif
#endif
