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
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <errno.h>

#include <err.h>

#include <jansson.h>

#include <rvgpu-generic/rvgpu-sanity.h>
#include <rvgpu-utils/rvgpu-utils.h>
#include <rvgpu-renderer/rvgpu-renderer.h>
#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/backend/rvgpu-gbm.h>
#include <rvgpu-renderer/backend/rvgpu-wayland.h>
#include <rvgpu-renderer/compositor/rvgpu-compositor.h>
#include <rvgpu-renderer/compositor/rvgpu-buffer-fd.h>
#include <rvgpu-renderer/compositor/rvgpu-json-helpers.h>
#include <rvgpu-renderer/compositor/rvgpu-connection.h>

static void usage(void)
{
	static const char program_name[] = "rvgpu-renderer";

	info("Usage: %s [options]\n", program_name);
	info("\t-B color\tcolor of initial screen in RGBA format");
	info("(0xRRGGBBAA, default is 0x%08x)\n", BACKEND_COLOR);
	info("\t-c capset\tdump capset into file\n");
	info("\t-b box\t\toverride scanout box (format WxH@X,Y)\n");
	info("\t-i ID\t\tset scanout window ID (for IVI shell)\n");
	info("\t-g card\t\tuse GBM mode on card (/dev/dri/cardN)\n");
	info("\t-d domain\tset domain name for unix socket\n");
	info("\t-S seat\t\tspecify seat for input in GBM mode\n");
	info("\t-f output\tset output id for fullscreen mode on Wayland\n");
	info("\t-p port\t\tport for listening (default: %u)\n",
	     RVGPU_DEFAULT_PORT);
	info("\t-V fps\t\tset vsync framerate (default: %u fps)\n",
	     RVGPU_DEFAULT_VSYNC_FRAMERATE);
	info("\t-F file\t\tdump FPS and frame time measurements into file\n");
	info("\t-a\t\tenable translucent mode on Wayland\n");
	info("\t-v\t\tRun in vsync mode (default: false)\n");
	info("\t-l\t\tuse layout draw mode based on layout information\n");
	info("\t-L\t\tenable layout sync mode\n");
	info("\t-h\t\tShow this message\n");
}

void signal_handler(int sig)
{
	static bool parent_exit = false;
	pid_t pgid = getpgrp();
	pid_t pid = getpid();
	//printf("Process pgid: %d, pid: %d received signal %d\n", pgid, pid, sig);
	if (sig == SIGTERM || sig == SIGINT || sig == SIGQUIT) {
		if (pgid == pid) {
			if (!parent_exit) {
				kill(0, sig);
				parent_exit = true;
			} else {
				exit(0);
			}
		} else {
			exit(0);
		}
	}
}

