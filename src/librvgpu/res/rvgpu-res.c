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
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>

#include <librvgpu/rvgpu-plugin.h>
#include <librvgpu/rvgpu-protocol.h>
#include <librvgpu/rvgpu.h>
#include <librvgpu/rvgpu-virgl-format.h>

static inline bool virgl_format_is_yuv(uint32_t format)
{
	switch (format) {
	case VIRGL_FORMAT_NV12:
	case VIRGL_FORMAT_P010:
	case VIRGL_FORMAT_YV12:
	case VIRGL_FORMAT_YV16:
	case VIRGL_FORMAT_IYUV:
	case VIRGL_FORMAT_NV21:
		return true;
	default:
		return false;
	}
}

static inline bool virgl_format_is_compressed(uint32_t format)
{
	switch (format) {
	case VIRGL_FORMAT_DXT1_RGB:
	case VIRGL_FORMAT_DXT1_RGBA:
	case VIRGL_FORMAT_DXT3_RGBA:
	case VIRGL_FORMAT_DXT5_RGBA:
	case VIRGL_FORMAT_DXT1_SRGB:
	case VIRGL_FORMAT_DXT1_SRGBA:
	case VIRGL_FORMAT_DXT3_SRGBA:
	case VIRGL_FORMAT_DXT5_SRGBA:
	case VIRGL_FORMAT_RGTC1_UNORM:
	case VIRGL_FORMAT_RGTC1_SNORM:
	case VIRGL_FORMAT_RGTC2_UNORM:
	case VIRGL_FORMAT_RGTC2_SNORM:
	case VIRGL_FORMAT_ETC1_RGB8:
	case VIRGL_FORMAT_ETC2_RGB8:
	case VIRGL_FORMAT_ETC2_SRGB8:
	case VIRGL_FORMAT_ETC2_RGB8A1:
	case VIRGL_FORMAT_ETC2_SRGB8A1:
	case VIRGL_FORMAT_ETC2_RGBA8:
	case VIRGL_FORMAT_ETC2_SRGBA8:
	case VIRGL_FORMAT_ETC2_R11_UNORM:
	case VIRGL_FORMAT_ETC2_R11_SNORM:
	case VIRGL_FORMAT_ETC2_RG11_UNORM:
	case VIRGL_FORMAT_ETC2_RG11_SNORM:
	case VIRGL_FORMAT_ASTC_4x4:
	case VIRGL_FORMAT_ASTC_5x4:
	case VIRGL_FORMAT_ASTC_5x5:
	case VIRGL_FORMAT_ASTC_6x5:
	case VIRGL_FORMAT_ASTC_6x6:
	case VIRGL_FORMAT_ASTC_8x5:
	case VIRGL_FORMAT_ASTC_8x6:
	case VIRGL_FORMAT_ASTC_8x8:
	case VIRGL_FORMAT_ASTC_10x5:
	case VIRGL_FORMAT_ASTC_10x6:
	case VIRGL_FORMAT_ASTC_10x8:
	case VIRGL_FORMAT_ASTC_10x10:
	case VIRGL_FORMAT_ASTC_12x10:
	case VIRGL_FORMAT_ASTC_12x12:
	case VIRGL_FORMAT_ASTC_4x4_SRGB:
	case VIRGL_FORMAT_ASTC_5x4_SRGB:
	case VIRGL_FORMAT_ASTC_5x5_SRGB:
	case VIRGL_FORMAT_ASTC_6x5_SRGB:
	case VIRGL_FORMAT_ASTC_6x6_SRGB:
	case VIRGL_FORMAT_ASTC_8x5_SRGB:
	case VIRGL_FORMAT_ASTC_8x6_SRGB:
	case VIRGL_FORMAT_ASTC_8x8_SRGB:
	case VIRGL_FORMAT_ASTC_10x5_SRGB:
	case VIRGL_FORMAT_ASTC_10x6_SRGB:
	case VIRGL_FORMAT_ASTC_10x8_SRGB:
	case VIRGL_FORMAT_ASTC_10x10_SRGB:
	case VIRGL_FORMAT_ASTC_12x10_SRGB:
	case VIRGL_FORMAT_ASTC_12x12_SRGB:
		return true;
	default:
		return false;
	}
}

struct patch_data {
	struct rvgpu_patch hdr;
	unsigned int niov;
	struct iovec iov[__IOV_MAX];
};

static void write_patch(struct rvgpu_ctx *ctx, struct patch_data *d)
{
	if (d->niov == 1)
		return;
	d->iov[0].iov_base = &d->hdr;
	d->iov[0].iov_len = sizeof(d->hdr);

	for (unsigned int i = 0; i < d->niov; i++) {
		if (rvgpu_ctx_send(ctx, d->iov[i].iov_base,
				   d->iov[i].iov_len)) {
			warn("short write");
		}
	}
}

