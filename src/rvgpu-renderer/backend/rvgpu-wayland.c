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

#include <wayland-egl.h>

#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <err.h>
#include <errno.h>

#include <sys/poll.h>

#include <librvgpu/rvgpu-protocol.h>
#include <rvgpu-renderer/ivi/ivi-application-client-protocol.h>
#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/renderer/rvgpu-input.h>

struct rvgpu_native {
	/* Window structures */
	struct wl_surface *surface;
	struct wl_shell_surface *shell_surface;
	struct wl_egl_window *egl_window;
	struct ivi_surface *ivi_surface;
};

struct rvgpu_wl_state {
	/* Wayland structures */
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_seat *seat;
	struct wl_touch *touch;
	struct wl_pointer *pointer;
	struct wl_keyboard *keyboard;
	struct wl_shell *shell;
	struct ivi_application *ivi_app;

	/* EGL structures */
	struct rvgpu_egl_state egl;

	/* Windows */
	bool fullscreen;
	bool translucent;

	/* Mouse pointer position coordinates */
	int pointer_pos_x, pointer_pos_y;

	/* Input handling */
	struct rvgpu_input_state *in;
};

static inline struct rvgpu_wl_state *to_wl(struct rvgpu_egl_state *e)
{
	return rvgpu_container_of(e, struct rvgpu_wl_state, egl);
}

static struct wl_seat_listener seat_listener;

static void registry_add_object(void *data, struct wl_registry *registry,
				uint32_t name, const char *interface,
				uint32_t version)
{
	(void)version;
	struct rvgpu_wl_state *r = data;

	if (!strcmp(interface, "wl_compositor")) {
		r->comp = wl_registry_bind(registry, name,
					   &wl_compositor_interface, 1);
	} else if (!strcmp(interface, "wl_shell")) {
		r->shell = wl_registry_bind(registry, name, &wl_shell_interface,
					    1);
	} else if (!strcmp(interface, "wl_seat")) {
		r->seat =
			wl_registry_bind(registry, name, &wl_seat_interface, 1);
		wl_seat_add_listener(r->seat, &seat_listener, r);
	} else if (!strcmp(interface, "ivi_application")) {
		r->ivi_app = wl_registry_bind(registry, name,
					      &ivi_application_interface, 1);
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
	(void)shell_surface;
	(void)edges;
	struct rvgpu_scanout *s = data;

	s->window.w = (unsigned int)width;
	s->window.h = (unsigned int)height;
	wl_egl_window_resize(s->native->egl_window, width, height, 0, 0);
}

static const struct wl_shell_surface_listener shell_surface_listener = {
	.ping = &shell_surface_ping,
	.configure = &shell_surface_configure,
};

static void handle_ivi_surface_configure(void *data,
					 struct ivi_surface *ivi_surface,
					 int32_t width, int32_t height)
{
	(void)ivi_surface;
	struct rvgpu_scanout *s = data;

	s->window.w = (unsigned int)width;
	s->window.h = (unsigned int)height;
	wl_egl_window_resize(s->native->egl_window, width, height, 0, 0);
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
	r->pointer_pos_x = wl_fixed_to_int(sx);
	r->pointer_pos_y = wl_fixed_to_int(sy);
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
				 uint32_t serial, struct wl_surface *surface)
{
	(void)data;
	(void)pointer;
	(void)serial;
	(void)surface;
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
				  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	struct rvgpu_wl_state *r = data;
	(void)pointer;
	(void)time;

	int relative_x = wl_fixed_to_int(sx) - r->pointer_pos_x;
	int relative_y = wl_fixed_to_int(sy) - r->pointer_pos_y;

	r->pointer_pos_x += relative_x;
	r->pointer_pos_y += relative_y;

	struct rvgpu_input_event evs[] = {
		{ EV_REL, REL_X, relative_x },
		{ EV_REL, REL_Y, relative_y },
	};

	if (relative_x == 0)
		rvgpu_in_events(r->in, RVGPU_INPUT_MOUSE, &evs[1], 1);
	else if (relative_y == 0)
		rvgpu_in_events(r->in, RVGPU_INPUT_MOUSE, &evs[0], 1);
	else
		rvgpu_in_events(r->in, RVGPU_INPUT_MOUSE, evs, 2);

	rvgpu_in_send(r->in, RVGPU_INPUT_MOUSE);
}

static void pointer_handle_axis(void *data, struct wl_pointer *pointer,
				uint32_t time, uint32_t axis, wl_fixed_t value)
{
	struct rvgpu_wl_state *r = data;
	(void)pointer;
	(void)time;

	struct rvgpu_input_event ev = {
		EV_KEY,
		(axis == WL_POINTER_AXIS_VERTICAL_SCROLL) ? REL_WHEEL :
							    REL_HWHEEL,
		wl_fixed_to_int(value)
	};

	rvgpu_in_events(r->in, RVGPU_INPUT_MOUSE, &ev, 1);
	rvgpu_in_send(r->in, RVGPU_INPUT_MOUSE);
}

static void pointer_handle_button(void *data, struct wl_pointer *pointer,
				  uint32_t serial, uint32_t time,
				  uint32_t button, uint32_t state)
{
	struct rvgpu_wl_state *r = data;
	(void)time;
	(void)pointer;

	if ((button == BTN_RIGHT) &&
	    (state == WL_POINTER_BUTTON_STATE_PRESSED)) {
		for (unsigned int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
			if (r->egl.scanouts[i].native) {
				struct wl_shell_surface *surface =
					r->egl.scanouts[i].native->shell_surface;
				if (surface)
					wl_shell_surface_move(surface, r->seat,
							      serial);
			}
		}
	}

	struct rvgpu_input_event evs[] = {
		{ EV_KEY, (uint16_t)button, (int32_t)state },
	};
	rvgpu_in_events(r->in, RVGPU_INPUT_MOUSE, evs, 1);
	rvgpu_in_send(r->in, RVGPU_INPUT_MOUSE);
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
	struct rvgpu_scanout *s;

	s = wl_surface_get_user_data(surface);
	assert(s);

	if (s->virgl.tex_id == 0) {
		/* No scanout assigned yet, ignore the touch */
		return;
	}

	rvgpu_in_add_slot(r->in, id, s->params.id, &s->window, &s->virgl.box,
			  &s->virgl.tex);
	rvgpu_in_move_slot(r->in, id, wl_fixed_to_double(x_w),
			   wl_fixed_to_double(y_w));
}

static void touch_handle_up(void *data, struct wl_touch *wl_touch,
			    uint32_t serial, uint32_t time, int32_t id)
{
	struct rvgpu_wl_state *r = data;
	(void)wl_touch;
	(void)serial;
	(void)time;
	rvgpu_in_remove_slot(r->in, id);
	rvgpu_in_send(r->in, RVGPU_INPUT_TOUCH);
}

static void touch_handle_motion(void *data, struct wl_touch *wl_touch,
				uint32_t time, int32_t id, wl_fixed_t x_w,
				wl_fixed_t y_w)
{
	(void)wl_touch;
	(void)time;
	struct rvgpu_wl_state *r = data;

	rvgpu_in_move_slot(r->in, id, wl_fixed_to_double(x_w),
			   wl_fixed_to_double(y_w));
}

static void touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
	struct rvgpu_wl_state *r = data;
	(void)wl_touch;
	rvgpu_in_send(r->in, RVGPU_INPUT_TOUCH);
}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
	struct rvgpu_wl_state *r = data;
	(void)wl_touch;
	rvgpu_in_clear(r->in, RVGPU_INPUT_TOUCH);
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
	(void)wl_keyboard;
	(void)serial;
	(void)time;
	struct rvgpu_wl_state *r = data;
	struct rvgpu_input_event evs[] = {
		{ EV_KEY, (uint16_t)key, (int32_t)state },
	};
	rvgpu_in_events(r->in, RVGPU_INPUT_KEYBOARD, evs, 1);
	rvgpu_in_send(r->in, RVGPU_INPUT_KEYBOARD);
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