static void wait_for_child(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

void rvgpu_handle_connection(struct rvgpu_compositor_params *params)
{
	platform_funcs_t pf_funcs = params->pf_funcs;
	struct rvgpu_egl_params egl_params = params->egl_params;
	struct rvgpu_fps_params fps_params = params->fps_params;
	struct rvgpu_layout_params layout_params = params->layout_params;
	bool vsync = params->vsync;
	uint16_t port_no = params->port_no;
	uint32_t max_vsync_rate = params->max_vsync_rate;
	char *carddev = params->carddev;
	char *domain_name = params->domain_name;
	char *capset_file = params->capset_file;

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

	int sock = -1;
	struct sockaddr_in server_addr = { 0 };
	int reuseaddr = 1;
	int fin_wait = 1;
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == -1) {
		err(1, "socket");
	}

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
		       sizeof(int)) == -1) {
		err(1, "setsockopt");
	}

	if (setsockopt(sock, SOL_TCP, TCP_LINGER2, &fin_wait, sizeof(int)) ==
	    -1) {
		err(1, "setsockopt");
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(port_no);

	if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) ==
	    -1) {
		err(1, "bind");
	}

	if (listen(sock, BACKLOG) == -1) {
		err(1, "listen");
	}

	json_t *proxy_list = json_array();
	int num_proxy = 0;
	while (1) {
		int newsock = accept4(sock, NULL, NULL, SOCK_NONBLOCK);
		if (newsock == -1)
			err(1, "accept");

		int rsocket = accept4(sock, NULL, NULL, SOCK_NONBLOCK);
		if (rsocket == -1)
			err(1, "accept");

		int result = 0;
		struct pollfd fds;
		fds.fd = newsock;
		fds.events = POLLIN;
		num_proxy++;
		int max_id_length = 256;
		char rvgpu_surface_id[max_id_length];
		json_t *json_proxy_obj = NULL;
		int ret = poll(&fds, 1, -1);
		if (ret == -1) {
			perror("rvgpu_compositor_run poll");
			result = -1;
		} else {
			if (fds.revents & POLLIN) {
				char *received_data = recv_str_all(newsock);
				strncpy(rvgpu_surface_id, received_data,
					max_id_length - 1);
				rvgpu_surface_id[max_id_length - 1] = '\0';
				free(received_data);
				if (strcmp(rvgpu_surface_id, "no") == 0) {
					snprintf(rvgpu_surface_id,
						 sizeof(rvgpu_surface_id), "%d",
						 num_proxy * 1000);
				}
				bool hasID = str_value_in_json_array_with_key(
					proxy_list, "rvgpu_surface_id",
					rvgpu_surface_id);
				if (hasID) {
					json_t *ext_json_proxy_obj =
						get_jsonobj_with_str_key(
							proxy_list,
							"rvgpu_surface_id",
							rvgpu_surface_id);
					int render_pid;
					get_int_from_jsonobj(ext_json_proxy_obj,
							     "render_pid",
							     &render_pid);
					if (kill(render_pid, 0) == 0) {
						printf("render_pid %d is alive\n",
						       render_pid);
					} else {
						printf("render_pid %d is not alive\n",
						       render_pid);
						remove_jsonobj_with_int_key(
							proxy_list,
							"render_pid",
							render_pid);
						hasID = false;
					}
				}
				if (!hasID) {
					json_proxy_obj = json_object();
					json_object_set_new(
						json_proxy_obj,
						"rvgpu_surface_id",
						json_string(rvgpu_surface_id));
				} else {
					fprintf(stderr,
						"has already used rvgpu_surface_id: %s\n",
						rvgpu_surface_id);
					result = -1;
				}
			}
		}

		if (result == -1) {
			close(newsock);
			close(rsocket);
			continue;
		}

		pid_t pid = fork();
		switch (pid) {
		case 0: {
			pid = getpid();
			struct render_params *render_params =
				(struct render_params *)calloc(
					1, sizeof(struct render_params));
			render_params->pf_funcs = pf_funcs;
			render_params->command_socket = newsock;
			render_params->resource_socket = rsocket;
			render_params->max_vsync_rate = max_vsync_rate;
			render_params->vsync = vsync;
			render_params->rvgpu_surface_id = rvgpu_surface_id;
			render_params->fps_params = fps_params;
			render_params->carddev = carddev;
			render_params->egl_params = egl_params;
			render_params->layout_params = layout_params;
			render_params->domain_params = domain_params;
			render_params->capset_file = capset_file;
			rvgpu_render(render_params);
			close(render_params->command_socket);
			close(render_params->resource_socket);
			free(render_params);
			printf("rvgpu_surface_id %s render process finished\n",
			       rvgpu_surface_id);
			_exit(0);
		}
		default:
			if (json_proxy_obj != NULL) {
				json_object_set_new(json_proxy_obj,
						    "render_pid",
						    json_integer(pid));
				json_array_append_new(
					proxy_list,
					json_deep_copy(json_proxy_obj));
				json_decref(json_proxy_obj);
			}
			close(newsock);
			close(rsocket);
		}
	}
	close(sock);
}

