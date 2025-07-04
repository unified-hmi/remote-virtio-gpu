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

#include <wayland-egl.h>
#include <wayland-client.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <linux/input-event-codes.h>

#include <librvgpu/rvgpu-protocol.h>

#include <rvgpu-renderer/ivi/ivi-application-client-protocol.h>
#include <rvgpu-renderer/shell/xdg-shell-client-protocol.h>
#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/renderer/rvgpu-input.h>
#include <rvgpu-renderer/backend/rvgpu-wayland.h>

static pthread_t wl_event_thread;
struct rvgpu_native *native;
static struct wl_seat_listener seat_listener;

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
	(void)data;
	xdg_wm_base_pong(xdg_wm_base, serial);
}
 
static const struct xdg_wm_base_listener xdg_wm_base_listener = {
	.ping = xdg_wm_base_ping,
};

static void display_handle_geometry(void *data, struct wl_output *wl_output,
				    int x, int y, int physical_width,
				    int physical_height, int subpixel,
				    const char *make, const char *model,
				    int transform)
{
	(void)wl_output;
	(void)subpixel;
	(void)transform;
	struct output_info *info = data;
	info->x = x;
	info->y = y;
	info->physical_width = physical_width;
	info->physical_height = physical_height;
	strncpy(info->make, make, sizeof(info->make));
	strncpy(info->model, model, sizeof(info->model));
}

static void display_handle_mode(void *data, struct wl_output *wl_output,
				uint32_t flags, int32_t width, int32_t height,
				int32_t subpixel)
{
	(void)wl_output;
	(void)flags;
	(void)subpixel;
	struct output_info *info = data;
	if (flags & WL_OUTPUT_MODE_CURRENT) {
		info->mode_width = width;
		info->mode_height = height;
		info->mode_known = true;
	}
}

static void display_handle_done(void *data, struct wl_output *wl_output)
{
	(void)data;
	(void)wl_output;
}

static void display_handle_scale(void *data, struct wl_output *wl_output,
				 int32_t scale)
{
	(void)data;
	(void)wl_output;
	(void)scale;
}

static const struct wl_output_listener output_listener = {
	.geometry = display_handle_geometry,
	.mode = display_handle_mode,
	.done = display_handle_done,
	.scale = display_handle_scale,
};

bool check_wl_output_info(struct rvgpu_wl_state *r, int output_id)
{
	if (output_id < MAX_OUTPUTS && r->outputs[output_id].output != NULL) {
		struct output_entry *entry = &r->outputs[output_id];
		printf("Output %d:\n", output_id);
		printf("  wl_output: %p\n", entry->output);
		printf("  Position: (%d, %d)\n", entry->info.x, entry->info.y);
		printf("  Physical size: %dmm x %dmm\n",
		       entry->info.physical_width, entry->info.physical_height);
		printf("  Make: %s\n", entry->info.make);
		printf("  Model: %s\n", entry->info.model);
		if (entry->info.mode_known) {
			printf("  Mode: %dx%d\n", entry->info.mode_width,
			       entry->info.mode_height);
		} else {
			printf("  Mode: unknown\n");
		}
		return true;
	} else {
		fprintf(stderr, "Output %d is not found\n", output_id);
	}
	return false;
}

#define min(a,b) ({ __typeof__ (a) _a = (a); \
		  __typeof__ (b) _b = (b); \
		  _a < _b ? _a : _b; })

static void registry_add_object(void *data, struct wl_registry *registry,
				uint32_t name, const char *interface,
				uint32_t version)
{
	(void)version;
	struct rvgpu_wl_state *r = data;

	if (!strcmp(interface, wl_compositor_interface.name)) {
		r->comp = wl_registry_bind(registry, name,
					   &wl_compositor_interface,
					   min(version, 4u));
	} else if (!strcmp(interface, wl_shell_interface.name)) {
		r->shell = wl_registry_bind(registry, name, &wl_shell_interface,
					    1);
	} else if (!strcmp(interface, xdg_wm_base_interface.name)) {
		r->wm_base = wl_registry_bind(registry, name,
					      &xdg_wm_base_interface,
					      min(version, 2u));
		xdg_wm_base_add_listener(r->wm_base, &xdg_wm_base_listener,
					 NULL);
	} else if (!strcmp(interface, wl_seat_interface.name)) {
		r->seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(r->seat, &seat_listener, r);
	} else if (!strcmp(interface, ivi_application_interface.name)) {
		r->ivi_app = wl_registry_bind(registry, name,
					      &ivi_application_interface, 1);
	} else if (!strcmp(interface, wl_output_interface.name)) {
		if (r->output_count < MAX_OUTPUTS) {
			struct output_entry *entry =
				&r->outputs[r->output_count];
			entry->output = wl_registry_bind(
				registry, name, &wl_output_interface, 2);
			wl_output_add_listener(entry->output, &output_listener,
					       &entry->info);
			r->output_count++;
		}
	}
}

