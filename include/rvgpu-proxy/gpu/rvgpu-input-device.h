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

#ifndef RVGPU_INPUT_DEVICE_H
#define RVGPU_INPUT_DEVICE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

struct input_device;
struct rvgpu_input_event;
struct rvgpu_input_header;

/**
 * @brief poll for input events
 * @param inpdev - input device
 * @return number of occured events
 */
int input_wait(struct input_device *inpdev);

/**
 * @brief get input events
 * @param inpdev - input device
 * @param buf - pointer to read buffer
 * @param len - size of read buffer
 * @param src - source id
 * @return number of read bytes
 */
int input_read(struct input_device *inpdev, void *buf, const size_t len,
	       uint8_t *src);

/**
 * @brief Initialize input device
 * @param b - pointer to rvgpu backend
 * @return pointer to initialized input device structure
 */
struct input_device *input_device_init(struct rvgpu_backend *b);

/**
 * @brief Process remote input event
 * @param g - pointer to initialized input device structure
 * @param hdr - input event header
 * @param event - remote event structure
 */
void input_device_serve(struct input_device *g,
			const struct rvgpu_input_header *hdr,
			const struct rvgpu_input_event *event);

/**
 * @brief Free input device resources
 * @param g - pointer to initialized input device structure
 */
void input_device_free(struct input_device *g);

#endif /* RVGPU_INPUT_DEVICE_H */
