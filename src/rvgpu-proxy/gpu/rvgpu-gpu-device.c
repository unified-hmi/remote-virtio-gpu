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

#include <assert.h>
#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/queue.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <linux/virtio_config.h>
#include <linux/virtio_gpu.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_lo.h>

#include <rvgpu-proxy/gpu/rvgpu-gpu-device.h>
#include <rvgpu-proxy/gpu/rvgpu-iov.h>
#include <rvgpu-proxy/gpu/rvgpu-map-guest.h>
#include <rvgpu-proxy/gpu/rvgpu-vqueue.h>

#include <librvgpu/rvgpu-plugin.h>
#include <librvgpu/rvgpu-protocol.h>

#include <rvgpu-generic/rvgpu-capset.h>
#include <rvgpu-generic/rvgpu-sanity.h>

#define GPU_MAX_CAPDATA 16

#if !defined(VIRTIO_GPU_RESP_ERR_DEVICE_RESET)
#define VIRTIO_GPU_RESP_ERR_DEVICE_RESET 0x1206
#endif

#if !defined(VIRTIO_GPU_F_VSYNC)
#define VIRTIO_GPU_F_VSYNC 5
#endif

#if !defined(VIRTIO_GPU_FLAG_VSYNC)
#define VIRTIO_GPU_FLAG_VSYNC (1 << 2)
#endif

#define VERSION_SYMBOL_NAME "rvgpu_backend_version"

#define GPU_BE_FIND_PLUGIN_VERSION(ver)                                        \
	do {                                                                   \
		uint32_t *ver_ptr =                                            \
			(uint32_t *)dlsym(plugin, VERSION_SYMBOL_NAME);        \
		if (ver_ptr == NULL)                                           \
			(ver) = 1u;                                            \
		else                                                           \
			(ver) = *ver_ptr;                                      \
	} while (0)
#define GPU_BE_PLUGIN_FIELD(plugin, field) plugin.ops.field
#define GPU_BE_OPS_FIELD(ver, field)                                           \
	GPU_BE_PLUGIN_FIELD(be->plugin_##ver, field)
#define GPU_BE_FIND_SYMBOL_OR_FAIL(ver, symbol)                                \
	do {                                                                   \
		GPU_BE_OPS_FIELD(ver, symbol) =                                \
			(typeof(GPU_BE_OPS_FIELD(ver, symbol)))(               \
				uintptr_t)dlsym(plugin, #symbol);              \
		if (GPU_BE_OPS_FIELD(ver, symbol) == NULL) {                   \
			warnx("failed to find plugin symbol '%s': %s",         \
			      #symbol, dlerror());                             \
			goto err_sym;                                          \
		}                                                              \
	} while (0)

struct gpu_capdata {
	struct capset hdr;
	uint8_t data[CAPSET_MAX_SIZE];
};

struct cmd {
	struct virtio_gpu_ctrl_hdr hdr;
	struct vqueue_request *req;

	TAILQ_ENTRY(cmd) cmds;
};

struct async_resp {
	TAILQ_HEAD(, cmd) async_cmds;
	int fence_pipe[2];
};

struct gpu_device {
	int lo_fd;
	int config_fd;
	int kick_fd;
	int vsync_fd;

	size_t max_mem;
	size_t curr_mem;
	const struct gpu_device_params *params;

	uint32_t scanres;
	uint32_t scan_id;
	int wait_vsync;

	unsigned int idx;
	struct gpu_capdata capdata[GPU_MAX_CAPDATA];
	size_t ncapdata;

	struct virtio_gpu_config config;
	pthread_t resource_thread;

	struct vqueue vq[2];
	struct rvgpu_backend *backend;
	struct async_resp *async_resp;
};

static inline uint64_t bit64(unsigned int shift)
{
	return ((uint64_t)1) << shift;
}
static enum reset_state gpu_reset_state;

int read_all(int fd, void *buf, size_t bytes)
{
	size_t offset = 0;

	while (offset < bytes) {
		ssize_t r = read(fd, (char *)buf + offset, bytes - offset);
		if (r > 0) {
			offset += (size_t)r;
		} else if (r == 0) {
			warnx("Connection was closed");
			return -1;
		} else if (errno != EAGAIN) {
			warn("Error while reading from socket");
			return -1;
		}
	}
	return offset;
}

int write_all(int fd, const void *buf, size_t bytes)
{
	size_t offset = 0;

	while (offset < bytes) {
		ssize_t written =
			write(fd, (const char *)buf + offset, bytes - offset);
		if (written >= 0) {
			offset += (size_t)written;
		} else if (errno != EAGAIN) {
			warn("Error while writing to socket");
			return -1;
		}
	}
	return offset;
}

static int rvgpu_init_backends(struct rvgpu_backend *b,
			       struct rvgpu_scanout_arguments *scanout_args)
{
	struct rvgpu_ctx *ctx = &b->plugin_v1.ctx;
	void *plugin = b->lib_handle;
	uint32_t version = b->plugin_version;

	for (unsigned int i = 0; i < ctx->scanout_num; i++) {
		struct rvgpu_scanout *be = &b->plugin_v1.scanout[i];

		switch (version) {
		case RVGPU_BACKEND_V1:
			GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_init);
			GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_destroy);
			GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_send);
			GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_recv);
			GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_recv_all);
			break;
		default:
			err(1, "unsupported backend version: %u\n", version);
			return -1;
		}

		be->plugin_v1.ops.rvgpu_init(ctx, be, scanout_args[i]);
	}

	return 0;
