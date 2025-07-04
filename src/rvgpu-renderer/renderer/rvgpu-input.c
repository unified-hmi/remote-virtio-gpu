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
#include <stdio.h>
#include <string.h>

#include <linux/input.h>
#include <librvgpu/rvgpu-protocol.h>

#include <jansson.h>

#include <rvgpu-renderer/renderer/rvgpu-input.h>
#include <rvgpu-utils/rvgpu-utils.h>
#include <rvgpu-renderer/compositor/rvgpu-compositor.h>

#define MAX_SLOTS (16)

struct rvgpu_input_queue {
	struct rvgpu_input_header hdr;
	struct rvgpu_input_event *events;
	size_t evmax;
};

struct rvgpu_input_slot {
	int32_t id;
	uint32_t window_id;
	double ax, bx, ay, by;
	int last_x, last_y;
};

struct rvgpu_input_state {
	struct rvgpu_input_queue iq[RVGPU_INPUT_MAX];
	struct rvgpu_input_slot slots[MAX_SLOTS];
	struct rvgpu_input_slot olds[MAX_SLOTS];
	int32_t last_slot, old_slot;
	uint32_t last_window_id, old_window_id;
	int32_t track_seq;
	FILE *out;
};

static void update_json_object_for_event(json_t *json_obj, int event_id,
					 double x, double y, uint32_t key,
					 uint32_t value)
{
	json_object_set_new(json_obj, "event_id", json_integer(event_id));
	if (x != -1 && y != -1) {
		json_object_set_new(json_obj, "x", json_real(x));
		json_object_set_new(json_obj, "y", json_real(y));
	}
	if (key != UINT32_MAX || value != UINT32_MAX) {
		json_object_set_new(json_obj, "key", json_integer(key));
		json_object_set_new(json_obj, "value", json_integer(value));
	}
}

static void send_event(int client_rvgpu_fd, json_t *json_obj, int event_id,
		       double x, double y, uint32_t key, uint32_t value)
{
	if (!json_obj) {
		return;
	}
	if (client_rvgpu_fd == -1) {
		return;
	}
	update_json_object_for_event(json_obj, event_id, x, y, key, value);

	char *json_cmd = json_dumps(json_obj, JSON_ENCODE_ANY);
	send_str_with_size(client_rvgpu_fd, json_cmd);
	free(json_cmd);
}

void touch_down_cb(int32_t input_id, double x, double y,
		   struct rvgpu_egl_state *egl)
{
	json_t *json_obj =
		get_focus_rvgpu_json_obj(x, y, egl->draw_list_params);
	json_object_set_new(json_obj, "input_id", json_integer(input_id));
	int client_rvgpu_fd =
		get_rvgpu_client_fd(json_obj, egl->draw_list_params);
	pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
	send_event(client_rvgpu_fd, json_obj, RVGPU_TOUCH_DOWN_EVENT_ID, x, y,
		   -1, -1);
	pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	egl->focus_state.touch_focused_json_obj = json_obj;
	egl->focus_state.keyboard_focused_json_obj = json_obj;
}

void touch_up_cb(int32_t input_id, struct rvgpu_egl_state *egl)
{
	if (egl->focus_state.touch_focused_json_obj != NULL) {
		json_object_set_new(egl->focus_state.touch_focused_json_obj,
				    "input_id", json_integer(input_id));
		int client_rvgpu_fd = get_rvgpu_client_fd(
			egl->focus_state.touch_focused_json_obj,
			egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd,
			   egl->focus_state.touch_focused_json_obj,
			   RVGPU_TOUCH_UP_EVENT_ID, -1, -1, -1, -1);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
		egl->focus_state.touch_focused_json_obj = NULL;
	}
}

