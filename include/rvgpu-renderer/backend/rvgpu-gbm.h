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

#ifndef RVGPU_GBM_H
#define RVGPU_GBM_H

#include <xf86drmMode.h>

struct rvgpu_gbm_state {
	/* GBM structures */
	int gbm_fd;
	uint32_t connector;
	drmModeCrtc *crtc;
	drmModeModeInfo mode;

	bool flip_pending;
	bool mode_set;
	struct gbm_device *gbm_device;
	struct gbm_surface *gbm_surface;

	struct gbm_bo *prev_bo;
	unsigned int prev_fb;

	struct gbm_bo *current_bo;
	unsigned int current_fb;

	/* EGL structures */
	struct rvgpu_egl_state egl;

	/* Input handling */
	struct rvgpu_input_state *in;
	struct libinput *libin;
	struct udev *udev;

	/* Cursor */
	uint32_t cursor_w;
	uint32_t cursor_h;
	uint64_t cursor_size;
	void *cursor_map;
	uint32_t cursor_handle;
};

typedef struct {
	const char *device;
	const char *seat;
} rvgpu_gbm_params;

static inline struct rvgpu_gbm_state *to_gbm(struct rvgpu_egl_state *e)
{
	return rvgpu_container_of(e, struct rvgpu_gbm_state, egl);
}

/** Initialize GBM frontend */
void *rvgpu_gbm_init(void *params, int *width, int *height);

void rvgpu_gbm_free(struct rvgpu_egl_state *e);

void *create_gbm_native_display(const char *device);

void destroy_gbm_native_display(void *native_dpy);

void rvgpu_gbm_swap(void *param, bool vsync);

uint32_t get_gbm_format(struct gbm_device *gbm);

#endif /* RVGPU_GBM_H */
