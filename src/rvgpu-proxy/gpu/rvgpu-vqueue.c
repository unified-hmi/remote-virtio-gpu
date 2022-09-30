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

#include <rvgpu-proxy/gpu/rvgpu-iov.h>
#include <rvgpu-proxy/gpu/rvgpu-map-guest.h>
#include <rvgpu-proxy/gpu/rvgpu-vqueue.h>

int vqueue_get_request(int vilo, struct vqueue *q, struct vqueue_request *req)
{
	uint16_t avail_idx = q->vr.avail->idx;
	uint16_t didx;

	if (avail_idx == q->last_avail_idx) {
		/* no more requests available */
		return 0;
	}
	vqueue_init_request(req, 1024);
	atomic_thread_fence(memory_order_seq_cst);
	req->idx = q->vr.avail->ring[q->last_avail_idx % q->vr.num];
	req->nr = 0;
	req->nw = 0;
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
			if (*pn >= req->maxnum)
				break;
		}

		if (d.flags & VRING_DESC_F_NEXT)
			didx = d.next;
		else
			break;
	}
	q->last_avail_idx++;
	return 1;
}

static void unmap_request(struct vqueue_request *req)
{
	size_t i;

	for (i = 0; i < req->nr; i++)
		unmap_guest(req->r[i].iov_base, req->r[i].iov_len);

	for (i = 0; i < req->nw; i++)
		unmap_guest(req->w[i].iov_base, req->w[i].iov_len);

	req->nr = req->nw = 0;
}

void vqueue_send_response(struct vqueue *q, struct vqueue_request *req,
			  void *resp, size_t resp_len)
{
	uint16_t idx = atomic_load((atomic_ushort *)&q->vr.used->idx);
	struct vring_used_elem *el = &q->vr.used->ring[idx % q->vr.num];
	struct timespec barrier_delay = { .tv_nsec = 10 };

	resp_len = copy_to_iov(req->w, req->nw, resp, resp_len);
	unmap_request(req);

	/* FIXME: Without this delay, kernel crashes in virtio-gpu driver
	 * most probable cause is race condition.
	 * Remove delay when race condition is fixed properly.
	 */
	clock_nanosleep(CLOCK_MONOTONIC, 0, &barrier_delay, NULL);

	atomic_store((atomic_uint *)&el->len, resp_len);
	atomic_store((atomic_uint *)&el->id, req->idx);
	idx++;
	atomic_store((atomic_ushort *)&q->vr.used->idx, idx);
	vqueue_free_request(req);
}

int vqueue_init_request(struct vqueue_request *req, size_t maxnum)
{
	req->r = calloc(maxnum, sizeof(struct iovec));
	req->w = calloc(maxnum, sizeof(struct iovec));
	req->maxnum = maxnum;
	req->idx = 0u;
	req->nr = req->nw = 0u;
	if (!req->r || !req->w) {
		free(req->r);
		free(req->w);
		return -1;
	}
	return 0;
}

void vqueue_free_request(struct vqueue_request *req)
{
	free(req->r);
	free(req->w);
	req->r = NULL;
	req->w = NULL;
}
