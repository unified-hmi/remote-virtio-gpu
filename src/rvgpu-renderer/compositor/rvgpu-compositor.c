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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <errno.h>
#include <err.h>

#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/mman.h>

#include <linux/input-event-codes.h>
#include <drm/drm_fourcc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <jansson.h>

#include <rvgpu-utils/rvgpu-utils.h>
#include <rvgpu-renderer/compositor/rvgpu-json-helpers.h>
#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/renderer/rvgpu-input.h>
#include <rvgpu-renderer/renderer/rvgpu-render2d.h>
#include <rvgpu-renderer/backend/rvgpu-offscreen.h>
#include <rvgpu-renderer/compositor/rvgpu-compositor.h>
#include <rvgpu-renderer/compositor/rvgpu-connection.h>

PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC
glEGLImageTargetRenderbufferStorageOES;

PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;

platform_funcs_t pf_funcs;

CheckInBoundsFunc isPointWithinBounds;
GetRvgpuFocusFunc getRvgpuFocus;

struct input_event_thread_params {
	int server_rvgpu_fd;
	int command_socket;
	struct rvgpu_scanout *scanouts;
	struct rvgpu_layout_params layout_params;
};

struct rvgpu_request_params {
	char *rvgpu_surface_id;
	int client_rvgpu_fd;
	int req_write_fd;
	pthread_mutex_t *rvgpu_request_mutex;
};

bool check_in_square(double x, double y, double dst_x, double dst_y,
		     double dst_w, double dst_h)
{
	bool in = false;
	if ((x >= dst_x && x <= dst_x + dst_w) &&
	    (y >= dst_y && y <= dst_y + dst_h)) {
		in = true;
	}
	return in;
}

bool check_in_rvgpu_layout_draw(json_t *rvgpu_json_obj, double x, double y)
{
	double dst_x, dst_y, dst_w, dst_h;
	get_double_from_jsonobj(rvgpu_json_obj, "dst_x", &dst_x);
	get_double_from_jsonobj(rvgpu_json_obj, "dst_y", &dst_y);
	get_double_from_jsonobj(rvgpu_json_obj, "dst_w", &dst_w);
	get_double_from_jsonobj(rvgpu_json_obj, "dst_h", &dst_h);

	return check_in_square(x, y, dst_x, dst_y, dst_w, dst_h);
}

int get_rvgpu_client_fd(json_t *json_obj,
			struct rvgpu_draw_list_params *draw_list_params)
{
	if (!json_obj) {
		return -1;
	}
	const char *rvgpu_surface_id;
	if (get_str_from_jsonobj(json_obj, "rvgpu_surface_id",
				 &rvgpu_surface_id) == -1) {
		return -1;
	}
	int client_rvgpu_fd = -1;
	int index;
	json_t *value;
	pthread_mutex_lock(draw_list_params->surface_list_mutex);
	json_array_foreach(draw_list_params->rvgpu_surface_list, index, value)
	{
		const char *ext_rvgpu_surface_id;
		if (get_str_from_jsonobj(value, "rvgpu_surface_id",
					 &ext_rvgpu_surface_id) == -1) {
			continue;
		}
		if (strcmp(ext_rvgpu_surface_id, rvgpu_surface_id) == 0) {
			get_int_from_jsonobj(value, "client_rvgpu_fd",
					     &client_rvgpu_fd);
			break;
		}
	}
	pthread_mutex_unlock(draw_list_params->surface_list_mutex);
	return client_rvgpu_fd;
}

bool check_in_rvgpu_draw(json_t *rvgpu_json_obj, double x, double y)
{
	double img_w, img_h;
	if (get_double_from_jsonobj(rvgpu_json_obj, "width", &img_w) == -1) {
		return false;
	}
	if (get_double_from_jsonobj(rvgpu_json_obj, "height", &img_h) == -1) {
		return false;
	}

	return check_in_square(x, y, 0, 0, img_w, img_h);
}

bool check_in_rvgpu_surface(json_t *rvgpu_json_obj, double x, double y)
{
	const char *rvgpu_surface_id;
	if (get_str_from_jsonobj(rvgpu_json_obj, "rvgpu_surface_id",
				 &rvgpu_surface_id) == -1) {
		return false;
	}

	return isPointWithinBounds(rvgpu_json_obj, x, y);
}

json_t *get_focus_rvgpu_layout(double x, double y,
			       struct rvgpu_draw_list_params *draw_list_params)
{
	json_t *focused_json_obj = NULL;
	pthread_mutex_lock(draw_list_params->layout_list_mutex);
	size_t array_size =
		json_array_size(draw_list_params->rvgpu_layout_list);
	for (int sfc_index = array_size - 1; sfc_index >= 0; --sfc_index) {
		json_t *sfc_value = json_array_get(
			draw_list_params->rvgpu_layout_list, sfc_index);
		if (!sfc_value) {
			continue;
		}

		const char *rvgpu_surface_id;
		if (get_str_from_jsonobj(sfc_value, "rvgpu_surface_id",
					 &rvgpu_surface_id) == -1) {
			continue;
		}

		double dst_x, dst_y, dst_w, dst_h;
		get_double_from_jsonobj(sfc_value, "dst_x", &dst_x);
		get_double_from_jsonobj(sfc_value, "dst_y", &dst_y);
		get_double_from_jsonobj(sfc_value, "dst_w", &dst_w);
		get_double_from_jsonobj(sfc_value, "dst_h", &dst_h);

		bool in = check_in_square(x, y, dst_x, dst_y, dst_w, dst_h);
		if (in) {
			focused_json_obj = sfc_value;
			break;
		}
	}
	pthread_mutex_unlock(draw_list_params->layout_list_mutex);
	return focused_json_obj;
}

json_t *get_focus_rvgpu(double x, double y,
			struct rvgpu_draw_list_params *draw_list_params)
{
	json_t *focused_json_obj = NULL;
	pthread_mutex_lock(draw_list_params->surface_list_mutex);
	size_t array_size =
		json_array_size(draw_list_params->rvgpu_surface_list);
	for (int sfc_index = array_size - 1; sfc_index >= 0; --sfc_index) {
		json_t *sfc_value = json_array_get(
			draw_list_params->rvgpu_surface_list, sfc_index);
		if (!sfc_value) {
			continue;
		}

		const char *rvgpu_surface_id;
		if (get_str_from_jsonobj(sfc_value, "rvgpu_surface_id",
					 &rvgpu_surface_id) == -1) {
			continue;
		}

		double img_w, img_h;
		if (get_double_from_jsonobj(sfc_value, "width", &img_w) == -1) {
			continue;
		}
		if (get_double_from_jsonobj(sfc_value, "height", &img_h) ==
		    -1) {
			continue;
		}

		bool in = check_in_square(x, y, 0, 0, img_w, img_h);
		if (in) {
			focused_json_obj = sfc_value;
			break;
		}
	}
	pthread_mutex_unlock(draw_list_params->surface_list_mutex);
	return focused_json_obj;
}

json_t *
get_focus_rvgpu_json_obj(double x, double y,
			 struct rvgpu_draw_list_params *draw_list_params)
{
	return getRvgpuFocus(x, y, draw_list_params);
}

