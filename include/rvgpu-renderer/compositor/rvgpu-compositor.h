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

#ifndef RVGPU_COMPOSITOR_H
#define RVGPU_COMPOSITOR_H

#include <jansson.h>

typedef enum { LAYOUT_NOTHING, LAYOUT_UPDATING, LAYOUT_COMPLETED } LayoutStatus;

typedef void *(*RVGPUEGLPFInitFunc)(void *egl_pf_init_params, uint32_t *width,
				    uint32_t *height);
typedef void (*RVGPUEGLPFFreeFunc)(void *egl_pf_free_params);
typedef void *(*RVGPUCreatePFNativeDisplayFunc)(void *arg);
typedef void (*RVGPUDestroyPFNativeDisplayFunc)(void *arg);
typedef void (*RVGPUPFSwapFunc)(void *param, bool vsync);
typedef void (*SendBufferHandleFunc)(uint32_t client_fd, void *buffer_handle);
typedef void *(*RecvBufferHandleFunc)(uint32_t client_fd);
typedef bool (*GetHardwareBufferCapFunc)(EGLDisplay dpy);
typedef void *(*CreateHardwareBufferFunc)(uint32_t width, uint32_t height);
typedef void *(*CreateSharedBufferFunc)(const char *name, uint32_t width,
					uint32_t height);
typedef void (*DestroyHardwareBufferFunc)(void *buffer_handle);
typedef void (*DestroySharedBufferFunc)(void *buffer_handle, const char *name);

typedef EGLImageKHR (*CreateEGLImageFunc)(EGLDisplay dpy, uint32_t width,
					  uint32_t height, void *buffer_handle);

typedef struct platform_funcs {
	RVGPUEGLPFInitFunc rvgpu_egl_pf_init;
	RVGPUEGLPFFreeFunc rvgpu_egl_pf_free;
	RVGPUCreatePFNativeDisplayFunc rvgpu_create_pf_native_display;
	RVGPUDestroyPFNativeDisplayFunc rvgpu_destroy_pf_native_display;
	RVGPUPFSwapFunc rvgpu_pf_swap;
	SendBufferHandleFunc send_buffer_handle;
	RecvBufferHandleFunc recv_buffer_handle;
	GetHardwareBufferCapFunc get_hardware_buffer_cap;
	CreateHardwareBufferFunc create_hardware_buffer;
	CreateSharedBufferFunc create_shared_buffer;
	DestroyHardwareBufferFunc destroy_hardware_buffer;
	DestroySharedBufferFunc destroy_shared_buffer;
	CreateEGLImageFunc create_egl_image;
} platform_funcs_t;

void *rvgpu_egl_pf_init(void *egl_pf_init_params, uint32_t *width,
			uint32_t *height, platform_funcs_t *pf_funcs);
void rvgpu_egl_pf_free(void *egl_pf_free_params, platform_funcs_t *pf_funcs);
void *rvgpu_create_pf_native_display(void *arg, platform_funcs_t *pf_funcs);
void rvgpu_destroy_pf_native_display(void *arg, platform_funcs_t *pf_funcs);
void rvgpu_pf_swap(void *param, bool vsync, platform_funcs_t *pf_funcs);
void send_buffer_handle(uint32_t client_fd, void *buffer_handle,
			platform_funcs_t *pf_funcs);
void *recv_buffer_handle(uint32_t client_fd, platform_funcs_t *pf_funcs);
bool get_hardware_buffer_cap(EGLDisplay dpy, platform_funcs_t *pf_funcs);
void *create_hardware_buffer(uint32_t width, uint32_t height,
			     platform_funcs_t *pf_funcs);
void *create_shared_buffer(const char *name, uint32_t width, uint32_t height,
			   platform_funcs_t *pf_funcs);
void destroy_hardware_buffer(void *buffer_handle, platform_funcs_t *pf_funcs);
void destroy_shared_buffer(void *buffer_handle, const char *name,
			   platform_funcs_t *pf_funcs);
EGLImageKHR create_egl_image(EGLDisplay dpy, uint32_t width, uint32_t height,
			     void *buffer_handle, platform_funcs_t *pf_funcs);

typedef bool (*CheckInBoundsFunc)(json_t *rvgpu_json_obj, double x, double y);
typedef json_t *(*GetRvgpuFocusFunc)(
	double x, double y, struct rvgpu_draw_list_params *draw_list_params);

struct rvgpu_domain_sock_params {
	char *rvgpu_compositor_sock_path;
	char *rvgpu_layout_sock_path;
};

struct rvgpu_layout_params {
	bool use_rvgpu_layout_draw;
	bool use_layout_sync;
	int *layout_status;
	json_t *rvgpu_layout_list;
	json_t *rvgpu_safety_areas;
	pthread_mutex_t *layout_list_mutex;
	pthread_mutex_t *layout_sync_mutex;
	pthread_mutex_t *safety_area_mutex;
	pthread_cond_t *layout_sync_cond;
};

struct request_thread_params {
	int event_fd;
	int req_write_fd;
	struct rvgpu_layout_params layout_params;
	struct rvgpu_domain_sock_params domain_params;
	pthread_mutex_t *rvgpu_request_mutex;
};

struct rvgpu_compositor_params {
	platform_funcs_t pf_funcs;
	void *egl_pf_init_params;
	struct rvgpu_egl_params egl_params;
	struct rvgpu_fps_params fps_params;
	struct rvgpu_layout_params layout_params;
	bool translucent;
	bool fullscreen;
	bool vsync;
	uint16_t port_no;
	uint32_t width;
	uint32_t height;
	uint32_t ivi_surface_id;
	uint32_t max_vsync_rate;
	char *carddev;
	char *seat;
	char *domain_name;
	char *capset_file;
};

struct compositor_params {
	platform_funcs_t pf_funcs;
	void *egl_pf_init_params;
	struct rvgpu_egl_params egl_params;
	struct rvgpu_layout_params layout_params;
	struct rvgpu_domain_sock_params domain_params;
	bool translucent;
	bool fullscreen;
	bool vsync;
	uint32_t width;
	uint32_t height;
	uint32_t ivi_surface_id;
	int req_read_fd;
	char *carddev;
	char *seat;
};

struct render_params {
	platform_funcs_t pf_funcs;
	int command_socket;
	int resource_socket;
	uint32_t max_vsync_rate;
	bool vsync;
	char *rvgpu_surface_id;
	struct rvgpu_fps_params fps_params;
	char *carddev;
	struct rvgpu_egl_params egl_params;
	struct rvgpu_layout_params layout_params;
	struct rvgpu_domain_sock_params domain_params;
	char *capset_file;
};

bool check_in_rvgpu_surface(json_t *rvgpu_json_obj, double x, double y);
int get_rvgpu_client_fd(json_t *json_obj,
			struct rvgpu_draw_list_params *draw_list_params);
json_t *
get_focus_rvgpu_json_obj(double x, double y,
			 struct rvgpu_draw_list_params *draw_list_params);
void *layout_event_loop(void *arg);
void compositor_render(struct compositor_params *params,
		       struct request_thread_params *request_tp);
void *regislation_read_loop(void *arg);
void rvgpu_render(struct render_params *params);
void rvgpu_compositor_run(struct rvgpu_compositor_params *params);

#define RVGPU_DRAW_EVENT_ID -1
#define RVGPU_REMOVE_EVENT_ID -2
#define RVGPU_LAYOUT_EVENT_ID -3
#define RVGPU_ADD_EVENT_ID -4
#define RVGPU_STOP_EVENT_ID -5

#endif /* RVGPU_COMPOSITOR_H */