err_sym:
	return -1;
}

static int rvgpu_init_ctx(struct rvgpu_backend *b, struct rvgpu_ctx_arguments ctx_args)
{
	struct rvgpu_ctx *ctx = &b->plugin_v1.ctx;
	struct rvgpu_backend *be = b;
	void *plugin = b->lib_handle;
	uint32_t version;

	GPU_BE_FIND_PLUGIN_VERSION(version);

	switch (version) {
	case RVGPU_BACKEND_V1:
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_init);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_destroy);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_frontend_reset_state);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_wait);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_wakeup);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_poll);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_send);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_transfer_to_host);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_res_create);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_res_find);
		GPU_BE_FIND_SYMBOL_OR_FAIL(v1, rvgpu_ctx_res_destroy);
		break;
	default:
		err(1, "unsupported backend version: %u", version);
	}

	be->plugin_version = version;
	be->plugin_v1.ops.rvgpu_ctx_init(ctx, ctx_args, &backend_reset_state);

	return 0;
err_sym:
	return -1;
}

struct rvgpu_backend *init_backend_rvgpu(struct host_conn *servers)
{
	struct rvgpu_scanout_arguments scanout_args[MAX_HOSTS] = { 0 };
	struct rvgpu_backend *rvgpu_be;

	rvgpu_be = calloc(1, sizeof(*rvgpu_be));
	if (rvgpu_be == NULL) {
		warnx("failed to allocate backend: %s", strerror(errno));
		goto error;
	}

	char str_lib[] = "librvgpu.so";

	/* Flush the current dl error state */
	dlerror();

	rvgpu_be->lib_handle = dlopen(str_lib, RTLD_NOW);
	if (rvgpu_be->lib_handle == NULL) {
		warnx("failed to open backend library '%s': %s", str_lib,
		      dlerror());
		goto error_free;
	}

	struct rvgpu_ctx_arguments ctx_args = {
		.conn_tmt_s = servers->conn_tmt_s,
		.reconn_intv_ms = servers->reconn_intv_ms,
		.scanout_num = servers->host_cnt,
	};

	if (rvgpu_init_ctx(rvgpu_be, ctx_args)) {
		warnx("failed to init rvgpu ctx");
		goto error_dlclose;
	}

	for (unsigned int i = 0; i < servers->host_cnt; i++) {
		scanout_args[i].tcp.ip = strdup(servers->hosts[i].hostname);
		scanout_args[i].tcp.port = strdup(servers->hosts[i].portnum);
	}

	if (rvgpu_init_backends(rvgpu_be, scanout_args)) {
		warnx("failed to init rvgpu backends");
		goto error_dlclose;
	}

	return rvgpu_be;

error_dlclose:
	dlclose(rvgpu_be->lib_handle);
error_free:
	free(rvgpu_be);
error:
	return NULL;
}

void destroy_backend_rvgpu(struct rvgpu_backend *b)
{
	struct rvgpu_ctx *ctx = &b->plugin_v1.ctx;

	for (unsigned int i = 0; i < ctx->scanout_num; i++) {
		struct rvgpu_scanout *s = &b->plugin_v1.scanout[i];

		s->plugin_v1.ops.rvgpu_destroy(ctx, s);
	}
	b->plugin_v1.ops.rvgpu_ctx_destroy(ctx);
	dlclose(b->lib_handle);
}

static void gpu_device_free_res(struct gpu_device *g, struct rvgpu_res *res)
{
	for (unsigned int i = 0; i < res->nbacking; i++) {
		unmap_guest(res->backing[i].iov_base, res->backing[i].iov_len);
		g->curr_mem -= res->backing[i].iov_len;
	}
}

static void gpu_capset_init(struct gpu_device *g, int capset)
{
	g->config.num_capsets = 0u;
	size_t i;

	for (i = 0u; i < GPU_MAX_CAPDATA; i++) {
		struct gpu_capdata *c = &g->capdata[i];

		while (1) {

			if (read(capset, &c->hdr, sizeof(c->hdr)) !=
			    (ssize_t)sizeof(c->hdr))
				goto done;

			if (c->hdr.size > sizeof(c->data)) {
				warnx("too long capset");
				goto done;
			}

			if (read(capset, c->data, c->hdr.size) !=
			    (ssize_t)c->hdr.size) {
				warn("cannot read capset data");
				goto done;
			}

			if (c->hdr.id == 1)
				break;
		}
	}

done:
	g->ncapdata = i;
	g->config.num_capsets = i;
}