static void registry_remove_object(void *data, struct wl_registry *registry,
				   uint32_t name)
{
	(void)data;
	(void)registry;
	(void)name;
}

static struct wl_registry_listener registry_listener = {
	&registry_add_object, &registry_remove_object
};

static void shell_surface_ping(void *data,
			       struct wl_shell_surface *shell_surface,
			       uint32_t serial)
{
	(void)data;
	wl_shell_surface_pong(shell_surface, serial);
}
static void shell_surface_configure(void *data,
				    struct wl_shell_surface *shell_surface,
				    uint32_t edges, int32_t width,
				    int32_t height)
{
	(void)data;
	(void)shell_surface;
	(void)edges;
	(void)width;
	(void)height;
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	.ping = &shell_surface_ping,
	.configure = &shell_surface_configure,
};

static void xdg_surface_configure(void *data,
	struct xdg_surface *xdg_surface, uint32_t serial)
{
	(void)data;
	xdg_surface_ack_configure(xdg_surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
	.configure = xdg_surface_configure,
};

static void xdg_toplevel_configure(void *data,
				   struct xdg_toplevel *xdg_toplevel,
				   int32_t width, int32_t height,
				   struct wl_array *states)
{
	struct rvgpu_native *n = data;
	struct rvgpu_wl_state *r = n->wl_state;

	(void)xdg_toplevel;
	(void)states;
	if (width && height) {
		*(r->width) = (unsigned int)width;
		*(r->height) = (unsigned int)height;
	}

	if (!n->egl_window) {
		n->egl_window = wl_egl_window_create(n->surface, *(r->width),
						     *(r->height));
		assert(n->egl_window);
	} else {
		wl_egl_window_resize(n->egl_window, *(r->width), *(r->height),
				     0, 0);
	}

	n->xdg_wm_base_waiting_for_configure = false;
}

static void
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
	(void)data;
	(void)toplevel;
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
	.configure = xdg_toplevel_configure,
	.close = xdg_toplevel_close,
};

static void handle_ivi_surface_configure(void *data,
					 struct ivi_surface *ivi_surface,
					 int32_t width, int32_t height)
{
	(void)data;
	(void)ivi_surface;
	wl_egl_window_resize(native->egl_window, width, height, 0, 0);
}

static const struct ivi_surface_listener ivi_surface_listener = {
	handle_ivi_surface_configure,
};

