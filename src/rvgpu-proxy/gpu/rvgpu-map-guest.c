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

#include <err.h>
#include <stdint.h>
#include <sys/mman.h>

#include <rvgpu-proxy/gpu/rvgpu-map-guest.h>

static inline uint64_t align_to_page_down(uint64_t a)
{
	return a & (~4095ull);
}

static inline uint64_t align_to_page_up(uint64_t a)
{
	return align_to_page_down(a + 4095ull);
}

void *map_guest(int fd, uint64_t gpa, int prot, size_t size)
{
	uint64_t realpa = align_to_page_down(gpa);
	size_t realsize = align_to_page_up(gpa + size) - realpa;
	void *ret;

	ret = mmap(NULL, realsize, prot, MAP_SHARED, fd, (off_t)realpa);
	if (ret == MAP_FAILED)
		return NULL;

	ret = (char *)ret + (gpa - realpa);
	return ret;
}

void unmap_guest(void *addr, size_t size)
{
	uintptr_t ai = (uintptr_t)addr;
	uintptr_t realpa = align_to_page_down(ai);
	size_t realsize = align_to_page_up(ai + size) - realpa;

	munmap((void *)realpa, realsize);
}
