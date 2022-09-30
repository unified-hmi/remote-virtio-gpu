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

#ifndef RVGPU_PLUGIN_H
#define RVGPU_PLUGIN_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/queue.h>
#include <sys/uio.h>

#define RVGPU_BACKEND_V1 1u

/* Maximum number of remote rendering targets */
#define MAX_HOSTS 16

/*
 * Header for the RVGPU commands.
 * Imported from the remote-virtio-gpu device.
 * Every virtio command should follow a rvgpu header.
 * E.g <rvgpu_header><virtio_cmd_header><virtio_cmd>
 */
struct rvgpu_plugin_header {
	uint32_t size; /**< Size of the command */
	/* Variables used only in remote-virtio-gpu device */
	uint16_t idx; /**< Source virtio descriptor idx */
	uint16_t flags; /**< Flags (see enum rvgpu_flags) */
};

/*
 * rvgpu establishes two connections to remote rendering backend.
 * One is used for generic virtio command processing and the another one is
 * used for resource transferring, if resource caching feature is enabled.
 */
enum pipe_type {
	COMMAND,
	RESOURCE,
};

/* Reset states of the GPU Resync feature */
enum reset_state {
	GPU_RESET_NONE,
	GPU_RESET_TRUE,
	GPU_RESET_INITIATED,
};

struct tcp_host {
	char *ip;
	char *port;
};

struct rvgpu_scanout_arguments {
	/* TCP connection arguments */
	struct tcp_host tcp;
};

struct rvgpu_ctx_arguments {
	/* Timeout in seconds to wait for all scanouts be connected */
	uint16_t conn_tmt_s;
	/* Scanout reconnection interval */
	uint16_t reconn_intv_ms;
	/* Number of scanouts */
	uint16_t scanout_num;
};

struct rvgpu_scanout;

struct rvgpu_ctx {
	uint16_t scanout_num;
	void *priv;
};

/*
 * RVGPU resource information
 */
struct rvgpu_res_info {
	uint32_t target;
	uint32_t format;
	uint32_t width;
	uint32_t height;
	uint32_t depth;
	uint32_t array_size;
	uint32_t last_level;
	uint32_t flags;
	uint32_t bpp;
};

/*
 * RVGPU resource
 */
struct rvgpu_res {
	unsigned int resid;
	struct iovec *backing;
	unsigned int nbacking;
	struct rvgpu_res_info info;

	LIST_ENTRY(rvgpu_res) entry;
};

/*
 * Unified structure to pass the 2d/3d transfer to host info
 */
struct rvgpu_res_transfer {
	uint32_t x, y, z;
	uint32_t w, h, d;
	uint32_t level;
	uint32_t stride;
	uint64_t offset;
};

struct rvgpu_rendering_ctx_ops {
	int (*rvgpu_ctx_init)(struct rvgpu_ctx *ctx,
			      struct rvgpu_ctx_arguments args,
			      void (*gpu_reset_cb)(struct rvgpu_ctx *ctx,
						   enum reset_state state));
	void (*rvgpu_ctx_destroy)(struct rvgpu_ctx *ctx);
	void (*rvgpu_backend_reset_state)(struct rvgpu_ctx *ctx,
					  enum reset_state state);
	void (*rvgpu_frontend_reset_state)(struct rvgpu_ctx *ctx,
					   enum reset_state state);
	void (*rvgpu_ctx_wait)(struct rvgpu_ctx *ctx, enum reset_state state);
	void (*rvgpu_ctx_wakeup)(struct rvgpu_ctx *ctx);
	int (*rvgpu_ctx_poll)(struct rvgpu_ctx *ctx, enum pipe_type p,
			      int timeo, short int *events, short int *revents);
	int (*rvgpu_ctx_send)(struct rvgpu_ctx *ctx, const void *buf,
			      size_t len);
	struct rvgpu_res *(*rvgpu_ctx_res_find)(struct rvgpu_ctx *ctx,
						uint32_t resource_id);
	int (*rvgpu_ctx_transfer_to_host)(struct rvgpu_ctx *ctx,
					  const struct rvgpu_res_transfer *t,
					  const struct rvgpu_res *res);
	int (*rvgpu_ctx_res_create)(struct rvgpu_ctx *ctx,
				    const struct rvgpu_res_info *res,
				    uint32_t resource_id);
	void (*rvgpu_ctx_res_destroy)(struct rvgpu_ctx *ctx,
				      uint32_t resource_id);
};

struct rvgpu_rendering_backend_ops {
	int (*rvgpu_init)(struct rvgpu_ctx *ctx, struct rvgpu_scanout *scanout,
			  struct rvgpu_scanout_arguments args);
	void (*rvgpu_destroy)(struct rvgpu_ctx *ctx,
			      struct rvgpu_scanout *scanout);
	int (*rvgpu_send)(struct rvgpu_scanout *scanout, enum pipe_type p,
			  const void *buf, size_t len);
	int (*rvgpu_recv)(struct rvgpu_scanout *scanout, enum pipe_type p,
			  void *buf, size_t len);
	int (*rvgpu_recv_all)(struct rvgpu_scanout *scanout, enum pipe_type p,
			      void *buf, size_t len);
};

struct rvgpu_scanout {
	uint32_t scanout_id;
	union {
		struct {
			struct rvgpu_rendering_backend_ops ops;
		} plugin_v1;
	};
	void *priv;
};

struct rvgpu_backend {
	void *lib_handle;
	uint32_t plugin_version;
	union {
		struct {
			struct rvgpu_rendering_ctx_ops ops;
			struct rvgpu_ctx ctx;
			struct rvgpu_scanout scanout[MAX_HOSTS];
		} plugin_v1;
	};
};

#endif /* RVGPU_PLUGIN */