void *layout_event_loop(void *arg)
{
	struct request_thread_params *params =
		(struct request_thread_params *)arg;
	pthread_mutex_t *rvgpu_request_mutex = params->rvgpu_request_mutex;
	int req_write_fd = params->req_write_fd;
	struct rvgpu_layout_params layout_params = params->layout_params;
	struct rvgpu_domain_sock_params domain_params = params->domain_params;
	int event_fd = params->event_fd;
	bool running = true;
	int server_rvgpu_layout_sock =
		create_server_socket(domain_params.rvgpu_layout_sock_path);
	if (server_rvgpu_layout_sock < 0) {
		fprintf(stderr, "Failed to create server socket\n");
	}

	int rvgpu_layout_fd = -1;
	struct pollfd fds[3];
	fds[0].fd = server_rvgpu_layout_sock;
	fds[0].events = POLLIN;
	fds[1].fd = rvgpu_layout_fd;
	fds[1].events = POLLIN;
	fds[2].fd = event_fd;
	fds[2].events = POLLIN;

	while (running) {
		int ret = poll(fds, 3, -1);
		if (ret == -1) {
			perror("layout_event_loop poll");
			continue;
		}

		// Check for termination signal
		if (fds[2].revents & POLLIN) {
			uint64_t u;
			ssize_t s = read(event_fd, &u, sizeof(uint64_t));
			if (s != sizeof(uint64_t)) {
				perror("event_fd read");
			} else {
				printf("Received termination signal: %ld, exiting loop.",
				       u);
			}
			if (rvgpu_layout_fd != -1) {
				close(rvgpu_layout_fd);
			}
			running = false;
			break;
		}

		// Check for new client connection
		if (fds[0].revents & POLLIN) {
			int new_fd =
				connect_to_client(server_rvgpu_layout_sock);
			if (new_fd != -1) {
				printf("New client connected, fd: %d", new_fd);
				if (rvgpu_layout_fd != -1) {
					close(rvgpu_layout_fd);
				}
				rvgpu_layout_fd = new_fd;
				fds[1].fd = rvgpu_layout_fd;
			} else {
				fprintf(stderr,
					"Failed to accept new client connection");
			}
		}

		// Check for data from the connected client
		if (rvgpu_layout_fd != -1 && (fds[1].revents & POLLIN)) {
			json_t *json_obj = recv_json(rvgpu_layout_fd);
			if (json_obj == NULL) {
				rvgpu_layout_fd = -1;
				continue;
			}
			json_t *json_command =
				json_object_get(json_obj, "command");
			if (!json_is_string(json_command)) {
				fprintf(stderr,
					"no command request or command property mismatch\n");
				continue;
			}
			json_t *json_surfaces =
				json_object_get(json_obj, "surfaces");
			if (!json_is_array(json_surfaces)) {
				fprintf(stderr,
					"no surfaces request or surfaces property mismatch\n");
				continue;
			}
			const char *command = json_string_value(json_command);
			size_t index, rvgpu_sfc_index;
			json_t *value, *rvgpu_sfc_value;
			if (strcmp(command, "initial_layout") == 0) {
				pthread_mutex_lock(
					layout_params.layout_list_mutex);
				json_array_clear(
					layout_params.rvgpu_layout_list);
				json_array_foreach(json_surfaces,
						   rvgpu_sfc_index,
						   rvgpu_sfc_value)
				{
					json_array_append(
						layout_params.rvgpu_layout_list,
						json_deep_copy(
							rvgpu_sfc_value));
				}
				pthread_mutex_unlock(
					layout_params.layout_list_mutex);

				json_t *json_safety_areas = json_object_get(
					json_obj, "safety_areas");
				if (!json_is_array(json_safety_areas)) {
					fprintf(stderr,
						"don't have safety areas\n");
				} else if (json_array_size(json_safety_areas) >
					   0) {
					pthread_mutex_lock(
						layout_params.safety_area_mutex);
					json_array_foreach(json_safety_areas,
							   index, value)
					{
						json_array_append(
							layout_params
								.rvgpu_safety_areas,
							json_deep_copy(value));
					}
					pthread_mutex_unlock(
						layout_params.safety_area_mutex);
				}

			} else {
				continue;
			}

#if 0
			int layout_id;
                        printf("rvgpu_layout_list order: ");
                        json_array_foreach(layout_params.rvgpu_layout_list, index, value) {
                                get_int_from_jsonobj (value, "id", &layout_id);
                                printf("%d ", layout_id);
                        }
                        printf("\n");
#endif

			char *json_cmd;
			json_t *json_cmd_obj = json_object();
			json_object_set_new(
				json_cmd_obj, "event_id",
				json_integer(RVGPU_LAYOUT_EVENT_ID));
			json_cmd = json_dumps(json_cmd_obj, JSON_ENCODE_ANY);
			pthread_mutex_lock(rvgpu_request_mutex);
			send_str_with_size(req_write_fd, json_cmd);
			pthread_mutex_unlock(rvgpu_request_mutex);

			if (layout_params.use_layout_sync) {
				*(layout_params.layout_status) =
					LAYOUT_UPDATING;
				pthread_mutex_lock(
					layout_params.layout_sync_mutex);
				while (*(layout_params.layout_status) !=
				       LAYOUT_COMPLETED) {
					pthread_cond_wait(
						layout_params.layout_sync_cond,
						layout_params.layout_sync_mutex);
				}
				pthread_mutex_unlock(
					layout_params.layout_sync_mutex);
			}
			const char *completion_msg = "Layout complete";
			write(rvgpu_layout_fd, completion_msg,
			      strlen(completion_msg));
		}
	}
	close(server_rvgpu_layout_sock);
	unlink(domain_params.rvgpu_layout_sock_path);
	return NULL;
}

void *request_event_loop(void *arg)
{
	struct rvgpu_request_params *params =
		(struct rvgpu_request_params *)arg;
	int client_rvgpu_fd = params->client_rvgpu_fd;
	int req_write_fd = params->req_write_fd;
	char *rvgpu_surface_id = params->rvgpu_surface_id;
	pthread_mutex_t *rvgpu_request_mutex = params->rvgpu_request_mutex;
	struct pollfd client_rvgpu_pfd = { .fd = client_rvgpu_fd,
					   .events = POLLIN };
	int event_id;
	while (1) {
		int ret = poll(&client_rvgpu_pfd, 1, -1);
		if (ret == -1) {
			perror("request_event_loop poll");
			break;
		}
		// update surface list based on request commands
		if (client_rvgpu_pfd.revents & POLLIN) {
			int img_w, img_h;
			int fd_index, need_update_fd;
			uintptr_t buf_handle;
			int initial_color;
			int scanout_id;
			json_t *json_obj = recv_json(client_rvgpu_fd);
			json_t *json_cmd_obj = json_object();

			if (json_obj != NULL) {
				if (get_int_from_jsonobj(json_obj, "event_id",
							 &event_id) == -1) {
					continue;
				}
				if (event_id == RVGPU_ADD_EVENT_ID) {
					if (get_int_from_jsonobj(
						    json_obj, "scanout_id",
						    &scanout_id) == -1) {
						continue;
					}
					json_object_set_new(
						json_cmd_obj, "client_rvgpu_fd",
						json_integer(client_rvgpu_fd));
					json_t *json_texture_array =
						json_array();
					json_object_set_new(json_cmd_obj,
							    "textures",
							    json_texture_array);
					json_t *json_fd_index_array =
						json_array();
					json_object_set_new(
						json_cmd_obj, "fd_indexs",
						json_fd_index_array);
					json_object_set_new(
						json_cmd_obj, "scanout_id",
						json_integer(scanout_id));
				} else if (event_id == RVGPU_DRAW_EVENT_ID) {
					if (get_int_from_jsonobj(
						    json_obj, "width",
						    &img_w) == -1) {
						continue;
					}
					if (get_int_from_jsonobj(
						    json_obj, "height",
						    &img_h) == -1) {
						continue;
					}
					if (get_int_from_jsonobj(
						    json_obj,
						    "shared_buffer_fd_index",
						    &fd_index) == -1) {
						continue;
					}
					if (get_int_from_jsonobj(
						    json_obj, "need_update_fd",
						    &need_update_fd) == -1) {
						continue;
					}
					if (get_int_from_jsonobj(
						    json_obj, "initial_color",
						    &initial_color) == -1) {
						continue;
					}
					if (get_int_from_jsonobj(
						    json_obj, "scanout_id",
						    &scanout_id) == -1) {
						continue;
					}
					if (need_update_fd) {
						while (1) {
							ret = poll(
								&client_rvgpu_pfd,
								1, -1);
							if (ret == -1) {
								perror("get buffer_handle poll");
								break;
							}
							if (client_rvgpu_pfd
								    .revents &
							    POLLIN) {
								void *buffer_handle = recv_buffer_handle(
									client_rvgpu_fd,
									&pf_funcs);
								buf_handle = (uintptr_t)
									buffer_handle;
								break;
							}
						}
					}
					json_object_set_new(
						json_cmd_obj, "width",
						json_integer(img_w));
					json_object_set_new(
						json_cmd_obj, "height",
						json_integer(img_h));
					json_object_set_new(
						json_cmd_obj,
						"shared_buffer_fd_index",
						json_integer(fd_index));
					json_object_set_new(
						json_cmd_obj, "need_update_fd",
						json_integer(need_update_fd));
					json_object_set_new(
						json_cmd_obj, "buf_handle",
						json_integer(buf_handle));
					json_object_set_new(
						json_cmd_obj, "initial_color",
						json_integer(initial_color));
					json_object_set_new(
						json_cmd_obj, "scanout_id",
						json_integer(scanout_id));
				}
			} else {
				event_id = RVGPU_REMOVE_EVENT_ID;
			}

			json_decref(json_obj);
			json_object_set_new(json_cmd_obj, "event_id",
					    json_integer(event_id));
			json_object_set_new(json_cmd_obj, "rvgpu_surface_id",
					    json_string(rvgpu_surface_id));
			char *json_cmd =
				json_dumps(json_cmd_obj, JSON_ENCODE_ANY);
			json_decref(json_cmd_obj);
			pthread_mutex_lock(rvgpu_request_mutex);
			send_str_with_size(req_write_fd, json_cmd);
			pthread_mutex_unlock(rvgpu_request_mutex);
			if (event_id == RVGPU_REMOVE_EVENT_ID) {
				break;
			}
		}
	}
	close(client_rvgpu_fd);
	free(params);
	return NULL;
}

