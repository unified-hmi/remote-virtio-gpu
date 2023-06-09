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

#ifndef RVGPU_MAP_GUEST_H
#define RVGPU_MAP_GUEST_H

#include <stdint.h>
#include <stdlib.h>

/**
 * @brief Map memory referenced by guest physical address
 * @param fd - file descriptor to map on
 * @param gpa - guest physical address
 * @param prot - required protection (PROT_READ etc)
 * @param size - size of mapping
 * @return pointer to mapped memory
 */
void *map_guest(int fd, uint64_t gpa, int prot, size_t size);

/**
 * @brief Unmaps memory previously returned by map_guest
 * @param addr - address returned by map_guest
 * @param size - size supplied to map_guest
 */
void unmap_guest(void *addr, size_t size);

#endif /* RVGPU_MAP_GUEST_H */
