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

#include <stdlib.h>

/* Include this before EGL stuff to select EGL implementation */
#include <gbm.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <sys/poll.h>

#include <assert.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <err.h>
#include <errno.h>

#include <libinput.h>
#include <libudev.h>
#include <linux/input.h>

#include <librvgpu/rvgpu-protocol.h>
#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/renderer/rvgpu-input.h>

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

	/* TODO: Support multiple scanouts */
	uint32_t scanout_id;

	/* EGL structures */
	struct rvgpu_egl_state egl;

	/* Input handling */
	struct rvgpu_input_state *in;
	struct libinput *libin;
	struct udev *udev;
};

static inline struct rvgpu_gbm_state *to_gbm(struct rvgpu_egl_state *e)
{
	return rvgpu_container_of(e, struct rvgpu_gbm_state, egl);
}

static void handle_touch_event(struct rvgpu_gbm_state *g,
			       struct libinput_event_touch *tev,
			       enum libinput_event_type t)
{
	int id = libinput_event_touch_get_slot(tev);
	double x, y;

	if (g->scanout_id == VIRTIO_GPU_MAX_SCANOUTS)
		return;

	if (t == LIBINPUT_EVENT_TOUCH_UP) {
		rvgpu_in_remove_slot(g->in, id);
		rvgpu_in_send(g->in, RVGPU_INPUT_TOUCH);
		return;
	}
	x = libinput_event_touch_get_x_transformed(tev, g->mode.hdisplay);
	y = libinput_event_touch_get_y_transformed(tev, g->mode.vdisplay);
	if (t == LIBINPUT_EVENT_TOUCH_DOWN) {
		struct rvgpu_scanout *s = &g->egl.scanouts[g->scanout_id];

		rvgpu_in_add_slot(g->in, id, s->params.id, &s->window,
				  &s->virgl.box, &s->virgl.tex);
	}
	rvgpu_in_move_slot(g->in, id, x, y);
}