static void init_patch(struct patch_data *d)
{
	/* iov[0] is reserved for header */
	d->niov = 1u;
}

static void add_patch(struct rvgpu_ctx *ctx, struct patch_data *d,
		      size_t offset, void *data, size_t len)
{
	if (d->niov == __IOV_MAX) {
		write_patch(ctx, d);
		init_patch(d);
	}
	assert(offset < UINT32_MAX);
	if (d->niov == 1) {
		d->hdr.offset = (uint32_t)offset;
		d->hdr.len = 0;
		d->hdr.type = RVGPU_PATCH_RES;
	}
	d->iov[d->niov].iov_base = data;
	d->iov[d->niov].iov_len = len;
	d->hdr.len += len;
	d->niov++;
}

static void gpu_device_send_data(struct rvgpu_ctx *ctx,
				 const struct iovec iovs[], size_t niov,
				 size_t skip, size_t length)
{
	size_t offset = 0u;
	struct patch_data d;

	init_patch(&d);

	for (size_t i = 0u; i < niov && length > 0u; i++) {
		const struct iovec *iov = &iovs[i];

		if (skip >= iov->iov_len) {
			skip -= iov->iov_len;
		} else {
			size_t l = iov->iov_len - skip;

			if (l > length)
				l = length;

			add_patch(ctx, &d, offset + skip,
				  (char *)iov->iov_base + skip, l);
			skip = 0u;
			length -= l;
		}
		offset += iov->iov_len;
	}
	write_patch(ctx, &d);
}

static struct rvgpu_res *gpu_device_get_res(struct ctx_priv *ctx_priv,
					    uint32_t resource_id)
{
	struct rvgpu_res *res;

	LIST_FOREACH(res, &ctx_priv->reslist, entry)
	{
		if (res->resid == resource_id)
			return res;
	}
	return NULL;
}


static inline uint32_t align_up_power_of_2(uint32_t n, uint32_t a)
{
	return (n + (a - 1)) & ~(a - 1);
}

size_t yuv_data_size(uint32_t format, uint32_t width, uint32_t height,
		     uint32_t stride)
{
	uint32_t bpp = (format == VIRGL_FORMAT_P010) ? 2 : 1;
	uint32_t y_align = (format == VIRGL_FORMAT_YV12) ? 32 : 16;
	uint32_t y_stride = (stride != 0) ?
				    stride :
				    align_up_power_of_2(width, y_align) * bpp;
	uint32_t y_size = y_stride * height;

	uint32_t uv_width;
	uint32_t uv_plane_count;
	uint32_t uv_height = height / 2;

	switch (format) {
	case VIRGL_FORMAT_NV12:
	case VIRGL_FORMAT_P010:
		uv_width = width;
		uv_plane_count = 1;
		break;
	case VIRGL_FORMAT_YV12:
		uv_width = width / 2;
		uv_plane_count = 2;
		break;
	default:
		fprintf(stderr, "Unknown yuv virgl format: %s\n",
			get_virgl_format_string(format));
		return 0;
	}

	uint32_t uv_align = 16;
	uint32_t uv_stride = align_up_power_of_2(uv_width, uv_align) * bpp;
	uint32_t uv_size = uv_stride * uv_height * uv_plane_count;

	size_t total_size = y_size + uv_size;
	return total_size;
}

