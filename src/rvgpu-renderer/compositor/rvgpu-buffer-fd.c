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

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>

#include <netinet/tcp.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/backend/rvgpu-gbm.h>
#include <rvgpu-renderer/compositor/rvgpu-buffer-fd.h>

void send_handle(int client_fd, void *handle)
{
	struct msghdr msg = { 0 };
	char buf[CMSG_SPACE(sizeof(void *))];
	memset(buf, 0, sizeof(buf));

	struct iovec io = { .iov_base = (void *)"", .iov_len = 1 };

	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(void *));

	*((void **)CMSG_DATA(cmsg)) = handle;

	if (sendmsg(client_fd, &msg, 0) == -1)
		perror("sendmsg");
}

void *recv_handle(int client_fd)
{
	struct msghdr msg = { 0 };

	char m_buffer[1];
	struct iovec io = { .iov_base = m_buffer, .iov_len = sizeof(m_buffer) };

	msg.msg_iov = &io;
	msg.msg_iovlen = 1;

	char buf[CMSG_SPACE(sizeof(void *))];
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);

	if (recvmsg(client_fd, &msg, 0) == -1) {
		perror("recvmsg");
		return NULL;
	}

	struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);

	void *handle = *((void **)CMSG_DATA(cmsg));

	return handle;
}

void *create_shm_fd(const char *shm_name, int width, int height)
{
	printf("try to get shared buffer fd using shm\n");
	shm_unlink(shm_name);
	int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
	if (shm_fd == -1) {
		perror("shm_open");
		return NULL;
	}

	size_t size = width * height * 4;
	if (ftruncate(shm_fd, size) == -1) {
		perror("ftruncate");
		close(shm_fd);
		shm_unlink(shm_name);
		return NULL;
	}

	return (void *)(uintptr_t)shm_fd;
}

bool get_cap_dma_buf_import_extensions(EGLDisplay dpy)
{
	const char *eglExtensions = eglQueryString(dpy, EGL_EXTENSIONS);
	bool ret = true;
	if (strstr(eglExtensions, "EGL_EXT_image_dma_buf_import") == NULL) {
		fprintf(stderr,
			"EGL_EXT_image_dma_buf_import is not supported\n");
		ret = false;
	}

	if (strstr(eglExtensions, "EGL_KHR_image_base") == NULL) {
		fprintf(stderr, "EGL_KHR_image_base is not supported\n");
		ret = false;
	}

	if (strstr(eglExtensions, "EGL_KHR_gl_texture_2D_image") == NULL) {
		fprintf(stderr,
			"EGL_KHR_gl_texture_2D_image is not supported\n");
		ret = false;
	}
	if (strstr(eglExtensions, "EGL_EXT_image_dma_buf_import_modifiers") ==
	    NULL) {
		fprintf(stderr,
			"EGL_EXT_image_dma_buf_import_modifiers is not supported\n");
		ret = false;
	}

	const char *glExtensions = (const char *)glGetString(GL_EXTENSIONS);
	if (strstr(glExtensions, "GL_OES_EGL_image") == NULL) {
		fprintf(stderr, "GL_OES_EGL_image is not supported\n");
		ret = false;
	}

	return ret;
}

void *create_dma_buffer_fd_by_drm(uint32_t width, uint32_t height)
{
	printf("try to get dma buffer fd using DRM\n");
	int drm_fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("Cannot open " DEVICE_PATH);
		return NULL;
	}

	struct drm_mode_create_dumb create = { 0 };
	create.width = width;
	create.height = height;
	create.bpp = 32;

	if (drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0) {
		perror("DRM_IOCTL_MODE_CREATE_DUMB failed");
		close(drm_fd);
		return NULL;
	}

	uint32_t handle = create.handle;

	struct drm_prime_handle prime_handle = {
		.handle = handle,
		.flags = DRM_CLOEXEC | DRM_RDWR,
		.fd = -1,
	};

	if (drmIoctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime_handle) < 0) {
		perror("DRM_IOCTL_PRIME_HANDLE_TO_FD failed");
		struct drm_mode_destroy_dumb destroy = { 0 };
		destroy.handle = handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
		close(drm_fd);
		return NULL;
	}

	int dma_buf_fd = prime_handle.fd;

	struct drm_mode_destroy_dumb destroy = { 0 };
	destroy.handle = handle;
	drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
	close(drm_fd);

	return (void *)(uintptr_t)dma_buf_fd;
}

void *create_dma_buffer_fd_by_gbm(uint32_t width, uint32_t height)
{
	printf("try to get dma buffer fd using GBM\n");
	int drm_fd = open(DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("open");
		return NULL;
	}
	struct gbm_device *gbm = gbm_create_device(drm_fd);
	if (!gbm) {
		perror("Cannot create GBM device");
		close(drm_fd);
		return NULL;
	}

	uint32_t format = get_gbm_format(gbm);
	uint32_t flags = GBM_BO_USE_RENDERING;
	struct gbm_bo *bo = gbm_bo_create(gbm, width, height, format, flags);
	if (!bo) {
		perror("Cannot create GBM buffer object");
		gbm_device_destroy(gbm);
		close(drm_fd);
		return NULL;
	}

	int dma_buf_fd = gbm_bo_get_fd(bo);
	if (dma_buf_fd < 0) {
		perror("Cannot get DMA buffer file descriptor");
		gbm_device_destroy(gbm);
		close(drm_fd);
		return NULL;
	}

	gbm_bo_destroy(bo);
	gbm_device_destroy(gbm);
	close(drm_fd);
	return (void *)(uintptr_t)dma_buf_fd;
}

