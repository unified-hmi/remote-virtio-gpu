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
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <assert.h>
#include <err.h>
#include <errno.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <linux/input.h>
#include <libinput.h>
#include <libudev.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#include <librvgpu/rvgpu-protocol.h>

#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/renderer/rvgpu-input.h>
#include <rvgpu-renderer/backend/rvgpu-gbm.h>

static pthread_t gbm_event_thread;

static void keyboard_handle_key(struct libinput_event *ev,
				struct rvgpu_egl_state *egl)
{
	struct libinput_event_keyboard *kev =
		libinput_event_get_keyboard_event(ev);
	uint32_t key = libinput_event_keyboard_get_key(kev);
	uint32_t state = libinput_event_keyboard_get_key_state(kev);
	keyboard_cb(key, state, egl);
}

static void pointer_handle_motion(struct libinput_event *ev,
				  struct rvgpu_egl_state *egl)
{
	struct libinput_event_pointer *pev =
		libinput_event_get_pointer_event(ev);
	double x = libinput_event_pointer_get_dx_unaccelerated(pev);
	double y = libinput_event_pointer_get_dy_unaccelerated(pev);
	pointer_motion_cb(x, y, egl);
}

static void pointer_handle_motion_abs(struct libinput_event *ev,
				      struct rvgpu_egl_state *egl)
{
	struct libinput_event_pointer *pev =
		libinput_event_get_pointer_event(ev);
	double x = libinput_event_pointer_get_absolute_x(pev);
	double y = libinput_event_pointer_get_absolute_y(pev);
	pointer_motion_cb(x, y, egl);
}

static void pointer_handle_button(struct libinput_event *ev,
				  struct rvgpu_egl_state *egl)
{
	struct libinput_event_pointer *pev =
		libinput_event_get_pointer_event(ev);
	uint32_t button = libinput_event_pointer_get_button(pev);
	uint32_t state = libinput_event_pointer_get_button_state(pev);
	pointer_button_cb(button, state, egl);
}

static void pointer_handle_axis(struct libinput_event *ev,
				struct rvgpu_egl_state *egl)
{
	struct libinput_event_pointer *pev =
		libinput_event_get_pointer_event(ev);
	if (libinput_event_pointer_has_axis(
		    pev, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL)) {
		uint32_t axis_h =
			libinput_event_pointer_get_axis_value_discrete(
				pev, LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL);
		if (axis_h != 0)
			pointer_axis_cb(REL_HWHEEL, axis_h, egl);
	}
	if (libinput_event_pointer_has_axis(
		    pev, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL)) {
		uint32_t axis_v =
			libinput_event_pointer_get_axis_value_discrete(
				pev, LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL);
		if (axis_v != 0.0)
			pointer_axis_cb(REL_WHEEL, axis_v, egl);
	}
}

static void touch_handle_down(struct libinput_event *ev, int width, int height,
			      struct rvgpu_egl_state *egl)
{
	struct libinput_event_touch *tev = libinput_event_get_touch_event(ev);
	int id = libinput_event_touch_get_slot(tev);
	double x = libinput_event_touch_get_x_transformed(tev, width);
	double y = libinput_event_touch_get_y_transformed(tev, height);
	touch_down_cb(id, x, y, egl);
}

static void touch_handle_up(struct libinput_event *ev,
			    struct rvgpu_egl_state *egl)
{
	struct libinput_event_touch *tev = libinput_event_get_touch_event(ev);
	int id = libinput_event_touch_get_slot(tev);
	touch_up_cb(id, egl);
}

static void touch_handle_motion(struct libinput_event *ev, int width,
				int height, struct rvgpu_egl_state *egl)
{
	struct libinput_event_touch *tev = libinput_event_get_touch_event(ev);
	int id = libinput_event_touch_get_slot(tev);
	double x = libinput_event_touch_get_x_transformed(tev, width);
	double y = libinput_event_touch_get_y_transformed(tev, height);
	touch_motion_cb(id, x, y, egl);
}