void touch_motion_cb(int32_t input_id, double x, double y,
		     struct rvgpu_egl_state *egl)
{
	if (egl->focus_state.touch_focused_json_obj != NULL) {
		json_object_set_new(egl->focus_state.touch_focused_json_obj,
				    "input_id", json_integer(input_id));
		int client_rvgpu_fd = get_rvgpu_client_fd(
			egl->focus_state.touch_focused_json_obj,
			egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd,
			   egl->focus_state.touch_focused_json_obj,
			   RVGPU_TOUCH_MOTION_EVENT_ID, x, y, -1, -1);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	}
}

void touch_frame_cb(struct rvgpu_egl_state *egl)
{
	if (egl->focus_state.touch_focused_json_obj != NULL) {
		int client_rvgpu_fd = get_rvgpu_client_fd(
			egl->focus_state.touch_focused_json_obj,
			egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd,
			   egl->focus_state.touch_focused_json_obj,
			   RVGPU_TOUCH_FRAME_EVENT_ID, -1, -1, -1, -1);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	}
}

void touch_cancel_cb(struct rvgpu_egl_state *egl)
{
	if (egl->focus_state.touch_focused_json_obj != NULL) {
		int client_rvgpu_fd = get_rvgpu_client_fd(
			egl->focus_state.touch_focused_json_obj,
			egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd,
			   egl->focus_state.touch_focused_json_obj,
			   RVGPU_TOUCH_CANCEL_EVENT_ID, -1, -1, -1, -1);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	}
}

void pointer_inout_cb(double x, double y, struct rvgpu_egl_state *egl)
{
	if (x != -1 || y != -1) {
		if (egl->focus_state.pointer_focused_json_obj == NULL) {
			json_t *json_obj = get_focus_rvgpu_json_obj(
				x, y, egl->draw_list_params);
			int client_rvgpu_fd = get_rvgpu_client_fd(
				json_obj, egl->draw_list_params);
			pthread_mutex_lock(
				egl->focus_state.input_send_event_mutex);
			send_event(client_rvgpu_fd, json_obj,
				   RVGPU_POINTER_ENTER_EVENT_ID, x, y, -1, -1);
			pthread_mutex_unlock(
				egl->focus_state.input_send_event_mutex);
		} else if (check_in_rvgpu_surface(
				   egl->focus_state.pointer_focused_json_obj, x,
				   y)) {
			int client_rvgpu_fd = get_rvgpu_client_fd(
				egl->focus_state.pointer_focused_json_obj,
				egl->draw_list_params);
			pthread_mutex_lock(
				egl->focus_state.input_send_event_mutex);
			send_event(client_rvgpu_fd,
				   egl->focus_state.pointer_focused_json_obj,
				   RVGPU_POINTER_ENTER_EVENT_ID, x, y, -1, -1);
			pthread_mutex_unlock(
				egl->focus_state.input_send_event_mutex);
		}
		egl->focus_state.pre_pointer_pos_x = x;
		egl->focus_state.pre_pointer_pos_y = y;
	}
}

void pointer_motion_cb(double x, double y, struct rvgpu_egl_state *egl)
{
	if (egl->focus_state.pointer_focused_json_obj == NULL) {
		json_t *json_obj =
			get_focus_rvgpu_json_obj(x, y, egl->draw_list_params);
		int client_rvgpu_fd =
			get_rvgpu_client_fd(json_obj, egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd, json_obj,
			   RVGPU_POINTER_MOTION_EVENT_ID, x, y, -1, -1);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	} else if (check_in_rvgpu_surface(
			   egl->focus_state.pointer_focused_json_obj, x, y)) {
		int client_rvgpu_fd = get_rvgpu_client_fd(
			egl->focus_state.pointer_focused_json_obj,
			egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd,
			   egl->focus_state.pointer_focused_json_obj,
			   RVGPU_POINTER_MOTION_EVENT_ID, x, y, -1, -1);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	}
	egl->focus_state.pre_pointer_pos_x = x;
	egl->focus_state.pre_pointer_pos_y = y;
}

