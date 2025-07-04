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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/time.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm/drm_fourcc.h>

#include <jansson.h>

#include <sys/mman.h>

#include <rvgpu-utils/rvgpu-utils.h>
#include <rvgpu-renderer/rvgpu-renderer.h>
#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/compositor/rvgpu-connection.h>
#include <rvgpu-renderer/compositor/rvgpu-compositor.h>
#include <rvgpu-renderer/compositor/rvgpu-buffer-fd.h>

extern PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
extern PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
	glEGLImageTargetRenderbufferStorageOES;
extern PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
extern PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

extern platform_funcs_t pf_funcs;

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
	GLsync *glsyncobjs = NULL;
	if (glsyncobjs_state->cnt >= glsyncobjs_state->size) {
		glsyncobjs_state->size *= 2; // Double the size for efficiency
		GLsync *glsyncobjs = (GLsync *)realloc(
			glsyncobjs_state->glsyncobjs,
			glsyncobjs_state->size * sizeof(GLsync));
		assert(glsyncobjs);
		glsyncobjs_state->glsyncobjs = glsyncobjs;
	}
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
	GLsync *tmp_glsyncobjs =
		(GLsync *)calloc(glsyncobjs_state->size, sizeof(GLsync));
	assert(tmp_glsyncobjs);
	void **tmp_ctxs =
		(void **)calloc(glsyncobjs_state->size, sizeof(void *));
	assert(tmp_ctxs);

	int j = 0;
	size_t glsyncobjs_cnt = glsyncobjs_state->cnt;
	for (size_t i = 0; i < glsyncobjs_cnt; ++i) {
		if (glsyncobjs_state->ctxs[i] == ctx) {
			glDeleteSync(glsyncobjs_state->glsyncobjs[i]);

			glsyncobjs_state->cnt--;
			continue; // Skip this context
		}
		tmp_glsyncobjs[j] = glsyncobjs_state->glsyncobjs[i];
		tmp_ctxs[j] = glsyncobjs_state->ctxs[i];
		j++;
	}

	free(glsyncobjs_state->glsyncobjs);
	free(glsyncobjs_state->ctxs);
	glsyncobjs_state->cnt = j; // Update count after deletion
	glsyncobjs_state->glsyncobjs = tmp_glsyncobjs;
	glsyncobjs_state->ctxs = tmp_ctxs;

	glsyncobjs_state->size--;
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
	rvgpu_increment_glsyncobjs_size(glsyncobjs_state);
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
				    EGL_DEPTH_SIZE,
				    24,
				    EGL_STENCIL_SIZE,
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

#if 0
	res = eglBindAPI(EGL_OPENGL_API);
	if (!res) {
		res = eglBindAPI(EGL_OPENGL_ES_API);
	}
#else
	res = eglBindAPI(EGL_OPENGL_ES_API);
#endif
	assert(res);
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
	if (e->cb && e->cb->set_scanout)
		e->cb->set_scanout(e, s);
}

void rvgpu_egl_create_scanout(struct rvgpu_egl_state *e,
			      struct rvgpu_scanout *s)
{
	if (e->cb && e->cb->create_scanout)
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
	if (s->fps_params.fps_dump_fp != NULL) {
		fclose(s->fps_params.fps_dump_fp);
	}
	if (e->cb && e->cb->destroy_scanout)
		e->cb->destroy_scanout(e, s);
}

void rvgpu_egl_free(struct rvgpu_egl_state *e)
{
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	eglDestroyContext(e->dpy, e->context);

	if (e->cb && e->cb->free)
		e->cb->free(e);
	eglTerminate(e->dpy);
}

