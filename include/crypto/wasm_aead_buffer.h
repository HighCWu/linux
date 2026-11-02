/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 linux-wasm-builder contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#ifndef _CRYPTO_WASM_AEAD_BUFFER_H
#define _CRYPTO_WASM_AEAD_BUFFER_H

#include <linux/errno.h>
#include <linux/net.h>
#include <linux/slab.h>
#include <linux/uio.h>

struct af_alg_wasm_aead_rx {
	struct iov_iter original;
	struct kvec vector;
	void *buffer;
	size_t capacity;
};

/* Stage the original request's available output, never independent page-sized
 * AEAD operations. All pages belong to the kernel; Wasm user memory cannot be
 * represented by the kernel's page structs. The existing RX budget still
 * bounds each request. Linear-memory destinations are readable and writable,
 * so a read-only prefix probe validates them without delivering plaintext.
 */
static int af_alg_wasm_prepare_aead_rx(struct msghdr *msg, size_t maxsize,
				     struct af_alg_wasm_aead_rx *rx)
{
	struct iov_iter probe;
	size_t wanted, copied;

	rx->buffer = NULL;
	if (!IS_ENABLED(CONFIG_WASM) || !user_backed_iter(&msg->msg_iter))
		return 0;
	wanted = min_t(size_t, maxsize, iov_iter_count(&msg->msg_iter));
	if (!wanted)
		return 0;
	rx->original = msg->msg_iter;
	rx->capacity = wanted;
	rx->buffer = kvmalloc(wanted, GFP_KERNEL | __GFP_ACCOUNT);
	if (!rx->buffer)
		return -ENOMEM;
	probe = rx->original;
	probe.data_source = true;
	copied = copy_from_iter(rx->buffer, wanted, &probe);
	if (!copied)
		return -EFAULT;
	rx->vector.iov_base = rx->buffer;
	rx->vector.iov_len = copied;
	iov_iter_kvec(&msg->msg_iter, ITER_DEST, &rx->vector, 1, copied);
	return 0;
}

/* The caller must first finish crypto and release every SG reference. In
 * particular EBADMSG must neither expose staged plaintext nor advance the
 * user's iterator. Erase the complete allocation, including a probe tail.
 */
static int af_alg_wasm_finish_aead_rx(struct msghdr *msg,
				    struct af_alg_wasm_aead_rx *rx, int result)
{
	if (!rx->buffer)
		return result;
	msg->msg_iter = rx->original;
	if (result > 0) {
		size_t copied;

		BUG_ON((size_t)result > rx->vector.iov_len);
		copied = copy_to_iter(rx->buffer, result, &msg->msg_iter);
		result = copied ? (int)copied : -EFAULT;
	}
	kvfree_sensitive(rx->buffer, rx->capacity);
	rx->buffer = NULL;
	return result;
}

#endif
