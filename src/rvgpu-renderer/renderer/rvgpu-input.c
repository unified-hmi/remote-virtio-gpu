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
#include <linux/input.h>
#include <stdio.h>
#include <string.h>

#include <librvgpu/rvgpu-protocol.h>
#include <rvgpu-renderer/renderer/rvgpu-input.h>

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
