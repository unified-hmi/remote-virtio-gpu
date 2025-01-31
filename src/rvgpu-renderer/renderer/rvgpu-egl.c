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
#include <stdio.h>

#include <rvgpu-generic/rvgpu-utils.h>
#include <rvgpu-renderer/rvgpu-renderer.h>
#include <rvgpu-renderer/renderer/rvgpu-egl.h>

void rvgpu_init_fps_dump(FILE **fps_dump_fp)
{
	char *fps_dump_path = getenv("RVGPU_FPS_DUMP_PATH");
	if (fps_dump_path) {
		*fps_dump_fp = fopen(fps_dump_path, "w");
		if (*fps_dump_fp != NULL) {
			fprintf(*fps_dump_fp,
				"Date FrameTime(ms) VirglTime(ms) FenceTime(ms) SwapTime(ms) Others(ms) FPS\n");
		} else {
			err(1, "cannot open fps dump file");
			fclose(*fps_dump_fp);
		}
	} else {
		info("Environment variable RVGPU_FPS_DUMP_PATH is not set. Please specify a path to dump file\n");
	}
}

struct rect {
	int x;
	int y;
	int width;
	int height;
};

void rvgpu_init_glsyncobjs_state(
	struct rvgpu_glsyncobjs_state *glsyncobjs_state, void *current_ctx)
{
	glsyncobjs_state->current_ctx = current_ctx;
	glsyncobjs_state->cnt = 0;
	glsyncobjs_state->size = 1;
	glsyncobjs_state->glsyncobjs =
		(GLsync *)calloc(glsyncobjs_state->size, sizeof(GLsync));
	assert(glsyncobjs_state->glsyncobjs);
	glsyncobjs_state->ctxs =
		(void **)calloc(glsyncobjs_state->size, sizeof(void *));
	assert(glsyncobjs_state->ctxs);
}

void rvgpu_increment_glsyncobjs_size(
	struct rvgpu_glsyncobjs_state *glsyncobjs_state)
{
	glsyncobjs_state->size++;
	GLsync *glsyncobjs =
		(GLsync *)realloc(glsyncobjs_state->glsyncobjs,
				  glsyncobjs_state->size * sizeof(GLsync));
	assert(glsyncobjs);

	if (glsyncobjs != NULL) {
		glsyncobjs_state->glsyncobjs = glsyncobjs;
	}

	void **ctxs = (void **)realloc(glsyncobjs_state->ctxs,
				       glsyncobjs_state->size * sizeof(void *));
	assert(ctxs);
	if (ctxs != NULL) {
		glsyncobjs_state->ctxs = ctxs;
	}
}

void rvgpu_decrement_glsyncobjs_size(
	struct rvgpu_glsyncobjs_state *glsyncobjs_state, void *ctx)
{
	assert(glsyncobjs_state->size > 0);
	glsyncobjs_state->size--;

	GLsync *tmp_glsyncobjs =
		(GLsync *)calloc(glsyncobjs_state->size, sizeof(GLsync));
	assert(tmp_glsyncobjs);
	void **tmp_ctxs =
		(void **)calloc(glsyncobjs_state->size, sizeof(void *));
	assert(tmp_ctxs);

	int j = 0;
	for (size_t i = 0; i < glsyncobjs_state->cnt; ++i) {
		if (glsyncobjs_state->ctxs[i] == ctx) {
			glDeleteSync(glsyncobjs_state->glsyncobjs[i]);
		} else {
			tmp_glsyncobjs[j] = glsyncobjs_state->glsyncobjs[i];
			tmp_ctxs[j] = glsyncobjs_state->ctxs[i];
			j++;
		}
	}

	for (size_t i = 0; i < glsyncobjs_state->size; ++i) {
		glsyncobjs_state->glsyncobjs[i] = tmp_glsyncobjs[i];
		glsyncobjs_state->ctxs[i] = tmp_ctxs[i];
	}
	free(tmp_glsyncobjs);
	free(tmp_ctxs);

	GLsync *glsyncobjs =
		(GLsync *)realloc(glsyncobjs_state->glsyncobjs,
				  glsyncobjs_state->size * sizeof(GLsync));
	assert(glsyncobjs);
	if (glsyncobjs != NULL) {
		glsyncobjs_state->glsyncobjs = glsyncobjs;
	}

	void **ctxs = (void **)realloc(glsyncobjs_state->ctxs,
				       glsyncobjs_state->size * sizeof(void *));
	assert(ctxs);
	if (ctxs != NULL) {
		glsyncobjs_state->ctxs = ctxs;
	}
}

void rvgpu_set_glsyncobj(struct rvgpu_glsyncobjs_state *glsyncobjs_state)
{
	GLsync glsyncobj = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	for (size_t i = 0; i < glsyncobjs_state->cnt; ++i) {
		if (glsyncobjs_state->ctxs[i] ==
		    glsyncobjs_state->current_ctx) {
			GLsync old_glsyncobj = glsyncobjs_state->glsyncobjs[i];
			glDeleteSync(old_glsyncobj);
			glsyncobjs_state->glsyncobjs[i] = glsyncobj;
			return;
		}
	}

	glsyncobjs_state->glsyncobjs[glsyncobjs_state->cnt] = glsyncobj;
	glsyncobjs_state->ctxs[glsyncobjs_state->cnt] =
		glsyncobjs_state->current_ctx;
	glsyncobjs_state->cnt++;
}

