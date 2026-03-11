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

#ifndef RVGPU_SANITY_H
#define RVGPU_SANITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/virtio_gpu.h>

#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))

#define VIRGL_CCMD_RESOURCE_INLINE_WRITE 9
#define VIRGL_CCMD_TRANSFER3D 43
#define VIRGL_CCMD_COPY_TRANSFER3D 45

#define VIRGL_TRANSFER3D_SIZE 13
#define VIRGL_COPY_TRANSFER3D_SIZE 14

#define VIRGL_COPY_TRANSFER3D_SRC_RES_HANDLE 12
#define VIRGL_COPY_TRANSFER3D_SRC_RES_OFFSET 13

#define VIRGL_RESOURCE_IW_RES_HANDLE 1
#define VIRGL_RESOURCE_IW_LEVEL 2
#define VIRGL_RESOURCE_IW_USAGE 3
#define VIRGL_RESOURCE_IW_STRIDE 4
#define VIRGL_RESOURCE_IW_LAYER_STRIDE 5
#define VIRGL_RESOURCE_IW_X 6
#define VIRGL_RESOURCE_IW_Y 7
#define VIRGL_RESOURCE_IW_Z 8
#define VIRGL_RESOURCE_IW_W 9
#define VIRGL_RESOURCE_IW_H 10
#define VIRGL_RESOURCE_IW_D 11
#define VIRGL_RESOURCE_IW_DATA_START 12

#define VIRGL_MAX_TBUF_DWORDS 1024
#define VIRGL_MAX_CMDBUF_DWORDS ((64 * 1024) + VIRGL_MAX_TBUF_DWORDS)

union virtio_gpu_cmd {
	struct virtio_gpu_ctrl_hdr hdr;
	struct virtio_gpu_resource_unref r_unref;
	struct virtio_gpu_resource_create_2d r_c2d;
	struct virtio_gpu_set_scanout s_set;
	struct virtio_gpu_resource_flush r_flush;
	struct virtio_gpu_transfer_to_host_2d t_2h2d;
	struct {
		struct virtio_gpu_resource_attach_backing r_att;
		struct virtio_gpu_mem_entry r_mem[1024];
	};
	struct virtio_gpu_resource_detach_backing r_det;
	struct virtio_gpu_transfer_host_3d t_h3d;
	struct virtio_gpu_resource_create_3d r_c3d;
	struct virtio_gpu_ctx_create c_create;
	struct virtio_gpu_ctx_destroy c_destroy;
	struct virtio_gpu_ctx_resource c_res;
	struct {
		struct virtio_gpu_cmd_submit c_submit;
		uint32_t c_cmdbuf[2];
	};
	struct virtio_gpu_get_capset capset;
	struct virtio_gpu_get_capset_info capset_info;
	struct virtio_gpu_update_cursor cursor;
	char buf[VIRGL_MAX_CMDBUF_DWORDS * 4 + sizeof(struct virtio_gpu_cmd_submit)];
};

/**
 * @brief Safely convert a string to unsigned number
 * @param str - string to convert
 * @param min - minimum value for a number
 * @param max - maximum value for a number
 * @param errstr - error string
 * @retval min in case of error
 * @retval converted value that is in range of [min..max]
 *
 * Converts given string to a number in range of [min..max]
 * If conversion succeeds, sets *errstr to NULL and returns the value
 * In case of error, errstr will hold a human-readable error string
 */
unsigned long sanity_strtounum(const char *str, unsigned long min,
			       unsigned long max, char **errstr);

/**
 * @brief Safely convert a string to signed number
 * @param str - string to convert
 * @param min - minimum value for a number
 * @param max - maximum value for a number
 * @param errstr - error string
 * @retval converted value that is in range of [min..max]
 *
 * Converts given string to a number in range of [min..max]
 * If conversion succeeds, sets *errstr to NULL and returns the value
 * In case of error, errstr will hold a human-readable error string
 */
signed long sanity_strtonum(const char *str, signed long min, signed long max,
			    char **errstr);

/**
 * @brief Check sanity of gpu ctrl command
 * @param cmd - command to check
 * @param size - size of command
 * @return VIRTIO_GPU_RESP_OK_NODATA if OK, valid err response otherwise
 */
enum virtio_gpu_ctrl_type sanity_check_gpu_ctrl(const union virtio_gpu_cmd *cmd,
						size_t size, bool strict);

/**
 * @brief Check sanity of gpu cursor command
 * @param cmd - command to check
 * @param size - size of command
 * @return VIRTIO_GPU_RESP_OK_NODATA if OK, valid err response otherwise
 */
enum virtio_gpu_ctrl_type
sanity_check_gpu_cursor(const union virtio_gpu_cmd *cmd, size_t size,
			bool strict);

/**
 * @brief Check if 3d box falls within width x height x depth
 * @param b - box to check
 * @param width - resource width
 * @param height - resource height
 * @param depth - resource depth
 * @return true if box is within dimensions, false otherwise
 */
bool sanity_check_resource_box(const struct virtio_gpu_box *b, uint32_t width,
			       uint32_t height, uint32_t depth);

/**
 * @brief Check if 2d rect falls within width x height
 * @param r - rect to check
 * @param width - resource width
 * @param height - resource height
 * @return true if rect is within dimensions, false otherwise
 */
bool sanity_check_resource_rect(const struct virtio_gpu_rect *r, uint32_t width,
				uint32_t height);

/**
 * @brief Returns human-readable name of virtio gpu command
 * @param type - type of command
 * @return human readable name of a command or UNKNOWN if unknown
 */
const char *sanity_cmd_by_type(unsigned type);

#endif /* RVGPU_SANITY_H */