size_t process_fences(struct gpu_device *g, uint32_t fence_id)
{
	struct async_resp *r = g->async_resp;
	struct cmd *cmd;
	size_t processed = 0;

	TAILQ_FOREACH(cmd, &r->async_cmds, cmds)
	{
		if ((cmd->hdr.fence_id > fence_id) ||
		    (cmd->hdr.flags & VIRTIO_GPU_FLAG_VSYNC))
			continue;

		vqueue_send_response(cmd->req, &cmd->hdr, sizeof(cmd->hdr));
		TAILQ_REMOVE(&r->async_cmds, cmd, cmds);
		free(cmd);
		processed++;
	}

	return processed;
}

void add_resp(struct gpu_device *g, struct virtio_gpu_ctrl_hdr *hdr,
	      struct vqueue_request *req)
{
	struct async_resp *r = g->async_resp;
	struct cmd *cmd;

	cmd = (struct cmd *)calloc(1, sizeof(*cmd));
	assert(cmd);

	memcpy(&cmd->hdr, hdr, sizeof(*hdr));
	cmd->req = req;

	TAILQ_INSERT_TAIL(&r->async_cmds, cmd, cmds);
}

void destroy_async_resp(struct gpu_device *g)
{
	struct async_resp *r = g->async_resp;

	close(r->fence_pipe[PIPE_READ]);
	close(r->fence_pipe[PIPE_WRITE]);

	free(r);
}

static struct async_resp *init_async_resp(void)
{
	struct async_resp *r;

	r = (struct async_resp *)calloc(1, sizeof(*r));
	assert(r);

	TAILQ_INIT(&r->async_cmds);

	if (pipe(r->fence_pipe) == -1)
		err(1, "pipe creation error");

	return r;
}

/**
 * @brief Wait for input events from rvgpu-renderer on resource socket
 * @param b - pointer to RVGPU backend
 * @param revents - events received on poll
 */
int wait_resource_events(struct rvgpu_backend *b, short int *revents)
{
	short int events[b->plugin_v1.ctx.scanout_num];

	memset(revents, 0, sizeof(short int) * b->plugin_v1.ctx.scanout_num);
	memset(events, POLLIN,
	       sizeof(short int) * b->plugin_v1.ctx.scanout_num);

	return b->plugin_v1.ops.rvgpu_ctx_poll(&b->plugin_v1.ctx, RESOURCE, -1,
					       events, revents);
}

static void gpu_device_send_command(struct rvgpu_backend *u, void *buf,
				    size_t size, bool notify_all)
{
	struct rvgpu_scanout *s;
	int ret;

	if (notify_all) {
		if (u->plugin_v1.ops.rvgpu_ctx_send(&u->plugin_v1.ctx, buf,
						    size)) {
			warn("short write");
		}
	} else {
		s = &u->plugin_v1.scanout[0];
		ret = s->plugin_v1.ops.rvgpu_send(s, COMMAND, buf, size);

		if (ret != (int)size)
			warn("short write");
	}
}

static void read_from_pipe(struct rvgpu_scanout *s, char *buf, size_t size)
{
	size_t offset = 0;
	int ret = 0;

	while (offset < size) {
		ret = s->plugin_v1.ops.rvgpu_recv(
		    s, RESOURCE, (buf) ? buf + offset : buf, size - offset);

		if (ret == (int)size)
			break;

		if (ret <= 0)
			err(1, "Short read res pipe");

		offset += ret;
		if (offset > size)
			err(1, "Buffer overflow");
	}
}

static void resource_update(struct rvgpu_scanout *s, const struct iovec iovs[],
			    size_t niov, size_t skip, size_t length)
{
	for (size_t i = 0u; i < niov && length > 0u; i++) {
		const struct iovec *iov = &iovs[i];
		if (skip >= iov->iov_len) {
			skip -= iov->iov_len;
		} else {
			size_t l = iov->iov_len - skip;
			if (l > length) {
				l = length;
			}
			read_from_pipe(s, (char *)iov->iov_base + skip, l);
			skip = 0u;
			length -= l;
		}
	}
}

static void resource_transfer(struct gpu_device *g, struct rvgpu_scanout *s)
{
	struct rvgpu_header header = {0, 0, 0};
	struct rvgpu_patch patch = {0, 0, 0};
	struct virtio_gpu_transfer_host_3d t;
	struct rvgpu_res *res;

	read_from_pipe(s, (char *)&header, sizeof(header));

	if (header.size != sizeof(t))
		err(1, "Resource transfer protocol error");

	read_from_pipe(s, (char *)&t, sizeof(t));
	read_from_pipe(s, (char *)&patch, sizeof(patch));

	res = g->backend->plugin_v1.ops.rvgpu_ctx_res_find(
	    &g->backend->plugin_v1.ctx, t.resource_id);

	if (!res || !res->backing) {
		fprintf(stderr, "insufficient resource id %d, res %p\n",
			t.resource_id, res);
		return;
	}

	resource_update(s, res->backing, res->nbacking, patch.offset,
			patch.len);
}

