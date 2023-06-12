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
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/virtio_gpu.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>
#include <virglrenderer.h>

#include <rvgpu-generic/rvgpu-capset.h>
#include <rvgpu-generic/rvgpu-sanity.h>
#include <rvgpu-generic/rvgpu-utils.h>

#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/rvgpu-renderer.h>
#include <rvgpu-renderer/virgl/rvgpu-virgl.h>

struct rvgpu_pr_state {
	struct rvgpu_egl_state *egl;
	struct rvgpu_pr_params pp;
	uint8_t *buffer[2];
	size_t buftotlen[2];
	size_t bufcurlen[2];
	size_t bufpos[2];
	int res_socket;
	atomic_uint fence_received, fence_sent;
};

static void clear_scanout(struct rvgpu_pr_state *p, struct rvgpu_scanout *s);

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

static virgl_renderer_gl_context
create_context(void *opaque, int scanout_idx,
	       struct virgl_renderer_gl_ctx_param *params)
{
	(void)scanout_idx;
	struct rvgpu_pr_state *state = (struct rvgpu_pr_state *)opaque;

	return (virgl_renderer_gl_context)rvgpu_egl_create_context(
		state->egl, params->major_ver, params->minor_ver,
		params->shared);
}

static void destroy_context(void *opaque, virgl_renderer_gl_context ctx)
{
	struct rvgpu_pr_state *state = (struct rvgpu_pr_state *)opaque;

	rvgpu_egl_destroy_context(state->egl, ctx);
}

static int make_context_current(void *opaque, int scanout_id,
				virgl_renderer_gl_context ctx)
{
	(void)scanout_id;
	struct rvgpu_pr_state *state = (struct rvgpu_pr_state *)opaque;

	return rvgpu_egl_make_context_current(state->egl, ctx);
}

static void virgl_write_fence(void *opaque, uint32_t fence)
{
	struct rvgpu_pr_state *state = (struct rvgpu_pr_state *)opaque;
	struct rvgpu_res_message_header msg = { .type = RVGPU_FENCE,
						.fence_id = fence };

	if (fence > state->fence_sent)
		state->fence_sent = fence;

	int res = write_all(state->res_socket, &msg,
			    sizeof(struct rvgpu_res_message_header));
	assert(res >= 0);
}

static struct virgl_renderer_callbacks virgl_cbs = {
	.version = 1,
	.write_fence = virgl_write_fence,
	.create_gl_context = create_context,
	.destroy_gl_context = destroy_context,
	.make_current = make_context_current,
};

static int rvgpu_pr_readbuf(struct rvgpu_pr_state *p, int stream)
{
	struct pollfd pfd[MAX_PFD];
	size_t n;
	int timeout = 0;
	struct timespec barrier_delay = { .tv_nsec = 1000 };

	pfd[0].fd = 0;

	pfd[0].events = POLLIN;
	n = rvgpu_egl_prepare_events(p->egl, &pfd[1], MAX_PFD - 1);
	if (p->fence_received == p->fence_sent)
		timeout = -1;

	while (poll(pfd, n + 1, timeout) == 0 &&
	       (p->fence_received != p->fence_sent)) {
		virgl_renderer_poll();
		clock_nanosleep(CLOCK_MONOTONIC, 0, &barrier_delay, NULL);

		if (p->fence_received == p->fence_sent)
			timeout = -1;
	}
	rvgpu_egl_process_events(p->egl, &pfd[1], n);
	if (pfd[0].revents & POLLIN) {
		ssize_t n;

		n = read(pfd[0].fd, p->buffer[stream], p->buftotlen[stream]);
		if (n <= 0)
			return 0;

		p->bufcurlen[stream] = (size_t)n;
		p->bufpos[stream] = 0u;
	}
	for (size_t i = 0; i <= n; i++) {
		if (pfd[i].revents & (POLLERR | POLLHUP | POLLNVAL))
			return 0;
	}
	return 1;
}