static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
				 uint32_t serial, struct wl_surface *surface,
				 wl_fixed_t sx, wl_fixed_t sy)
{
	struct rvgpu_wl_state *r = data;
	(void)pointer;
	(void)serial;
	(void)surface;
	double x = wl_fixed_to_double(sx);
	double y = wl_fixed_to_double(sy);
	pointer_inout_cb(x, y, &r->egl);
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
				 uint32_t serial, struct wl_surface *surface)
{
	struct rvgpu_wl_state *r = data;
	(void)pointer;
	(void)serial;
	(void)surface;
	pointer_inout_cb(-1, -1, &r->egl);
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
				  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	struct rvgpu_wl_state *r = data;
	(void)pointer;
	(void)time;
	double x = wl_fixed_to_double(sx);
	double y = wl_fixed_to_double(sy);
	pointer_motion_cb(x, y, &r->egl);
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer,
				uint32_t time, uint32_t axis, wl_fixed_t value)
{
	struct rvgpu_wl_state *r = data;
	(void)pointer;
	(void)time;
	uint16_t wheel = (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ? REL_WHEEL :
								     REL_HWHEEL;
	pointer_axis_cb(wheel, wl_fixed_to_int(value), &r->egl);
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer,
				  uint32_t serial, uint32_t time,
				  uint32_t button, uint32_t state)
{
	struct rvgpu_wl_state *r = data;
	(void)pointer;
	(void)serial;
	(void)time;
	pointer_button_cb(button, state, &r->egl);
}

static void pointer_handle_frame(void *data, struct wl_pointer *pointer)
{
	(void)data;
	(void)pointer;
}

static void pointer_handle_axis_source(void *data,
				       struct wl_pointer *wl_pointer,
				       uint32_t axis_source)
{
	(void)data;
	(void)wl_pointer;
	(void)axis_source;
}

static void pointer_handle_axis_stop(void *data, struct wl_pointer *wl_pointer,
				     uint32_t time, uint32_t axis)
{
	(void)data;
	(void)wl_pointer;
	(void)time;
	(void)axis;
}

static void pointer_handle_axis_discrete(void *data,
					 struct wl_pointer *wl_pointer,
					 uint32_t axis, int32_t discrete)
{
	(void)data;
	(void)wl_pointer;
	(void)axis;
	(void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = pointer_handle_enter,
	.leave = pointer_handle_leave,
	.motion = pointer_handle_motion,
	.button = pointer_handle_button,
	.axis = pointer_handle_axis,
	.frame = pointer_handle_frame,
	.axis_source = pointer_handle_axis_source,
	.axis_stop = pointer_handle_axis_stop,
	.axis_discrete = pointer_handle_axis_discrete,
};

static void touch_handle_down(void *data, struct wl_touch *wl_touch,
			      uint32_t serial, uint32_t time,
			      struct wl_surface *surface, int32_t id,
			      wl_fixed_t x_w, wl_fixed_t y_w)
{
	struct rvgpu_wl_state *r = data;

	(void)wl_touch;
	(void)serial;
	(void)time;
	(void)surface;

	double x = wl_fixed_to_double(x_w);
	double y = wl_fixed_to_double(y_w);
	touch_down_cb(id, x, y, &r->egl);
}

static void touch_handle_up(void *data, struct wl_touch *wl_touch,
			    uint32_t serial, uint32_t time, int32_t id)
{
	struct rvgpu_wl_state *r = data;
	(void)wl_touch;
	(void)serial;
	(void)time;

	touch_up_cb(id, &r->egl);
}

static void touch_handle_motion(void *data, struct wl_touch *wl_touch,
				uint32_t time, int32_t id, wl_fixed_t x_w,
				wl_fixed_t y_w)
{
	(void)wl_touch;
	(void)time;
	struct rvgpu_wl_state *r = data;

	double x = wl_fixed_to_double(x_w);
	double y = wl_fixed_to_double(y_w);
	touch_motion_cb(id, x, y, &r->egl);
}

static void touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
	struct rvgpu_wl_state *r = data;
	(void)wl_touch;
	touch_frame_cb(&r->egl);
}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
	struct rvgpu_wl_state *r = data;
	(void)wl_touch;
	touch_cancel_cb(&r->egl);
}

static const struct wl_touch_listener touch_listener = {
	.down = touch_handle_down,
	.up = touch_handle_up,
	.motion = touch_handle_motion,
	.frame = touch_handle_frame,
	.cancel = touch_handle_cancel,
};

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
				  uint32_t serial, struct wl_surface *surface,
				  struct wl_array *keys)
{
	(void)data;
	(void)wl_keyboard;
	(void)serial;
	(void)surface;
	(void)keys;
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
				  uint32_t serial, struct wl_surface *surface)
{
	(void)data;
	(void)wl_keyboard;
	(void)serial;
	(void)surface;
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
				uint32_t serial, uint32_t time, uint32_t key,
				uint32_t state)
{
	struct rvgpu_wl_state *r = data;
	(void)wl_keyboard;
	(void)serial;
	(void)time;
	keyboard_cb(key, state, &r->egl);
}

static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
				   uint32_t format, int32_t fd, uint32_t size)
{
	(void)data;
	(void)wl_keyboard;
	(void)format;
	(void)fd;
	(void)size;
}

static void keyboard_handle_modifiers(void *data,
				      struct wl_keyboard *wl_keyboard,
				      uint32_t serial, uint32_t mods_depressed,
				      uint32_t mods_latched,
				      uint32_t mods_locked, uint32_t group)
{
	(void)data;
	(void)wl_keyboard;
	(void)serial;
	(void)mods_depressed;
	(void)mods_latched;
	(void)mods_locked;
	(void)group;
}

static const struct wl_keyboard_listener keyboard_listener = {
	.enter = keyboard_handle_enter,
	.leave = keyboard_handle_leave,
	.key = keyboard_handle_key,
	.keymap = keyboard_handle_keymap,
	.modifiers = keyboard_handle_modifiers,
};

static void seat_handle_capabilities(void *data, struct wl_seat *seat,
				     enum wl_seat_capability caps)
{
	struct rvgpu_wl_state *r = data;

	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !r->pointer) {
		r->pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(r->pointer, &pointer_listener, r);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && r->pointer) {
		wl_pointer_destroy(r->pointer);
		r->pointer = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !r->touch) {
		r->touch = wl_seat_get_touch(seat);
		wl_touch_add_listener(r->touch, &touch_listener, r);
	} else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && r->touch) {
		wl_touch_destroy(r->touch);
		r->touch = NULL;
	}

	if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !r->keyboard) {
		r->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(r->keyboard, &keyboard_listener, r);
	} else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && r->keyboard) {
		wl_keyboard_destroy(r->keyboard);
		r->keyboard = NULL;
	}
}