static void rvgpu_wl_destroy_scanout(struct rvgpu_egl_state *e,
				     struct rvgpu_scanout *s)
{
	(void)e;
	if (s->native == NULL)
		return;

	wl_egl_window_destroy(s->native->egl_window);
	if (s->native->shell_surface)
		wl_shell_surface_destroy(s->native->shell_surface);

	if (s->native->ivi_surface)
		ivi_surface_destroy(s->native->ivi_surface);

	wl_surface_destroy(s->native->surface);
	free(s->native);
}

static void rvgpu_wl_free(struct rvgpu_egl_state *e)
{
	struct rvgpu_wl_state *r = to_wl(e);

	rvgpu_in_free(r->in);

	wl_registry_destroy(r->reg);
	if (r->ivi_app)
		ivi_application_destroy(r->ivi_app);

	if (r->shell)
		wl_shell_destroy(r->shell);

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

static size_t rvgpu_wl_prepare_events(struct rvgpu_egl_state *e, void *ev,
				      size_t max)
{
	assert(max >= 1);
	struct rvgpu_wl_state *r = to_wl(e);
	int fd = wl_display_get_fd(r->dpy);
	short events = POLLIN;

	if (wl_display_flush(r->dpy) == -1 && errno == EAGAIN)
		events |= POLLOUT;

	while (wl_display_prepare_read(r->dpy) == -1)
		wl_display_dispatch_pending(r->dpy);

	struct pollfd *fds = (struct pollfd *)ev;

	fds->fd = fd;
	fds->events = events;
	return 1;
}

static void rvgpu_wl_process_events(struct rvgpu_egl_state *e, const void *ev,
				    size_t n)
{
	assert(n >= 1);
	struct rvgpu_wl_state *r = to_wl(e);
	short revents;

	struct pollfd *fds = (struct pollfd *)ev;

	revents = fds->revents;
	if (revents) {
		wl_display_read_events(r->dpy);
		wl_display_dispatch_pending(r->dpy);
	} else {
		wl_display_cancel_read(r->dpy);
	}
}

static void rvgpu_wl_set_scanout(struct rvgpu_egl_state *e,
				 struct rvgpu_scanout *s)
{
	struct rvgpu_wl_state *r = to_wl(e);

	if (!r->fullscreen) {
		s->window = s->virgl.box;
		if (s->native) {
			if (!r->translucent) {
				struct wl_region *region =
					wl_compositor_create_region(r->comp);
				wl_region_add(region, 0, 0, (int)s->window.w,
					      (int)s->window.h);
				wl_surface_set_opaque_region(s->native->surface,
							     region);
				wl_region_destroy(region);
			}
			wl_egl_window_resize(s->native->egl_window,
					     (int)s->window.w, (int)s->window.h,
					     0, 0);
		}
	}
}

static void rvgpu_wl_create_scanout(struct rvgpu_egl_state *e,
				    struct rvgpu_scanout *s)
{
	struct rvgpu_wl_state *r = to_wl(e);
	struct rvgpu_native *n;
	const struct rvgpu_scanout_params *sp = &s->params;

	n = calloc(1, sizeof(*n));
	assert(n);

	s->native = n;
	if (r->fullscreen) {
		s->window.h = 2048u;
		s->window.w = 2048u;
	} else if (sp && sp->boxed) {
		s->window = sp->box;
	} else if (s->window.w == 0 || s->window.h == 0) {
		s->window.h = 100;
		s->window.w = 100;
	}

	n->surface = wl_compositor_create_surface(r->comp);
	assert(n->surface);
	wl_surface_set_user_data(n->surface, s);

	if (r->ivi_app) {
		uint32_t id;

		if (sp->id == 0) {
			id = 9000u +
			     (uint32_t)getpid() * VIRTIO_GPU_MAX_SCANOUTS +
			     s->scanout_id;
		} else {
			id = sp->id;
		}

		n->ivi_surface = ivi_application_surface_create(r->ivi_app, id,
								n->surface);
		assert(n->ivi_surface);
		ivi_surface_add_listener(n->ivi_surface, &ivi_surface_listener,
					 s);
	} else if (r->shell) {
		char title[32];

		n->shell_surface =
			wl_shell_get_shell_surface(r->shell, n->surface);
		assert(n->shell_surface);

		wl_shell_surface_add_listener(n->shell_surface,
					      &shell_surface_listener, s);
		if (sp->id != 0) {
			snprintf(title, sizeof(title), "rvgpu scanout %u ID %u",
				 s->scanout_id, sp->id);
		} else {
			snprintf(title, sizeof(title), "rvgpu scanout %u",
				 s->scanout_id);
		}
		wl_shell_surface_set_title(n->shell_surface, title);

		if (r->fullscreen) {
			wl_shell_surface_set_fullscreen(
				n->shell_surface,
				WL_SHELL_SURFACE_FULLSCREEN_METHOD_DEFAULT, 0,
				NULL);
		} else {
			wl_shell_surface_set_toplevel(n->shell_surface);
			if (!r->translucent) {
				struct wl_region *region =
					wl_compositor_create_region(r->comp);
				assert(region);
				wl_region_add(region, 0, 0, (int)s->window.w,
					      (int)s->window.h);
				wl_surface_set_opaque_region(n->surface,
							     region);
				wl_region_destroy(region);
			}
		}
	}

	n->egl_window = wl_egl_window_create(n->surface, (int)s->window.w,
					     (int)s->window.h);
	assert(n->egl_window);

	s->surface =
		eglCreateWindowSurface(e->dpy, e->config, n->egl_window, NULL);
	assert(s->surface);
	eglMakeCurrent(e->dpy, s->surface, s->surface, e->context);

	glGenFramebuffers(1, &s->fb);
}

static const struct rvgpu_egl_callbacks wl_callbacks = {
	.prepare_events = rvgpu_wl_prepare_events,
	.process_events = rvgpu_wl_process_events,
	.free = rvgpu_wl_free,
	.set_scanout = rvgpu_wl_set_scanout,
	.create_scanout = rvgpu_wl_create_scanout,
	.destroy_scanout = rvgpu_wl_destroy_scanout,
};

struct rvgpu_egl_state *rvgpu_wl_init(bool fullscreen, bool translucent,
				      FILE *events_out)
{
	struct rvgpu_wl_state *r = calloc(1, sizeof(*r));
	int res;

	assert(r);
	r->fullscreen = fullscreen;
	r->translucent = translucent;

	/* Wayland initialization */
	r->dpy = wl_display_connect(NULL);
	assert(r->dpy);

	r->reg = wl_display_get_registry(r->dpy);
	assert(r->reg);

	wl_registry_add_listener(r->reg, &registry_listener, r);
	res = wl_display_roundtrip(r->dpy);
	assert(res != -1);

	/* EGL initialization */
	r->egl.dpy = eglGetDisplay(r->dpy);
	assert(r->egl.dpy);
	rvgpu_egl_init_context(&r->egl);

	r->egl.cb = &wl_callbacks;

	/* Input initialization */
	r->in = rvgpu_in_init(events_out);

	/* Wayland supports spawned windows */
	r->egl.spawn_support = true;

	return &r->egl;
}