static void rvgpu_gbm_input(struct rvgpu_gbm_state *g)
{
	struct libinput_event *ev;

	while ((ev = libinput_get_event(g->libin)) != NULL) {
		enum libinput_event_type t = libinput_event_get_type(ev);
		switch (t) {
		case LIBINPUT_EVENT_KEYBOARD_KEY:
			keyboard_handle_key(ev, &g->egl);
			break;
		case LIBINPUT_EVENT_POINTER_MOTION:
			pointer_handle_motion(ev, &g->egl);
			break;
		case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
			pointer_handle_motion_abs(ev, &g->egl);
			break;
		case LIBINPUT_EVENT_POINTER_BUTTON:
			pointer_handle_button(ev, &g->egl);
			break;
		case LIBINPUT_EVENT_POINTER_AXIS:
			pointer_handle_axis(ev, &g->egl);
			break;
		case LIBINPUT_EVENT_TOUCH_FRAME:
			touch_frame_cb(&g->egl);
			break;
		case LIBINPUT_EVENT_TOUCH_CANCEL:
			touch_cancel_cb(&g->egl);
			break;
		case LIBINPUT_EVENT_TOUCH_DOWN:
			touch_handle_down(ev, g->mode.hdisplay,
					  g->mode.vdisplay, &g->egl);
			break;
		case LIBINPUT_EVENT_TOUCH_MOTION:
			touch_handle_motion(ev, g->mode.hdisplay,
					    g->mode.vdisplay, &g->egl);
			break;
		case LIBINPUT_EVENT_TOUCH_UP:
			touch_handle_up(ev, &g->egl);
			break;
		default:
			break;
		}
		libinput_event_destroy(ev);
	}
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

void rvgpu_gbm_swap(void *param, bool vsync)
{
	struct rvgpu_egl_state *egl = (struct rvgpu_egl_state *)param;
	struct rvgpu_gbm_state *g = to_gbm(egl);
	unsigned int fb;
	struct gbm_bo *bo;
	unsigned int handle, stride;

	if (g->flip_pending)
		return;

	eglSwapBuffers(egl->dpy, egl->sfc);

	bo = gbm_surface_lock_front_buffer(g->gbm_surface);
	handle = gbm_bo_get_handle(bo).u32;
	stride = gbm_bo_get_stride(bo);

	drmModeAddFB(g->gbm_fd, g->mode.hdisplay, g->mode.vdisplay, 24, 32,
		     stride, handle, &fb);
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

static void *event_loop(void *arg)
{
	struct rvgpu_egl_state *e = (struct rvgpu_egl_state *)arg;
	struct rvgpu_gbm_state *g = to_gbm(e);
	struct pollfd fds[2];
	fds[0].fd = g->gbm_fd;
	fds[0].events = POLLIN;
	fds[1].fd = libinput_get_fd(g->libin);
	fds[1].events = POLLIN;
	while (1) {
		int ret = poll(fds, 2, -1);
		if (ret < 0) {
			perror("poll");
			break;
		}
		if (fds[0].revents)
			drmHandleEvent(g->gbm_fd, &gbm_evctx);

		if (fds[1].revents) {
			libinput_dispatch(g->libin);
			rvgpu_gbm_input(g);
		}
	}
	return NULL;
}

/*
 * These are drmModeDumbBuffer*() functions from libdrm that were added in
 * libdrm-2.4.114 (see 3be3b1a8).  This is to compile everything on
 * Ubuntu 20.04
 */
static inline int DRM_IOCTL_COMPAT(int fd, unsigned long cmd, void *arg)
{
	int ret = drmIoctl(fd, cmd, arg);
	return ret < 0 ? -errno : ret;
}

static int
drmModeCreateDumbBufferCompat(int fd, uint32_t width, uint32_t height, uint32_t bpp,
                        uint32_t flags, uint32_t *handle, uint32_t *pitch,
                        uint64_t *size)
{
	int ret;
	struct drm_mode_create_dumb create = {
		.width = width,
		.height = height,
		.bpp = bpp,
		.flags = flags,
	};

	ret = DRM_IOCTL_COMPAT(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create);
	if (ret != 0)
		return ret;

	*handle = create.handle;
	*pitch = create.pitch;
	*size = create.size;
	return 0;
}

static int
drmModeMapDumbBufferCompat(int fd, uint32_t handle, uint64_t *offset)
{
	int ret;
	struct drm_mode_map_dumb map = {
		.handle = handle,
	};

	ret = DRM_IOCTL_COMPAT(fd, DRM_IOCTL_MODE_MAP_DUMB, &map);
	if (ret != 0)
		return ret;

	*offset = map.offset;
	return 0;
}

static int rvgpu_cursor_init(struct rvgpu_gbm_state *g)
{
	uint64_t value;
	uint32_t pitch;
	uint64_t offset;
	int err;

	g->cursor_w = 64;
	g->cursor_h = 64;

	err = drmGetCap(g->gbm_fd, DRM_CAP_CURSOR_WIDTH, &value);
	if (!err) {
		g->cursor_w = value;
	}

	err = drmGetCap(g->gbm_fd, DRM_CAP_CURSOR_HEIGHT, &value);
	if (!err) {
		g->cursor_h = value;
	}

	err = drmModeCreateDumbBufferCompat(g->gbm_fd, g->cursor_w,
					    g->cursor_h, 32, 0,
					    &g->cursor_handle, &pitch,
					    &g->cursor_size);
	if (err)
		return err;

	err = drmModeMapDumbBufferCompat(g->gbm_fd, g->cursor_handle, &offset);
	if (err)
		return err;

	g->cursor_map = mmap64(0, g->cursor_size, PROT_READ | PROT_WRITE, MAP_SHARED, g->gbm_fd, offset);
	if (g->cursor_map == MAP_FAILED)
		return -1;

	return 0;
}

#if 0
/*
 * FIXME: Implement rvgpu_gbm_done()
 * and use these finalization functions there
 */

static int
drmModeDestroyDumbBufferCompat(int fd, uint32_t handle)
{
	struct drm_mode_destroy_dumb destroy = {
		.handle = handle,
	};

	return DRM_IOCTL_COMPAT(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
}

static void rvgpu_cursor_done(struct rvgpu_gbm_state *g)
{
	int err;

	err = munmap(g->cursor_map, g->cursor_size);
	if (err)
		return;

	drmModeDestroyDumbBufferCompat(g->gbm_fd, g->cursor_handle);
}
#endif

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

const char *get_gbm_format_name(uint32_t format)
{
	switch (format) {
	case GBM_FORMAT_ARGB8888:
		return "GBM_FORMAT_ARGB8888";
	case GBM_FORMAT_XRGB8888:
		return "GBM_FORMAT_XRGB8888";
	case GBM_FORMAT_RGB565:
		return "GBM_FORMAT_RGB565";
	case GBM_FORMAT_XRGB2101010:
		return "GBM_FORMAT_XRGB2101010";
	case GBM_FORMAT_ARGB2101010:
		return "GBM_FORMAT_ARGB2101010";
	case GBM_FORMAT_YUYV:
		return "GBM_FORMAT_YUYV";
	case GBM_FORMAT_NV12:
		return "GBM_FORMAT_NV12";
	default:
		return "UNKNOWN FORMAT";
	}
}

uint32_t get_gbm_format(struct gbm_device *gbm)
{
	uint32_t formats[] = { GBM_FORMAT_ARGB8888,    GBM_FORMAT_XRGB8888,
			       GBM_FORMAT_RGB565,      GBM_FORMAT_XRGB2101010,
			       GBM_FORMAT_ARGB2101010, GBM_FORMAT_YUYV,
			       GBM_FORMAT_NV12 };
	size_t num_formats = sizeof(formats) / sizeof(formats[0]);
	for (size_t i = 0; i < num_formats; ++i) {
		struct gbm_surface *surface = gbm_surface_create(
			gbm, 128, 128, formats[i],
			GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
		if (surface) {
			printf("%s (0x%x) is supported.\n",
			       get_gbm_format_name(formats[i]), formats[i]);
			gbm_surface_destroy(surface);
			return formats[i];
		} else {
			printf("%s (0x%x) is not supported.\n",
			       get_gbm_format_name(formats[i]), formats[i]);
		}
	}
	return GBM_FORMAT_ARGB8888;
}

void rvgpu_gbm_free(struct rvgpu_egl_state *e)
{
	struct rvgpu_gbm_state *g = to_gbm(e);

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

void *create_gbm_native_display(const char *device)
{
	int drm_fd = open(device, O_RDWR | O_CLOEXEC);
	struct gbm_device *gbm = gbm_create_device(drm_fd);
	void *native_dpy = (void *)gbm;
	close(drm_fd);
	return native_dpy;
}

void destroy_gbm_native_display(void *native_dpy)
{
	struct gbm_device *gbm = (struct gbm_device *)native_dpy;
	if (gbm) {
		gbm_device_destroy(gbm);
	}
}

void *rvgpu_gbm_init(void *params, int *width, int *height)
{
	rvgpu_gbm_params *gbm_params = (rvgpu_gbm_params *)params;
	const char *device = gbm_params->device;
	const char *seat = gbm_params->seat;
	struct rvgpu_gbm_state *g = calloc(1, sizeof(*g));
	drmModeRes *res;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder;

	assert(g);

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
#ifdef EGL_VERSION_GE_1_5
	g->egl.dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, g->gbm_device,
					   NULL);
#else
	PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
			"eglGetPlatformDisplayEXT");
	if (eglGetPlatformDisplayEXT) {
		g->egl.dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_KHR,
						      g->gbm_device, NULL);
	} else {
		g->egl.dpy = eglGetDisplay(g->gbm_device);
	}
#endif
	assert(g->egl.dpy);

	/* GBM requires to use a specific native format */
	g->egl.use_native_format = true;
	g->egl.native_format = get_gbm_format(g->gbm_device);

	rvgpu_egl_init_context(&g->egl);

	g->gbm_surface =
		gbm_surface_create(g->gbm_device, g->mode.hdisplay,
				   g->mode.vdisplay, g->egl.native_format,
				   GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
	assert(g->gbm_surface);
	*width = g->mode.hdisplay;
	*height = g->mode.vdisplay;
	g->egl.sfc = eglCreateWindowSurface(g->egl.dpy, g->egl.config,
					    g->gbm_surface, NULL);

	g->udev = udev_new();
	g->libin = libinput_udev_create_context(&interface, NULL, g->udev);
	libinput_log_set_priority(g->libin, LIBINPUT_LOG_PRIORITY_INFO);
	libinput_udev_assign_seat(g->libin, seat);
	libinput_dispatch(g->libin);

	rvgpu_cursor_init(g);

	pthread_create(&gbm_event_thread, NULL, event_loop, &g->egl);
	return &g->egl;
}
