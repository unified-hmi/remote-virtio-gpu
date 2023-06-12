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

int rvgpu_ctx_transfer_to_host(struct rvgpu_ctx *ctx,
			       const struct rvgpu_res_transfer *t,
			       const struct rvgpu_res *res)
{
	struct rvgpu_patch p = { .len = 0 };
	const unsigned int bpp = 4;
	uint32_t stride;

	if (res->info.target == 0) {
		gpu_device_send_data(ctx, res->backing, res->nbacking,
				     t->offset, t->w);
	} else if (res->info.target == 2) {
		stride = t->stride;
		if (stride == 0)
			stride = bpp * res->info.width;

		for (size_t h = 0u; h < t->h; h++) {
			gpu_device_send_data(ctx, res->backing, res->nbacking,
					     t->offset + h * stride,
					     t->w * bpp);
		}
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