static size_t compressed_data_size(uint32_t format, uint32_t width,
				   uint32_t height)
{
	switch (format) {
	case VIRGL_FORMAT_DXT1_RGB:
	case VIRGL_FORMAT_DXT1_RGBA:
	case VIRGL_FORMAT_DXT1_SRGB:
	case VIRGL_FORMAT_DXT1_SRGBA:
		// DXT1 block size is 8 bytes for a 4x4 pixel block
		return ((width + 3) / 4) * ((height + 3) / 4) * 8;
	case VIRGL_FORMAT_DXT3_RGBA:
	case VIRGL_FORMAT_DXT5_RGBA:
	case VIRGL_FORMAT_DXT3_SRGBA:
	case VIRGL_FORMAT_DXT5_SRGBA:
		// DXT3/DXT5 block size is 16 bytes for a 4x4 pixel block
		return ((width + 3) / 4) * ((height + 3) / 4) * 16;
	case VIRGL_FORMAT_RGTC1_UNORM:
	case VIRGL_FORMAT_RGTC1_SNORM:
		// RGTC1 block size is 8 bytes for a 4x4 pixel block
		return ((width + 3) / 4) * ((height + 3) / 4) * 8;
	case VIRGL_FORMAT_RGTC2_UNORM:
	case VIRGL_FORMAT_RGTC2_SNORM:
		// RGTC2 block size is 16 bytes for a 4x4 pixel block
		return ((width + 3) / 4) * ((height + 3) / 4) * 16;
	case VIRGL_FORMAT_ETC1_RGB8:
	case VIRGL_FORMAT_ETC2_RGB8:
	case VIRGL_FORMAT_ETC2_SRGB8:
	case VIRGL_FORMAT_ETC2_RGB8A1:
	case VIRGL_FORMAT_ETC2_SRGB8A1:
  case VIRGL_FORMAT_ETC2_R11_UNORM:
  case VIRGL_FORMAT_ETC2_R11_SNORM:
		// ETC1/ETC2/EAC R11 block size is 8 bytes for a 4x4 pixel block
		return ((width + 3) / 4) * ((height + 3) / 4) * 8;
	case VIRGL_FORMAT_ETC2_RGBA8:
	case VIRGL_FORMAT_ETC2_SRGBA8:
  case VIRGL_FORMAT_ETC2_RG11_UNORM:
  case VIRGL_FORMAT_ETC2_RG11_SNORM:
		// ETC2 RGBA and EAC RG11 block size is 16 bytes for a 4x4 pixel block
		return ((width + 3) / 4) * ((height + 3) / 4) * 16;
	case VIRGL_FORMAT_ASTC_4x4:
	case VIRGL_FORMAT_ASTC_4x4_SRGB:
		// ASTC 4x4 block size is 16 bytes for a 4x4 pixel block
		return ((width + 3) / 4) * ((height + 3) / 4) * 16;
	case VIRGL_FORMAT_ASTC_5x4:
	case VIRGL_FORMAT_ASTC_5x4_SRGB:
		// ASTC 5x4 block size is 16 bytes for a 5x4 pixel block
		return ((width + 4) / 5) * ((height + 3) / 4) * 16;
	case VIRGL_FORMAT_ASTC_5x5:
	case VIRGL_FORMAT_ASTC_5x5_SRGB:
		// ASTC 5x5 block size is 16 bytes for a 5x5 pixel block
		return ((width + 4) / 5) * ((height + 4) / 5) * 16;
	case VIRGL_FORMAT_ASTC_6x5:
	case VIRGL_FORMAT_ASTC_6x5_SRGB:
		// ASTC 6x5 block size is 16 bytes for a 6x5 pixel block
		return ((width + 5) / 6) * ((height + 4) / 5) * 16;
	case VIRGL_FORMAT_ASTC_6x6:
	case VIRGL_FORMAT_ASTC_6x6_SRGB:
		// ASTC 6x6 block size is 16 bytes for a 6x6 pixel block
		return ((width + 5) / 6) * ((height + 5) / 6) * 16;
	case VIRGL_FORMAT_ASTC_8x5:
	case VIRGL_FORMAT_ASTC_8x5_SRGB:
		// ASTC 8x5 block size is 16 bytes for a 8x5 pixel block
		return ((width + 7) / 8) * ((height + 4) / 5) * 16;
	case VIRGL_FORMAT_ASTC_8x6:
	case VIRGL_FORMAT_ASTC_8x6_SRGB:
		// ASTC 8x6 block size is 16 bytes for a 8x6 pixel block
		return ((width + 7) / 8) * ((height + 5) / 6) * 16;
	case VIRGL_FORMAT_ASTC_8x8:
	case VIRGL_FORMAT_ASTC_8x8_SRGB:
		// ASTC 8x8 block size is 16 bytes for a 8x8 pixel block
		return ((width + 7) / 8) * ((height + 7) / 8) * 16;
	case VIRGL_FORMAT_ASTC_10x5:
	case VIRGL_FORMAT_ASTC_10x5_SRGB:
		// ASTC 10x5 block size is 16 bytes for a 10x5 pixel block
		return ((width + 9) / 10) * ((height + 4) / 5) * 16;
	case VIRGL_FORMAT_ASTC_10x6:
	case VIRGL_FORMAT_ASTC_10x6_SRGB:
		// ASTC 10x6 block size is 16 bytes for a 10x6 pixel block
		return ((width + 9) / 10) * ((height + 5) / 6) * 16;
	case VIRGL_FORMAT_ASTC_10x8:
	case VIRGL_FORMAT_ASTC_10x8_SRGB:
		// ASTC 10x8 block size is 16 bytes for a 10x8 pixel block
		return ((width + 9) / 10) * ((height + 7) / 8) * 16;
	case VIRGL_FORMAT_ASTC_10x10:
	case VIRGL_FORMAT_ASTC_10x10_SRGB:
		// ASTC 10x10 block size is 16 bytes for a 10x10 pixel block
		return ((width + 9) / 10) * ((height + 9) / 10) * 16;
	case VIRGL_FORMAT_ASTC_12x10:
	case VIRGL_FORMAT_ASTC_12x10_SRGB:
		// ASTC 12x10 block size is 16 bytes for a 12x10 pixel block
		return ((width + 11) / 12) * ((height + 9) / 10) * 16;
	case VIRGL_FORMAT_ASTC_12x12:
	case VIRGL_FORMAT_ASTC_12x12_SRGB:
		// ASTC 12x12 block size is 16 bytes for a 12x12 pixel block
		return ((width + 11) / 12) * ((height + 11) / 12) * 16;
	default:
		fprintf(stderr, "Unknown compressed format: %s\n",
			get_virgl_format_string(format));
		return 0;
	}
}

