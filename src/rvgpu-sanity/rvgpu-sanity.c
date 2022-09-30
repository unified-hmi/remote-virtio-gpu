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

#include <err.h>
#include <stdlib.h>

#include <rvgpu-generic/rvgpu-utils.h>
#include <rvgpu-generic/rvgpu-sanity.h>

unsigned long sanity_strtounum(const char *str, unsigned long min,
			       unsigned long max, char **errstr)
{
	char *endptr;
	unsigned long result = strtoul(str, &endptr, 0);

	if (*endptr != '\0') {
		*errstr = "Invalid number";
		return min;
	}
	if (result < min) {
		*errstr = "Number is too low";
		return min;
	}
	if (result > max) {
		*errstr = "Number is too high";
		return max;
	}
	*errstr = NULL;
	return result;
}

signed long sanity_strtonum(const char *str, signed long min, signed long max,
			    char **errstr)
{
	char *endptr;
	signed long result = strtol(str, &endptr, 0);

	if (*endptr != '\0') {
		*errstr = "Invalid number";
		return min;
	}
	if (result < min) {
		*errstr = "Number is too low";
		return min;
	}
	if (result > max) {
		*errstr = "Number is too high";
		return max;
	}
	*errstr = NULL;
	return result;
}

static const struct {
	unsigned int type;
	const char *name;
} virtio_gpu_commands[] = {
	{ VIRTIO_GPU_CMD_GET_DISPLAY_INFO, "GET_DISPLAY_INFO" },
	{ VIRTIO_GPU_CMD_RESOURCE_CREATE_2D, "RESOURCE_CREATE_2D" },
	{ VIRTIO_GPU_CMD_RESOURCE_UNREF, "RESOURCE_UNREF" },
	{ VIRTIO_GPU_CMD_SET_SCANOUT, "SET_SCANOUT" },
	{ VIRTIO_GPU_CMD_RESOURCE_FLUSH, "RESOURCE_FLUSH" },
	{ VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D, "TRANSFER_TO_HOST_2D" },
	{ VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING, "RESOURCE_ATTACH_BACKING" },
	{ VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING, "RESOURCE_DETACH_BACKING" },
	{ VIRTIO_GPU_CMD_GET_CAPSET_INFO, "GET_CAPSET_INFO" },
	{ VIRTIO_GPU_CMD_GET_CAPSET, "GET_CAPSET" },

	/* 3d commands */
	{ VIRTIO_GPU_CMD_CTX_CREATE, "CTX_CREATE" },
	{ VIRTIO_GPU_CMD_CTX_DESTROY, "CTX_DESTROY" },
	{ VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, "CTX_ATTACH_RESOURCE" },
	{ VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, "CTX_DETACH_RESOURCE" },
	{ VIRTIO_GPU_CMD_RESOURCE_CREATE_3D, "RESOURCE_CREATE_3D" },
	{ VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D, "TRANSFER_TO_HOST_3D" },
	{ VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D, "TRANSFER_FROM_HOST_3D" },
	{ VIRTIO_GPU_CMD_SUBMIT_3D, "SUBMIT_3D" },

	/* Cursor commands */
	{ VIRTIO_GPU_CMD_MOVE_CURSOR, "MOVE_CURSOR" },
	{ VIRTIO_GPU_CMD_UPDATE_CURSOR, "UPDATE_CURSOR" },
};

const char *sanity_cmd_by_type(unsigned int type)
{
	unsigned int i;

	for (i = 0u; i < ARRAY_SIZE(virtio_gpu_commands); i++) {
		if (virtio_gpu_commands[i].type == type)
			return virtio_gpu_commands[i].name;
	}
	return "UNKNOWN";
}

static bool sanity_check_bounds(uint32_t x, uint32_t y, uint32_t width,
				uint32_t height)
{
	if (x > INT32_MAX || y > INT32_MAX)
		return false;
	if (width == 0 || width > INT32_MAX)
		return false;
	if (height == 0 || height > INT32_MAX)
		return false;

	if ((uint64_t)height * width * 4u > INT32_MAX)
		return false;

	return true;
}

static inline bool sanity_check_rect(const struct virtio_gpu_rect *r)
{
	return sanity_check_bounds(r->x, r->y, r->width, r->height);
}

static inline bool sanity_check_box(const struct virtio_gpu_box *b)
{
	if (b->z > INT32_MAX || b->d > INT32_MAX)
		return false;
	return sanity_check_bounds(b->x, b->y, b->w, b->h);
}

bool sanity_check_resource_rect(const struct virtio_gpu_rect *r, uint32_t width,
				uint32_t height)
{
	if ((uint64_t)r->x + r->width > width)
		return false;
	if ((uint64_t)r->y + r->height > height)
		return false;
	return true;
}

