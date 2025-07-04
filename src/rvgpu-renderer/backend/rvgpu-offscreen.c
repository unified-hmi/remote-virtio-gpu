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
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>

#include <linux/input-event-codes.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <librvgpu/rvgpu-protocol.h>

#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/renderer/rvgpu-input.h>
#include <rvgpu-renderer/compositor/rvgpu-compositor.h>

extern platform_funcs_t pf_funcs;

static void rvgpu_compositor_offscreen_set_scanout(struct rvgpu_egl_state *e,
						   struct rvgpu_scanout *s)
{
	(void)e;
	s->window = s->virgl.box;
}

static void rvgpu_compositor_offscreen_create_scanout(struct rvgpu_egl_state *e,
						      struct rvgpu_scanout *s)
{
	s->buf_state = (struct rvgpu_buffer_state *)calloc(
		1, sizeof(struct rvgpu_buffer_state));
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, e->context);
	glGenFramebuffers(1, &s->fb);

	//for dma buffer export
	for (int i = 0; i < 2; i++) {
		glGenFramebuffers(1, &s->dma_fb[i]);
		glGenTextures(1, &s->dma_tex[i]);
		glBindTexture(GL_TEXTURE_2D, s->dma_tex[i]);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
				GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
				GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
				GL_CLAMP_TO_EDGE);

		s->buf_state->eglImages[i] = EGL_NO_IMAGE_KHR;
		s->buf_state->shared_buffer_handles[i] = NULL;
		s->buf_state->width[i] = 0;
		s->buf_state->height[i] = 0;
	}
	s->buf_state->shared_buffer_fd_index = 0;

	//for shm mem
	glGenBuffers(1, &s->shm_pb);

	glBindTexture(GL_TEXTURE_2D, 0);
}

static void
rvgpu_compositor_offscreen_destroy_scanout(struct rvgpu_egl_state *e,
					   struct rvgpu_scanout *s)
{
	for (int i = 0; i < 2; i++) {
		glDeleteFramebuffers(1, &s->dma_fb[i]);
		if (s->buf_state->shared_buffer_handles[i] != NULL) {
			if (e->hardware_buffer_enabled) {
				destroy_hardware_buffer(
					s->buf_state->shared_buffer_handles[i],
					&pf_funcs);
			} else {
				char shm_name[256];
				snprintf(shm_name, sizeof(shm_name),
					 "shm_name_%d_%s_%d", i,
					 e->rvgpu_surface_id,
					 e->scanouts[i].scanout_id);
				destroy_shared_buffer(
					s->buf_state->shared_buffer_handles[i],
					shm_name, &pf_funcs);
			}
		}
	}
	glDeleteFramebuffers(1, &s->shm_pb);
	free(s->buf_state);
}

static void rvgpu_compositor_offscreen_free(struct rvgpu_egl_state *e)
{
	for (unsigned int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
		rvgpu_egl_destroy_scanout(e, &e->scanouts[i]);

	rvgpu_destroy_all_vscanouts(e);
	rvgpu_glsyncobjs_state_free(e->glsyncobjs_state);
}

const struct rvgpu_egl_callbacks rvgpu_offscreen_callbacks = {
	.prepare_events = NULL,
	.process_events = NULL,
	.free = rvgpu_compositor_offscreen_free,
	.set_scanout = rvgpu_compositor_offscreen_set_scanout,
	.create_scanout = rvgpu_compositor_offscreen_create_scanout,
	.destroy_scanout = rvgpu_compositor_offscreen_destroy_scanout,
};

struct rvgpu_egl_state *rvgpu_offscreen_init(void *offscreen_display)
{
	struct rvgpu_egl_state *egl = (struct rvgpu_egl_state *)calloc(
		1, sizeof(struct rvgpu_egl_state));
	egl->dpy = eglGetDisplay((EGLNativeDisplayType)offscreen_display);
	rvgpu_egl_init_context(egl);
	egl->glsyncobjs_state = (struct rvgpu_glsyncobjs_state *)calloc(
		1, sizeof(struct rvgpu_glsyncobjs_state));
	rvgpu_init_glsyncobjs_state(egl->glsyncobjs_state, egl->context);
	egl->cb = &rvgpu_offscreen_callbacks;
	eglMakeCurrent(egl->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl->context);
	LIST_INIT(&egl->vscanouts);
	return egl;
}