static void seat_handle_name(void *data, struct wl_seat *seat, const char *name)
{
	(void)data;
	(void)seat;
	(void)name;
}

static struct wl_seat_listener seat_listener = {
	.capabilities = seat_handle_capabilities,
	.name = seat_handle_name,
};

static void *event_loop(void *arg)
{
	struct rvgpu_wl_state *r = (struct rvgpu_wl_state *)arg;
	int fd = wl_display_get_fd(r->dpy);
	while (1) {
		short events = POLLIN;
		if (wl_display_flush(r->dpy) == -1 && errno == EAGAIN) {
			events |= POLLOUT;
		}
		while (wl_display_prepare_read(r->dpy) == -1) {
			wl_display_dispatch_pending(r->dpy);
		}

		struct pollfd fds;
		fds.fd = fd;
		fds.events = events;
		fds.revents = 0;
		int ret = poll(&fds, 1, -1);
		if (ret == -1) {
			if (errno != EINTR) {
				perror("poll error");
				break;
			}
			continue;
		}

		if (fds.revents & POLLIN) {
			wl_display_read_events(r->dpy);
			wl_display_dispatch_pending(r->dpy);
		}

		if (fds.revents & POLLOUT) {
			if (wl_display_flush(r->dpy) == -1 && errno != EAGAIN) {
				perror("wl_display_flush error");
				break;
			}
		}

		if (!(fds.revents & (POLLIN | POLLOUT))) {
			wl_display_cancel_read(r->dpy);
		}
	}
	return NULL;
}

void rvgpu_wl_create_window(struct rvgpu_egl_state *e, uint32_t width,
			    uint32_t height, uint32_t ivi_surface_id)
{
	char title[32];
	char id[64];
	struct rvgpu_wl_state *r = to_wl(e);
	native = calloc(1, sizeof(*native));
	assert(native);
	native->wl_state = r;
	native->surface = wl_compositor_create_surface(r->comp);
	snprintf(title, sizeof(title), "rvgpu compositor");
	snprintf(id, sizeof(id), "com.github.remote-virtio-gpu.compositor");
	if (r->ivi_app) {
		uint32_t id;

		if (ivi_surface_id == 0) {
			id = 9000u + (uint32_t)getpid();
		} else {
			id = ivi_surface_id;
		}
		native->ivi_surface = ivi_application_surface_create(
			r->ivi_app, id, native->surface);
		assert(native->ivi_surface);
		ivi_surface_add_listener(native->ivi_surface,
					 &ivi_surface_listener, NULL);

		native->egl_window =
			wl_egl_window_create(native->surface, width, height);
		assert(native->egl_window);
	} else if (r->wm_base) {
		if (r->shell)
			wl_shell_destroy(r->shell);
		r->shell = NULL;
		native->xdg_wm_base_waiting_for_configure = true;
		native->egl_window = NULL;
		native->xdg_surface = xdg_wm_base_get_xdg_surface(
			r->wm_base, native->surface);
		assert(native->xdg_surface);

		xdg_surface_add_listener(native->xdg_surface,
					 &xdg_surface_listener, native);
		native->xdg_toplevel =
			xdg_surface_get_toplevel(native->xdg_surface);
		assert(native->xdg_toplevel);
		xdg_toplevel_add_listener(native->xdg_toplevel,
					  &xdg_toplevel_listener, native);

		xdg_toplevel_set_app_id(native->xdg_toplevel, id);
		xdg_toplevel_set_title(native->xdg_toplevel, title);
		wl_display_roundtrip(r->dpy);

		if (r->fullscreen && check_wl_output_info(r, r->output_id)) {
			xdg_toplevel_set_fullscreen(
				native->xdg_toplevel,
				r->outputs[r->output_id].output);
		} else {
			if (!r->translucent) {
				struct wl_region *region =
					wl_compositor_create_region(r->comp);
				assert(region);
				wl_region_add(region, 0, 0, *(r->width),
					      *(r->height));
				wl_surface_set_opaque_region(native->surface,
							     region);
				wl_region_destroy(region);
			}
		}

		wl_surface_commit(native->surface);

		while (native->xdg_wm_base_waiting_for_configure)
			wl_display_roundtrip(r->dpy);

	} else if (r->shell) {
		native->shell_surface =
			wl_shell_get_shell_surface(r->shell, native->surface);
		assert(native->shell_surface);

		wl_shell_surface_add_listener(native->shell_surface,
					      &shell_surface_listener, NULL);
		wl_shell_surface_set_title(native->shell_surface, title);

		if (r->fullscreen) {
			wl_shell_surface_set_fullscreen(
				native->shell_surface,
				WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0,
				NULL);
		} else {
			wl_shell_surface_set_toplevel(native->shell_surface);
			if (!r->translucent) {
				struct wl_region *region =
					wl_compositor_create_region(r->comp);
				assert(region);
				wl_region_add(region, 0, 0, width, height);
				wl_surface_set_opaque_region(native->surface,
							     region);
				wl_region_destroy(region);
			}
		}
		native->egl_window =
			wl_egl_window_create(native->surface, width, height);
		assert(native->egl_window);
	}
	e->sfc = eglCreateWindowSurface(e->dpy, e->config, native->egl_window,
					NULL);
	assert(e->sfc);
}