bool sanity_check_resource_box(const struct virtio_gpu_box *b, uint32_t width,
			       uint32_t height, uint32_t depth)
{
	if ((uint64_t)b->x + b->w > width)
		return false;
	if ((uint64_t)b->y + b->h > height)
		return false;
	if ((uint64_t)b->z + b->d > depth)
		return false;
	return true;
}

enum virtio_gpu_ctrl_type sanity_check_gpu_ctrl(const union virtio_gpu_cmd *cmd,
						size_t size, bool strict)
{
	if (size < sizeof(cmd->hdr))
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	if (size > sizeof(*cmd))
		return VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY;

	switch (cmd->hdr.type) {
	/* 2d commands */
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
		if (size != sizeof(cmd->hdr))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;

	case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D:
		if (size != sizeof(cmd->r_c2d))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->r_c2d.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		if (!sanity_check_bounds(0, 0, cmd->r_c2d.width,
					 cmd->r_c2d.height))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		switch ((enum virtio_gpu_formats)cmd->r_c2d.format) {
		case VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM:
		case VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM:
		case VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM:
		case VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM:

		case VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM:
		case VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM:

		case VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM:
		case VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM:
			break;
		default:
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;
		}
		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_RESOURCE_UNREF:
		if (size != sizeof(cmd->r_unref))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->r_unref.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_SET_SCANOUT:
		if (size != sizeof(cmd->s_set))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->s_set.scanout_id >= VIRTIO_GPU_MAX_SCANOUTS)
			return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;

		if (cmd->s_set.resource_id != 0 &&
		    !sanity_check_rect(&cmd->s_set.r))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_RESOURCE_FLUSH:
		if (size != sizeof(cmd->r_flush))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (strict && cmd->r_flush.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		if (!sanity_check_rect(&cmd->r_flush.r))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D:
		if (size != sizeof(cmd->t_2h2d))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->t_2h2d.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		if (!sanity_check_rect(&cmd->t_2h2d.r))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING:
		if (size <= sizeof(cmd->r_att))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->r_att.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		if (size != (sizeof(cmd->r_att) +
			     (cmd->r_att.nr_entries *
			      sizeof(struct virtio_gpu_mem_entry))))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING:
		if (size != sizeof(cmd->r_det))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->r_det.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
		if (size != sizeof(cmd->capset_info))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_GET_CAPSET:
		if (size != sizeof(cmd->capset))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;

	/* 3d commands */
	case VIRTIO_GPU_CMD_CTX_CREATE:
		if (size != sizeof(cmd->c_create))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->hdr.ctx_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;

		if (cmd->c_create.nlen > sizeof(cmd->c_create.debug_name))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_CTX_DESTROY:
		if (size != sizeof(cmd->c_destroy))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->hdr.ctx_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
	case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
		if (size != sizeof(cmd->c_res))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->hdr.ctx_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;

		if (strict && cmd->c_res.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
		if (size != sizeof(cmd->r_c3d))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->r_c3d.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		if (!sanity_check_bounds(0, 0, cmd->r_c3d.width,
					 cmd->r_c3d.height))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		/* TODO: add more checks */
		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
	case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
		if (size != sizeof(cmd->t_h3d))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->hdr.ctx_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;

		if (cmd->t_h3d.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;

		if (!sanity_check_box(&cmd->t_h3d.box))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;
	case VIRTIO_GPU_CMD_SUBMIT_3D:
		if (size <= sizeof(cmd->c_submit))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		if (cmd->hdr.ctx_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID;

		if (size != (cmd->c_submit.size + sizeof(cmd->c_submit)))
			return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

		return VIRTIO_GPU_RESP_OK_NODATA;

	/* cursor commands unsupported */
	case VIRTIO_GPU_CMD_UPDATE_CURSOR:
	case VIRTIO_GPU_CMD_MOVE_CURSOR:
	case VIRTIO_GPU_UNDEFINED:
	default:
		return VIRTIO_GPU_RESP_ERR_UNSPEC;
	}
}

enum virtio_gpu_ctrl_type
sanity_check_gpu_cursor(const union virtio_gpu_cmd *cmd, size_t size,
			bool strict)
{
	(void)strict;
	if (size != sizeof(cmd->cursor))
		return VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER;

	switch (cmd->hdr.type) {
	case VIRTIO_GPU_CMD_UPDATE_CURSOR:
		if (cmd->cursor.resource_id == 0)
			return VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID;
		/* FALLTHROUGH */
	case VIRTIO_GPU_CMD_MOVE_CURSOR:
		if (cmd->cursor.pos.scanout_id >= VIRTIO_GPU_MAX_SCANOUTS)
			return VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID;

		return VIRTIO_GPU_RESP_OK_NODATA;

	default:
		return VIRTIO_GPU_RESP_ERR_UNSPEC;
	}
}
