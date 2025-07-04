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

#ifndef RVGPU_WAYLAND_H
#define RVGPU_WAYLAND_H

#define MAX_OUTPUTS 16

typedef struct {
	uint32_t ivi_surface_id;
	uint32_t output_id;
	bool fullscreen;
	bool translucent;
} rvgpu_wl_params;

struct output_info {
	struct wl_output *output;
	int x, y;
	int physical_width, physical_height;
	int mode_width, mode_height;
	bool mode_known;
	char make[64];
	char model[64];
};

struct output_entry {
	struct wl_output *output;
	struct output_info info;
};

struct rvgpu_native {
	struct rvgpu_wl_state *wl_state;
	bool xdg_wm_base_waiting_for_configure;

	/* Window structures */
	struct wl_surface *surface;
	struct wl_shell_surface *shell_surface;
	struct xdg_surface *xdg_surface;
	struct xdg_toplevel *xdg_toplevel;
	struct wl_egl_window *egl_window;
	struct ivi_surface *ivi_surface;
};

struct rvgpu_wl_state {
	/* Compositor Size */
	uint32_t *width;
	uint32_t *height;

	/* Wayland structures */
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_seat *seat;
	struct wl_touch *touch;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_shell *shell;
	struct xdg_wm_base *wm_base;
	struct ivi_application *ivi_app;

	/* EGL structures */
	struct rvgpu_egl_state egl;

	/* Windows */
	bool fullscreen;
	bool translucent;
	uint32_t output_id;
	uint32_t output_count;
	struct output_entry outputs[MAX_OUTPUTS];

	/* Mouse pointer position coordinates */
	int pointer_pos_x, pointer_pos_y;

	/* Input handling */
	struct rvgpu_input_state *in;
};

static inline struct rvgpu_wl_state *to_wl(struct rvgpu_egl_state *e)
{
	return rvgpu_container_of(e, struct rvgpu_wl_state, egl);
}

void *create_wl_native_display(void *wl_display_name);
void destroy_wl_native_display(void *native_dpy);
void rvgpu_wl_swap(void *param, bool vsync);
void *rvgpu_wl_init(void *params, uint32_t *width, uint32_t *height);
void rvgpu_wl_free(struct rvgpu_egl_state *e);

#endif /* RVGPU_WAYLAND_H */
