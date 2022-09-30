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

#include <rvgpu-proxy/gpu/rvgpu-iov.h>
#include <string.h>

size_t copy_from_iov(const struct iovec iov[], size_t n, void *buffer,
		     size_t len)
{
	size_t ret = 0u;

	for (size_t i = 0; i < n; i++) {
		if (iov[i].iov_len < (len - ret)) {
			memcpy((char *)buffer + ret, iov[i].iov_base,
			       iov[i].iov_len);
			ret += iov[i].iov_len;
		} else {
			memcpy((char *)buffer + ret, iov[i].iov_base,
			       len - ret);
			ret += len - ret;
			break;
		}
	}
	return ret;
}

size_t copy_to_iov(struct iovec iov[], size_t n, void *buffer, size_t len)
{
	size_t ret = 0u;

	for (size_t i = 0; i < n; i++) {
		if (iov[i].iov_len < (len - ret)) {
			memcpy(iov[i].iov_base, (const char *)buffer + ret,
			       iov[i].iov_len);
			ret += iov[i].iov_len;
		} else {
			memcpy(iov[i].iov_base, (const char *)buffer + ret,
			       len - ret);
			ret += len - ret;
			break;
		}
	}
	return ret;
}

size_t iov_size(const struct iovec iov[], size_t n)
{
	size_t i, result = 0;

	for (i = 0; i < n; i++)
		result += iov[i].iov_len;

	return result;
}