int rvgpu_ctx_transfer_to_host(struct rvgpu_ctx *ctx,
			       const struct rvgpu_res_transfer *t,
			       const struct rvgpu_res *res)
{
	struct rvgpu_patch p = { .len = 0 };
	size_t size = 0;
	if (res->info.target == 0) {
		if (t->stride > 0) {
			if (t->d > 1 && t->h > 1) {
				// Transfer h rows, each of stride bytes, for d slices
				// (3D texture)
				size = t->d * t->h * t->stride;
			} else if (t->h > 1) {
				// Transfer h rows, each of stride bytes
				size = t->h * t->stride;
			} else if (t->w > 0) {
				// Transfer a single row of tride bytes
				size = t->stride;
			}
		} else if (t->w > 0) {
			size = t->w;
		}
		if (size > 0)
			gpu_device_send_data(ctx, res->backing, res->nbacking,
					     t->offset, size);
	} else if (res->info.target == 2) {
		if (virgl_format_is_compressed(res->info.format)) {
			size_t compressed_size = compressed_data_size(
				res->info.format, t->w, t->h);
			gpu_device_send_data(ctx, res->backing, res->nbacking,
					     t->offset, compressed_size);
		} else if (virgl_format_is_yuv(res->info.format)) {
			size_t yuv_size = yuv_data_size(res->info.format, t->w,
							t->h, t->stride);
			gpu_device_send_data(ctx, res->backing, res->nbacking,
					     t->offset, yuv_size);
		} else {
			unsigned int bpp = get_format_bpp(res->info.format);
			uint32_t stride = (t->stride != 0) ?
						  t->stride :
						  bpp * res->info.width;
			size_t size = (t->h - 1U) * stride + t->w * bpp;
			gpu_device_send_data(ctx, res->backing, res->nbacking,
					     t->offset, size);
		}
	} else if ( res->info.target == 3)
	{
		// 3D texture
		unsigned int bpp = get_format_bpp(res->info.format);
		uint32_t width = res->info.width >> t->level; // calculate width at level
		uint32_t height = res->info.height >> t->level; // calculate height at level
		uint32_t stride = (t->stride != 0) ?
						t->stride :
						bpp * width;
		uint32_t layer_stride = (t->layer_stride != 0) ?
						t->layer_stride :
						stride * height;
		size_t size = (t->h - 1U) * stride + t->w * bpp + layer_stride * (t->d - 1U);
		gpu_device_send_data(ctx, res->backing, res->nbacking,
					t->offset, size);
	} else {
		gpu_device_send_data(ctx, res->backing, res->nbacking,
				     t->offset, SIZE_MAX);
	}

	if (rvgpu_ctx_send(ctx, &p, sizeof(p))) {
		warn("short write");
		return -1;
	}
	return 0;
}

struct rvgpu_res *rvgpu_ctx_res_find(struct rvgpu_ctx *ctx,
				     uint32_t resource_id)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;

	return gpu_device_get_res(ctx_priv, resource_id);
}

void rvgpu_ctx_res_destroy(struct rvgpu_ctx *ctx, uint32_t resource_id)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	struct rvgpu_res *res;

	res = gpu_device_get_res(ctx_priv, resource_id);
	if (res) {
		LIST_REMOVE(res, entry);
		free(res->backing);
		free(res);
	} else {
		warnx("%s can't destroy resource. Res %u doesn't exist",
		      __func__, resource_id);
	}
}

int rvgpu_ctx_res_create(struct rvgpu_ctx *ctx,
			 const struct rvgpu_res_info *info,
			 uint32_t resource_id)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	struct rvgpu_res *res;

	res = calloc(1, sizeof(*res));
	if (res == NULL)
		return -1;
	res->resid = resource_id;
	memcpy(&res->info, info, sizeof(*info));
	res->info.bpp = 4u;
	LIST_INSERT_HEAD(&ctx_priv->reslist, res, entry);

	return 0;
}