static size_t rvgpu_pr_read(struct rvgpu_pr_state *p, void *buf, size_t size,
			    size_t nmemb, int stream)
{
	size_t offset = 0u;
	size_t total = size * nmemb;

	while (offset < total) {
		size_t avail = p->bufcurlen[stream] - p->bufpos[stream];

		if (avail > (total - offset))
			avail = (total - offset);

		if (buf) {
			memcpy((char *)buf + offset,
			       &p->buffer[stream][p->bufpos[stream]], avail);
		}
		offset += avail;
		p->bufpos[stream] += avail;
		if (offset == total)
			break;

		assert(p->bufpos[stream] == p->bufcurlen[stream]);
		/* actually read from input now */
		if (!rvgpu_pr_readbuf(p, stream))
			break;
	}

	return offset / size;
}

struct rvgpu_pr_state *rvgpu_pr_init(struct rvgpu_egl_state *e,
				     const struct rvgpu_pr_params *params,
				     int res_socket)
{
	int ret, buf_size;
	struct rvgpu_pr_state *p = calloc(1, sizeof(*p));

	assert(p);

	buf_size = INBUFSIZE;

	p->pp = *params;
	p->egl = e;

	ret = virgl_renderer_init(p, 0, &virgl_cbs);
	assert(ret == 0);

	ret = fcntl(0, F_GETFL);
	if (ret != -1)
		fcntl(0, F_SETFL, ret | O_NONBLOCK);

	p->buffer[COMMAND] = malloc(buf_size);
	assert(p->buffer[COMMAND]);
	p->buftotlen[COMMAND] = buf_size;

	for (uint32_t i = 0; i < p->pp.nsp; i++) {
		if (p->pp.sp[i].boxed)
			clear_scanout(p, &e->scanouts[i]);
	}

	p->res_socket = res_socket;

	return p;
}

void rvgpu_pr_free(struct rvgpu_pr_state *p)
{
	virgl_renderer_force_ctx_0();
	virgl_renderer_cleanup(p);

	free(p->buffer[COMMAND]);
	free(p);
}

static void
resource_attach_backing(struct virtio_gpu_resource_attach_backing *r,
			struct virtio_gpu_mem_entry entries[])
{
	size_t length = 0;
	struct iovec *p;
	void *resmem;

	for (unsigned int i = 0; i < r->nr_entries; i++)
		length += entries[i].length;

	if (length == 0)
		errx(1, "invalid length of backing storage");

	p = malloc(sizeof(*p));
	if (p == NULL)
		err(1, "Out of mem");

	resmem = malloc(length);
	if (resmem == NULL)
		err(1, "Out of mem");

	memset(resmem, 0x0, length);

	p->iov_base = resmem;
	p->iov_len = length;

	virgl_renderer_resource_attach_iov(r->resource_id, p, 1);
}

static void load_resource_patched(struct rvgpu_pr_state *state, struct iovec *p)
{
	struct rvgpu_patch header = { 0, 0 };
	int stream = COMMAND;
	uint32_t offset = 0;

	while (rvgpu_pr_read(state, &header, sizeof(header), 1, stream) == 1) {
		if (header.len == 0)
			break;

		if (stream == COMMAND)
			offset = header.offset;

		if ((((uint64_t)offset + header.len) > p[0].iov_len))
			errx(1, "Wrong patch format!");

		if (rvgpu_pr_read(state, (char *)p[0].iov_base + offset, 1,
				  header.len, stream) != header.len) {
			err(1, "Short read");
		}
	}
}

static bool load_resource(struct rvgpu_pr_state *state, unsigned int res_id)
{
	struct iovec *p = NULL;
	int iovn = 0;
	bool load = true;

	virgl_renderer_resource_detach_iov(res_id, &p, &iovn);
	if (p == NULL)
		load = false;

	if (load) {
		load_resource_patched(state, p);
		virgl_renderer_resource_attach_iov(res_id, p, iovn);
	}

	return load;
}

