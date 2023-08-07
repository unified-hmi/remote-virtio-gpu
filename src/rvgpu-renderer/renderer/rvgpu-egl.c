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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <assert.h>
#include <err.h>
#include <stdlib.h>

#include <rvgpu-renderer/renderer/rvgpu-egl.h>

struct rect {
	int x;
	int y;
	int width;
	int height;
};

void rvgpu_egl_init_context(struct rvgpu_egl_state *e)
{
	EGLint config_attribs[] = { EGL_SURFACE_TYPE,
				    EGL_WINDOW_BIT,
				    EGL_RED_SIZE,
				    8,
				    EGL_GREEN_SIZE,
				    8,
				    EGL_BLUE_SIZE,
				    8,
				    EGL_CONFORMANT,
				    EGL_OPENGL_ES2_BIT,
				    EGL_RENDERABLE_TYPE,
				    EGL_OPENGL_ES2_BIT,
				    EGL_NONE };
	EGLint ctxattr[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
			     EGL_CONTEXT_MINOR_VERSION_KHR, 0, EGL_NONE };

	EGLint n = 0;
	EGLBoolean res;
	EGLConfig *configs;

	res = eglInitialize(e->dpy, NULL, NULL);
	assert(res);
	(void)res;

	eglBindAPI(EGL_OPENGL_ES_API);

	eglChooseConfig(e->dpy, config_attribs, NULL, 0, &n);
	assert(n > 0);

	configs = calloc(n, sizeof(EGLConfig));
	assert(configs);

	eglChooseConfig(e->dpy, config_attribs, configs, n, &n);
	assert(n > 0);

	if (e->use_native_format) {
		int config_index;
		for (config_index = 0; config_index < n; config_index++) {
			EGLint attr;

			eglGetConfigAttrib(e->dpy, configs[config_index],
			                   EGL_NATIVE_VISUAL_ID, &attr);
			if ((uint32_t)attr == e->native_format)
				break;
		}
		if (config_index == n)
			err(1, "native format %d is not supported by EGL",
			    e->native_format);

		e->config = configs[config_index];
	} else {
		e->config = configs[0];
	}

	free(configs);

	e->context =
		eglCreateContext(e->dpy, e->config, EGL_NO_CONTEXT, ctxattr);
	assert(e->context);

	LIST_INIT(&e->vscanouts);
}

void *rvgpu_egl_create_context(struct rvgpu_egl_state *e, int major, int minor,
			       int shared)
{
	EGLint ctxattr[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, major,
			     EGL_CONTEXT_MINOR_VERSION_KHR, minor, EGL_NONE };
	return eglCreateContext(e->dpy, e->config,
				shared ? eglGetCurrentContext() : e->context,
				ctxattr);
}

void rvgpu_egl_destroy_context(struct rvgpu_egl_state *e, void *ctx)
{
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(e->dpy, ctx);
}

int rvgpu_egl_make_context_current(struct rvgpu_egl_state *e, void *ctx)
{
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
	return 0;
}

void rvgpu_egl_set_scanout(struct rvgpu_egl_state *e, struct rvgpu_scanout *s,
			   const struct rvgpu_virgl_params *sp)
{
	s->virgl = *sp;
	if (e->cb->set_scanout)
		e->cb->set_scanout(e, s);
}

void rvgpu_egl_create_scanout(struct rvgpu_egl_state *e,
			      struct rvgpu_scanout *s)
{
	assert(e->cb->create_scanout);
	e->cb->create_scanout(e, s);
}

void rvgpu_egl_destroy_scanout(struct rvgpu_egl_state *e,
			       struct rvgpu_scanout *s)
{
	if (s->surface) {
		eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE,
			       e->context);
		eglDestroySurface(e->dpy, s->surface);
		glDeleteFramebuffers(1, &s->fb);
	}

	if ((e->cb->destroy_scanout))
		e->cb->destroy_scanout(e, s);
}

