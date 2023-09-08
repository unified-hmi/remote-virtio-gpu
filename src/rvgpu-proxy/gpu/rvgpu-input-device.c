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
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>

#include <rvgpu-generic/rvgpu-sanity.h>
#include <rvgpu-generic/rvgpu-utils.h>

#include <rvgpu-proxy/gpu/rvgpu-gpu-device.h>
#include <rvgpu-proxy/gpu/rvgpu-input-device.h>

#include <librvgpu/rvgpu-protocol.h>

#define UINPUT_PATH "/dev/uinput"

static const int TOUCH_AXIS_MAX = 4095; /**< Maximum axis dimension */
static const int TOUCH_AXIS_RESOLUTION = 16; /**< Touch points per mm */

/** @brief maximum touch slots for touchscreen emulation */
#define TOUCH_MAX_SLOTS (64)

/**
 * @brief Struct for tracking slot assignment
 */
struct input_slot {
	int8_t src; /**< source of this target slot */
	unsigned int slot; /**< source slot number of this target slot */
};

struct input_device {
	struct rvgpu_backend *backend;
	short int revents[MAX_HOSTS];
	int mouse, mouse_abs, keyboard, touch;
	unsigned int slot;
	struct input_slot slots[TOUCH_MAX_SLOTS];
	unsigned int src_slots[MAX_HOSTS];
	uint32_t src_window_id[MAX_HOSTS];
	uint16_t tracking_id;
	uint32_t window_id;
};

struct input_device_init {
	unsigned long ioctl_num;
	unsigned long ioctl_value;
};

static const struct input_device_init mouse[] = {
	/* mouse buttons */
	{ UI_SET_EVBIT, EV_KEY },
	{ UI_SET_KEYBIT, BTN_LEFT },
	{ UI_SET_KEYBIT, BTN_RIGHT },
	{ UI_SET_KEYBIT, BTN_MIDDLE },
	/* mouse movements */
	{ UI_SET_EVBIT, EV_REL },
	{ UI_SET_RELBIT, REL_X },
	{ UI_SET_RELBIT, REL_Y },
	/* wheel */
	{ UI_SET_RELBIT, REL_WHEEL },
	{ UI_SET_RELBIT, REL_HWHEEL },
};

static const struct input_device_init mouse_abs[] = {
	/* absolute from QEMU */
	{ UI_SET_EVBIT, EV_KEY },
	{ UI_SET_KEYBIT, BTN_LEFT },
	{ UI_SET_KEYBIT, BTN_RIGHT },
	{ UI_SET_KEYBIT, BTN_MIDDLE },
	{ UI_SET_EVBIT, EV_ABS },
	{ UI_SET_ABSBIT, ABS_X },
	{ UI_SET_ABSBIT, ABS_Y },
};

/* Emulate touchscreen */
static const struct input_device_init touch[] = {
	/* touchscreen buttons */
	{ UI_SET_EVBIT, EV_KEY },
	{ UI_SET_KEYBIT, BTN_TOUCH },
	/* touch abs movements */
	{ UI_SET_EVBIT, EV_ABS },
	{ UI_SET_PROPBIT, INPUT_PROP_DIRECT },
};

/* Emulate keyboard */
static const struct input_device_init keyboard[] = {
	/* keyboard button */
	{ UI_SET_EVBIT, EV_KEY },
};

int input_wait(struct input_device *inpdev)
{
	struct rvgpu_backend *b = inpdev->backend;

	short int events[b->plugin_v1.ctx.scanout_num];

	memset(inpdev->revents, 0,
	       sizeof(short int) * b->plugin_v1.ctx.scanout_num);
	memset(events, POLLIN,
	       sizeof(short int) * b->plugin_v1.ctx.scanout_num);

	return b->plugin_v1.ops.rvgpu_ctx_poll(&b->plugin_v1.ctx, COMMAND, -1,
					       events, inpdev->revents);
}

int input_read(struct input_device *inpdev, void *buf, const size_t len,
	       uint8_t *src)
{
	struct rvgpu_backend *b = inpdev->backend;
	ssize_t ret = 0;

	input_wait(inpdev);

	for (int i = 0; i < b->plugin_v1.ctx.scanout_num; i++) {
		if (inpdev->revents[i] & POLLIN) {
			struct rvgpu_scanout *s = &b->plugin_v1.scanout[i];
			ssize_t ret = s->plugin_v1.ops.rvgpu_recv_all(
				s, COMMAND, buf, len);
			if (ret > 0) {
				if (src)
					*src = i;
				return ret;
			}
		}
	}
	return ret;
}