static void set_scanout(struct rvgpu_pr_state *p,
			struct virtio_gpu_set_scanout *set,
			struct rvgpu_scanout *s)
{
	struct virgl_renderer_resource_info info;
	const struct rvgpu_scanout_params *sp = &s->params;

	if (set->resource_id &&
	    virgl_renderer_resource_get_info(set->resource_id, &info) == 0) {
		struct rvgpu_virgl_params params = {
			.box = { .x = set->r.x,
				 .y = set->r.y,
				 .w = set->r.width,
				 .h = set->r.height },
			.tex_id = info.tex_id,
			.tex = { .w = info.width, .h = info.height },
			.res_id = set->resource_id,
			.y0_top = info.flags & 1
		};
		if (sp->boxed) {
			params.box = sp->box;
		} else if (set->r.width == 0 || set->r.height == 0) {
			params.box.w = info.width;
			params.box.h = info.height;
		}
		if (!sanity_check_resource_rect(&set->r, info.width,
						info.height)) {
			err(1, "Invalid rectangle for set scanout");
		}

		rvgpu_egl_set_scanout(p->egl, s, &params);
	} else {
		clear_scanout(p, s);
	}
}

static void clear_scanout(struct rvgpu_pr_state *p, struct rvgpu_scanout *s)
{
	struct rvgpu_virgl_params params = {
		.box = { .w = 100, .h = 100 },
		.tex_id = 0,
	};
	if (s->params.boxed)
		params.box = s->params.box;
	rvgpu_egl_set_scanout(p->egl, s, &params);
}

static void dump_capset(struct rvgpu_pr_state *p)
{
	for (unsigned int id = 1;; id++) {
		uint32_t maxver, maxsize;

		virgl_renderer_get_cap_set(id, &maxver, &maxsize);
		if (maxsize == 0 || maxsize >= 1024) {
			warnx("Error while getting capset %u", id);
			break;
		}

		for (unsigned int version = 1; version <= maxver; version++) {
			struct capset hdr = { .id = id,
					      .version = version,
					      .size = maxsize };
			uint8_t data[1024];

			memset(data, 0, maxsize);
			virgl_renderer_fill_caps(id, version, data);
			hdr.size = maxsize;

			if (fwrite(&hdr, sizeof(hdr), 1, p->pp.capset) != 1)
				warn("Error while dumping capset");

			if (fwrite(data, maxsize, 1, p->pp.capset) != 1)
				warn("Error while dumping capset");

			warnx("capset dumped for id %u version %u size %u", id,
			      version, maxsize);
		}
	}
	fflush(p->pp.capset);
	p->pp.capset = NULL;
}

static bool check_rect(uint32_t resource_id, const struct virtio_gpu_rect *r)
{
	struct virgl_renderer_resource_info info;

	if (virgl_renderer_resource_get_info((int)resource_id, &info) != 0)
		return false;

	return sanity_check_resource_rect(r, info.width, info.height);
}

static bool check_box(uint32_t resource_id, const struct virtio_gpu_box *b)
{
	struct virgl_renderer_resource_info info;

	if (virgl_renderer_resource_get_info((int)resource_id, &info) != 0)
		return false;

	return sanity_check_resource_box(b, info.width, info.height,
					 info.depth);
}

static unsigned int rvgpu_serve_vscanout(struct rvgpu_pr_state *pr,
					 struct rvgpu_egl_state *e,
					 unsigned int cmd_type,
					 unsigned int scanout_id,
					 unsigned int res_id)
{
	struct rvgpu_scanout *s;

	switch (cmd_type) {
	case RVGPU_WINDOW_CREATE:
		s = rvgpu_create_vscanout(e, scanout_id);
		set_scanout(pr,
			    &(struct virtio_gpu_set_scanout){ .resource_id =
								      res_id },
			    s);
		return res_id;
	case RVGPU_WINDOW_DESTROY:
		s = rvgpu_get_vscanout(e, scanout_id);
		if (s != NULL)
			rvgpu_destroy_vscanout(e, s);
		return 0;
	case RVGPU_WINDOW_UPDATE:
		s = rvgpu_get_vscanout(e, scanout_id);
		if (s != NULL) {
			set_scanout(pr,
				    &(struct virtio_gpu_set_scanout){
					    .resource_id = res_id },
				    s);
			return res_id;
		}
		return 0;
	case RVGPU_WINDOW_HIDE:
		/* TODO: hide window */
		return 0;
	case RVGPU_WINDOW_SHOW:
		/* TODO: show window */
		return 0;
	case RVGPU_WINDOW_DESTROYALL:
		rvgpu_destroy_all_vscanouts(e);
		return 0;
	default:
		return 0;
	}
}