void *regislation_read_loop(void *arg)
{
	struct request_thread_params *params =
		(struct request_thread_params *)arg;

	struct rvgpu_domain_sock_params domain_params = params->domain_params;
	pthread_mutex_t *rvgpu_request_mutex = params->rvgpu_request_mutex;
	bool running = true;
	int server_rvgpu_sock =
		create_server_socket(domain_params.rvgpu_compositor_sock_path);
	if (server_rvgpu_sock < 0) {
		fprintf(stderr, "Failed to create server socket\n");
		exit(EXIT_FAILURE);
	}
	while (running) {
		struct rvgpu_request_params *req_params =
			(struct rvgpu_request_params *)calloc(
				1, sizeof(struct rvgpu_request_params));

		req_params->req_write_fd = params->req_write_fd;
		req_params->client_rvgpu_fd =
			connect_to_client(server_rvgpu_sock);
		printf("connect_to_client req_params->client_rvgpu_fd: %d\n",
		       req_params->client_rvgpu_fd);
		if (req_params->client_rvgpu_fd < 0) {
			continue;
		}

		struct pollfd read_pfd = { .fd = req_params->client_rvgpu_fd,
					   .events = POLLIN };
		while (running) {
			int ret = poll(&read_pfd, 1, -1);
			if (ret == -1) {
				perror("regislation_read_loop poll");
				close(req_params->client_rvgpu_fd);
				break;
			}
			if (read_pfd.revents & POLLIN) {
				char *rvgpu_surface_id = recv_str_all(
					req_params->client_rvgpu_fd);
				if (strcmp(rvgpu_surface_id, "stop") == 0) {
					int event_id = RVGPU_STOP_EVENT_ID;
					json_t *json_obj = json_object();
					json_object_set_new(
						json_obj, "event_id",
						json_integer(event_id));
					char *json_add_cmd = json_dumps(
						json_obj, JSON_ENCODE_ANY);
					pthread_mutex_lock(rvgpu_request_mutex);
					send_str_with_size(params->req_write_fd,
							   json_add_cmd);
					pthread_mutex_unlock(
						rvgpu_request_mutex);
					running = false;
				} else {
					req_params->rvgpu_surface_id =
						strdup(rvgpu_surface_id);
					req_params->rvgpu_request_mutex =
						rvgpu_request_mutex;
					printf("recv_str_all from client to server, req_params->rvgpu_surface_id: %s\n",
					       req_params->rvgpu_surface_id);
					free(rvgpu_surface_id);

					int result;
					pthread_t request_event_loop_thread;
					result = pthread_create(
						&request_event_loop_thread,
						NULL, request_event_loop,
						(void *)req_params);
					send_int(req_params->client_rvgpu_fd,
						 result);
					break;
				}
			}
		}
	}
	close(server_rvgpu_sock);
	unlink(domain_params.rvgpu_compositor_sock_path);
	return NULL;
}

void compositor_render(struct compositor_params *params,
		       struct request_thread_params *request_tp)
{
	EGL_GET_PROC_ADDR(glEGLImageTargetTexture2DOES);
	EGL_GET_PROC_ADDR(glEGLImageTargetRenderbufferStorageOES);
	EGL_GET_PROC_ADDR(eglCreateImageKHR);
	EGL_GET_PROC_ADDR(eglDestroyImageKHR);
	pf_funcs = (platform_funcs_t)params->pf_funcs;
	void *egl_pf_init_params = params->egl_pf_init_params;
	struct rvgpu_egl_params egl_params = params->egl_params;
	struct rvgpu_layout_params layout_params = params->layout_params;
	bool vsync = params->vsync;
	uint32_t width = params->width;
	uint32_t height = params->height;
	int req_read_fd = params->req_read_fd;
	int ret;
	int layout_status = LAYOUT_NOTHING;
	bool running = true;

	int event_fd = eventfd(0, 0);
	if (event_fd == -1) {
		perror("eventfd");
		return;
	}

	// EGL Initialize
	struct rvgpu_egl_state *main_egl =
		(struct rvgpu_egl_state *)rvgpu_egl_pf_init(
			egl_pf_init_params, &width, &height, &pf_funcs);

	// Mutex Init
	pthread_mutex_t surface_list_mutex, input_send_event_mutex,
		layout_list_mutex, layout_sync_mutex, safety_area_mutex;
	pthread_mutex_init(&surface_list_mutex, NULL);
	pthread_mutex_init(&input_send_event_mutex, NULL);
	pthread_mutex_init(&layout_list_mutex, NULL);
	pthread_mutex_init(&layout_sync_mutex, NULL);
	pthread_mutex_init(&safety_area_mutex, NULL);
	pthread_cond_t layout_sync_cond;
	pthread_cond_init(&layout_sync_cond, NULL);
	main_egl->focus_state.input_send_event_mutex = &input_send_event_mutex;

	// Set surface and layout list with mutex for input focus
	json_t *rvgpu_surface_list = json_array();
	json_t *rvgpu_layout_list = json_array();
	json_t *rvgpu_safety_areas = json_array();
	struct rvgpu_draw_list_params draw_list_params = { rvgpu_surface_list,
							   rvgpu_layout_list,
							   &surface_list_mutex,
							   &layout_list_mutex };
	main_egl->draw_list_params = &draw_list_params;

	// Start Regislation Loop
	pthread_mutex_t rvgpu_request_mutex;
	pthread_mutex_init(&rvgpu_request_mutex, NULL);
	request_tp->event_fd = event_fd;
	request_tp->rvgpu_request_mutex = &rvgpu_request_mutex;

	pthread_t regislation_read_loop_thread;
	ret = pthread_create(&regislation_read_loop_thread, NULL,
			     regislation_read_loop, request_tp);