void *create_dma_buffer_fd(uint32_t width, uint32_t height)
{
	void *dma_buf_fd = create_dma_buffer_fd_by_gbm(width, height);
	if (dma_buf_fd != NULL) {
		printf("Successfully created DMA buffer using GBM\n");
		return dma_buf_fd;
	}

	dma_buf_fd = create_dma_buffer_fd_by_drm(width, height);
	if (dma_buf_fd != NULL) {
		printf("Successfully created DMA buffer using DRM\n");
		return dma_buf_fd;
	}

	printf("Failed to create DMA buffer using both GBM and DRM\n");
	return NULL;
}

void destroy_shm_fd(void *buffer_handle, const char *shm_name)
{
	if (buffer_handle == NULL) {
		fprintf(stderr, "Invalid handle\n");
		return;
	}

	int shm_fd = (int)(uintptr_t)buffer_handle;

	if (close(shm_fd) == -1) {
		perror("close");
		return;
	}

	if (shm_name != NULL) {
		if (shm_unlink(shm_name) == -1) {
			perror("shm_unlink");
			return;
		}
	}
}

void destroy_dma_buffer_handle(void *buffer_handle)
{
	int dma_fd = (int)(uintptr_t)buffer_handle;
	if (close(dma_fd) == -1) {
		perror("close");
	}
}

EGLuint64KHR select_modifier(EGLDisplay dpy, EGLint format)
{
	PFNEGLQUERYDMABUFMODIFIERSEXTPROC eglQueryDmaBufModifiersEXT;
	EGL_GET_PROC_ADDR(eglQueryDmaBufModifiersEXT);

	if (!eglQueryDmaBufModifiersEXT) {
		fprintf(stderr,
			"Failed to get eglQueryDmaBufModifiersEXT function pointer\n");
		return DRM_FORMAT_MOD_INVALID;
	}

	EGLuint64KHR modifiers[MAX_MODIFIERS];
	EGLBoolean external_only[MAX_MODIFIERS];
	EGLint num_modifiers = 0;

	if (!eglQueryDmaBufModifiersEXT(dpy, format, MAX_MODIFIERS, modifiers,
					external_only, &num_modifiers)) {
		fprintf(stderr, "Failed to query modifiers for format 0x%x\n",
			format);
		return DRM_FORMAT_MOD_INVALID;
	}

	printf("Modifiers for pixel format: 0x%x\n", format);
	EGLuint64KHR selected_modifier = DRM_FORMAT_MOD_INVALID;
	for (EGLint i = 0; i < num_modifiers; ++i) {
		printf("Modifier: 0x%lx, External Only: %s\n", modifiers[i],
		       external_only[i] ? "Yes" : "No");
		if (!external_only[i] &&
                    selected_modifier == DRM_FORMAT_MOD_INVALID) {
                        selected_modifier = modifiers[i];
                }
	}
	printf("Selected modifier: 0x%lx\n", selected_modifier);
	return selected_modifier;
}

EGLImageKHR create_egl_image_from_dma(EGLDisplay dpy, uint32_t width,
				      uint32_t height, void *dma_fd_handle)
{
	int index = 0;
	int dma_fd = (int)(uintptr_t)dma_fd_handle;
	int pitch = ALIGN(width * 4, RVGPU_DMA_ALIGNMENT_SIZE);
	PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
	EGL_GET_PROC_ADDR(eglCreateImageKHR);
	EGLint attrs[32];
	EGLint format = DRM_FORMAT_ARGB8888;
	EGLuint64KHR selected_modifier = select_modifier(dpy, format);

	attrs[index++] = EGL_WIDTH;
	attrs[index++] = width;
	attrs[index++] = EGL_HEIGHT;
	attrs[index++] = height;
	attrs[index++] = EGL_LINUX_DRM_FOURCC_EXT;
	attrs[index++] = format;
	attrs[index++] = EGL_DMA_BUF_PLANE0_FD_EXT;
	attrs[index++] = dma_fd;
	attrs[index++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
	attrs[index++] = 0;
	attrs[index++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
	attrs[index++] = pitch;

	if (selected_modifier != DRM_FORMAT_MOD_INVALID) {
		attrs[index++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
		attrs[index++] = (EGLint)(selected_modifier & 0xFFFFFFFFULL);
		attrs[index++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
		attrs[index++] = (EGLint)(selected_modifier >> 32);
	}

	attrs[index] = EGL_NONE;

	EGLImageKHR eglImage =
		eglCreateImageKHR(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
				  (EGLClientBuffer)NULL, attrs);
	if (eglImage == EGL_NO_IMAGE_KHR) {
		fprintf(stderr, "eglCreateImageKHR failed with error: 0x%x\n",
			eglGetError());
		return EGL_NO_IMAGE_KHR;
	}

	return eglImage;
}
