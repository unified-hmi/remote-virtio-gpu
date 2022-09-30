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

#ifndef RVGPU_VQUEUE_H
#define RVGPU_VQUEUE_H

#include <linux/virtio_ring.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/uio.h>

/**
 * @brief Virtqueue structure (device part)
 */
struct vqueue {
	struct vring vr; /**< actual vring in guest memory */
	uint16_t last_avail_idx; /**< index of last read avail entry */
};

/**
 * @brief Virtqueue request (device part)
 */
struct vqueue_request {
	struct iovec *r; /**< iovectors for reading */
	struct iovec *w; /**< iovectors for writing */
	size_t nr; /**< number of read iovecs */
	size_t nw; /**< number of write iovecs */
	size_t maxnum; /**< maximum number of iovecs */
	uint16_t idx; /**< index of first descriptior in the chain */
};

/**
 * @brief Init vqueue request structure
 * @param req - request to initialize
 * @param maxnum - maximum number of descriptors (size of queue)
 * @retval 0 if everything is OK
 * @retval -1 if there is no memory
 */
int vqueue_init_request(struct vqueue_request *req, size_t maxnum);

/**
 * @brief Frees resources of request
 * @param req - request to free
 */
void vqueue_free_request(struct vqueue_request *req);

/**
 * @brief Get next request from the vqueue
 * @param vilo - descriptor needed for memory mapping
 * @param q - queue to get requests from
 * @param req - structure for request
 * @retval 0 if no more pending requests are in the queue
 * @retval 1 if there is a pending request in the queue
 */
int vqueue_get_request(int vilo, struct vqueue *q, struct vqueue_request *req);

/**
 * @brief Send response to certain request
 * @param q - queue to send response to (should match request)
 * @param req - request to send reply to
 * @param resp - response buffer
 * @param resp_len - size of response buffer
 */
void vqueue_send_response(struct vqueue *q, struct vqueue_request *req,
			  void *resp, size_t resp_len);

#endif /* RVGPU_VQUEUE_H */