	eglMakeCurrent(main_egl->dpy, main_egl->sfc, main_egl->sfc,
		       main_egl->context);
	init_2d_renderer(width, height);
	if (vsync) {
		eglSwapInterval(main_egl->dpy, vsync ? 1 : 0);
	}
	if (!layout_params.use_rvgpu_layout_draw) {
		glClearColor(((egl_params.clear_color >> 24) & 0xFF) / 255.0f,
			     ((egl_params.clear_color >> 16) & 0xFF) / 255.0f,
			     ((egl_params.clear_color >> 8) & 0xFF) / 255.0f,
			     ((egl_params.clear_color >> 0) & 0xFF) / 255.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		rvgpu_pf_swap(main_egl, vsync, &pf_funcs);
	}

	main_egl->hardware_buffer_enabled =
		get_hardware_buffer_cap(main_egl->dpy, &pf_funcs);

	GLuint transparentTex;
	pthread_t layout_event_loop_thread;
	if (layout_params.use_rvgpu_layout_draw) {
		// Start Layout Event Loop
		request_tp->layout_params.rvgpu_layout_list = rvgpu_layout_list;
		request_tp->layout_params.layout_list_mutex =
			&layout_list_mutex;
		request_tp->layout_params.layout_sync_mutex =
			&layout_sync_mutex;
		request_tp->layout_params.layout_sync_cond = &layout_sync_cond;
		request_tp->layout_params.layout_status = &layout_status;
		request_tp->layout_params.rvgpu_safety_areas =
			rvgpu_safety_areas;
		request_tp->layout_params.safety_area_mutex =
			&safety_area_mutex;

		ret = pthread_create(&layout_event_loop_thread, NULL,
				     layout_event_loop, request_tp);

		// Create transparent texture for safety areas
		glGenTextures(1, &transparentTex);
		glBindTexture(GL_TEXTURE_2D, transparentTex);
		GLubyte transparentPixel[4] = { 255, 255, 255, 0 };
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA,
			     GL_UNSIGNED_BYTE, transparentPixel);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
				GL_NEAREST);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
				GL_NEAREST);

		// Initialize stencil buffer for safety areas
		glEnable(GL_STENCIL_TEST);
		glClear(GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	}

	struct pollfd cmd_pfd = { .fd = req_read_fd, .events = POLLIN };

	size_t index;
	json_t *value;
	while (running || json_array_size(rvgpu_surface_list) > 0) {
		json_t *json_client_rvgpu_fd_array = json_array();
		int event_num = 0;
		int layout_event_num = 0;
		int timeout_ms = -1;
		bool layout_event = false;
		while (running || json_array_size(rvgpu_surface_list) > 0) {
			ret = poll(&cmd_pfd, 1, timeout_ms);
			if (ret == -1) {
				perror("compositor_render poll");
				continue;
			}
			// Update surface list based on request commands
			if (cmd_pfd.revents & POLLIN) {
				const char *rvgpu_surface_id;
				int event_id;
				int img_w, img_h;
				int fd_index;
				int need_update_fd;
				uintptr_t buf_handle;
				int initial_color = 0;
				int scanout_id;
				json_t *json_cmd_obj = recv_json(req_read_fd);

				if (json_cmd_obj) {
#if 0
					printf("JSON Command Object: %s\n", json_dumps(json_cmd_obj, 0));
#endif
					if (get_int_from_jsonobj(
						    json_cmd_obj, "event_id",
						    &event_id) == -1) {
						continue;
					}
					event_num++;
					timeout_ms = 0;
					if (event_id == RVGPU_ADD_EVENT_ID) {
						pthread_mutex_lock(
							&surface_list_mutex);
						json_object_del(json_cmd_obj,
								"event_id");
						json_array_append_new(
							rvgpu_surface_list,
							json_deep_copy(
								json_cmd_obj));

						pthread_mutex_unlock(
							&surface_list_mutex);
						continue;
					} else if (event_id ==
						   RVGPU_DRAW_EVENT_ID) {
						if (get_str_from_jsonobj(
							    json_cmd_obj,
							    "rvgpu_surface_id",
							    &rvgpu_surface_id) ==
						    -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    json_cmd_obj,
							    "width",
							    &img_w) == -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    json_cmd_obj,
							    "height",
							    &img_h) == -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    json_cmd_obj,
							    "shared_buffer_fd_index",
							    &fd_index) == -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    json_cmd_obj,
							    "need_update_fd",
							    &need_update_fd) ==
						    -1) {
							continue;
						}
						if (get_uintptr_from_jsonobj(
							    json_cmd_obj,
							    "buf_handle",
							    &buf_handle) ==
						    -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    json_cmd_obj,
							    "initial_color",
							    &initial_color) ==
						    -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    json_cmd_obj,
							    "scanout_id",
							    &scanout_id) ==
						    -1) {
							continue;
						}
					} else if (event_id ==
						   RVGPU_REMOVE_EVENT_ID) {
						if (get_str_from_jsonobj(
							    json_cmd_obj,
							    "rvgpu_surface_id",
							    &rvgpu_surface_id) ==
						    -1) {
							continue;
						}
						pthread_mutex_lock(
							&surface_list_mutex);
						remove_jsonobj_with_str_key(
							rvgpu_surface_list,
							"rvgpu_surface_id",
							rvgpu_surface_id);
						pthread_mutex_unlock(
							&surface_list_mutex);
						continue;
					} else if (event_id ==
						   RVGPU_LAYOUT_EVENT_ID) {
						layout_event = true;
						layout_event_num++;
						continue;
					} else if (event_id ==
						   RVGPU_STOP_EVENT_ID) {
						running = false;
						uint64_t u = 1;
						write(event_fd, &u,
						      sizeof(uint64_t));
						break;
					}
				}

				pthread_mutex_lock(&surface_list_mutex);
				json_array_foreach(rvgpu_surface_list, index,
						   value)
				{
					const char *ext_rvgpu_surface_id;
					if (get_str_from_jsonobj(
						    value, "rvgpu_surface_id",
						    &ext_rvgpu_surface_id) ==
					    -1) {
						continue;
					}
					int ext_scanout_id;
					if (get_int_from_jsonobj(
						    value, "scanout_id",
						    &ext_scanout_id) == -1) {
						continue;
					}
					if (strcmp(ext_rvgpu_surface_id,
						   rvgpu_surface_id) == 0 &&
					    ext_scanout_id == scanout_id) {
						json_object_set(
							value, "width",
							json_integer(img_w));
						json_object_set(
							value, "height",
							json_integer(img_h));
						json_object_set(
							value,
							"shared_buffer_fd_index",
							json_integer(fd_index));
						json_object_set(
							value, "initial_color",
							json_integer(
								initial_color));
						int client_rvgpu_fd;
						if (get_int_from_jsonobj(
							    value,
							    "client_rvgpu_fd",
							    &client_rvgpu_fd) ==
						    -1) {
							continue;
						}
						json_array_append_new(
							json_client_rvgpu_fd_array,
							json_integer(
								client_rvgpu_fd));

						json_t *json_texture_array =
							json_object_get(
								value,
								"textures");
						json_t *json_fd_index_array =
							json_object_get(
								value,
								"fd_indexs");
						if (!int_value_in_json_array(
							    json_fd_index_array,
							    fd_index)) {
							insert_integar_json_array_with_index(
								json_fd_index_array,
								fd_index,
								json_integer(
									fd_index));

							GLuint tex_id;
							glGenTextures(1,
								      &tex_id);
							glBindTexture(
								GL_TEXTURE_2D,
								tex_id);
							glTexParameteri(
								GL_TEXTURE_2D,
								GL_TEXTURE_MIN_FILTER,
								GL_LINEAR);
							glTexParameteri(
								GL_TEXTURE_2D,
								GL_TEXTURE_MAG_FILTER,
								GL_LINEAR);
							glTexParameteri(
								GL_TEXTURE_2D,
								GL_TEXTURE_WRAP_S,
								GL_CLAMP_TO_EDGE);
							glTexParameteri(
								GL_TEXTURE_2D,
								GL_TEXTURE_WRAP_T,
								GL_CLAMP_TO_EDGE);
							glBindTexture(
								GL_TEXTURE_2D,
								0);
							json_array_append_new(
								json_texture_array,
								json_integer(
									tex_id));
						}
						if (main_egl->hardware_buffer_enabled &&
						    need_update_fd) {
							json_t *json_tex_id =
								json_array_get(
									json_texture_array,
									fd_index);
							GLuint tex_id = (GLuint)
								json_integer_value(
									json_tex_id);
							json_t *json_buf_handle_array =
								json_object_get(
									value,
									"buf_handles");
							if (!json_buf_handle_array) {
								json_buf_handle_array =
									json_array();
							}
							json_t *json_pre_buf_handle =
								json_array_get(
									json_buf_handle_array,
									fd_index);
							if (json_pre_buf_handle) {
								uintptr_t pre_buf_handle =
									json_integer_value(
										json_pre_buf_handle);
								destroy_hardware_buffer(
									(void *)pre_buf_handle,
									&pf_funcs);
							}
							insert_integar_json_array_with_index(
								json_buf_handle_array,
								fd_index,
								json_integer(
									buf_handle));
							json_object_set(
								value,
								"buf_handles",
								json_buf_handle_array);
							json_t *json_egl_image_array =
								json_object_get(
									value,
									"egl_images");
							if (!json_egl_image_array) {
								json_egl_image_array =
									json_array();
								json_object_set_new(
									value,
									"egl_images",
									json_egl_image_array);
							}
							json_t *json_egl_image =
								json_array_get(
									json_egl_image_array,
									fd_index);

							if (json_egl_image &&
							    json_is_integer(
								    json_egl_image)) {
								uintptr_t egl_image_ptr =
									(uintptr_t)json_integer_value(
										json_egl_image);
								EGLImageKHR pre_eglImage =
									(EGLImageKHR)
										egl_image_ptr;
								if (pre_eglImage !=
								    EGL_NO_IMAGE_KHR) {
									eglDestroyImageKHR(
										main_egl->dpy,
										pre_eglImage);
								}
							}
							EGLImageKHR eglImage = create_egl_image(
								main_egl->dpy,
								img_w, img_h,
								(void *)buf_handle,
								&pf_funcs);
							insert_integar_json_array_with_index(
								json_egl_image_array,
								fd_index,
								json_integer(
									(uintptr_t)
										eglImage));

							glBindTexture(
								GL_TEXTURE_2D,
								tex_id);
							glEGLImageTargetTexture2DOES(
								GL_TEXTURE_2D,
								eglImage);
							printf("update dma buf Surface JSON Object: %s\n",
							       json_dumps(value,
									  0));
						}

						if (!main_egl->hardware_buffer_enabled) {
							if (need_update_fd) {
								json_t *json_buf_handle_array =
									json_object_get(
										value,
										"buf_handles");
								if (!json_buf_handle_array) {
									json_buf_handle_array =
										json_array();
								}
								json_t *json_pre_buf_handle =
									json_array_get(
										json_buf_handle_array,
										fd_index);
								if (json_pre_buf_handle) {
									int pre_buf_handle =
										json_integer_value(
											json_pre_buf_handle);
									destroy_shared_buffer(
										(void *)(uintptr_t)
											pre_buf_handle,
										NULL,
										&pf_funcs);
								}
								insert_integar_json_array_with_index(
									json_buf_handle_array,
									fd_index,
									json_integer(
										buf_handle));
								json_object_set(
									value,
									"buf_handles",
									json_buf_handle_array);
							}

							json_t *json_buf_handle_array =
								json_object_get(
									value,
									"buf_handles");
							json_t *json_buf_handle =
								json_array_get(
									json_buf_handle_array,
									fd_index);
							buf_handle = (int)json_integer_value(
								json_buf_handle);
							void *shm_buffer_ptr = mmap(
								NULL,
								img_w * img_h *
									4,
								PROT_READ,
								MAP_SHARED,
								(int)(uintptr_t)
									buf_handle,
								0);
							if (shm_buffer_ptr ==
							    MAP_FAILED) {
								perror("mmap");
							}

							json_t *json_tex_id =
								json_array_get(
									json_texture_array,
									fd_index);
							if (json_is_integer(
								    json_tex_id)) {
								GLuint tex_id =
									(GLuint)json_integer_value(
										json_tex_id);
								glBindTexture(
									GL_TEXTURE_2D,
									tex_id);

								glTexImage2D(
									GL_TEXTURE_2D,
									0,
									GL_RGBA,
									img_w,
									img_h,
									0,
									GL_RGBA,
									GL_UNSIGNED_BYTE,
									shm_buffer_ptr);

								glBindTexture(
									GL_TEXTURE_2D,
									0);
							}
							munmap(shm_buffer_ptr,
							       img_w * img_h *
								       4);
						}
						break;
					} //target surface
				} //rvgpu surface for loop
				pthread_mutex_unlock(&surface_list_mutex);
			} else {
				//no pollin event
				if (event_num > 0) {
					break;
				}
			}
		} // event while

		struct timespec start;
		clock_gettime(CLOCK_MONOTONIC, &start);

		//draw all surfaces using texture
		glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);

		size_t draw_num = 0;
		if (layout_params.use_rvgpu_layout_draw) {
			size_t safety_index;
			json_t *safety_value;
			if (json_array_size(rvgpu_safety_areas) > 0) {
				json_array_foreach(rvgpu_safety_areas,
						   safety_index, safety_value)
				{
					json_t *x_json = json_object_get(
						safety_value, "x");
					json_t *y_json = json_object_get(
						safety_value, "y");
					json_t *width_json = json_object_get(
						safety_value, "width");
					json_t *height_json = json_object_get(
						safety_value, "height");
					if (!json_is_integer(x_json) ||
					    !json_is_integer(y_json) ||
					    !json_is_integer(width_json) ||
					    !json_is_integer(height_json)) {
						fprintf(stderr,
							"Invalid JSON format for safety area.\n");
						json_array_remove(
							rvgpu_safety_areas,
							safety_index);
						continue;
					}
					int safety_x =
						(int)json_integer_value(x_json);
					int safety_y =
						(int)json_integer_value(y_json);
					int safety_width =
						(int)json_integer_value(
							width_json);
					int safety_height =
						(int)json_integer_value(
							height_json);
					// Check if the area is valid within the display bounds
					if (safety_width <= 0 ||
					    safety_height <= 0 ||
					    safety_x < 0 || safety_y < 0 ||
					    (uint32_t)(safety_x +
						       safety_width) > width ||
					    (uint32_t)(safety_y +
						       safety_height) >
						    height) {
						json_array_remove(
							rvgpu_safety_areas,
							safety_index);
					}
				}
			}
			if (json_array_size(rvgpu_safety_areas) > 0) {
#if 0
				printf("RVGPU Safety Areas: %s\n", json_dumps(rvgpu_safety_areas, 0));
#endif
				glColorMask(GL_FALSE, GL_FALSE, GL_FALSE,
					    GL_FALSE);
				glDepthMask(GL_FALSE);
				glStencilFunc(GL_ALWAYS, 1, 0xFF);
				glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
				json_array_foreach(rvgpu_safety_areas,
						   safety_index, safety_value)
				{
					json_t *x_json = json_object_get(
						safety_value, "x");
					json_t *y_json = json_object_get(
						safety_value, "y");
					json_t *width_json = json_object_get(
						safety_value, "width");
					json_t *height_json = json_object_get(
						safety_value, "height");
					int safety_x =
						(int)json_integer_value(x_json);
					int safety_y =
						(int)json_integer_value(y_json);
					int safety_width =
						(int)json_integer_value(
							width_json);
					int safety_height =
						(int)json_integer_value(
							height_json);
					draw_2d_texture_layout(
						transparentTex, safety_width,
						safety_height, safety_x,
						safety_y, safety_width,
						safety_height, safety_x,
						safety_y, safety_width,
						safety_height, 0);
				}
				glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
				glDepthMask(GL_TRUE);

				glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
				glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
			}

			size_t sfc_index;
			json_t *sfc_value;
			pthread_mutex_lock(&layout_list_mutex);
			json_array_foreach(rvgpu_layout_list, sfc_index,
					   sfc_value)
			{
#if 0
				printf("Layout Surface List JSON Object %zu: %s\n", sfc_index, json_dumps(sfc_value, 0));
#endif
				int layout_id;
				if (get_int_from_jsonobj(sfc_value, "id",
							 &layout_id) == -1) {
					continue;
				}
				const char *rvgpu_surface_id;
				if (get_str_from_jsonobj(
					    sfc_value, "rvgpu_surface_id",
					    &rvgpu_surface_id) == -1) {
					continue;
				}
				int scanout_id;
				if (get_int_from_jsonobj(sfc_value,
							 "scanout_id",
							 &scanout_id) == -1) {
					// Warning: 'scanout_id' not found in layout JSON. Defaulting to scanout_id = 0
					scanout_id = 0;
				}
				int need_draw = 0;
				int initial_color = 0;
				int img_w, img_h;
				int fd_index;
				GLuint texture_id;
				pthread_mutex_lock(&surface_list_mutex);
				json_array_foreach(rvgpu_surface_list, index,
						   value)
				{
					const char *ext_rvgpu_surface_id;
					if (get_str_from_jsonobj(
						    value, "rvgpu_surface_id",
						    &ext_rvgpu_surface_id) ==
					    -1) {
						continue;
					}
					int ext_scanout_id;
					if (get_int_from_jsonobj(
						    value, "scanout_id",
						    &ext_scanout_id) == -1) {
						continue;
					}
					if (strcmp(ext_rvgpu_surface_id,
						   rvgpu_surface_id) == 0 &&
					    ext_scanout_id == scanout_id) {
						if (get_int_from_jsonobj(
							    value, "width",
							    &img_w) == -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    value, "height",
							    &img_h) == -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    value,
							    "shared_buffer_fd_index",
							    &fd_index) == -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    value,
							    "initial_color",
							    &initial_color) ==
						    -1) {
							continue;
						}
						if (get_int_from_jsonobj(
							    value, "scanout_id",
							    &scanout_id) ==
						    -1) {
							continue;
						}
						json_t *json_texture_array =
							json_object_get(
								value,
								"textures");
						json_t *json_tex_id =
							json_array_get(
								json_texture_array,
								fd_index);
						texture_id = (GLuint)
							json_integer_value(
								json_tex_id);
						need_draw = 1;
						break;
					}
				}
				pthread_mutex_unlock(&surface_list_mutex);
				if (need_draw) {
					double src_x = 0;
					double src_y = 0;
					double src_w = (double)img_w;
					double src_h = (double)img_h;
					double dst_x = 0;
					double dst_y = 0;
					double dst_w = (double)width;
					double dst_h = (double)height;

					get_double_from_jsonobj(
						sfc_value, "src_x", &src_x);
					get_double_from_jsonobj(
						sfc_value, "src_y", &src_y);
					get_double_from_jsonobj(
						sfc_value, "src_w", &src_w);
					get_double_from_jsonobj(
						sfc_value, "src_h", &src_h);
					get_double_from_jsonobj(
						sfc_value, "dst_x", &dst_x);
					get_double_from_jsonobj(
						sfc_value, "dst_y", &dst_y);
					get_double_from_jsonobj(
						sfc_value, "dst_w", &dst_w);
					get_double_from_jsonobj(
						sfc_value, "dst_h", &dst_h);

					if (!initial_color) {
						if (src_x > img_w ||
						    src_x + src_w < 0) {
							continue;
						}

						if (src_y > img_h ||
						    src_y + src_h < 0) {
							continue;
						}

						if (dst_x > width ||
						    dst_x + dst_w < 0) {
							continue;
						}

						if (dst_y > height ||
						    dst_y + dst_h < 0) {
							continue;
						}

						if (src_x < 0) {
							src_w += src_x;
							src_x = 0;
						}
						if (src_x + src_w > img_w) {
							src_w = img_w - src_x;
						}

						if (src_y < 0) {
							src_h += src_y;
							src_y = 0;
						}

						if (src_y + src_h > img_h) {
							src_h = img_h - src_y;
						}
					} else {
						src_x = 0;
						src_y = 0;
						src_w = img_w;
						src_h = img_h;
					}
					draw_2d_texture_layout(
						texture_id, img_w, img_h, src_x,
						src_y, src_w, src_h, dst_x,
						dst_y, dst_w, dst_h, 0);
					draw_num++;
#if 0
                                        printf("draw_2d_texture_layout layout_id: %d, rvgpu_surface_id: %s, scanout_id: %d, img_w: %d, img_h: %d src_x: %.3f, src_y: %.3f, src_w: %.3f, src_h: %.3f, dst_x: %.3f, dst_y: %.3f, dst_w: %.3f, dst_h: %.3f\n",
                                                        layout_id, rvgpu_surface_id, scanout_id, img_w, img_h,
                                                        src_x, src_y, src_w, src_h,
                                                        dst_x, dst_y, dst_w, dst_h);
#endif
				}
			}
			pthread_mutex_unlock(&layout_list_mutex);
		} else {
			json_array_foreach(rvgpu_surface_list, index, value)
			{
				int img_w, img_h;
				int fd_index;
				GLuint texture_id;
				if (get_int_from_jsonobj(value, "width",
							 &img_w) == -1) {
					continue;
				}
				if (get_int_from_jsonobj(value, "height",
							 &img_h) == -1) {
					continue;
				}
				if (get_int_from_jsonobj(
					    value, "shared_buffer_fd_index",
					    &fd_index) == -1) {
					continue;
				}
				json_t *json_texture_array =
					json_object_get(value, "textures");
				json_t *json_tex_id = json_array_get(
					json_texture_array, fd_index);
				texture_id =
					(GLuint)json_integer_value(json_tex_id);
				draw_2d_texture_layout(texture_id, img_w, img_h,
						       0, 0, img_w, img_h, 0, 0,
						       img_w, img_h, 0);
				draw_num++;
#if 0
				printf("Draw JSON Object %zu: %s\n", index, json_dumps(value, 0));
#endif
			}
		}

		rvgpu_pf_swap(main_egl, vsync, &pf_funcs);

		if (layout_params.use_layout_sync && layout_event) {
			pthread_mutex_lock(&layout_sync_mutex);
			layout_status = LAYOUT_COMPLETED;
			pthread_cond_signal(&layout_sync_cond);
			pthread_mutex_unlock(&layout_sync_mutex);
		}