static void *resource_thread_func(void *param)
{
	struct gpu_device *g = (struct gpu_device *)param;
	struct async_resp *r = (struct async_resp *)g->async_resp;
	struct rvgpu_backend *b = g->backend;
	struct rvgpu_res_message_header msg;
	short int revents[MAX_HOSTS];

	uint32_t recv_fence_ids[b->plugin_v1.ctx.scanout_num];
	int recv_fence_flags[b->plugin_v1.ctx.scanout_num];

	for (int i = 0; i < b->plugin_v1.ctx.scanout_num; i++) {
		recv_fence_ids[i] = 0;
		recv_fence_flags[i] = 0;
	}

	while (1) {
		wait_resource_events(b, revents);
		for (int i = 0; i < b->plugin_v1.ctx.scanout_num; i++) {
			if (revents[i] & POLLIN) {
				struct rvgpu_scanout *s =
					&b->plugin_v1.scanout[i];

				ssize_t ret = s->plugin_v1.ops.rvgpu_recv_all(
					s, RESOURCE, &msg, sizeof(msg));
				assert(ret > 0);
				(void)ret;

				if (msg.type == RVGPU_FENCE) {
					recv_fence_flags[i] = 1;
					uint32_t sync_fence_id = msg.fence_id;
					int recv_scanout_id = i;
					recv_fence_ids[i] = msg.fence_id;
					for (int j = 0;
					     j < b->plugin_v1.ctx.scanout_num;
					     j++) {
						if (sync_fence_id >
						    recv_fence_ids[j]) {
							sync_fence_id =
								recv_fence_ids[j];
							recv_scanout_id = j;
						}
					}

					if (recv_fence_flags[recv_scanout_id] ==
					    1) {
						ret = write_all(
							r->fence_pipe[PIPE_WRITE],
							&sync_fence_id,
							sizeof(sync_fence_id));
						assert(ret >= 0);
					}
				} else if (msg.type == RVGPU_RES_TRANSFER) {
					resource_transfer(
					    g, &b->plugin_v1.scanout[i]);
				}
			}
		}
	}

	return NULL;
}

struct gpu_device *gpu_device_init(int lo_fd, int efd, int capset,
				   const struct gpu_device_params *params,
				   struct rvgpu_backend *b)
{
	struct gpu_device *g;
	struct virtio_lo_qinfo q[2];
	unsigned int i;

	struct virtio_lo_devinfo info = {
		.nqueues = 2u,
		.qinfo = q,
		.device_id = VIRTIO_ID_GPU,
		.vendor_id = 0x1af4, /* PCI_VENDOR_ID_REDHAT_QUMRANET */
		.config_size = sizeof(struct virtio_gpu_config),
		.features =
			bit64(VIRTIO_GPU_F_VIRGL) | bit64(VIRTIO_F_VERSION_1),
	};
	if (params->framerate)
		info.features |= bit64(VIRTIO_GPU_F_VSYNC);

	g = (struct gpu_device *)calloc(1, sizeof(*g));
	if (!g) {
		warn("not enough memory");
		return NULL;
	}
	g->params = params;
	g->lo_fd = lo_fd;
	g->config_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	g->kick_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	g->config.num_scanouts = params->num_scanouts;
	g->max_mem = params->mem_limit * 1024 * 1024;
	if (capset != -1)
		gpu_capset_init(g, capset);

	info.card_index = params->card_index;
	info.config = (__u8 *)&g->config;
	info.config_kick = g->config_fd;

	for (i = 0u; i < 2u; i++) {
		q[i].kickfd = g->kick_fd;
		q[i].size = 1024u;
	}
	if (ioctl(lo_fd, VIRTIO_LO_ADDDEV, &info))
		err(1, "add virtio-lo-device");

	g->idx = info.idx;

	for (i = 0u; i < 2u; i++) {
		struct vring *vr = &g->vq[i].vr;

		vr->num = q[i].size;
		vr->desc = (struct vring_desc *)map_guest(
			lo_fd, q[i].desc, PROT_READ, q[i].size * 16u);
		vr->avail = (struct vring_avail *)map_guest(
			lo_fd, q[i].avail, PROT_READ, q[i].size * 2u + 6u);
		vr->used =
			(struct vring_used *)map_guest(lo_fd, q[i].used,
						       PROT_READ | PROT_WRITE,
						       q[i].size * 8u + 6u);
	}

	if (params->framerate) {
		g->vsync_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
		if (g->vsync_fd == -1)
			err(1, "timerfd_create");

		epoll_ctl(efd, EPOLL_CTL_ADD, g->vsync_fd,
			  &(struct epoll_event){ .events = EPOLLIN | EPOLLET,
						 .data = { .u32 = PROXY_GPU_QUEUES } });
	} else {
		g->vsync_fd = -1;
	}

	epoll_ctl(efd, EPOLL_CTL_ADD, g->config_fd,
		  &(struct epoll_event){ .events = EPOLLIN,
					 .data = { .u32 = PROXY_GPU_CONFIG } });
	epoll_ctl(efd, EPOLL_CTL_ADD, g->kick_fd,
		  &(struct epoll_event){ .events = EPOLLIN,
					 .data = { .u32 = PROXY_GPU_QUEUES } });

	g->async_resp = init_async_resp();
	epoll_ctl(efd, EPOLL_CTL_ADD, g->async_resp->fence_pipe[PIPE_READ],
		  &(struct epoll_event){ .events = EPOLLIN,
					 .data = { .u32 = PROXY_GPU_QUEUES } });