void pointer_button_cb(uint32_t button, uint32_t state,
		       struct rvgpu_egl_state *egl)
{
	static bool focus = false;
	static uint32_t button_states = 0;
	if (state == 1) {
		button_states |= (1 << (button - 1));
	} else {
		button_states &= ~(1 << (button - 1));
	}

	if (!focus && button_states != 0) {
		focus = true;
		json_t *json_obj = get_focus_rvgpu_json_obj(
			egl->focus_state.pre_pointer_pos_x,
			egl->focus_state.pre_pointer_pos_y,
			egl->draw_list_params);
		egl->focus_state.pointer_focused_json_obj = json_obj;
		egl->focus_state.keyboard_focused_json_obj = json_obj;
	}

	int client_rvgpu_fd =
		get_rvgpu_client_fd(egl->focus_state.pointer_focused_json_obj,
				    egl->draw_list_params);
	pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
	send_event(client_rvgpu_fd, egl->focus_state.pointer_focused_json_obj,
		   RVGPU_POINTER_BUTTON_EVENT_ID, -1, -1, button, state);
	pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);

	if (focus && button_states == 0) {
		focus = false;
		egl->focus_state.pointer_focused_json_obj = NULL;
	}
}

void pointer_axis_cb(uint32_t axis, uint32_t value, struct rvgpu_egl_state *egl)
{
	if (egl->focus_state.pointer_focused_json_obj == NULL) {
		json_t *json_obj = get_focus_rvgpu_json_obj(
			egl->focus_state.pre_pointer_pos_x,
			egl->focus_state.pre_pointer_pos_y,
			egl->draw_list_params);
		int client_rvgpu_fd =
			get_rvgpu_client_fd(json_obj, egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd, json_obj,
			   RVGPU_POINTER_AXIS_EVENT_ID, -1, -1, axis, value);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	} else if (check_in_rvgpu_surface(
			   egl->focus_state.pointer_focused_json_obj,
			   egl->focus_state.pre_pointer_pos_x,
			   egl->focus_state.pre_pointer_pos_y)) {
		int client_rvgpu_fd = get_rvgpu_client_fd(
			egl->focus_state.pointer_focused_json_obj,
			egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd,
			   egl->focus_state.pointer_focused_json_obj,
			   RVGPU_POINTER_AXIS_EVENT_ID, -1, -1, axis, value);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	}
}

void keyboard_cb(uint32_t key, uint32_t state, struct rvgpu_egl_state *egl)
{
	if (egl->focus_state.keyboard_focused_json_obj != NULL) {
		int client_rvgpu_fd = get_rvgpu_client_fd(
			egl->focus_state.keyboard_focused_json_obj,
			egl->draw_list_params);
		pthread_mutex_lock(egl->focus_state.input_send_event_mutex);
		send_event(client_rvgpu_fd,
			   egl->focus_state.keyboard_focused_json_obj,
			   RVGPU_KEYBOARD_EVENT_ID, -1, -1, key, state);
		pthread_mutex_unlock(egl->focus_state.input_send_event_mutex);
	}
}

static int find_slot(const struct rvgpu_input_state *in, int32_t id)
{
	for (size_t i = 0; i < MAX_SLOTS; i++) {
		if (in->slots[i].id == id)
			return (int)i;
	}
	return -1;
}

static int find_free_slot(const struct rvgpu_input_state *in)
{
	for (size_t i = 0; i < MAX_SLOTS; i++) {
		if (in->slots[i].id == -1)
			return (int)i;
	}
	return -1;
}

struct rvgpu_input_state *rvgpu_in_init(FILE *stream)
{
	struct rvgpu_input_state *in = calloc(1, sizeof(*in));

	assert(in);
	in->out = stream;

	for (enum rvgpu_input_dev i = 0; i < RVGPU_INPUT_MAX; i++)
		in->iq[i].hdr.dev = (int8_t)i;

