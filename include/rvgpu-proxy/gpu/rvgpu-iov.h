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

#ifndef RVGPU_IOV_H
#define RVGPU_IOV_H

#include <stdlib.h>
#include <sys/uio.h>

/**
 * @brief Copy data from set of iovecs to single buffer
 * @param iov - set of iovecs
 * @param n - number of iovecs
 * @param buffer - buffer to copy to
 * @param len - size of buffer
 * @return number of bytes copied
 */
size_t copy_from_iov(const struct iovec iov[], size_t n, void *buffer,
		     size_t len);

/**
 * @brief Copy data from single buffer to set of iovecs
 * @param iov - set of iovecs
 * @param n - number of iovecs
 * @param buffer - buffer to copy from
 * @param len - size of buffer
 * @return number of bytes copied
 */
size_t copy_to_iov(struct iovec iov[], size_t n, void *buffer, size_t len);

/**
 * @brief Calculate total size of iovecs set
 * @param iov - set of iovecs
 * @param n - number of iovecs
 * @return number of bytes in iovecs set
 */
size_t iov_size(const struct iovec iov[], size_t n);

#endif /* RVGPU_IOV_H */