	g->backend = b;

	if (pthread_create(&g->resource_thread, NULL, resource_thread_func,
			   g) != 0) {
		err(1, "resource thread create");
	}
	return g;
}

void gpu_device_free(struct gpu_device *g)
{
	unsigned int i;

	for (i = 0u; i < 2u; i++) {
		struct vring *vr = &g->vq[i].vr;

		unmap_guest(vr->desc, vr->num * 16u);
		unmap_guest(vr->avail, vr->num * 2u + 6u);
		unmap_guest(vr->used, vr->num * 8u + 6u);
	}

	close(g->vsync_fd);
	close(g->config_fd);
	close(g->kick_fd);
	if (g->backend)
		destroy_backend_rvgpu(g->backend);
	destroy_async_resp(g);

	free(g);
}

void gpu_device_config(struct gpu_device *g)
{
	struct virtio_gpu_config c;
	struct virtio_lo_config cfg = { .idx = g->idx,
					.config = (__u8 *)&c,
					.len = sizeof(c) };

	if (ioctl(g->lo_fd, VIRTIO_LO_GCONF, &cfg) != 0)
		return;

	if (c.events_clear) {
		g->config.events_read &= ~c.events_clear;
		cfg.config = (__u8 *)&g->config;
		ioctl(g->lo_fd, VIRTIO_LO_SCONF, &cfg);
	}
}

static unsigned int gpu_device_create_res(struct gpu_device *g,
					  unsigned int resid,
					  const struct rvgpu_res_info *info)
{
	struct rvgpu_backend *b = g->backend;
	struct rvgpu_res *res;

	res = b->plugin_v1.ops.rvgpu_ctx_res_find(&b->plugin_v1.ctx, resid);
	if (res != NULL)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

	if (b->plugin_v1.ops.rvgpu_ctx_res_create(&b->plugin_v1.ctx, info,
						  resid))
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;

	return VIRTIO_GPU_RESP_OK_NODATA;
}

static unsigned int gpu_device_destroy_res(struct gpu_device *g,
					   unsigned int resid)
{
	struct rvgpu_backend *b = g->backend;
	struct rvgpu_res *res;

	res = b->plugin_v1.ops.rvgpu_ctx_res_find(&b->plugin_v1.ctx, resid);
	if (res == NULL)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

	gpu_device_free_res(g, res);
	b->plugin_v1.ops.rvgpu_ctx_res_destroy(&b->plugin_v1.ctx, resid);
	return VIRTIO_GPU_RESP_OK_NODATA;
}

static void gpu_device_send_patched(struct gpu_device *g,
				    const struct rvgpu_res *res,
				    const struct rvgpu_res_transfer *t)
{
	struct rvgpu_backend *b = g->backend;

	if (b->plugin_v1.ops.rvgpu_ctx_transfer_to_host(&b->plugin_v1.ctx, t,
							res)) {
		warn("short write");
	}
}

static unsigned int gpu_device_send_res(struct gpu_device *g,
					unsigned int resid,
					const struct rvgpu_res_transfer *t)
{
	struct rvgpu_backend *b = g->backend;
	struct rvgpu_res *res;

	res = b->plugin_v1.ops.rvgpu_ctx_res_find(&b->plugin_v1.ctx, resid);
	if (!res)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

	if (!res->backing)
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	gpu_device_send_patched(g, res, t);

	return VIRTIO_GPU_RESP_OK_NODATA;
}

static unsigned int gpu_device_attach(struct gpu_device *g, unsigned int resid,
				      struct virtio_gpu_mem_entry mem[],
				      unsigned int n)
{
	struct rvgpu_backend *b = g->backend;
	struct rvgpu_res *res;
	unsigned int i;
	size_t sentsize = 0u;

	res = b->plugin_v1.ops.rvgpu_ctx_res_find(&b->plugin_v1.ctx, resid);
	if (!res)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

	if (res->backing)
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	res->backing = calloc(n, sizeof(struct iovec));
	if (!res->backing) {
		warn("Out of memory on attach");
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	}
	res->nbacking = n;
	for (i = 0u; i < n; i++) {
		res->backing[i].iov_base = map_guest(g->lo_fd, mem[i].addr,
						     PROT_READ | PROT_WRITE,
						     mem[i].length);
		res->backing[i].iov_len = mem[i].length;
		sentsize += mem[i].length;
	}
	if (g->max_mem != 0 && (g->curr_mem + sentsize) > g->max_mem) {
		for (i = 0u; i < n; i++) {
			unmap_guest(res->backing[i].iov_base,
				    res->backing[i].iov_len);
		}
		warnx("Out of memory on attach");
		free(res->backing);
		res->backing = NULL;
		res->nbacking = 0u;
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;
	}

	g->curr_mem += sentsize;

	return VIRTIO_GPU_RESP_OK_NODATA;
}