#if 0
		static uint32_t swap_cnt;
		static double total_time;
		struct timespec end;
		swap_cnt++;
		clock_gettime(CLOCK_MONOTONIC, &end);
		double duration = (end.tv_sec - start.tv_sec) * 1000.0 +
			   (end.tv_nsec - start.tv_nsec) / 1e6;
		total_time += duration;

		printf("eglSwapBuffers (draw_num: %ld, evnum (layout/draw): %d/%d) Overhead: %.3f milliseconds (ave: %.3f)\n", draw_num, layout_event_num, event_num, duration, total_time / swap_cnt);
#endif

	} //main while
	pthread_join(regislation_read_loop_thread, NULL);
	if (layout_params.use_rvgpu_layout_draw) {
		pthread_join(layout_event_loop_thread, NULL);
	}
	close(event_fd);
	rvgpu_egl_free(main_egl);
	rvgpu_egl_pf_free(main_egl, &pf_funcs);
	pthread_mutex_destroy(&surface_list_mutex);
	pthread_mutex_destroy(&input_send_event_mutex);

	pthread_mutex_destroy(&layout_list_mutex);
	pthread_mutex_destroy(&rvgpu_request_mutex);
	pthread_mutex_destroy(&layout_sync_mutex);
	pthread_cond_destroy(&layout_sync_cond);
	json_decref(rvgpu_surface_list);
	json_decref(rvgpu_layout_list);
	json_decref(rvgpu_safety_areas);
	if (layout_params.use_rvgpu_layout_draw) {
		glDisable(GL_STENCIL_TEST);
	}
}

