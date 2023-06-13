// SPDX-License-Identifier: Apache-2.0
/**
 * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdatomic.h>
#include <sys/mman.h>
#include <time.h>
#include <assert.h>

#include <rvgpu-proxy/gpu/rvgpu-iov.h>
#include <rvgpu-proxy/gpu/rvgpu-map-guest.h>
#include <rvgpu-proxy/gpu/rvgpu-vqueue.h>

static struct vqueue_request *vqueue_init_request(void)
{
	struct vqueue_request *req = calloc(1, sizeof(struct vqueue_request));

	if (!req)
		return NULL;

	req->idx = 0u;
	req->nr = req->nw = 0u;
	req->refcount = 1;
	req->mapped = false;

	return req;
}

void vqueue_request_unref(struct vqueue_request *req)
{
	req->refcount--;
	if (req->refcount > 0)
		return;

	assert(!req->mapped);
	free(req);
}

struct vqueue_request *vqueue_get_request(int vilo, struct vqueue *q)
{
	struct vqueue_request *req;
	uint16_t didx;

	assert(vqueue_are_requests_available(q));

	req = vqueue_init_request();
	if (!req)
		return NULL;

	atomic_thread_fence(memory_order_seq_cst);
	req->idx = q->vr.avail->ring[q->last_avail_idx % q->vr.num];
	req->nr = 0;
	req->nw = 0;
	req->q = q;
	for (didx = req->idx;;) {
		struct vring_desc d = q->vr.desc[didx % q->vr.num];
		struct iovec *iov;
		int prot;
		size_t *pn;

		if (d.flags & VRING_DESC_F_WRITE) {
			iov = &req->w[req->nw];
			prot = PROT_READ | PROT_WRITE;
			pn = &req->nw;
		} else {
			iov = &req->r[req->nr];
			prot = PROT_READ;
			pn = &req->nr;
		}
		iov->iov_len = d.len;
		iov->iov_base = map_guest(vilo, d.addr, prot, d.len);
		if (iov->iov_base != NULL) {
			(*pn)++;
			if (*pn >= VQUEUE_REQUEST_IOVEC_LEN)
				break;
		}

		if (d.flags & VRING_DESC_F_NEXT)
			didx = d.next;
		else
			break;
	}
	q->last_avail_idx++;
	req->mapped = true;
	return req;
}

void vqueue_send_response(struct vqueue_request *req,
			  void *resp, size_t resp_len)
{
	struct vqueue *q = req->q;
	uint16_t idx = atomic_load((atomic_ushort *)&q->vr.used->idx);
	struct vring_used_elem *el = &q->vr.used->ring[idx % q->vr.num];
	struct timespec barrier_delay = { .tv_nsec = 10 };
	size_t i;

	resp_len = copy_to_iov(req->w, req->nw, resp, resp_len);

	for (i = 0; i < req->nr; i++)
		unmap_guest(req->r[i].iov_base, req->r[i].iov_len);

	for (i = 0; i < req->nw; i++)
		unmap_guest(req->w[i].iov_base, req->w[i].iov_len);

	req->mapped = false;

	/* FIXME: Without this delay, kernel crashes in virtio-gpu driver
	 * most probable cause is race condition.
	 * Remove delay when race condition is fixed properly.
	 */
	clock_nanosleep(CLOCK_MONOTONIC, 0, &barrier_delay, NULL);

	atomic_store((atomic_uint *)&el->len, resp_len);
	atomic_store((atomic_uint *)&el->id, req->idx);
	idx++;
	atomic_store((atomic_ushort *)&q->vr.used->idx, idx);
	vqueue_request_unref(req);
}