static unsigned int gpu_device_detach(struct gpu_device *g, unsigned int resid)
{
	struct rvgpu_backend *b = g->backend;
	struct rvgpu_res *res;
	unsigned int i;

	res = b->plugin_v1.ops.rvgpu_ctx_res_find(&b->plugin_v1.ctx, resid);
	if (!res)
		return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

	if (!res->backing)
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	for (i = 0u; i < res->nbacking; i++) {
		unmap_guest(res->backing[i].iov_base, res->backing[i].iov_len);
		g->curr_mem -= res->backing[i].iov_len;
	}
	free(res->backing);
	res->backing = NULL;
	res->nbacking = 0u;
	return VIRTIO_GPU_RESP_OK_NODATA;
}

static unsigned int gpu_device_capset_info(struct gpu_device *g, unsigned int index,
					   struct virtio_gpu_resp_capset_info *ci)
{
	if (index < g->ncapdata) {
		const struct gpu_capdata *c = g->capdata + index;
		ci->capset_id = c->hdr.id;
		ci->capset_max_version = c->hdr.version;
		ci->capset_max_size = c->hdr.size;
		return VIRTIO_GPU_RESP_OK_CAPSET_INFO;
	} else {
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	}
}

static size_t gpu_device_capset(struct gpu_device *g, unsigned int capset_id,
				unsigned int capset_version,
				struct virtio_gpu_resp_capset *c)
{
	size_t i;

	for (i = 0; i < g->ncapdata; i++) {
		const struct gpu_capdata *cd = g->capdata + i;
		if (cd->hdr.id == capset_id && cd->hdr.version == capset_version) {
			memcpy(c->capset_data, cd->data, cd->hdr.size);
			c->hdr.type = VIRTIO_GPU_RESP_OK_CAPSET;
			return sizeof(*c) + cd->hdr.size;
		}
	}

	c->hdr.type = VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
	return sizeof(c->hdr);
}

static uint64_t gpu_device_read_vsync(struct gpu_device *g)
{
	uint64_t res = 0;
	ssize_t n;

	if (g->vsync_fd == -1)
		return 1;

	n = read(g->vsync_fd, &res, sizeof(res));

	if (n == -1 && errno == EAGAIN)
		res = 0u;
	else if (n != (ssize_t)sizeof(res))
		err(1, "Invalid vsync read");

	return res;
}

void backend_reset_state(struct rvgpu_ctx *ctx, enum reset_state state)
{
	(void)ctx;
	gpu_reset_state = state;
}

static unsigned long delta_time_nsec(struct timespec start,
				     struct timespec stop)
{
	return (unsigned long)((stop.tv_sec - start.tv_sec) * 1000000000 +
			       (stop.tv_nsec - start.tv_nsec));
}

static void set_timer(int timerfd, unsigned long framerate,
		      unsigned long vsync_time)
{
	struct itimerspec ts = { { 0 }, { 0 } };

	if (framerate > 0) {
		unsigned long vsync_delta = 0, rate = 1000000000UL / framerate;

		if (vsync_time > 0) {
			if ((vsync_time - rate) < rate)
				vsync_delta = vsync_time - rate;
		}

		ts.it_value.tv_nsec = rate - vsync_delta;
		if (ts.it_value.tv_nsec == 1000000000UL) {
			ts.it_value.tv_sec += 1;
			ts.it_value.tv_nsec = 0;
		}
	}

	if (timerfd_settime(timerfd, 0, &ts, NULL) == -1)
		fprintf(stderr, "Failed to set timerfd: %s\n", strerror(errno));
}

static size_t gpu_device_serve_vsync(struct gpu_device *g)
{
	struct async_resp *r = g->async_resp;
	struct cmd *cmd;
	size_t processed = 0;

	TAILQ_FOREACH(cmd, &r->async_cmds, cmds)
	{
		if (cmd->hdr.flags & VIRTIO_GPU_FLAG_VSYNC) {
			vqueue_send_response(cmd->req, &cmd->hdr, sizeof(cmd->hdr));
			TAILQ_REMOVE(&r->async_cmds, cmd, cmds);
			free(cmd);
			processed++;
		}
	}
	return processed;
}

static int gpu_device_serve_fences(struct gpu_device *g)
{
	struct async_resp *r = g->async_resp;
	struct pollfd pfd;
	int processed = 0;
	uint32_t fence_id;

	pfd.fd = r->fence_pipe[PIPE_READ];
	pfd.events = POLLIN;

	while (poll(&pfd, 1, 0) > 0) {
		if (pfd.revents & POLLIN) {
			int rc = read_all(r->fence_pipe[PIPE_READ], &fence_id,
					  sizeof(fence_id));
			if (rc != sizeof(fence_id))
				warnx("read error: %d", rc);

			processed += process_fences(g, fence_id);
		}
	}
	return processed;
}

static void gpu_device_trigger_vsync(struct gpu_device *g,
				     struct virtio_gpu_ctrl_hdr *hdr,
				     struct vqueue_request *req,
				     unsigned int flags,
				     struct timespec vsync_ts)
{
	if (!(flags & VIRTIO_GPU_FLAG_VSYNC))
		return;

	hdr->flags |= VIRTIO_GPU_FLAG_VSYNC;
	/* use padding bytes to pass scanout_id to virtio-gpu driver */
	hdr->padding = g->scan_id;
	add_resp(g, hdr, req);

