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

#ifndef RVGPU_PROTOCOL_H
#define RVGPU_PROTOCOL_H

/* All the fields in the protocol are in host endianness, so it only
 * works between SoCs with the same endianness */

/*
 * All timestamps are from CLOCK_REALTIME.
 */
#include <stdint.h>

/*
 * rvgpu-proxy -> rvgpu-renderer protocol (OpenGL commands forwarding)
 */

/**
 * @brief Flags for command headers
 */
enum rvgpu_flags {
	RVGPU_IDX = 1 << 0, /**< header.idx field is valid */
	RVGPU_CURSOR = 1 << 4, /**< cursor command */
};

/**
 * @brief Header of every command
 */
struct rvgpu_header {
	uint32_t size; /**< Size of the command */
	uint16_t idx; /**< Source virtio descriptor idx */
	uint16_t flags; /**< Flags (see enum rvgpu_flags) */
};

/**
 * @brief RVGPU patch type
 */
enum rvgpu_patch_type {
	RVGPU_PATCH_RES = 1 << 0, /**< patch contains resource */
};

/**
 * @brief Structure for sending resources (for TRANSFER_TO_HOST_XX)
 */
struct rvgpu_patch {
	uint8_t type; /**< type of RVGPU patch (rvgpu_patch_type) */
	uint32_t offset; /**< offset from start of the resource */
	uint32_t len; /**< length of the patch */
};

/*
 * rvgpu-proxy -> rvgpu-renderer protocol (input events transfer)
 */

/**
 * @brief Type of uinput device
 */
enum rvgpu_input_dev {
	RVGPU_INPUT_MOUSE, /**< mouse emulation */
	RVGPU_INPUT_KEYBOARD, /**< keyboard emulation */
	RVGPU_INPUT_TOUCH, /**< multi touch emulation */
	RVGPU_INPUT_MAX, /**< for boundary check */
};

/**
 * @brief Header of every packet sent
 */
struct rvgpu_input_header {
	int8_t dev; /**< one of enum rvgpu_input_dev */
	uint8_t src; /**< source id for separate src tracking */
	uint16_t evnum; /**< number of events in packet */
};

/**
 * @brief Events coming after header
 */
struct rvgpu_input_event {
	uint16_t type; /**< type of input_event */
	uint16_t code; /**< code of input_event */
	int32_t value; /**< value of input_event */
};

/**
 * @brief Type of resource socket messages
 */
enum rvgpu_res_message_type {
	RVGPU_RES_REQ = 1 << 0, /**< request for resource */
	RVGPU_RES_RESP = 1 << 1, /**< response with the resource */
	RVGPU_RES_NOT = 1 << 2, /**< notification that resource is not needed */
	RVGPU_FENCE = 1 << 3, /**< fence completion notification */
};

/**
 * @brief Header of every packet sent on resource socket
 */
struct rvgpu_res_message_header {
	uint8_t type; /**< type of resource socket message */
	union {
		uint32_t fence_id; /**< fence identificator */
	};
};

/**
 * @brief Flags for window spawn command
 *
 * These flags are used to have a mechanism of windows manipulating on the
 * target side.
 * Source calls the drmModeSetCursor2 function to transfer a spawn window
 * command to the target. The hot_x argument of the drmModeSetCursor2 function
 * is used to pass these flags.
 */
enum rvgpu_spawn_window_flags {
	RVGPU_WINDOW_CREATE = 0x80000001,
	RVGPU_WINDOW_DESTROY,
	RVGPU_WINDOW_UPDATE,
	RVGPU_WINDOW_HIDE,
	RVGPU_WINDOW_SHOW,
	RVGPU_WINDOW_DESTROYALL,
};

#endif /* RVGPU_PROTOCOL_H */