int main(int argc, char **argv)
{
	struct rvgpu_egl_params egl_params = {
		.clear_color = BACKEND_COLOR,
	};
	char *errstr = NULL;
	char *carddev = NULL;
	char *capset_file = NULL;
	char *domain_name = NULL;
	char *seat = "seat0";
	int w, h, x, y, opt = 0;
	uint16_t port_no = RVGPU_DEFAULT_PORT;
	bool fullscreen = false, vsync = false, translucent = false;

	uint32_t width = 800;
	uint32_t height = 600;
	uint32_t ivi_surface_id = 0;
	uint32_t output_id = 0;
	uint32_t max_vsync_rate = RVGPU_DEFAULT_VSYNC_FRAMERATE;
	struct rvgpu_fps_params fps_params = { 0 };
	struct rvgpu_layout_params layout_params = { 0 };

	while ((opt = getopt(argc, argv, "ahvlLi:c:s:S:f:b:B:p:g:d:V:F:")) !=
	       -1) {
		switch (opt) {
		case 'a':
			translucent = true;
			break;
		case 'B':
			egl_params.clear_color = (unsigned int)sanity_strtonum(
				optarg, 0, 0xFFFFFFFF, &errstr);
			if (errstr != NULL) {
				warnx("Background color should be in 0 - 0xFFFFFFFF\n");
				errx(1,
				     "Invalid background color specified %s:%s",
				     optarg, errstr);
			}
			break;

		case 'c':
			capset_file = optarg;
			break;
		case 'b':
			if (sscanf(optarg, "%dx%d@%d,%d", &w, &h, &x, &y) !=
			    4) {
				errx(1, "invalid scanout box %s", optarg);
			} else {
				if (w > 0 && h > 0) {
					width = (unsigned int)w;
					height = (unsigned int)h;
				}
			}
			break;
		case 'i':
			ivi_surface_id = (uint32_t)sanity_strtonum(
				optarg, 1, UINT32_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "Invalid IVI id specified %s:%s",
				     optarg, errstr);
			break;
		case 'g':
			carddev = optarg;
			break;
		case 'S':
			seat = optarg;
			break;
		case 'f':
			fullscreen = true;
			output_id = (uint32_t)sanity_strtonum(
				optarg, 0, UINT32_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "Invalid output id specified %s:%s",
				     optarg, errstr);
			break;
		case 'd':
			domain_name = optarg;
			break;
		case 'p':
			port_no = (uint16_t)sanity_strtonum(optarg,
							    MIN_PORT_NUMBER,
							    MAX_PORT_NUMBER,
							    &errstr);
			if (errstr != NULL) {
				warnx("Port number should be in [%u..%u]\n",
				      MIN_PORT_NUMBER, MAX_PORT_NUMBER);
				errx(1, "Invalid port number %s:%s", optarg,
				     errstr);
			}
			break;
		case 'V':
			max_vsync_rate = (uint32_t)sanity_strtonum(
				optarg, 1, UINT32_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "Invalid vsync rate specified %s:%s",
				     optarg, errstr);
			break;
		case 'v':
			vsync = true;
			break;
		case 'F':
			fps_params.fps_dump_path = optarg;
			fps_params.show_fps = true;
			break;
		case 'l':
			layout_params.use_rvgpu_layout_draw = true;
			break;
		case 'L':
			layout_params.use_layout_sync = true;
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		default:
			usage();
			exit(EXIT_FAILURE);
		}
	}

	setpgid(0, 0);
	struct sigaction sa;
	sa.sa_handler = signal_handler;
	sa.sa_flags = 0;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGQUIT, &sa, NULL);

	struct sigaction sa_chld;
	sa_chld.sa_handler = wait_for_child;
	sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
	sigemptyset(&sa_chld.sa_mask);
	sigaction(SIGCHLD, &sa_chld, NULL);

	platform_funcs_t pf_funcs = {
		.rvgpu_egl_pf_init = NULL,
		.rvgpu_egl_pf_free = NULL,
		.rvgpu_create_pf_native_display = NULL,
		.rvgpu_destroy_pf_native_display = NULL,
		.rvgpu_pf_swap = NULL,
		.send_buffer_handle = (SendBufferHandleFunc)send_handle,
		.recv_buffer_handle = (RecvBufferHandleFunc)recv_handle,
		.get_hardware_buffer_cap = (GetHardwareBufferCapFunc)
			get_cap_dma_buf_import_extensions,
		.create_hardware_buffer =
			(CreateHardwareBufferFunc)create_dma_buffer_fd,
		.create_shared_buffer = (CreateSharedBufferFunc)create_shm_fd,
		.destroy_hardware_buffer =
			(DestroyHardwareBufferFunc)destroy_dma_buffer_handle,
		.destroy_shared_buffer =
			(DestroySharedBufferFunc)destroy_shm_fd,
		.create_egl_image =
			(CreateEGLImageFunc)create_egl_image_from_dma
	};

	void *egl_pf_init_params = NULL;
	rvgpu_wl_params *wl_params = NULL;
	rvgpu_gbm_params *gbm_params = NULL;
	if (carddev == NULL) {
		rvgpu_wl_params *wl_params =
			(rvgpu_wl_params *)calloc(1, sizeof(rvgpu_wl_params));
		wl_params->fullscreen = fullscreen;
		wl_params->output_id = output_id;
		wl_params->translucent = translucent;
		wl_params->ivi_surface_id = ivi_surface_id;
		pf_funcs.rvgpu_egl_pf_init = (RVGPUEGLPFInitFunc)rvgpu_wl_init;
		pf_funcs.rvgpu_egl_pf_free = (RVGPUEGLPFFreeFunc)rvgpu_wl_free;
		pf_funcs.rvgpu_create_pf_native_display =
			(RVGPUCreatePFNativeDisplayFunc)create_wl_native_display;
		pf_funcs.rvgpu_destroy_pf_native_display =
			(RVGPUDestroyPFNativeDisplayFunc)
				destroy_wl_native_display;
		pf_funcs.rvgpu_pf_swap = (RVGPUPFSwapFunc)rvgpu_wl_swap;
		egl_pf_init_params = (void *)wl_params;
	} else {
		rvgpu_gbm_params *gbm_params =
			(rvgpu_gbm_params *)calloc(1, sizeof(rvgpu_gbm_params));
		gbm_params->device = carddev;
		gbm_params->seat = seat;
		pf_funcs.rvgpu_egl_pf_init = (RVGPUEGLPFInitFunc)rvgpu_gbm_init;
		pf_funcs.rvgpu_egl_pf_free = (RVGPUEGLPFFreeFunc)rvgpu_gbm_free;
		pf_funcs.rvgpu_create_pf_native_display =
			(RVGPUCreatePFNativeDisplayFunc)
				create_gbm_native_display;
		pf_funcs.rvgpu_destroy_pf_native_display =
			(RVGPUDestroyPFNativeDisplayFunc)
				destroy_gbm_native_display;
		pf_funcs.rvgpu_pf_swap = (RVGPUPFSwapFunc)rvgpu_gbm_swap;
		egl_pf_init_params = (void *)gbm_params;
	}

	char pid_str[16];
	if (domain_name == NULL) {
		snprintf(pid_str, sizeof(pid_str), "%d", getpid());
		domain_name = pid_str;
	}

	struct rvgpu_compositor_params params = {
		.pf_funcs = pf_funcs,
		.egl_pf_init_params = egl_pf_init_params,
		.egl_params = egl_params,
		.fps_params = fps_params,
		.layout_params = layout_params,
		.translucent = translucent,
		.fullscreen = fullscreen,
		.vsync = vsync,
		.port_no = port_no,
		.width = width,
		.height = height,
		.ivi_surface_id = ivi_surface_id,
		.max_vsync_rate = max_vsync_rate,
		.carddev = carddev,
		.seat = seat,
		.domain_name = domain_name,
		.capset_file = capset_file
	};
	pid_t pid = fork();
	switch (pid) {
	case 0: {
		rvgpu_handle_connection(&params);
		_exit(0);
	}
	default:
		printf("forked for rvgpu_handle_connection\n");
	}
	rvgpu_compositor_run(&params);
	free(wl_params);
	free(gbm_params);
	return EXIT_SUCCESS;
}