void rvgpu_set_wait_glsyncobjs(struct rvgpu_glsyncobjs_state *glsyncobjs_state)
{
	for (size_t i = 0; i < glsyncobjs_state->cnt; ++i) {
		if (glsyncobjs_state->glsyncobjs[i] &&
		    glsyncobjs_state->ctxs[i]) {
			glWaitSync(glsyncobjs_state->glsyncobjs[i], 0,
				   GL_TIMEOUT_IGNORED);
			glDeleteSync(glsyncobjs_state->glsyncobjs[i]);
			glsyncobjs_state->glsyncobjs[i] = NULL;
			glsyncobjs_state->ctxs[i] = NULL;
		}
	}
	glsyncobjs_state->cnt = 0;
}

void rvgpu_glsyncobjs_state_free(struct rvgpu_glsyncobjs_state *glsyncobjs_state)
{
	free(glsyncobjs_state->glsyncobjs);
	glsyncobjs_state->glsyncobjs = NULL;

	free(glsyncobjs_state->ctxs);
	glsyncobjs_state->ctxs = NULL;

	free(glsyncobjs_state);
}

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
				    EGL_ALPHA_SIZE,
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

	e->glsyncobjs_state = (struct rvgpu_glsyncobjs_state *)calloc(
		1, sizeof(struct rvgpu_glsyncobjs_state));
	assert(e->glsyncobjs_state);
	rvgpu_init_glsyncobjs_state(e->glsyncobjs_state, e->context);
}

void *rvgpu_egl_create_context(struct rvgpu_egl_state *e, int major, int minor,
			       int shared)
{
	rvgpu_increment_glsyncobjs_size(e->glsyncobjs_state);
	EGLint ctxattr[] = { EGL_CONTEXT_MAJOR_VERSION_KHR, major,
			     EGL_CONTEXT_MINOR_VERSION_KHR, minor, EGL_NONE };
	return eglCreateContext(e->dpy, e->config,
				shared ? eglGetCurrentContext() : e->context,
				ctxattr);
}

void rvgpu_egl_destroy_context(struct rvgpu_egl_state *e, void *ctx)
{
	rvgpu_decrement_glsyncobjs_size(e->glsyncobjs_state, ctx);
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(e->dpy, ctx);
}

int rvgpu_egl_make_context_current(struct rvgpu_egl_state *e, void *ctx)
{
	rvgpu_set_glsyncobj(e->glsyncobjs_state);
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx);
	e->glsyncobjs_state->current_ctx = ctx;
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
	if (e->fps_params.fps_dump_fp != NULL) {
		close(e->fps_params.fps_dump_fp);
	}
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(e->dpy, e->context);
	for (unsigned int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++)
		rvgpu_egl_destroy_scanout(e, &e->scanouts[i]);

	rvgpu_destroy_all_vscanouts(e);
	eglTerminate(e->dpy);
	if (e->cb->free)
		e->cb->free(e);

	rvgpu_glsyncobjs_state_free(e->glsyncobjs_state);
}

void rvgpu_egl_draw(struct rvgpu_egl_state *e, struct rvgpu_scanout *s,
		    bool vsync)
{
	double virgl_fence_laptime = 0, swap_laptime = 0;
	double rvgpu_interval_ms = 0, virgl_fence_time_ms = 0,
	       draw_swap_time_ms = 0;
	if (e->fps_params.show_fps) {
		rvgpu_interval_ms =
			current_get_time_ms() - e->fps_params.rvgpu_laptime_ms;
		virgl_fence_laptime = current_get_time_ms();
	}

	if (!s->native)
		rvgpu_egl_create_scanout(e, s);

	if (!s->native)
		return;

	rvgpu_set_glsyncobj(e->glsyncobjs_state);
	eglMakeCurrent(e->dpy, s->surface, s->surface, e->context);
	e->glsyncobjs_state->current_ctx = e->context;
	rvgpu_set_wait_glsyncobjs(e->glsyncobjs_state);
	if (e->fps_params.show_fps) {
		virgl_fence_time_ms =
			current_get_time_ms() - virgl_fence_laptime;
		swap_laptime = current_get_time_ms();
	}
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
		if (e->fps_params.show_fps) {
			e->fps_params.swap_cnt++;
		}
	}

	if (e->cb->draw)
		e->cb->draw(e, s, vsync);
	else
		eglSwapBuffers(e->dpy, s->surface);

	if (e->fps_params.show_fps && s->virgl.tex_id != 0 &&
	    e->fps_params.swap_cnt > 0) {
		draw_swap_time_ms = current_get_time_ms() - swap_laptime;
		if (e->fps_params.fps_dump_fp != NULL) {
			double frame_time_ms = rvgpu_interval_ms +
					       virgl_fence_time_ms +
					       draw_swap_time_ms;
			double others_ms = frame_time_ms -
					   e->fps_params.virgl_cmd_time_ms -
					   virgl_fence_time_ms -
					   draw_swap_time_ms;
			struct timeval tv;
			gettimeofday(&tv, NULL);
			double fps = 1000 / frame_time_ms;
			fprintf(e->fps_params.fps_dump_fp,
				"%ld.%03ld %.3f %.3f %.3f %.3f %.3f %.3f\n",
				(long)tv.tv_sec, (long)tv.tv_usec / 1000,
				frame_time_ms, e->fps_params.virgl_cmd_time_ms,
				virgl_fence_time_ms, draw_swap_time_ms,
				others_ms, fps);
			e->fps_params.virgl_cmd_time_ms = 0;
		}
	}
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
