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

#ifndef RVGPU_EGL_H
#define RVGPU_EGL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/poll.h>
#include <sys/queue.h>
#include <pthread.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <linux/virtio_gpu.h>

#include <jansson.h>

#define SHM_MUTEX "/shm_mutex"

#define EGL_GET_PROC_ADDR(name)                                                \
	do {                                                                   \
		name = (void *)eglGetProcAddress(#name);                       \
		if (!name) {                                                   \
			fprintf(stderr, "%s\n", __FUNCTION__);                 \
		}                                                              \
	} while (0)

struct pollfd;

/**
 * @brief Additional parameters for EGL state
 */
struct rvgpu_egl_params {
	unsigned int clear_color; /**< Color of empty screen */
};

struct rvgpu_buffer_state {
	int shared_buffer_fd_index;
	EGLImageKHR eglImages[2];
	void *shared_buffer_handles[2];
	uint32_t width[2];
	uint32_t height[2];
};

struct rvgpu_fps_params {
	bool show_fps;
	double rvgpu_laptime_ms;
	double virgl_cmd_time_ms;
	int swap_cnt;
	char *fps_dump_path;
	FILE *fps_dump_fp;
};

struct rvgpu_focus_state {
	json_t *touch_focused_json_obj;
	json_t *pointer_focused_json_obj;
	json_t *keyboard_focused_json_obj;
	double pre_pointer_pos_x;
	double pre_pointer_pos_y;
	pthread_mutex_t *input_send_event_mutex;
};

struct rvgpu_draw_list_params {
	json_t *rvgpu_surface_list;
	json_t *rvgpu_layout_list;
	pthread_mutex_t *surface_list_mutex;
	pthread_mutex_t *layout_list_mutex;
};

struct rvgpu_box {
	unsigned int x, y;
	unsigned int w, h;
};

struct rvgpu_virgl_params {
	/* virgl scanout dimensionss */
	struct rvgpu_box box;
	uint32_t res_id;
	/* texture params */
	uint32_t tex_id;
	struct rvgpu_box tex;
	int y0_top;
};

struct rvgpu_native;
/**
 * @brief Scanout params from command line
 */
struct rvgpu_scanout_params {
	struct rvgpu_box box; /**< Overridable box parameters */
	uint32_t id; /**< ID for scanout window (i.e. IVI id)*/
	bool enabled; /**< enable/disable scanout */
	bool boxed; /**< box is set by user */
};

struct rvgpu_scanout {
	struct rvgpu_virgl_params virgl;
	/* EGL surface for scanout */
	EGLSurface surface;
	/* Framebuffer for drawing */
	unsigned int fb;
	unsigned int shm_pb;
	unsigned int dma_fb[2];
	unsigned int dma_tex[2];
	/* Output window parameters */
	struct rvgpu_box window;
	/* Glue to native system */
	struct rvgpu_native *native;
	/* Scanout ID for virtual or regular scanouts */
	unsigned int scanout_id;
	/* Scanout params */
	struct rvgpu_scanout_params params;
	struct rvgpu_buffer_state *buf_state;
	struct rvgpu_fps_params fps_params;
	/* Linked list entry */
	LIST_ENTRY(rvgpu_scanout) rvgpu_scanout_node;
};

struct rvgpu_egl_state;

struct rvgpu_egl_callbacks {
	size_t (*prepare_events)(struct rvgpu_egl_state *e, void *ev,
				 size_t max);
	void (*process_events)(struct rvgpu_egl_state *e, const void *ev,
			       size_t n);
	void (*set_scanout)(struct rvgpu_egl_state *e, struct rvgpu_scanout *s);
	void (*create_scanout)(struct rvgpu_egl_state *e,
			       struct rvgpu_scanout *s);
	void (*destroy_scanout)(struct rvgpu_egl_state *e,
				struct rvgpu_scanout *s);
	void (*draw)(struct rvgpu_egl_state *e, struct rvgpu_scanout *s,
		     bool vsync);
	void (*free)(struct rvgpu_egl_state *e);
	void (*set_cursor)(struct rvgpu_egl_state *e, uint32_t w, uint32_t h,
			   void *data);
	void (*move_cursor)(struct rvgpu_egl_state *e, uint32_t x, uint32_t y);
};

/**
 * @brief glFenceSync object list before eglSwapBuffers
 */
struct rvgpu_glsyncobjs_state {
	void *current_ctx;
	GLsync *glsyncobjs;
	size_t cnt;
	size_t size;
	void **ctxs;
};

struct rvgpu_egl_state {
	/* regular scanouts */
	struct rvgpu_scanout scanouts[VIRTIO_GPU_MAX_SCANOUTS];

	/* virtual scanouts */
	LIST_HEAD(, rvgpu_scanout) vscanouts;

	/* EGL pointers */
	EGLDisplay dpy;
	EGLSurface sfc;
	EGLConfig config;
	EGLContext context;
	char *rvgpu_surface_id;
	int server_rvgpu_fd;
	bool hardware_buffer_enabled;
	/* callbacks */
	const struct rvgpu_egl_callbacks *cb;

	/* additional params */
	struct rvgpu_egl_params egl_params;

	/* backend requires specific native buffer format */
	bool use_native_format;
	uint32_t native_format;

	/* support GPU commands synchronization between rvgpu and virglrenderer */
	struct rvgpu_glsyncobjs_state *glsyncobjs_state;

	bool has_submit_3d_draw;
	struct rvgpu_focus_state focus_state;
	struct rvgpu_draw_list_params *draw_list_params;
};

/** Initialize main context */
void rvgpu_egl_init_context(struct rvgpu_egl_state *e);

/** Create context */
void *rvgpu_egl_create_context(struct rvgpu_egl_state *e, int major, int minor,
			       int shared);

/** Destroy context */
void rvgpu_egl_destroy_context(struct rvgpu_egl_state *e, void *ctx);

/** Make current context */
int rvgpu_egl_make_context_current(struct rvgpu_egl_state *e, void *ctx);

/** Set scanout params */
void rvgpu_egl_set_scanout(struct rvgpu_egl_state *e, struct rvgpu_scanout *s,
			   const struct rvgpu_virgl_params *sp);

/** Create scanout */
void rvgpu_egl_create_scanout(struct rvgpu_egl_state *e,
			      struct rvgpu_scanout *s);

/** Destroy scanout */
void rvgpu_egl_destroy_scanout(struct rvgpu_egl_state *e,
			       struct rvgpu_scanout *s);

/** Draw Virgl output on the surface */
void rvgpu_egl_draw(struct rvgpu_egl_state *e, struct rvgpu_scanout *s,
		    bool vsync);

/** Redraw all scanouts with given resource id */
void rvgpu_egl_drawall(struct rvgpu_egl_state *e, unsigned int res_id,
		       bool vsync);

/* Async event handling */
/** Call before polling */
size_t rvgpu_egl_prepare_events(struct rvgpu_egl_state *e, void *ev,
				size_t max);

/** Call after polling */
void rvgpu_egl_process_events(struct rvgpu_egl_state *e, void *ev, size_t n);

/** Free EGL resources */
void rvgpu_egl_free(struct rvgpu_egl_state *e);

/** Get virtual scanout */
struct rvgpu_scanout *rvgpu_get_vscanout(struct rvgpu_egl_state *e,
					 unsigned int scanout_id);
/** Create new virtual scanout */
struct rvgpu_scanout *rvgpu_create_vscanout(struct rvgpu_egl_state *e,
					    unsigned int scanout_id);

/** Destroy virtual scanout */
void rvgpu_destroy_vscanout(struct rvgpu_egl_state *e, struct rvgpu_scanout *s);

/** Destroy all virtual scanouts */
void rvgpu_destroy_all_vscanouts(struct rvgpu_egl_state *e);

void rvgpu_init_glsyncobjs_state(
	struct rvgpu_glsyncobjs_state *glsyncobjs_state, void *current_ctx);

void rvgpu_glsyncobjs_state_free(
	struct rvgpu_glsyncobjs_state *glsyncobjs_state);

/** Container of macro for callback implementation */
#define rvgpu_container_of(ptr, typ, member)                                   \
	({ (typ *)((char *)ptr - offsetof(typ, member)); })

#endif /* RVGPU_EGL_H */
