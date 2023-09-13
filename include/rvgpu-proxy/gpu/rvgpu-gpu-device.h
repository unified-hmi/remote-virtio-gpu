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

#ifndef RVGPU_GPU_DEVICE_H
#define RVGPU_GPU_DEVICE_H

#include <linux/virtio_gpu.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include <rvgpu-proxy/rvgpu-proxy.h>

#include <librvgpu/rvgpu-plugin.h>

#define PIPE_READ (0)
#define PIPE_WRITE (1)

struct gpu_device;

struct gpu_device_params {
	bool split_resources;
	int card_index;
	unsigned int num_scanouts;
	unsigned int mem_limit;
	unsigned long framerate;
	struct virtio_gpu_display_one dpys[VIRTIO_GPU_MAX_SCANOUTS];
};

/**
 * @brief GPU reset state callback
 *
 * @param ctx - pointer to rvgpu context
 * @param state - GPU reset state
 *
 * @return void
 */
void backend_reset_state(struct rvgpu_ctx *ctx, enum reset_state state);

/**
 * @brief Initialize rvgpu backend
 *
 * @param servers - pointer to remote targets settings
 *
 * @return pointer to rvgpu backend
 */
struct rvgpu_backend *init_backend_rvgpu(struct host_conn *servers);

/**
 * @brief Destroy rvgpu backend
 *
 * @param b - pointer to rvgpu backend
 *
 * @return void
 */
void destroy_backend_rvgpu(struct rvgpu_backend *b);

/**
 * @brief Initializes new virtio-gpu device using virtio loopback
 * @param lo_fd - virtio loopback descriptor
 * @param efd - epoll file descriptor to wait on
 * @param capset - capset file descriptor
 * @param params - pointer to input params
 * @param b - RVGPU plugin for network communications
 * @return pointer to gpu device structure
 */
struct gpu_device *gpu_device_init(int lo_fd, int efd, int capset,
				   const struct gpu_device_params *params,
				   struct rvgpu_backend *b);

/**
 * @brief Serves virtio-gpu config change from driver side
 * @param g - pointer to gpu device structure
 */
void gpu_device_config(struct gpu_device *g);

/**
 * @brief Serves virtio-gpu driver requests
 * @param g - pointer to gpu device structure
 */
void gpu_device_serve(struct gpu_device *g);

/**
 * @brief Frees resources allocated by gpu_device_init
 * @param g - pointer to gpu device structure
 */
void gpu_device_free(struct gpu_device *g);

#endif /* RVGPU_GPU_DEVICE_H */