void rvgpu_wl_free(struct rvgpu_egl_state *e)
{
	struct rvgpu_wl_state *r = to_wl(e);

	wl_registry_destroy(r->reg);
	if (r->ivi_app)
		ivi_application_destroy(r->ivi_app);

	if (r->shell)
		wl_shell_destroy(r->shell);

	if (r->wm_base)
		xdg_wm_base_destroy(r->wm_base);

	wl_seat_destroy(r->seat);
	if (r->pointer)
		wl_pointer_destroy(r->pointer);

	if (r->keyboard)
		wl_keyboard_destroy(r->keyboard);

	if (r->touch)
		wl_touch_destroy(r->touch);

	wl_compositor_destroy(r->comp);
	wl_display_disconnect(r->dpy);
	free(r);
}

void *create_wl_native_display(void *wl_display_name)
{
	char *name = (char *)wl_display_name;
	struct wl_display *display = wl_display_connect(name);
	if (!display) {
		fprintf(stderr, "Failed to connect to Wayland display\n");
		return NULL;
	}
	return (void *)display;
}

void destroy_wl_native_display(void *native_dpy)
{
	struct wl_display *display = (struct wl_display *)native_dpy;
	if (display) {
		wl_display_disconnect(display);
	}
}

void rvgpu_wl_swap(void *param, bool vsync)
{
	struct rvgpu_egl_state *egl = (struct rvgpu_egl_state *)param;
	struct rvgpu_wl_state *r = to_wl(egl);
	eglSwapBuffers(egl->dpy, egl->sfc);
	if (vsync) {
		wl_display_dispatch(r->dpy);
	} else {
		wl_display_dispatch_pending(r->dpy);
	}
}

void *rvgpu_wl_init(void *params, uint32_t *width, uint32_t *height)
{
	rvgpu_wl_params *wl_params = (rvgpu_wl_params *)params;
	uint32_t ivi_surface_id = wl_params->ivi_surface_id;
	uint32_t output_id = wl_params->output_id;
	bool fullscreen = wl_params->fullscreen;
	bool translucent = wl_params->translucent;

	struct rvgpu_wl_state *r = calloc(1, sizeof(*r));
	int res;

	assert(r);
	r->width = width;
	r->height = height;
	r->fullscreen = fullscreen;
	r->output_id = output_id;
	r->translucent = translucent;

	/* Wayland initialization */
	r->dpy = create_wl_native_display(NULL);
	assert(r->dpy);

	r->reg = wl_display_get_registry(r->dpy);
	assert(r->reg);

	wl_registry_add_listener(r->reg, &registry_listener, r);
	res = wl_display_roundtrip(r->dpy);
	assert(res != -1);
	(void)res;

	/* EGL initialization */
#ifdef EGL_VERSION_GE_1_5
	r->egl.dpy =
		eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_EXT, r->dpy, NULL);
#else
	PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
			"eglGetPlatformDisplayEXT");
	if (eglGetPlatformDisplayEXT) {
		r->egl.dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_WAYLAND_EXT,
						      r->dpy, NULL);
	} else {
		r->egl.dpy = eglGetDisplay(r->dpy);
	}
#endif
	assert(r->egl.dpy);

	/* Wayland does not require to use any specific native format */
	r->egl.use_native_format = false;

	rvgpu_egl_init_context(&r->egl);

	pthread_create(&wl_event_thread, NULL, event_loop, r);

	rvgpu_wl_create_window(&r->egl, *width, *height, ivi_surface_id);
	return &r->egl;
}