	if ((!vsync_ts.tv_sec) && (!vsync_ts.tv_nsec)) {
		set_timer(g->vsync_fd, g->params->framerate, 0);
	} else {
		struct timespec now;

		clock_gettime(CLOCK_REALTIME, &now);
		set_timer(g->vsync_fd, g->params->framerate,
			  delta_time_nsec(vsync_ts, now));
	}

	g->wait_vsync = 1;
}

static void gpu_device_serve_ctrl(struct gpu_device *g)
{
	struct rvgpu_backend *b = g->backend;
	int kick = 0;
	static bool reset;
	static struct timespec vsync_ts;

	union {
		struct virtio_gpu_ctrl_hdr hdr;
		struct virtio_gpu_resp_display_info rdi;
		struct virtio_gpu_resp_capset_info ci;
		struct virtio_gpu_resp_capset c;
		uint8_t data[4096];
	} resp;
	memset(&resp.hdr, 0, sizeof(resp.hdr));
	if (g->wait_vsync) {
		if (gpu_device_read_vsync(g) > 0u) {
			g->wait_vsync = 0;
			kick += gpu_device_serve_vsync(g);
			set_timer(g->vsync_fd, 0, 0);
		}
	}
	kick += gpu_device_serve_fences(g);
	while (1) {
		struct vqueue_request *req;
		size_t resp_len = sizeof(resp.hdr);
		union virtio_gpu_cmd r;
		struct rvgpu_header rhdr = {
			.idx = 0,
			.flags = 0,
		};

		if (!vqueue_are_requests_available(&g->vq[0]))
			break;

		req = vqueue_get_request(g->lo_fd, &g->vq[0]);
		if (!req)
			errx(1, "out of memory");

		rhdr.size = (uint32_t)iov_size(req->r, req->nr),

		copy_from_iov(req->r, req->nr, &r, sizeof(r));

		resp.hdr.flags = 0;
		resp.hdr.fence_id = 0;
		resp.hdr.type = sanity_check_gpu_ctrl(&r, rhdr.size, true);

		if (resp.hdr.type == VIRTIO_GPU_RESP_OK_NODATA) {
			bool notify_all = true;
			size_t i;

			if (r.hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
				resp.hdr.flags = VIRTIO_GPU_FLAG_FENCE;
				resp.hdr.fence_id = r.hdr.fence_id;
				resp.hdr.ctx_id = r.hdr.ctx_id;
				add_resp(g, &resp.hdr, vqueue_request_ref(req));
			}

			if (r.hdr.type == VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D)
				notify_all = false;

			gpu_device_send_command(b, &rhdr, sizeof(rhdr),
						notify_all);
			for (i = 0u; i < req->nr; i++) {
				struct iovec *iov = &req->r[i];

				gpu_device_send_command(
					b, iov->iov_base, iov->iov_len, notify_all);
			}

			/* command is sane, parse it */
			switch (r.hdr.type) {
			case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
				memcpy(resp.rdi.pmodes, g->params->dpys,
				       g->params->num_scanouts *
					       sizeof(struct virtio_gpu_display_one));
				resp.hdr.type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
				resp_len = sizeof(resp.rdi);
				break;
			case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
				resp.hdr.type = gpu_device_create_res(
					g, r.r_c2d.resource_id,
					&(struct rvgpu_res_info){
						.target = 2,
						.depth = 1,
						.array_size = 1,
						.format = r.r_c2d.format,
						.width = r.r_c2d.width,
						.height = r.r_c2d.height,
						.flags =
							VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP,
					});
				break;
			case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
				resp.hdr.type = gpu_device_create_res(
					g, r.r_c3d.resource_id,
					&(struct rvgpu_res_info){
						.target = r.r_c3d.target,
						.width = r.r_c3d.width,
						.height = r.r_c3d.height,
						.depth = r.r_c3d.depth,
						.array_size =
							r.r_c3d.array_size,
						.format = r.r_c3d.format,
						.flags = r.r_c3d.flags,
						.last_level =
							r.r_c3d.last_level,
					});
				break;
			case VIRTIO_GPU_CMD_RESOURCE_UNREF:
				resp.hdr.type = gpu_device_destroy_res(
					g, r.r_unref.resource_id);
				break;
			case VIRTIO_GPU_CMD_SET_SCANOUT:
				if (r.s_set.scanout_id == 0)
					g->scanres = r.s_set.resource_id;
				g->scan_id = r.s_set.scanout_id;
				break;
			case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
				if (r.r_flush.resource_id == g->scanres) {
					if (gpu_device_read_vsync(g) == 0) {
						gpu_device_trigger_vsync(
							g, &resp.hdr, vqueue_request_ref(req),
							r.hdr.flags, vsync_ts);
						clock_gettime(CLOCK_REALTIME,
							      &vsync_ts);
					}
				}
				break;
			case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
				resp.hdr.type = gpu_device_send_res(
					g, r.t_2h2d.resource_id,
					&(struct rvgpu_res_transfer){
						.x = r.t_2h2d.r.x,
						.y = r.t_2h2d.r.y,
						.w = r.t_2h2d.r.width,
						.h = r.t_2h2d.r.height,
						.offset = r.t_2h2d.offset,
						.d = 1,
					});
				break;
			case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
				resp.hdr.type = gpu_device_send_res(
					g, r.t_h3d.resource_id,
					&(struct rvgpu_res_transfer){
						.x = r.t_h3d.box.x,
						.y = r.t_h3d.box.y,
						.z = r.t_h3d.box.z,
						.w = r.t_h3d.box.w,
						.h = r.t_h3d.box.h,
						.d = r.t_h3d.box.d,
						.level = r.t_h3d.level,
						.stride = r.t_h3d.stride,
						.offset = r.t_h3d.offset,
					});
				break;
			case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
				resp.hdr.type = gpu_device_attach(
					g, r.r_att.resource_id, r.r_mem,
					r.r_att.nr_entries);
				break;
			case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
				resp.hdr.type = gpu_device_detach(
					g, r.r_det.resource_id);
				break;
			case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
				resp.hdr.type = gpu_device_capset_info(
					g, r.capset_info.capset_index,
					&resp.ci);
				resp_len = sizeof(resp.ci);
				break;
			case VIRTIO_GPU_CMD_GET_CAPSET:
				resp_len = gpu_device_capset(
					g, r.capset.capset_id,
					r.capset.capset_version, &resp.c);
				break;
			default:
				break;
			}
		}
		if (gpu_reset_state) {
			resp.hdr.type = VIRTIO_GPU_RESP_ERR_DEVICE_RESET;
			reset = true;
		}
		if ((!(resp.hdr.flags & VIRTIO_GPU_FLAG_FENCE)) &&
		    (!(resp.hdr.flags & VIRTIO_GPU_FLAG_VSYNC))) {
			vqueue_send_response(req, &resp, resp_len);
			kick++;
		} else {
			vqueue_request_unref(req);
		}
	}
	if (kick) {
		struct virtio_lo_kick k = {
			.idx = g->idx,
			.qidx = 0,
		};
		if (ioctl(g->lo_fd, VIRTIO_LO_KICK, &k) != 0)
			warn("ctrl kick failed");
	}
	if (reset) {
		struct rvgpu_ctx *ctx = &b->plugin_v1.ctx;

		if (gpu_reset_state == GPU_RESET_NONE) {
			reset = false;
			b->plugin_v1.ops.rvgpu_ctx_wait(ctx, GPU_RESET_NONE);

		} else if (gpu_reset_state == GPU_RESET_TRUE) {
			b->plugin_v1.ops.rvgpu_frontend_reset_state(
				ctx, GPU_RESET_INITIATED);
			gpu_reset_state = GPU_RESET_INITIATED;
			b->plugin_v1.ops.rvgpu_ctx_wakeup(ctx);
		}
	}
}