	for (size_t i = 0; i < MAX_SLOTS; i++) {
		in->slots[i] = (struct rvgpu_input_slot){
			.id = -1,
			.last_x = -1,
			.last_y = -1,
		};
		in->olds[i] = in->slots[i];
	}
	in->last_slot = 0;

	return in;
}

void rvgpu_in_free(struct rvgpu_input_state *in)
{
	for (size_t i = 0; i < RVGPU_INPUT_MAX; i++)
		free(in->iq[i].events);
	free(in);
}

void rvgpu_in_add_slot(struct rvgpu_input_state *in, int32_t id,
		       uint32_t window_id, const struct rvgpu_box *window_box,
		       const struct rvgpu_box *frame_box,
		       const struct rvgpu_box *scanout_box)
{
	int slot = find_slot(in, id);
	struct rvgpu_input_slot *s;
	struct rvgpu_input_event evs[] = {
		{ EV_ABS, ABS_MISC, (int32_t)window_id },
		{ EV_ABS, ABS_MT_SLOT, 0 },
		{ EV_ABS, ABS_MT_TRACKING_ID, (uint16_t)(in->track_seq++) },
		{ EV_KEY, BTN_TOUCH, 1 },
	};
	if (slot != -1)
		rvgpu_in_remove_slot(in, id);

	slot = find_free_slot(in);
	if (slot == -1)
		return;

	s = &in->slots[slot];
	s->id = id;
	s->window_id = window_id;
	evs[1].value = slot;

	if (in->last_window_id == window_id)
		rvgpu_in_events(in, RVGPU_INPUT_TOUCH, &evs[1], 3);
	else
		rvgpu_in_events(in, RVGPU_INPUT_TOUCH, evs, 4);

	in->last_slot = slot;
	in->last_window_id = window_id;

	/* Coordinate translation
	 * Terms:
	 *
	 * window_box - width and height of the actual graphical window
	 * Touch events from actual system come in window coordinates.
	 * Top left corner is 0,0 and bottom right is width, height
	 *
	 * scanout_box - width and height of the scanout resource which is
	 * painted in the window partially or fully.
	 * top left corner of scanout_box has final coordinates 0,0 and
	 * bottom right corner has final coordinates 4096,4096
	 *
	 * frame_box - x, y, width and height of part of scanout resource which
	 * is displayed in the graphical window.
	 * Top left corner of frame box corresponds to 0,0 in window coordinates
	 * Bottom right corner of frame box corresponds to window_box.w,
	 * window_box.h in window coordinates
	 *
	 * Final coordinates are values in between 0 and 4096.
	 * final X = ax * x + bx
	 * final Y = ay * y + by
	 */
	s->ax = frame_box->w * 4096.0 / scanout_box->w / window_box->w;
	s->bx = frame_box->x * 4096.0 / scanout_box->w;
	s->ay = frame_box->h * 4096.0 / scanout_box->h / window_box->h;
	s->by = frame_box->y * 4096.0 / scanout_box->h;
}

void rvgpu_in_move_slot(struct rvgpu_input_state *in, int32_t id, double x,
			double y)
{
	int slot = find_slot(in, id);
	struct rvgpu_input_slot *s;
	int new_x, new_y;

	if (slot == -1)
		return;

	s = &in->slots[slot];
	new_x = (int)(x * s->ax + s->bx);
	new_y = (int)(y * s->ay + s->by);

	if (new_x != s->last_x || new_y != s->last_y) {
		if (in->last_window_id != s->window_id) {
			rvgpu_in_events(in, RVGPU_INPUT_TOUCH,
					&(struct rvgpu_input_event){
						EV_ABS, ABS_MISC,
						(int32_t)s->window_id },
					1);
			in->last_window_id = s->window_id;
		}

		if (in->last_slot != slot) {
			rvgpu_in_events(in, RVGPU_INPUT_TOUCH,
					&(struct rvgpu_input_event){
						EV_ABS, ABS_MT_SLOT, slot },
					1);
			in->last_slot = slot;
		}
		if (s->last_x != new_x) {
			rvgpu_in_events(in, RVGPU_INPUT_TOUCH,
					&(struct rvgpu_input_event){
						EV_ABS, ABS_MT_POSITION_X,
						new_x },
					1);
			s->last_x = new_x;
		}
		if (s->last_y != new_y) {
			rvgpu_in_events(in, RVGPU_INPUT_TOUCH,
					&(struct rvgpu_input_event){
						EV_ABS, ABS_MT_POSITION_Y,
						new_y },
					1);
			s->last_y = new_y;
		}
	}
}

