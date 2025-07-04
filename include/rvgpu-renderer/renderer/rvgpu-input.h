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

#ifndef RVGPU_INPUT_H
#define RVGPU_INPUT_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/rvgpu-renderer.h>

#define RVGPU_TOUCH_DOWN_EVENT_ID 0
#define RVGPU_TOUCH_UP_EVENT_ID 1
#define RVGPU_TOUCH_MOTION_EVENT_ID 2
#define RVGPU_TOUCH_FRAME_EVENT_ID 3
#define RVGPU_TOUCH_CANCEL_EVENT_ID 4
#define RVGPU_POINTER_ENTER_EVENT_ID 5
#define RVGPU_POINTER_LEAVE_EVENT_ID 6
#define RVGPU_POINTER_MOTION_EVENT_ID 7
#define RVGPU_POINTER_BUTTON_EVENT_ID 8
#define RVGPU_POINTER_AXIS_EVENT_ID 9
#define RVGPU_KEYBOARD_EVENT_ID 10

struct rvgpu_input_state;

/**
 * @brief Initialize new input state
 * @return pointer to initialized state
 */
struct rvgpu_input_state *rvgpu_in_init(FILE *stream);

/**
 * @brief Free resources allocated in rvgpu_in_init
 * @param in - pointer to initialized state
 */
void rvgpu_in_free(struct rvgpu_input_state *in);

/**
 * @brief Queue events for sending
 * @param in - pointer to initialized state
 * @param dev - type of input device
 * @param ev - array of events to queue
 * @param nev - number of events to queue
 */
void rvgpu_in_events(struct rvgpu_input_state *in, enum rvgpu_input_dev dev,
		     const struct rvgpu_input_event ev[], size_t nev);

/**
 * @brief Clear all queued events without sending
 * @param in - pointer to initialized state
 * @param dev - type of input device
 */
void rvgpu_in_clear(struct rvgpu_input_state *in, enum rvgpu_input_dev dev);

/**
 * @brief Send all queued events immediately
 * @param in - pointer to initialized state
 * @param dev - type of input device
 */
void rvgpu_in_send(struct rvgpu_input_state *in, enum rvgpu_input_dev dev);

/* Touch slot support */

/**
 * @brief Adds tracking ID to a new slot with linear trasformation
 * @param in - pointer to initialized state
 * @param id - tracking id of the new slot
 * @param window_id - source window id (or scanout_id for regular scanouts)
 * @param window_box - dimensions of display window
 * @param frame_box - dimensions of frame scanout
 * @param scanout_box - dimensions of scanout
 */
void rvgpu_in_add_slot(struct rvgpu_input_state *in, int32_t id,
		       uint32_t window_id, const struct rvgpu_box *window_box,
		       const struct rvgpu_box *frame_box,
		       const struct rvgpu_box *scanout_box);

/**
 * @brief Remove tracking ID from its slot
 * @param in - pointer to initialized state
 * @param id - tracking id of the removed slot
 */
void rvgpu_in_remove_slot(struct rvgpu_input_state *in, int32_t id);

/**
 * @brief Issue move events within the slot
 * @param in - pointer to initialized state
 * @param id - tracking id of the moved slot
 * @param x - x in window coordinates system
 * @param y - y in window coordinates system
 */
void rvgpu_in_move_slot(struct rvgpu_input_state *in, int32_t id, double x,
			double y);

void touch_down_cb(int32_t id, double x, double y, struct rvgpu_egl_state *egl);
void touch_up_cb(int32_t id, struct rvgpu_egl_state *egl);
void touch_motion_cb(int32_t id, double x, double y,
		     struct rvgpu_egl_state *egl);
void touch_frame_cb(struct rvgpu_egl_state *egl);
void touch_cancel_cb(struct rvgpu_egl_state *egl);

void pointer_inout_cb(double x, double y, struct rvgpu_egl_state *egl);
void pointer_motion_cb(double x, double y, struct rvgpu_egl_state *egl);
void pointer_button_cb(uint32_t button, uint32_t state,
		       struct rvgpu_egl_state *egl);
void pointer_axis_cb(uint32_t axis, uint32_t value,
		     struct rvgpu_egl_state *egl);

void keyboard_cb(uint32_t key, uint32_t state, struct rvgpu_egl_state *egl);

#endif /* RVGPU_INPUT_H */