static int setup_touch_axis(int fd)
{
	const struct uinput_abs_setup axis[] = {
		/* X axis */
		{
			.code = ABS_MT_POSITION_X,
			.absinfo =
				{
					.minimum = 0,
					.maximum = TOUCH_AXIS_MAX,
					.resolution = TOUCH_AXIS_RESOLUTION,
				},
		},
		{
			.code = ABS_X,
			.absinfo =
				{
					.minimum = 0,
					.maximum = TOUCH_AXIS_MAX,
					.resolution = TOUCH_AXIS_RESOLUTION,
				},
		},
		/* Y axis */
		{
			.code = ABS_MT_POSITION_Y,
			.absinfo =
				{
					.minimum = 0,
					.maximum = TOUCH_AXIS_MAX,
					.resolution = TOUCH_AXIS_RESOLUTION,
				},
		},
		{
			.code = ABS_Y,
			.absinfo =
				{
					.minimum = 0,
					.maximum = TOUCH_AXIS_MAX,
					.resolution = TOUCH_AXIS_RESOLUTION,
				},
		},
		/* MT Slot */
		{
			.code = ABS_MT_SLOT,
			.absinfo =
				{
					.minimum = 0,
					.maximum = TOUCH_MAX_SLOTS - 1,
				},
		},
		{
			.code = ABS_MT_TRACKING_ID,
			.absinfo =
				{
					.minimum = 0,
					.maximum = 0xFFFF,
				},
		},
		{
			.code = ABS_MISC,
			.absinfo =
				{
					.minimum = 0,
					.maximum = INT32_MAX,
				},
		},
	};
	for (size_t i = 0; i < ARRAY_SIZE(axis); i++) {
		if (ioctl(fd, UI_ABS_SETUP, &axis[i]) == -1)
			return -1;
	}
	return 0;
}

static int setup_keyboard_keys(int fd)
{
	for (size_t i = 1; i < 195; i++) {
		if (ioctl(fd, UI_SET_KEYBIT, i) == -1)
			return -1;
	}
	return 0;
}

static int setup_mouse_abs(int fd)
{
	static struct uinput_abs_setup ioctl_data = {
		.absinfo = {
			.minimum = 0,
			.maximum = 65535,
		},
	};

	ioctl_data.code = ABS_X;
	if (ioctl(fd, UI_ABS_SETUP, &ioctl_data) == -1)
		return -1;

	ioctl_data.code = ABS_Y;
	if (ioctl(fd, UI_ABS_SETUP, &ioctl_data) == -1)
		return -1;

	return 0;
}

static int create_input_device(const struct input_device_init initctl[],
			       size_t n, const char name[],
			       int (*add_setup)(int fd))
{
	int result = open(UINPUT_PATH, O_WRONLY | O_NONBLOCK);
	int saveerrno;
	struct uinput_setup setup = { .id = {
					      .bustype = BUS_VIRTUAL,
					      .vendor = 1,
					      .product = 1,
					      .version = 1,
				      } };
	if (result == -1)
		return -1;
	if (ioctl(result, UI_SET_EVBIT, EV_SYN) == -1)
		goto err_close;

	for (size_t i = 0; i < n; i++) {
		if (ioctl(result, initctl[i].ioctl_num,
			  initctl[i].ioctl_value) == -1)
			goto err_close;
	}
	strncpy(setup.name, name, sizeof(setup.name) - 1);

	if (add_setup != NULL && (add_setup(result) == -1))
		goto err_close;

	if (ioctl(result, UI_DEV_SETUP, &setup) == -1)
		goto err_close;

	if (ioctl(result, UI_DEV_CREATE) == -1)
		goto err_close;

	return result;
err_close:
	saveerrno = errno;
	close(result);
	errno = saveerrno;
	return -1;
}

struct input_device *input_device_init(struct rvgpu_backend *b)
{
	struct input_device *g;

	g = calloc(1, sizeof(*g));
	assert(g);

	g->mouse = create_input_device(mouse, ARRAY_SIZE(mouse), "rvgpu_mouse",
				       NULL);
	if (g->mouse == -1)
		goto err_free;

	g->mouse_abs = create_input_device(mouse_abs, ARRAY_SIZE(mouse_abs), "rvgpu_mouse_abs",
				       setup_mouse_abs);
	if (g->mouse_abs == -1)
		goto err_close_mouse;

	g->touch = create_input_device(touch, ARRAY_SIZE(touch), "rvgpu_touch",
				       setup_touch_axis);
	if (g->touch == -1)
		goto err_close_mouse_abs;

	g->keyboard =
		create_input_device(keyboard, ARRAY_SIZE(keyboard),
				    "rvgpu_keyboard", setup_keyboard_keys);
	if (g->keyboard == -1)
		goto err_close_touch;

	for (size_t i = 0; i < TOUCH_MAX_SLOTS; i++)
		g->slots[i].src = -1;