static void gpu_device_serve_cursor(struct gpu_device *g)
{
	struct rvgpu_backend *b = g->backend;
	int kick = 0;

	while (1) {
		struct vqueue_request *req;
		union virtio_gpu_cmd r;
		struct virtio_gpu_ctrl_hdr resp = { .flags = 0, .fence_id = 0 };
		struct rvgpu_header rhdr = {
			.idx = 0,
			.flags = RVGPU_CURSOR,
		};

		if (!vqueue_are_requests_available(&g->vq[1]))
			break;

		req = vqueue_get_request(g->lo_fd, &g->vq[1]);
		if (!req)
			errx(1, "out of memory");

		size_t cmdsize = iov_size(req->r, req->nr);
		rhdr.size = (uint32_t)cmdsize,

		copy_from_iov(req->r, req->nr, &r, sizeof(r));

		resp.type = sanity_check_gpu_cursor(&r, cmdsize, true);
		if (resp.type == VIRTIO_GPU_RESP_OK_NODATA) {
			if (r.hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
				resp.flags = VIRTIO_GPU_FLAG_FENCE;
				resp.fence_id = r.hdr.fence_id;
				resp.ctx_id = r.hdr.ctx_id;
			}

			gpu_device_send_command(b, &rhdr, sizeof(rhdr), true);
			for (unsigned int i = 0u; i < req->nr; i++) {
				struct iovec *iov = &req->r[i];

				gpu_device_send_command(b, iov->iov_base,
							iov->iov_len, true);
			}
		}
		vqueue_send_response(req, &resp, sizeof(resp));
		kick = 1;
	}
	if (kick) {
		struct virtio_lo_kick k = {
			.idx = g->idx,
			.qidx = 1,
		};
		if (ioctl(g->lo_fd, VIRTIO_LO_KICK, &k) != 0)
			warn("cursor kick failed");
	}
}

void gpu_device_serve(struct gpu_device *g)
{
	uint64_t ev;
	ssize_t ret;

	ret = read(g->kick_fd, &ev, sizeof(ev));
	if (ret > 0 && ret != (ssize_t)sizeof(ev)) {
		err(1, "wrong read from eventfd");
	} else if (ret < 0 && errno != EAGAIN) {
		err(1, "read failed from eventfd");
	}

	gpu_device_serve_ctrl(g);
	gpu_device_serve_cursor(g);
}