static void rvgpu_gbm_input(struct rvgpu_gbm_state *g)
{
	struct libinput_event *ev;

	while ((ev = libinput_get_event(g->libin)) != NULL) {
		enum libinput_event_type t = libinput_event_get_type(ev);

		switch (t) {
		case LIBINPUT_EVENT_KEYBOARD_KEY: {
			struct libinput_event_keyboard *kev =
				libinput_event_get_keyboard_event(ev);
			struct rvgpu_input_event uiev = {
				EV_KEY,
				(uint16_t)libinput_event_keyboard_get_key(kev),
				libinput_event_keyboard_get_key_state(kev)
			};
			rvgpu_in_events(g->in, RVGPU_INPUT_KEYBOARD, &uiev, 1);
			rvgpu_in_send(g->in, RVGPU_INPUT_KEYBOARD);
			break;
		}
		case LIBINPUT_EVENT_POINTER_MOTION: {
			struct libinput_event_pointer *pev =
				libinput_event_get_pointer_event(ev);
			double dx = libinput_event_pointer_get_dx_unaccelerated(
				       pev),
			       dy = libinput_event_pointer_get_dy_unaccelerated(
				       pev);
			struct rvgpu_input_event uiev[] = {
				{ EV_REL, REL_X, (int32_t)dx },
				{ EV_REL, REL_Y, (int32_t)dy },
			};
			if (dx == 0.0) {
				rvgpu_in_events(g->in, RVGPU_INPUT_MOUSE,
						&uiev[1], 1);
			} else if (dy == 0.0) {
				rvgpu_in_events(g->in, RVGPU_INPUT_MOUSE,
						&uiev[0], 1);
			} else {
				rvgpu_in_events(g->in, RVGPU_INPUT_MOUSE, uiev,
						2);
			}
			rvgpu_in_send(g->in, RVGPU_INPUT_MOUSE);
			break;
		}
		case LIBINPUT_EVENT_POINTER_BUTTON: {
			struct libinput_event_pointer *pev =
				libinput_event_get_pointer_event(ev);
			struct rvgpu_input_event uiev = {
				EV_KEY,
				(uint16_t)libinput_event_pointer_get_button(
					pev),
				libinput_event_pointer_get_button_state(pev)
			};
			rvgpu_in_events(g->in, RVGPU_INPUT_MOUSE, &uiev, 1);
			rvgpu_in_send(g->in, RVGPU_INPUT_MOUSE);
			break;
		}
		case LIBINPUT_EVENT_POINTER_AXIS: {
			struct libinput_event_pointer *pev =
				libinput_event_get_pointer_event(ev);
			double hor = libinput_event_pointer_get_axis_value_discrete(
				       pev,
				       LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL),
			       vert = libinput_event_pointer_get_axis_value_discrete(
				       pev,
				       LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
			struct rvgpu_input_event uiev[] = {
				{ EV_REL, REL_HWHEEL, (int32_t)hor },
				{ EV_REL, REL_WHEEL, (int32_t)vert },
			};
			if (hor == 0.0) {
				rvgpu_in_events(g->in, RVGPU_INPUT_MOUSE,
						&uiev[1], 1);
			} else if (vert == 0.0) {
				rvgpu_in_events(g->in, RVGPU_INPUT_MOUSE,
						&uiev[0], 1);
			} else {
				rvgpu_in_events(g->in, RVGPU_INPUT_MOUSE, uiev,
						2);
			}
			rvgpu_in_send(g->in, RVGPU_INPUT_MOUSE);
			break;
		}
		case LIBINPUT_EVENT_TOUCH_FRAME:
			rvgpu_in_send(g->in, RVGPU_INPUT_TOUCH);
			break;
		case LIBINPUT_EVENT_TOUCH_CANCEL:
			rvgpu_in_clear(g->in, RVGPU_INPUT_TOUCH);
			break;
		case LIBINPUT_EVENT_TOUCH_DOWN:
		case LIBINPUT_EVENT_TOUCH_MOTION:
		case LIBINPUT_EVENT_TOUCH_UP: {
			struct libinput_event_touch *tev =
				libinput_event_get_touch_event(ev);
			handle_touch_event(g, tev, t);
			break;
		}
		default:
			break;
		}
		libinput_event_destroy(ev);
	}
}

static void rvgpu_gbm_free(struct rvgpu_egl_state *e)
{
	struct rvgpu_gbm_state *g = to_gbm(e);

	rvgpu_in_free(g->in);
	libinput_unref(g->libin);
	udev_unref(g->udev);

	/* GBM deinit */
	drmModeSetCrtc(g->gbm_fd, g->crtc->crtc_id, g->crtc->buffer_id,
		       g->crtc->x, g->crtc->y, &g->connector, 1,
		       &g->crtc->mode);
	drmModeFreeCrtc(g->crtc);

	if (g->prev_bo) {
		drmModeRmFB(g->gbm_fd, g->prev_fb);
		gbm_surface_release_buffer(g->gbm_surface, g->prev_bo);
	}

	if (g->current_bo) {
		drmModeRmFB(g->gbm_fd, g->current_fb);
		gbm_surface_release_buffer(g->gbm_surface, g->current_bo);
	}

	gbm_surface_destroy(g->gbm_surface);
	gbm_device_destroy(g->gbm_device);

	close(g->gbm_fd);
	free(g);
}

static void rvgpu_gbm_create_scanout(struct rvgpu_egl_state *e,
				     struct rvgpu_scanout *s)
{
	struct rvgpu_gbm_state *g = to_gbm(e);

	if (g->scanout_id != VIRTIO_GPU_MAX_SCANOUTS)
		errx(1, "GBM backend only supports one scanout for now!");

	s->native = (struct rvgpu_native *)g;
	g->scanout_id = s->scanout_id;
	s->window.h = g->mode.vdisplay;
	s->window.w = g->mode.hdisplay;
	s->surface =
		eglCreateWindowSurface(e->dpy, e->config, g->gbm_surface, NULL);
	assert(s->surface);

	eglMakeCurrent(e->dpy, s->surface, s->surface, e->context);
	glGenFramebuffers(1, &s->fb);
}

static void rvgpu_gbm_page_flip_handler(int fd, unsigned int sequence,
					unsigned int tv_sec,
					unsigned int tv_usec, void *user_data)
{
	struct rvgpu_gbm_state *g = user_data;

	g->flip_pending = false;

	if (g->prev_bo) {
		drmModeRmFB(g->gbm_fd, g->prev_fb);
		gbm_surface_release_buffer(g->gbm_surface, g->prev_bo);
	}
	g->prev_bo = g->current_bo;
	g->prev_fb = g->current_fb;

	(void)fd;
	(void)sequence;
	(void)tv_sec;
	(void)tv_usec;
}

static drmEventContext gbm_evctx = {
	.version = 2,
	.page_flip_handler = rvgpu_gbm_page_flip_handler,
};

static void rvgpu_gbm_draw(struct rvgpu_egl_state *e, struct rvgpu_scanout *s,
			   bool vsync)
{
	(void)vsync;
	struct rvgpu_gbm_state *g = to_gbm(e);
	unsigned int fb;
	struct gbm_bo *bo;
	unsigned int handle, stride;

	if (g->flip_pending)
		return;

	eglSwapBuffers(e->dpy, s->surface);

	bo = gbm_surface_lock_front_buffer(g->gbm_surface);
	handle = gbm_bo_get_handle(bo).u32;
	stride = gbm_bo_get_stride(bo);

	drmModeAddFB(g->gbm_fd, s->window.w, s->window.h, 24, 32, stride,
		     handle, &fb);
	if (!g->mode_set) {
		drmModeSetCrtc(g->gbm_fd, g->crtc->crtc_id, fb, 0, 0,
			       &g->connector, 1, &g->mode);
		g->mode_set = true;
	} else {
		int status = drmModePageFlip(g->gbm_fd, g->crtc->crtc_id, fb,
					     DRM_MODE_PAGE_FLIP_EVENT, g);
		if (status != 0) {
			errno = -status;
			warn("PageFlip failed");
		} else {
			g->flip_pending = true;
		}
		if (vsync)
			drmHandleEvent(g->gbm_fd, &gbm_evctx);
	}
	g->current_bo = bo;
	g->current_fb = fb;
}

static size_t rvgpu_gbm_prepare_events(struct rvgpu_egl_state *e, void *ev,
				       size_t max)
{
	struct rvgpu_gbm_state *g = to_gbm(e);

	assert(max >= 2);
	(void)max;
	struct pollfd *fds = (struct pollfd *)ev;

	fds[0].fd = g->gbm_fd;
	fds[0].events = POLLIN;
	fds[1].fd = libinput_get_fd(g->libin);
	fds[1].events = POLLIN;
	return 2u;
}

static void rvgpu_gbm_process_events(struct rvgpu_egl_state *e, const void *ev,
				     size_t n)
{
	struct rvgpu_gbm_state *g = to_gbm(e);
	short revents[n];

	assert(n >= 2);
	struct pollfd *fds = (struct pollfd *)ev;

	revents[0] = fds[0].revents;
	revents[1] = fds[1].revents;
	if (revents[0])
		drmHandleEvent(g->gbm_fd, &gbm_evctx);

	if (revents[1]) {
		libinput_dispatch(g->libin);
		rvgpu_gbm_input(g);
	}
}

static const struct rvgpu_egl_callbacks gbm_callbacks = {
	.free = rvgpu_gbm_free,
	.draw = rvgpu_gbm_draw,
	.create_scanout = rvgpu_gbm_create_scanout,
	.prepare_events = rvgpu_gbm_prepare_events,
	.process_events = rvgpu_gbm_process_events,
};

static int open_restricted(const char *path, int flags, void *user_data)
{
	(void)user_data;
	int fd = open(path, flags);

	return fd >= 0 ? fd : -errno;
}

static void close_restricted(int fd, void *user_data)
{
	(void)user_data;
	close(fd);
}

static const struct libinput_interface interface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

#define NATIVE_GBM_FORMAT GBM_FORMAT_XRGB8888

struct rvgpu_egl_state *rvgpu_gbm_init(const char *device, const char *seat,
				       FILE *events_out)
{
	struct rvgpu_gbm_state *g = calloc(1, sizeof(*g));
	drmModeRes *res;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder;

	assert(g);
	g->scanout_id = VIRTIO_GPU_MAX_SCANOUTS;

	g->gbm_fd = open(device, O_RDWR);
	if (g->gbm_fd == -1)
		err(1, "open %s", device);

	/* GBM initialization */
	g->gbm_device = gbm_create_device(g->gbm_fd);
	assert(g->gbm_device);

	res = drmModeGetResources(g->gbm_fd);
	assert(res);

	for (int i = 0; i < res->count_connectors; i++) {
		drmModeConnector *c =
			drmModeGetConnector(g->gbm_fd, res->connectors[i]);
		if (c->connection == DRM_MODE_CONNECTED) {
			connector = c;
			break;
		}
		drmModeFreeConnector(c);
	}
	assert(connector);
	assert(connector->encoder_id);

	g->mode = connector->modes[0];
	g->connector = connector->connector_id;
	warnx("Connector %u, resolution %ux%u, vsync %d",
	      connector->connector_id, g->mode.hdisplay, g->mode.vdisplay,
	      g->mode.vrefresh);

	encoder = drmModeGetEncoder(g->gbm_fd, connector->encoder_id);
	assert(encoder);
	assert(encoder->crtc_id);

	g->crtc = drmModeGetCrtc(g->gbm_fd, encoder->crtc_id);
	assert(g->crtc);

	drmModeFreeEncoder(encoder);
	drmModeFreeConnector(connector);
	drmModeFreeResources(res);

	/* GBM->EGL glue */
	g->egl.dpy = eglGetDisplay(g->gbm_device);
	assert(g->egl.dpy);

	/* GBM doesn't support spawned windows */
	g->egl.spawn_support = false;

	/* GBM requires to use a specific native format */
	g->egl.use_native_format = true;
	g->egl.native_format = NATIVE_GBM_FORMAT;

	rvgpu_egl_init_context(&g->egl);

	g->gbm_surface =
		gbm_surface_create(g->gbm_device, g->mode.hdisplay,
				   g->mode.vdisplay, NATIVE_GBM_FORMAT,
				   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	assert(g->gbm_surface);

	g->egl.cb = &gbm_callbacks;

	/* input initialization */
	g->in = rvgpu_in_init(events_out);
	g->udev = udev_new();
	g->libin = libinput_udev_create_context(&interface, NULL, g->udev);
	libinput_log_set_priority(g->libin, LIBINPUT_LOG_PRIORITY_INFO);
	libinput_udev_assign_seat(g->libin, seat);
	libinput_dispatch(g->libin);

	return &g->egl;
}