static void *rvgpu_input_event_loop(void *arg)
{
	struct input_event_thread_params *params =
		(struct input_event_thread_params *)arg;
	int server_rvgpu_fd = params->server_rvgpu_fd;
	int command_socket = params->command_socket;
	struct rvgpu_scanout *scanouts = params->scanouts;
	struct rvgpu_layout_params layout_params = params->layout_params;
	FILE *input_stream = fdopen(command_socket, "w");
	setvbuf(input_stream, NULL, _IOFBF, BUFSIZ);
	struct rvgpu_input_state *in = rvgpu_in_init(input_stream);
	static double pointer_pos_x;
	static double pointer_pos_y;

	struct pollfd pfd = { .fd = server_rvgpu_fd, .events = POLLIN };
	while (1) {
		int ret = poll(&pfd, 1, -1);
		if (ret == -1) {
			perror("rvgpu_input_event_loop poll");
			break;
		}
		if (pfd.revents & POLLIN) {
			json_t *json_obj = recv_json(server_rvgpu_fd);
			int event_id;
			if (get_int_from_jsonobj(json_obj, "event_id",
						 &event_id) == -1) {
				continue;
			}
			double x, y;
			int32_t key, value;
			int input_id;
			get_int_from_jsonobj(json_obj, "input_id", &input_id);
			get_double_from_jsonobj(json_obj, "x", &x);
			get_double_from_jsonobj(json_obj, "y", &y);
			get_int_from_jsonobj(json_obj, "key", &key);
			get_int_from_jsonobj(json_obj, "value", &value);

			if (layout_params.use_rvgpu_layout_draw) {
				double src_x, src_y, src_w, src_h;
				double dst_x, dst_y, dst_w, dst_h;
				get_double_from_jsonobj(json_obj, "src_x",
							&src_x);
				get_double_from_jsonobj(json_obj, "src_y",
							&src_y);
				get_double_from_jsonobj(json_obj, "src_w",
							&src_w);
				get_double_from_jsonobj(json_obj, "src_h",
							&src_h);
				get_double_from_jsonobj(json_obj, "dst_x",
							&dst_x);
				get_double_from_jsonobj(json_obj, "dst_y",
							&dst_y);
				get_double_from_jsonobj(json_obj, "dst_w",
							&dst_w);
				get_double_from_jsonobj(json_obj, "dst_h",
							&dst_h);
				if (dst_w > 0 && dst_h > 0) {
					double relative_x = x - dst_x;
					double relative_y = y - dst_y;
					double scale_x = (double)src_w / dst_w;
					double scale_y = (double)src_h / dst_h;
					x = relative_x * scale_x + src_x;
					y = relative_y * scale_y + src_y;
				}
			}

			//TODO only support scanout 0
			struct rvgpu_scanout *s = &scanouts[0];
			switch (event_id) {
			case RVGPU_TOUCH_DOWN_EVENT_ID:
				rvgpu_in_add_slot(in, input_id, 0, &s->window,
						  &s->virgl.box, &s->virgl.tex);
				rvgpu_in_move_slot(in, input_id, (double)x,
						   (double)y);
				break;
			case RVGPU_TOUCH_UP_EVENT_ID:
				rvgpu_in_remove_slot(in, input_id);
				rvgpu_in_send(in, RVGPU_INPUT_TOUCH);
				break;
			case RVGPU_TOUCH_MOTION_EVENT_ID:
				rvgpu_in_move_slot(in, input_id, (double)x,
						   (double)y);
				break;
			case RVGPU_TOUCH_FRAME_EVENT_ID:
				rvgpu_in_send(in, RVGPU_INPUT_TOUCH);
				break;
			case RVGPU_TOUCH_CANCEL_EVENT_ID:
				rvgpu_in_clear(in, RVGPU_INPUT_TOUCH);
				break;
			case RVGPU_POINTER_ENTER_EVENT_ID:
				pointer_pos_x = x;
				pointer_pos_y = y;
				struct rvgpu_input_event evs[] = {
					{ EV_ABS, ABS_X, pointer_pos_x },
					{ EV_ABS, ABS_Y, pointer_pos_y },
				};
				rvgpu_in_events(in, RVGPU_INPUT_MOUSE_ABS, evs,
						2);
				rvgpu_in_send(in, RVGPU_INPUT_MOUSE_ABS);
				break;
			case RVGPU_POINTER_LEAVE_EVENT_ID:
				break;
			case RVGPU_POINTER_MOTION_EVENT_ID: {
				int relative_x = x - pointer_pos_x;
				int relative_y = y - pointer_pos_y;
				pointer_pos_x += relative_x;
				pointer_pos_y += relative_y;

				struct rvgpu_input_event evs[] = {
					{ EV_REL, REL_X, relative_x },
					{ EV_REL, REL_Y, relative_y },
				};
				if (relative_x == 0) {
					rvgpu_in_events(in, RVGPU_INPUT_MOUSE,
							&evs[1], 1);
				} else if (relative_y == 0) {
					rvgpu_in_events(in, RVGPU_INPUT_MOUSE,
							&evs[0], 1);
				} else {
					rvgpu_in_events(in, RVGPU_INPUT_MOUSE,
							evs, 2);
				}
				rvgpu_in_send(in, RVGPU_INPUT_MOUSE);
				break;
			}
			case RVGPU_POINTER_BUTTON_EVENT_ID: {
				struct rvgpu_input_event evs[] = {
					{ EV_KEY, (uint16_t)key,
					  (int32_t)value },
				};
				rvgpu_in_events(in, RVGPU_INPUT_MOUSE, evs, 1);
				rvgpu_in_send(in, RVGPU_INPUT_MOUSE);
				break;
			}
			case RVGPU_POINTER_AXIS_EVENT_ID: {
				struct rvgpu_input_event evs[] = {
					{ EV_REL, (uint16_t)key,
					  (int32_t)value },
				};
				rvgpu_in_events(in, RVGPU_INPUT_MOUSE, evs, 1);
				rvgpu_in_send(in, RVGPU_INPUT_MOUSE);
				break;
			}
			case RVGPU_KEYBOARD_EVENT_ID: {
				struct rvgpu_input_event evs[] = {
					{ EV_KEY, (uint16_t)key,
					  (int32_t)value },
				};
				rvgpu_in_events(in, RVGPU_INPUT_KEYBOARD, evs,
						1);
				rvgpu_in_send(in, RVGPU_INPUT_KEYBOARD);
				break;
			}
			default:
				break;
			}
#if 0
			char *json_str = json_dumps(json_obj, JSON_ENCODE_ANY);
			printf("rvgpu_input_event_loop json_str: %s\n", json_str);
#endif
		}
	}
	return NULL;
}