	g->backend = b;

	return g;
err_close_touch:
	close(g->touch);
err_close_mouse_abs:
	close(g->mouse_abs);
err_close_mouse:
	close(g->mouse);
err_free:
	free(g);
	return NULL;
}

static void free_slot(struct input_device *g, unsigned int slot)
{
	g->slots[slot].src = -1;
}

static unsigned int get_slot(struct input_device *g, uint8_t src,
			     unsigned int src_slot)
{
	int first_free = -1;

	assert(src < MAX_HOSTS);
	g->src_slots[src] = src_slot;
	for (unsigned int i = 0; i < TOUCH_MAX_SLOTS; i++) {
		if (g->slots[i].src == src && g->slots[i].slot == src_slot)
			return i;

		if (first_free == -1 && g->slots[i].src == -1)
			first_free = (int)i;
	}

	/* TODO: what to do if all slots are occupied? */
	assert(first_free >= 0);
	g->slots[first_free].slot = src_slot;
	g->slots[first_free].src = (int8_t)src;
	return (unsigned int)first_free;
}

static unsigned int get_current_slot(struct input_device *g, uint8_t src)
{
	assert(src < MAX_HOSTS);
	return get_slot(g, src, g->src_slots[src]);
}

static void touch_translate(struct input_device *g, uint8_t src,
			    struct input_event ie[], size_t *ev, size_t max)
{
	size_t i = *ev;
	unsigned int slot;
	uint32_t window_id = g->src_window_id[src];

	if (ie[i].code == ABS_MT_SLOT)
		slot = get_slot(g, src, (uint32_t)ie[i].value);
	else
		slot = get_current_slot(g, src);

	if (ie[i].code == ABS_MISC) {
		window_id = (uint32_t)ie[i].value;
		g->src_window_id[src] = window_id;
	}

	if (window_id != g->window_id) {
		if (ie[i].code != ABS_MISC && (i + 1) < max) {
			/* insert new ABS_MISC event BEFORE current event */
			memmove(&ie[i + 1], &ie[i], sizeof(ie[0]));
			ie[i].code = ABS_MISC;
			ie[i].value = (int32_t)window_id;
			i++;
		}
		g->window_id = window_id;
	}

	if (slot != g->slot) {
		if (ie[i].code == ABS_MT_SLOT) {
			ie[i].value = (int32_t)slot;
		} else if (i + 1 < max) {
			/* insert new MT_SLOT event BEFORE current event */
			memmove(&ie[i + 1], &ie[i], sizeof(ie[0]));
			ie[i].code = ABS_MT_SLOT;
			ie[i].value = (int32_t)slot;
			i++;
		}
		g->slot = slot;
	}

	if (ie[i].code == ABS_MT_TRACKING_ID) {
		if (ie[i].value == -1) {
			/* allow slot reusing */
			free_slot(g, slot);
		} else {
			/* tracking id translation */
			ie[i].value = (uint16_t)(g->tracking_id++);
		}
	}
	i++;
	if (i < max)
		*ev = i;
}

void input_device_serve(struct input_device *g,
			const struct rvgpu_input_header *hdr,
			const struct rvgpu_input_event *event)

{
	int fd;
	struct input_event ie[UINT16_MAX];
	size_t ev = 0;

	switch (hdr->dev) {
	case RVGPU_INPUT_MOUSE:
		fd = g->mouse;
		break;
	case RVGPU_INPUT_MOUSE_ABS:
		fd = g->mouse_abs;
		break;
	case RVGPU_INPUT_KEYBOARD:
		fd = g->keyboard;
		break;
	case RVGPU_INPUT_TOUCH:
		fd = g->touch;
		break;
	default:
		fd = -1;
		break;
	}

	memset(ie, 0, hdr->evnum * sizeof(ie[0]));

	for (size_t i = 0; i < hdr->evnum; i++) {
		ie[ev].code = event[i].code;
		ie[ev].type = event[i].type;
		ie[ev].value = event[i].value;
		if (ie[ev].type == EV_ABS && hdr->dev == RVGPU_INPUT_TOUCH)
			touch_translate(g, hdr->src, ie, &ev, UINT16_MAX);
		else
			ev++;
	}
	if (fd != -1) {
		if (write(fd, ie, ev * sizeof(ie[0])) <
		    (ssize_t)(ev * sizeof(ie[0]))) {
			warn("short write");
		}
	}
}

static void free_device(int fd)
{
	if (fd != -1) {
		ioctl(fd, UI_DEV_DESTROY);
		close(fd);
	}
}

void input_device_free(struct input_device *g)
{
	if (!g)
		return;
	free_device(g->mouse);
	free_device(g->mouse_abs);
	free_device(g->keyboard);
	free_device(g->touch);
	free(g);
}