unsigned int rvgpu_pr_dispatch(struct rvgpu_pr_state *p)
{
	struct rvgpu_header uhdr;

	if (p->pp.capset)
		dump_capset(p);

	while (rvgpu_pr_read(p, &uhdr, sizeof(uhdr), 1, COMMAND) == 1) {
		struct iovec *piov;
		union virtio_gpu_cmd r;
		size_t ret;
		int n;
		unsigned int draw = 0;
		enum virtio_gpu_ctrl_type sane;

		memset(&r.hdr, 0, sizeof(r.hdr));
		if (uhdr.size > sizeof(r))
			errx(1, "Too long read (%u)", uhdr.size);

		ret = rvgpu_pr_read(p, &r, 1, uhdr.size, COMMAND);
		if (ret != uhdr.size)
			errx(1, "Too short read(%zu < %u)", ret, uhdr.size);

		if (uhdr.flags & RVGPU_CURSOR)
			sane = sanity_check_gpu_cursor(&r, uhdr.size, false);
		else
			sane = sanity_check_gpu_ctrl(&r, uhdr.size, false);

		if (sane != VIRTIO_GPU_RESP_OK_NODATA)
			errx(1, "insane command issued: %x", (int)r.hdr.type);

		virgl_renderer_force_ctx_0();
		virgl_renderer_poll();
		switch (r.hdr.type) {
		case VIRTIO_GPU_CMD_CTX_CREATE:
			virgl_renderer_context_create(r.hdr.ctx_id,
						      r.c_create.nlen,
						      r.c_create.debug_name);
			break;
		case VIRTIO_GPU_CMD_CTX_DESTROY:
			virgl_renderer_context_destroy(r.hdr.ctx_id);
			break;
		case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
			virgl_renderer_resource_create(
				&(struct virgl_renderer_resource_create_args){
					.handle = r.r_c2d.resource_id,
					.target = 2,
					.format = r.r_c2d.format,
					.bind = 2,
					.width = r.r_c2d.width,
					.height = r.r_c2d.height,
					.depth = 1,
					.array_size = 1,
					.flags =
						VIRTIO_GPU_RESOURCE_FLAG_Y_0_TOP,
				},
				NULL, 0);
			break;
		case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
			virgl_renderer_resource_create(
				&(struct virgl_renderer_resource_create_args){
					.handle = r.r_c3d.resource_id,
					.target = r.r_c3d.target,
					.format = r.r_c3d.format,
					.bind = r.r_c3d.bind,
					.width = r.r_c3d.width,
					.height = r.r_c3d.height,
					.depth = r.r_c3d.depth,
					.array_size = r.r_c3d.array_size,
					.last_level = r.r_c3d.last_level,
					.nr_samples = r.r_c3d.nr_samples,
					.flags = r.r_c3d.flags,
				},
				NULL, 0);
			break;
		case VIRTIO_GPU_CMD_SUBMIT_3D:
			virgl_renderer_submit_cmd(r.c_cmdbuf, (int)r.hdr.ctx_id,
						  r.c_submit.size / 4);
			break;
		case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
			if (!load_resource(p, r.t_2h2d.resource_id)) {
				break;
			}
			if (check_rect(r.t_2h2d.resource_id, &r.t_2h2d.r)) {
				virgl_renderer_transfer_write_iov(
					r.t_2h2d.resource_id, 0, 0, 0, 0,
					(struct virgl_box *)&(
						struct virtio_gpu_box){
						.x = r.t_2h2d.r.x,
						.y = r.t_2h2d.r.y,
						.w = r.t_2h2d.r.width,
						.h = r.t_2h2d.r.height,
						.d = 1 },
					r.t_2h2d.offset, NULL, 0);
			} else {
				errx(1, "Invalid rectangle transfer");
			}
			break;
		case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
			if (!load_resource(p, r.t_h3d.resource_id)) {
				break;
			}
			if (check_box(r.t_h3d.resource_id, &r.t_h3d.box)) {
				virgl_renderer_transfer_write_iov(
					r.t_h3d.resource_id, r.hdr.ctx_id,
					(int)r.t_h3d.level, r.t_h3d.stride,
					r.t_h3d.layer_stride,
					(struct virgl_box *)&r.t_h3d.box,
					r.t_h3d.offset, NULL, 0);
			} else {
				errx(1, "Invalid box transfer");
			}
			break;
		case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
			if (check_box(r.t_h3d.resource_id, &r.t_h3d.box)) {
				virgl_renderer_transfer_read_iov(
					r.t_h3d.resource_id, r.hdr.ctx_id,
					r.t_h3d.level, r.t_h3d.stride,
					r.t_h3d.layer_stride,
					(struct virgl_box *)&r.t_h3d.box,
					r.t_h3d.offset, NULL, 0);
			} else {
				errx(1, "Invalid box transfer");
			}
			break;
		case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
			resource_attach_backing(&r.r_att, r.r_mem);
			break;
		case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
			virgl_renderer_resource_detach_iov(r.r_det.resource_id,
							   &piov, &n);
			if (piov != NULL && n > 0) {
				free(piov[0].iov_base);
				free(piov);
			}
			break;
		case VIRTIO_GPU_CMD_SET_SCANOUT: {
			struct rvgpu_scanout *s =
				&p->egl->scanouts[r.s_set.scanout_id];
			if (s->params.enabled)
				set_scanout(p, &r.s_set, s);
			break;
		}
		case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
			/* Call draw function if it's for scanout */
			draw = r.r_flush.resource_id;
			break;
		case VIRTIO_GPU_CMD_RESOURCE_UNREF:
			virgl_renderer_resource_detach_iov(
				r.r_unref.resource_id, &piov, &n);
			if (piov != NULL && n > 0) {
				free(piov[0].iov_base);
				free(piov);
			}
			virgl_renderer_resource_unref(r.r_unref.resource_id);
			break;
		case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
			virgl_renderer_ctx_attach_resource(r.hdr.ctx_id,
							   r.c_res.resource_id);
			break;
		case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
			virgl_renderer_ctx_detach_resource(r.hdr.ctx_id,
							   r.c_res.resource_id);
			break;
		case VIRTIO_GPU_CMD_GET_CAPSET:
		case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
		case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
			/* ignore command */
			break;

		case VIRTIO_GPU_CMD_UPDATE_CURSOR:
			if (!p->egl->spawn_support)
				break;
			draw = rvgpu_serve_vscanout(p, p->egl, r.cursor.hot_x,
						    r.cursor.hot_y,
						    r.cursor.resource_id);
			break;
		case VIRTIO_GPU_CMD_MOVE_CURSOR:
			/* TODO: handle cursor */
			break;
		default:
			warnx("Unknown command %d", r.hdr.type);
			return 0;
		}

		if (r.hdr.flags & VIRTIO_GPU_FLAG_FENCE) {
			uint32_t hdr_type = r.hdr.type;
			uint64_t hdr_fence_id = r.hdr.fence_id;

			ret = virgl_renderer_create_fence(hdr_fence_id,
							  hdr_type);

			if (ret != 0) {
				fprintf(stderr, "%s(): err create fence: %s\n",
					__func__, strerror(ret));
			} else {
				if (hdr_fence_id > p->fence_received)
					p->fence_received = hdr_fence_id;
				virgl_renderer_poll();
			}
		}

		if (draw)
			return draw;
	}
	return 0;
}