void rvgpu_frame_sync_wait(double frame_rate, double *last_frame_time)
{
	if (*last_frame_time == 0) {
		*last_frame_time = current_get_time_ms();
		return;
	}

	double elapsed_ms = current_get_time_ms() - *last_frame_time;
	double sleep_ms = 1000.0 / frame_rate - elapsed_ms;
	if (sleep_ms > 0) {
		struct timespec req, rem;
		req.tv_sec = (time_t)(sleep_ms / 1000);
		req.tv_nsec = (long)((sleep_ms - req.tv_sec * 1000) * 1e6);
		nanosleep(&req, &rem);
	}

	*last_frame_time = current_get_time_ms();
}

void rvgpu_render(struct render_params *params)
{
	EGL_GET_PROC_ADDR(glEGLImageTargetTexture2DOES);
	EGL_GET_PROC_ADDR(glEGLImageTargetRenderbufferStorageOES);
	EGL_GET_PROC_ADDR(eglCreateImageKHR);
	EGL_GET_PROC_ADDR(eglDestroyImageKHR);
	pf_funcs = (platform_funcs_t)params->pf_funcs;
	int command_socket = params->command_socket;
	int resource_socket = params->resource_socket;
	uint32_t max_vsync_rate = params->max_vsync_rate;
	bool vsync = params->vsync;
	char *rvgpu_surface_id = params->rvgpu_surface_id;
	char *carddev = params->carddev;
	char *capset_file = params->capset_file;
	struct rvgpu_egl_params egl_params = params->egl_params;
	struct rvgpu_layout_params layout_params = params->layout_params;
	struct rvgpu_domain_sock_params domain_params = params->domain_params;
	fprintf(stderr, "rvgpu_compositor_sock_path: %s\n",
		domain_params.rvgpu_compositor_sock_path);
	int server_rvgpu_fd =
		connect_to_server(domain_params.rvgpu_compositor_sock_path);
	struct rvgpu_fps_params fps_params = params->fps_params;
	printf("connect_to_server server_rvgpu_fd: %d\n", server_rvgpu_fd);
	if (server_rvgpu_fd < 0) {
		fprintf(stderr, "Failed to connect to server\n");
		return;
	}

	send_str_with_size(server_rvgpu_fd, rvgpu_surface_id);
	int ret;
	recv_int(server_rvgpu_fd, &ret);
	if (ret < 0) {
		fprintf(stderr, "Failed to start renderer\n");
		close(server_rvgpu_fd);
		return;
	}

	void *offscreen_display;
	if (carddev != NULL) {
		offscreen_display =
			rvgpu_create_pf_native_display(carddev, &pf_funcs);
	} else {
		offscreen_display =
			rvgpu_create_pf_native_display(NULL, &pf_funcs);
	}

	struct rvgpu_egl_state *egl = rvgpu_offscreen_init(offscreen_display);
	egl->hardware_buffer_enabled =
		get_hardware_buffer_cap(egl->dpy, &pf_funcs);
	egl->rvgpu_surface_id = rvgpu_surface_id;
	egl->server_rvgpu_fd = server_rvgpu_fd;
	egl->egl_params = egl_params;

	struct rvgpu_scanout_params sp[VIRTIO_GPU_MAX_SCANOUTS];
	struct rvgpu_pr_params pp = {
		.sp = sp,
		.nsp = VIRTIO_GPU_MAX_SCANOUTS,
	};
	if (capset_file != NULL)
		pp.capset = fopen(capset_file, "w");

	struct rvgpu_pr_state *pr =
		rvgpu_pr_init(egl, &pp, command_socket, resource_socket);

	json_t *add_scanout_json_obj = json_object();
	json_object_set_new(add_scanout_json_obj, "event_id",
			    json_integer(RVGPU_ADD_EVENT_ID));
	for (unsigned int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
		struct rvgpu_scanout *s = &egl->scanouts[i];
		s->scanout_id = i;
		sp[i].boxed = false;
		sp[i].enabled = true;
		s->params = sp[i];
		if (sp[i].enabled) {
			egl->cb->create_scanout(egl, &egl->scanouts[i]);
			if (fps_params.show_fps) {
				s->fps_params = fps_params;
				s->fps_params.rvgpu_laptime_ms =
					current_get_time_ms();
			}
			char *add_scanout_json_str;
			json_object_set_new(add_scanout_json_obj, "scanout_id",
					    json_integer(s->scanout_id));
			add_scanout_json_str = json_dumps(add_scanout_json_obj,
							  JSON_ENCODE_ANY);
			send_str_with_size(server_rvgpu_fd,
					   add_scanout_json_str);
			if (layout_params.use_rvgpu_layout_draw) {
				// Initialize the background color for each rvgpu-proxy.
				rvgpu_egl_draw(egl, &egl->scanouts[i], vsync);
			}
		}
	}
	json_decref(add_scanout_json_obj);

	struct input_event_thread_params *input_params =
		(struct input_event_thread_params *)calloc(
			1, sizeof(struct input_event_thread_params));
	input_params->server_rvgpu_fd = egl->server_rvgpu_fd;
	input_params->command_socket = command_socket;
	input_params->scanouts = egl->scanouts;
	input_params->layout_params = layout_params;
	pthread_t rvgpu_event_thread;
	pthread_create(&rvgpu_event_thread, NULL, rvgpu_input_event_loop,
		       input_params);

	unsigned int res_id;
	double last_frame_time = 0.0;
	while ((res_id = rvgpu_pr_dispatch(pr))) {
		rvgpu_egl_drawall(egl, res_id, vsync);
		if (vsync)
			rvgpu_frame_sync_wait(max_vsync_rate, &last_frame_time);
	}

	char *json_str;
	json_t *json_cmd_obj = json_object();
	json_object_set_new(json_cmd_obj, "event_id",
			    json_integer(RVGPU_REMOVE_EVENT_ID));
	json_str = json_dumps(json_cmd_obj, JSON_ENCODE_ANY);
	send_str_with_size(egl->server_rvgpu_fd, json_str);

	rvgpu_pr_free(pr);
	rvgpu_egl_free(egl);
	rvgpu_destroy_pf_native_display(offscreen_display, &pf_funcs);
	close(server_rvgpu_fd);
	free(egl);
	free(input_params);
}

