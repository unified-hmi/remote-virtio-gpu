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

#ifndef RVGPU_BUFFER_FD_H
#define RVGPU_BUFFER_FD_H

#include <EGL/eglext.h>

#define DEVICE_PATH "/dev/dri/card0"
#define RVGPU_DMA_ALIGNMENT_SIZE 64
#define MAX_MODIFIERS 32
#define ALIGN(value, alignment) (((value) + ((alignment)-1)) & ~((alignment)-1))

void send_handle(int client_fd, void *handle);
void *recv_handle(int client_fd);
bool get_cap_dma_buf_import_extensions(EGLDisplay dpy);
void *create_shm_fd(const char *shm_name, int width, int height);
void *create_dma_buffer_fd(uint32_t width, uint32_t height);
void destroy_shm_fd(void *buffer_handle, const char *shm_name);
void destroy_dma_buffer_handle(void *buffer_handle);

EGLImageKHR create_egl_image_from_dma(EGLDisplay dpy, uint32_t width,
				      uint32_t height, void *dma_fd_handle);

#endif /* RVGPU_BUFFER_FD_H */