void rvgpu_in_remove_slot(struct rvgpu_input_state *in, int32_t id)
{
	int slot = find_slot(in, id);

	if (slot != -1) {
		struct rvgpu_input_event evs[] = {
			{ EV_ABS, ABS_MT_SLOT, slot },
			{ EV_ABS, ABS_MT_TRACKING_ID, -1 },
			{ EV_KEY, BTN_TOUCH, 0 },
		};
		if (in->last_slot == slot) {
			rvgpu_in_events(in, RVGPU_INPUT_TOUCH, &evs[1], 2);
		} else {
			rvgpu_in_events(in, RVGPU_INPUT_TOUCH, evs, 3);
			in->last_slot = slot;
		}
		in->slots[slot].id = -1;
		in->slots[slot].last_x = -1;
		in->slots[slot].last_y = -1;
	}
}

void rvgpu_in_events(struct rvgpu_input_state *in, enum rvgpu_input_dev dev,
		     const struct rvgpu_input_event ev[], size_t nev)
{
	assert(dev < RVGPU_INPUT_MAX);
	assert(dev >= 0);
	struct rvgpu_input_queue *iq = &in->iq[dev];

	if (iq->hdr.evnum + nev > iq->evmax) {
		size_t newmax = iq->hdr.evnum + nev;
		void *p = realloc(iq->events, newmax * sizeof(ev[0]));

		if (p == NULL) {
			warn("cannot realloc queue for %zu elements", newmax);
			return;
		}
		iq->events = p;
		iq->evmax = newmax;
	}
	memcpy(&iq->events[iq->hdr.evnum], ev, nev * sizeof(ev[0]));
	iq->hdr.evnum += nev;
}

void rvgpu_in_send(struct rvgpu_input_state *in, enum rvgpu_input_dev dev)
{
	struct rvgpu_input_event ev = { EV_SYN, SYN_REPORT, 0 };
	struct rvgpu_input_queue *iq;

	assert(dev < RVGPU_INPUT_MAX);
	assert(dev >= 0);
	iq = &in->iq[dev];

	if (iq->hdr.evnum != 0) {
		rvgpu_in_events(in, dev, &ev, 1);
		fwrite(&iq->hdr, sizeof(struct rvgpu_input_header), 1, in->out);
		fwrite(iq->events, sizeof(struct rvgpu_input_event),
		       iq->hdr.evnum, in->out);
		fflush(in->out);
	}
	iq->hdr.evnum = 0;
	if (dev == RVGPU_INPUT_TOUCH) {
		memcpy(in->olds, in->slots, sizeof(in->olds));
		in->old_slot = in->last_slot;
		in->old_window_id = in->last_window_id;
	}
}

void rvgpu_in_clear(struct rvgpu_input_state *in, enum rvgpu_input_dev dev)
{
	assert(dev < RVGPU_INPUT_MAX);
	assert(dev >= 0);
	in->iq[dev].hdr.evnum = 0;
	if (dev == RVGPU_INPUT_TOUCH) {
		in->last_slot = in->old_slot;
		in->last_window_id = in->old_window_id;
		memcpy(in->slots, in->olds, sizeof(in->olds));
	}
}