void rvgpu_egl_free(struct rvgpu_egl_state *e)
{
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(e->dpy, e->context);
	for (unsigned int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
		rvgpu_egl_destroy_scanout(e, &e->scanouts[i]);

	rvgpu_destroy_all_vscanouts(e);
	eglTerminate(e->dpy);
	if (e->cb->free)
		e->cb->free(e);
}

void rvgpu_egl_draw(struct rvgpu_egl_state *e, struct rvgpu_scanout *s,
		    bool vsync)
{
	if (!s->native)
		rvgpu_egl_create_scanout(e, s);

	if (!s->native)
		return;

	eglMakeCurrent(e->dpy, s->surface, s->surface, e->context);

	eglSwapInterval(e->dpy, vsync ? 1 : 0);
	glViewport(0, 0, (int)s->window.w, (int)s->window.h);

	if (e->params == NULL) {
		glClearColor(0.0f, 1.0f, 0.0f, 0.2f);
	} else {
		glClearColor(((e->params->clear_color >> 24) & 0xFF) / 255.0f,
			     ((e->params->clear_color >> 16) & 0xFF) / 255.0f,
			     ((e->params->clear_color >> 8) & 0xFF) / 255.0f,
			     ((e->params->clear_color >> 0) & 0xFF) / 255.0f);
	}
	glClear(GL_COLOR_BUFFER_BIT);

	if (s->virgl.tex_id != 0) {
		const struct rvgpu_box *vbox = &s->virgl.box;
		unsigned int y1, y2;

		if (s->virgl.y0_top) {
			y1 = vbox->y;
			y2 = vbox->y + vbox->h;
		} else {
			y1 = vbox->y + vbox->h;
			y2 = vbox->y;
		}

		glBindFramebuffer(GL_FRAMEBUFFER, s->fb);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				       GL_TEXTURE_2D, s->virgl.tex_id, 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		glBlitFramebuffer((int)vbox->x, (int)y1,
				  (int)(vbox->x + vbox->w), (int)y2, 0, 0,
				  (int)s->window.w, (int)s->window.h,
				  GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}
	if (e->cb->draw)
		e->cb->draw(e, s, vsync);
	else
		eglSwapBuffers(e->dpy, s->surface);
}

void rvgpu_egl_drawall(struct rvgpu_egl_state *e, unsigned int res_id,
		       bool vsync)
{
	struct rvgpu_scanout *last = NULL;
	struct rvgpu_scanout *vs = NULL;

	for (unsigned int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
		struct rvgpu_scanout *s = &e->scanouts[i];

		if (s->virgl.res_id == res_id)
			last = s;
	}
	LIST_FOREACH(vs, &e->vscanouts, rvgpu_scanout_node)
	{
		if (vs->virgl.res_id == res_id)
			last = vs;
	}

	if (last == NULL)
		return;

	for (unsigned int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
		struct rvgpu_scanout *s = &e->scanouts[i];

		if (s->virgl.res_id == res_id)
			rvgpu_egl_draw(e, s, (s == last) ? vsync : false);
	}
	LIST_FOREACH(vs, &e->vscanouts, rvgpu_scanout_node)
	{
		if (vs->virgl.res_id == res_id)
			rvgpu_egl_draw(e, vs, (vs == last) ? vsync : false);
	}
}

/** Call before polling */
size_t rvgpu_egl_prepare_events(struct rvgpu_egl_state *e, void *ev, size_t max)
{
	if (e->cb->prepare_events)
		return e->cb->prepare_events(e, ev, max);

	return 0;
}

/** Call after polling */
void rvgpu_egl_process_events(struct rvgpu_egl_state *e, void *ev, size_t n)
{
	if (e->cb->process_events)
		e->cb->process_events(e, ev, n);
}

struct rvgpu_scanout *rvgpu_get_vscanout(struct rvgpu_egl_state *e,
					 unsigned int scanout_id)
{
	struct rvgpu_scanout *s = NULL;

	LIST_FOREACH(s, &e->vscanouts, rvgpu_scanout_node)
	{
		if (s->scanout_id == scanout_id)
			return s;
	}
	return NULL;
}

struct rvgpu_scanout *rvgpu_create_vscanout(struct rvgpu_egl_state *e,
					    unsigned int scanout_id)
{
	struct rvgpu_scanout *s = rvgpu_get_vscanout(e, scanout_id);

	if (s != NULL)
		return s;

	s = calloc(1, sizeof(*s));
	assert(s);
	s->scanout_id = scanout_id;
	s->params.id = scanout_id;
	s->params.enabled = true;
	LIST_INSERT_HEAD(&e->vscanouts, s, rvgpu_scanout_node);
	rvgpu_egl_create_scanout(e, s);
	return s;
}

void rvgpu_destroy_vscanout(struct rvgpu_egl_state *e, struct rvgpu_scanout *s)
{
	rvgpu_egl_destroy_scanout(e, s);
	LIST_REMOVE(s, rvgpu_scanout_node);
	free(s);
}

void rvgpu_destroy_all_vscanouts(struct rvgpu_egl_state *e)
{
	while (!LIST_EMPTY(&e->vscanouts)) {
		struct rvgpu_scanout *s = LIST_FIRST(&e->vscanouts);

		rvgpu_destroy_vscanout(e, s);
	}
}