void rvgpu_egl_draw(struct rvgpu_egl_state *e, struct rvgpu_scanout *s,
		    bool vsync)
{
	double virgl_fence_laptime = 0, swap_laptime = 0;
	double rvgpu_interval_ms = 0, virgl_fence_time_ms = 0,
	       draw_swap_time_ms = 0;
	if (s->fps_params.show_fps) {
		rvgpu_interval_ms =
			current_get_time_ms() - s->fps_params.rvgpu_laptime_ms;
		virgl_fence_laptime = current_get_time_ms();
	}
	rvgpu_set_glsyncobj(e->glsyncobjs_state);
	eglMakeCurrent(e->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, e->context);
	e->glsyncobjs_state->current_ctx = e->context;
	rvgpu_set_wait_glsyncobjs(e->glsyncobjs_state);
	if (s->fps_params.show_fps) {
		virgl_fence_time_ms =
			current_get_time_ms() - virgl_fence_laptime;
		swap_laptime = current_get_time_ms();
	}
	eglSwapInterval(e->dpy, vsync ? 1 : 0);

	EGLImageKHR *eglImages = s->buf_state->eglImages;
	void **shared_buffer_handles = s->buf_state->shared_buffer_handles;
	uint32_t *width = s->buf_state->width;
	uint32_t *height = s->buf_state->height;

	json_t *json_obj = json_object();
	char *json_str;

	bool initialColor = false;
	GLuint rvgpu_tex_id = 0;
	GLuint rvgpu_fb = 0;

	if (!e->has_submit_3d_draw) {
		GLuint background_tex_id;
		GLuint background_fb;
		glGenFramebuffers(1, &background_fb);
		glBindFramebuffer(GL_FRAMEBUFFER, background_fb);
		glGenTextures(1, &background_tex_id);
		glBindTexture(GL_TEXTURE_2D, background_tex_id);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
			     GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
				GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
				GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
				GL_CLAMP_TO_EDGE);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				       GL_TEXTURE_2D, background_tex_id, 0);
		glClearColor(
			((e->egl_params.clear_color >> 24) & 0xFF) / 255.0f,
			((e->egl_params.clear_color >> 16) & 0xFF) / 255.0f,
			((e->egl_params.clear_color >> 8) & 0xFF) / 255.0f,
			((e->egl_params.clear_color >> 0) & 0xFF) / 255.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		rvgpu_tex_id = background_tex_id;
		rvgpu_fb = background_fb;
		s->window.w = 1;
		s->window.h = 1;
		initialColor = true;
	} else if (s->virgl.tex_id != 0) {
		rvgpu_tex_id = s->virgl.tex_id;
		rvgpu_fb = s->fb;
	}
	glBindTexture(GL_TEXTURE_2D, rvgpu_tex_id);
	glBindFramebuffer(GL_FRAMEBUFFER, rvgpu_fb);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			       GL_TEXTURE_2D, rvgpu_tex_id, 0);

	bool isNeedUpdateFD = false;
	if (width[s->buf_state->shared_buffer_fd_index] !=
	    (uint32_t)s->window.w) {
		width[s->buf_state->shared_buffer_fd_index] =
			(uint32_t)s->window.w;
		isNeedUpdateFD = true;
	}
	if (height[s->buf_state->shared_buffer_fd_index] !=
	    (uint32_t)s->window.h) {
		height[s->buf_state->shared_buffer_fd_index] =
			(uint32_t)s->window.h;
		isNeedUpdateFD = true;
	}
	if (shared_buffer_handles[s->buf_state->shared_buffer_fd_index] ==
	    NULL) {
		isNeedUpdateFD = true;
	}
	json_object_set_new(json_obj, "event_id",
			    json_integer(RVGPU_DRAW_EVENT_ID));
	json_object_set_new(
		json_obj, "width",
		json_integer(width[s->buf_state->shared_buffer_fd_index]));
	json_object_set_new(
		json_obj, "height",
		json_integer(height[s->buf_state->shared_buffer_fd_index]));
	json_object_set_new(json_obj, "shared_buffer_fd_index",
			    json_integer(s->buf_state->shared_buffer_fd_index));
	json_object_set_new(json_obj, "need_update_fd",
			    json_integer(isNeedUpdateFD));
	json_object_set_new(json_obj, "initial_color",
			    json_integer(initialColor));
	json_object_set_new(json_obj, "scanout_id",
			    json_integer(s->scanout_id));
	json_str = json_dumps(json_obj, JSON_ENCODE_ANY);
	json_decref(json_obj);

	if (rvgpu_tex_id != 0) {
		if (isNeedUpdateFD) {
			if (e->hardware_buffer_enabled) {
				if (shared_buffer_handles
					    [s->buf_state
						     ->shared_buffer_fd_index] !=
				    NULL) {
					destroy_hardware_buffer(
						shared_buffer_handles
							[s->buf_state
								 ->shared_buffer_fd_index],
						&pf_funcs);
				}
				void *hardware_buffer_handle = create_hardware_buffer(
					width[s->buf_state
						      ->shared_buffer_fd_index],
					height[s->buf_state
						       ->shared_buffer_fd_index],
					&pf_funcs);
				shared_buffer_handles
					[s->buf_state->shared_buffer_fd_index] =
						hardware_buffer_handle;
			} else {
				char shm_name[256];
				snprintf(shm_name, sizeof(shm_name),
					 "shm_name_%d_%s_%d",
					 s->buf_state->shared_buffer_fd_index,
					 e->rvgpu_surface_id, s->scanout_id);
				if (shared_buffer_handles
					    [s->buf_state
						     ->shared_buffer_fd_index] !=
				    NULL) {
					destroy_shared_buffer(
						shared_buffer_handles
							[s->buf_state
								 ->shared_buffer_fd_index],
						shm_name, &pf_funcs);
				}
				shared_buffer_handles[s->buf_state
							      ->shared_buffer_fd_index] =
					create_shared_buffer(
						shm_name,
						width[s->buf_state
							      ->shared_buffer_fd_index],
						height[s->buf_state
							       ->shared_buffer_fd_index],
						&pf_funcs);
			}

			if (shared_buffer_handles
				    [s->buf_state->shared_buffer_fd_index] ==
			    NULL) {
				printf("child render cannot get shared buffer fds\n");
				return;
			}
		}

		if (e->hardware_buffer_enabled) {
			//use EGL Extension dma buffer import
			if (isNeedUpdateFD) {
				if (eglImages[s->buf_state
						      ->shared_buffer_fd_index] !=
				    EGL_NO_IMAGE_KHR) {
					eglDestroyImageKHR(
						e->dpy,
						eglImages[s->buf_state
								  ->shared_buffer_fd_index]);
					eglImages[s->buf_state
							  ->shared_buffer_fd_index] =
						EGL_NO_IMAGE_KHR;
				}
				printf("create egl Image: %d\n",
				       s->buf_state->shared_buffer_fd_index);
				eglImages[s->buf_state->shared_buffer_fd_index] = create_egl_image(
					e->dpy,
					width[s->buf_state
						      ->shared_buffer_fd_index],
					height[s->buf_state
						       ->shared_buffer_fd_index],
					shared_buffer_handles
						[s->buf_state
							 ->shared_buffer_fd_index],
					&pf_funcs);
				glBindTexture(
					GL_TEXTURE_2D,
					s->dma_tex
						[s->buf_state
							 ->shared_buffer_fd_index]);
				glEGLImageTargetTexture2DOES(
					GL_TEXTURE_2D,
					eglImages[s->buf_state
							  ->shared_buffer_fd_index]);
				glBindFramebuffer(
					GL_FRAMEBUFFER,
					s->dma_fb[s->buf_state
							  ->shared_buffer_fd_index]);
				glFramebufferTexture2D(
					GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					GL_TEXTURE_2D,
					s->dma_tex
						[s->buf_state
							 ->shared_buffer_fd_index],
					0);
			}
			glBindTexture(GL_TEXTURE_2D, rvgpu_tex_id);
			glBindFramebuffer(GL_FRAMEBUFFER, rvgpu_fb);
			glFramebufferTexture2D(GL_FRAMEBUFFER,
					       GL_COLOR_ATTACHMENT0,
					       GL_TEXTURE_2D, rvgpu_tex_id, 0);

			glBindFramebuffer(
				GL_DRAW_FRAMEBUFFER,
				s->dma_fb[s->buf_state->shared_buffer_fd_index]);
			glBlitFramebuffer(
				0, 0,
				width[s->buf_state->shared_buffer_fd_index],
				height[s->buf_state->shared_buffer_fd_index], 0,
				0, width[s->buf_state->shared_buffer_fd_index],
				height[s->buf_state->shared_buffer_fd_index],
				GL_COLOR_BUFFER_BIT, GL_NEAREST);
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
		} else {
			//use PBO without EGL Extension dma buffer import
			static void *shmBufferPtr = NULL;
			static void *pboPtr = NULL;
			static size_t bufferSize = 0;
			bufferSize =
				width[s->buf_state->shared_buffer_fd_index] *
				height[s->buf_state->shared_buffer_fd_index] *
				4;

			glBindBuffer(GL_PIXEL_PACK_BUFFER, s->shm_pb);
			glBufferData(GL_PIXEL_PACK_BUFFER, bufferSize, NULL,
				     GL_STREAM_READ);
			glBindFramebuffer(GL_FRAMEBUFFER, rvgpu_fb);
			glFramebufferTexture2D(GL_FRAMEBUFFER,
					       GL_COLOR_ATTACHMENT0,
					       GL_TEXTURE_2D, rvgpu_tex_id, 0);

			glReadBuffer(GL_COLOR_ATTACHMENT0);
			glReadPixels(
				0, 0,
				width[s->buf_state->shared_buffer_fd_index],
				height[s->buf_state->shared_buffer_fd_index],
				GL_RGBA, GL_UNSIGNED_BYTE, 0);

			pboPtr = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0,
						  bufferSize, GL_MAP_READ_BIT);
			if (pboPtr) {
				shmBufferPtr = mmap(
					NULL, bufferSize,
					PROT_READ | PROT_WRITE, MAP_SHARED,
					(int)(uintptr_t)shared_buffer_handles
						[s->buf_state
							 ->shared_buffer_fd_index],
					0);
				if (shmBufferPtr != MAP_FAILED) {
					memcpy(shmBufferPtr, pboPtr,
					       bufferSize);
				}
				glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
				munmap(shmBufferPtr, bufferSize);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		}

		//wait texture update
		GLsync sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
		GLenum waitReturn = glClientWaitSync(
			sync, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
		if (waitReturn != GL_ALREADY_SIGNALED &&
		    waitReturn != GL_CONDITION_SATISFIED) {
			fprintf(stderr, "Failed to wait for sync object\n");
			glDeleteSync(sync);
			return;
		}
		glDeleteSync(sync);
#if 0
		//send command
		printf("send_str_with_size write: %s\n", json_str);
#endif
		send_str_with_size(e->server_rvgpu_fd, json_str);
		if (isNeedUpdateFD) {
			send_buffer_handle(
				e->server_rvgpu_fd,
				shared_buffer_handles
					[s->buf_state->shared_buffer_fd_index],
				&pf_funcs);
		}

		if (s->fps_params.show_fps) {
			draw_swap_time_ms =
				current_get_time_ms() - swap_laptime;
			if (s->fps_params.fps_dump_fp == NULL) {
				char fps_dump_file[256];
				snprintf(fps_dump_file, sizeof(fps_dump_file),
					 "%s.%s_%d",
					 s->fps_params.fps_dump_path,
					 e->rvgpu_surface_id, s->scanout_id);
				s->fps_params.fps_dump_fp =
					fopen(fps_dump_file, "w");
				if (s->fps_params.fps_dump_fp == NULL) {
					err(1, "cannot open fps dump file");
					s->fps_params.show_fps = false;
				}
			}
			if (s->fps_params.fps_dump_fp != NULL) {
				double frame_time_ms = rvgpu_interval_ms +
						       virgl_fence_time_ms +
						       draw_swap_time_ms;
				double others_ms =
					frame_time_ms -
					s->fps_params.virgl_cmd_time_ms -
					virgl_fence_time_ms - draw_swap_time_ms;
				struct timeval tv;
				gettimeofday(&tv, NULL);
				double fps = 1000 / frame_time_ms;
				if (s->fps_params.swap_cnt == 0)
					fprintf(s->fps_params.fps_dump_fp,
						"Date FrameTime(ms) VirglTime(ms) FenceTime(ms) SwapTime(ms) Others(ms) FPS\n");
				fprintf(s->fps_params.fps_dump_fp,
					"%ld.%03ld %.3f %.3f %.3f %.3f %.3f %.3f\n",
					(long)tv.tv_sec,
					(long)tv.tv_usec / 1000, frame_time_ms,
					s->fps_params.virgl_cmd_time_ms,
					virgl_fence_time_ms, draw_swap_time_ms,
					others_ms, fps);
				s->fps_params.virgl_cmd_time_ms = 0;
			}
			s->fps_params.swap_cnt++;
		}

		s->buf_state->shared_buffer_fd_index =
			(s->buf_state->shared_buffer_fd_index + 1) % 2;
	}
	s->fps_params.rvgpu_laptime_ms = current_get_time_ms();
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
		if (s->virgl.res_id == res_id && e->has_submit_3d_draw)
			rvgpu_egl_draw(e, s, (s == last) ? vsync : false);
	}
	LIST_FOREACH(vs, &e->vscanouts, rvgpu_scanout_node)
	{
		if (vs->virgl.res_id == res_id && e->has_submit_3d_draw)
			rvgpu_egl_draw(e, vs, (vs == last) ? vsync : false);
	}
}

/** Call before polling */
size_t rvgpu_egl_prepare_events(struct rvgpu_egl_state *e, void *ev, size_t max)
{
	if (e->cb && e->cb->prepare_events)
		return e->cb->prepare_events(e, ev, max);

	return 0;
}

/** Call after polling */
void rvgpu_egl_process_events(struct rvgpu_egl_state *e, void *ev, size_t n)
{
	if (e->cb && e->cb->process_events)
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