void *rvgpu_egl_pf_init(void *egl_pf_init_params, uint32_t *width,
			uint32_t *height, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->rvgpu_egl_pf_init) {
		return pf_funcs->rvgpu_egl_pf_init(egl_pf_init_params, width,
						   height);
	}
	return NULL;
}

void rvgpu_egl_pf_free(void *egl_pf_free_params, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->rvgpu_egl_pf_free) {
		return pf_funcs->rvgpu_egl_pf_free(egl_pf_free_params);
	}
}

void *rvgpu_create_pf_native_display(void *arg, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->rvgpu_create_pf_native_display) {
		return pf_funcs->rvgpu_create_pf_native_display(arg);
	}
	return NULL;
}

void rvgpu_destroy_pf_native_display(void *arg, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->rvgpu_destroy_pf_native_display) {
		pf_funcs->rvgpu_destroy_pf_native_display(arg);
	}
}

void rvgpu_pf_swap(void *param, bool vsync, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->rvgpu_pf_swap) {
		pf_funcs->rvgpu_pf_swap(param, vsync);
	}
}

void send_buffer_handle(uint32_t client_fd, void *buffer_handle,
			platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->send_buffer_handle) {
		pf_funcs->send_buffer_handle(client_fd, buffer_handle);
	}
}

void *recv_buffer_handle(uint32_t client_fd, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->recv_buffer_handle) {
		return pf_funcs->recv_buffer_handle(client_fd);
	}
	return NULL;
}

bool get_hardware_buffer_cap(EGLDisplay dpy, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->get_hardware_buffer_cap) {
		return pf_funcs->get_hardware_buffer_cap(dpy);
	}
	return false;
}

void *create_hardware_buffer(uint32_t width, uint32_t height,
			     platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->create_hardware_buffer) {
		return pf_funcs->create_hardware_buffer(width, height);
	}
	return NULL;
}

void *create_shared_buffer(const char *name, uint32_t width, uint32_t height,
			   platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->create_shared_buffer) {
		return pf_funcs->create_shared_buffer(name, width, height);
	}
	return NULL;
}

void destroy_hardware_buffer(void *buffer_handle, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->destroy_hardware_buffer) {
		pf_funcs->destroy_hardware_buffer(buffer_handle);
	}
}

void destroy_shared_buffer(void *buffer_handle, const char *name,
			   platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->destroy_shared_buffer) {
		pf_funcs->destroy_shared_buffer(buffer_handle, name);
	}
}

EGLImageKHR create_egl_image(EGLDisplay dpy, uint32_t width, uint32_t height,
			     void *buffer_handle, platform_funcs_t *pf_funcs)
{
	if (pf_funcs && pf_funcs->create_egl_image) {
		return pf_funcs->create_egl_image(dpy, width, height,
						  buffer_handle);
	}
	return EGL_NO_IMAGE_KHR;
}

void rvgpu_compositor_run(struct rvgpu_compositor_params *params)
{
	platform_funcs_t pf_funcs = params->pf_funcs;
	void *egl_pf_init_params = params->egl_pf_init_params;
	struct rvgpu_egl_params egl_params = params->egl_params;
	struct rvgpu_layout_params layout_params = params->layout_params;
	if (layout_params.use_rvgpu_layout_draw) {
		isPointWithinBounds = check_in_rvgpu_layout_draw;
		getRvgpuFocus = get_focus_rvgpu_layout;
	} else {
		isPointWithinBounds = check_in_rvgpu_draw;
		getRvgpuFocus = get_focus_rvgpu;
	}
	bool translucent = params->translucent;
	bool fullscreen = params->fullscreen;
	bool vsync = params->vsync;
	uint32_t width = params->width;
	uint32_t height = params->height;
	uint32_t ivi_surface_id = params->ivi_surface_id;
	char *carddev = params->carddev;
	char *seat = params->seat;
	char *domain_name = params->domain_name;

	char rvgpu_compositor_sock_path[256];
	char rvgpu_layout_sock_path[256];
	snprintf(rvgpu_compositor_sock_path, sizeof(rvgpu_compositor_sock_path),
		 "%s.%s", UHMI_RVGPU_COMPOSITOR_SOCK, domain_name);
	snprintf(rvgpu_layout_sock_path, sizeof(rvgpu_layout_sock_path),
		 "%s.%s", UHMI_RVGPU_LAYOUT_SOCK, domain_name);

	struct rvgpu_domain_sock_params domain_params = {
		rvgpu_compositor_sock_path, rvgpu_layout_sock_path
	};

	if (access(rvgpu_compositor_sock_path, F_OK) == 0) {
		fprintf(stderr, "Error: The domain is already in use (%s).\n",
			rvgpu_compositor_sock_path);
		exit(0);
	}

	if (access(rvgpu_layout_sock_path, F_OK) == 0) {
		fprintf(stderr, "Error: The domain is already in use (%s).\n",
			rvgpu_layout_sock_path);
		exit(0);
	}

	// Communication pipe between the compositor thread and request threads
	int pipefd[2];
	if (pipe(pipefd) == -1) {
		fprintf(stderr, "Failed to create pipe\n");
	}

	struct compositor_params *compositor_params =
		(struct compositor_params *)calloc(
			1, sizeof(struct compositor_params));
	compositor_params->pf_funcs = pf_funcs;
	compositor_params->egl_pf_init_params = egl_pf_init_params;
	compositor_params->egl_params = egl_params;
	compositor_params->layout_params = layout_params;
	compositor_params->domain_params = domain_params;
	compositor_params->translucent = translucent;
	compositor_params->fullscreen = fullscreen;
	compositor_params->vsync = vsync;
	compositor_params->width = width;
	compositor_params->height = height;
	compositor_params->ivi_surface_id = ivi_surface_id;
	compositor_params->req_read_fd = pipefd[0];
	compositor_params->carddev = carddev;
	compositor_params->seat = seat;

	struct request_thread_params *request_tp =
		(struct request_thread_params *)calloc(
			1, sizeof(struct request_thread_params));
	request_tp->req_write_fd = pipefd[1];
	request_tp->layout_params = layout_params;
	request_tp->domain_params = domain_params;

	compositor_render(compositor_params, request_tp);
	free(compositor_params);
	free(request_tp);
}
