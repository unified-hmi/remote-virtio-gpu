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
#include <stddef.h>
#include <stdint.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <poll.h>
#include <pthread.h>

#include <rvgpu-utils/rvgpu-utils.h>
#include <rvgpu-renderer/compositor/rvgpu-connection.h>
#include <rvgpu-renderer/tests/rvgpu-wm.h>

pthread_mutex_t layout_mutex;

struct thread_params {
	int rvgpu_wm_fd;
	int rvgpu_server_fd;
};

void *layout_loop(void *arg)
{
	struct thread_params *tp = (struct thread_params *)arg;
	int rvgpu_wm_fd = tp->rvgpu_wm_fd;
	int sock = tp->rvgpu_server_fd;

	while (1) {
		struct pollfd fds[1];
		fds[0].fd = rvgpu_wm_fd;
		fds[0].events = POLLIN;

		int ret = poll(fds, 1, -1);
		if (ret < 0) {
			perror("poll");
			close(rvgpu_wm_fd);
			close(sock);
			break;
		}

		if (fds[0].revents & POLLIN) {
			ssize_t received_bytes;

			uint32_t buffer_size_nb;
			received_bytes = read(rvgpu_wm_fd, &buffer_size_nb,
					      sizeof(buffer_size_nb));
			if (received_bytes <= 0) {
				close(rvgpu_wm_fd);
				break;
			}

			uint32_t buffer_size = ntohl(buffer_size_nb);
			char *data = (char *)calloc(1, buffer_size + 1);

			ssize_t total_received_bytes = 0;
			while (total_received_bytes < buffer_size) {
				received_bytes = read(
					rvgpu_wm_fd,
					data + total_received_bytes,
					buffer_size - total_received_bytes);
				if (received_bytes <= 0) {
					free(data);
					break;
				}
				total_received_bytes += received_bytes;
			}

			pthread_mutex_lock(&layout_mutex);
			send_str_with_size(sock, data);
			struct pollfd pfd = { .fd = sock, .events = POLLIN };
			while (1) {
				ret = poll(&pfd, 1, -1);
				if (ret == -1) {
					perror("poll");
					break;
				}
				if (pfd.revents & POLLIN) {
					char buffer[256];
					ssize_t bytes_read =
						read(sock, buffer,
						     sizeof(buffer) - 1);
					buffer[bytes_read] = '\0';
					//printf("rvgpu_wm return: %s\n", buffer);
					break;
				}
			}
			pthread_mutex_unlock(&layout_mutex);

			const char *completion_msg = "Layout complete";
			write(rvgpu_wm_fd, completion_msg,
			      strlen(completion_msg));

			if (data) {
				free(data);
			}
		}
	}
	return NULL;
}

int main(int argc, char *argv[])
{
	char rvgpu_layout_sock_path[256];
	char *domain_name = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "d:")) != -1) {
		switch (opt) {
		case 'd':
			domain_name = optarg;
			break;
		default:
			fprintf(stderr, "Usage: %s -d <socket_name>\n",
				argv[0]);
			return 1;
		}
	}
	if (domain_name == NULL) {
		domain_name = "default";
	}

	snprintf(rvgpu_layout_sock_path, sizeof(rvgpu_layout_sock_path),
		 "%s.%s", UHMI_RVGPU_LAYOUT_SOCK, domain_name);
	int sock = connect_to_server(rvgpu_layout_sock_path);
	if (sock < 0) {
		fprintf(stderr, "Failed to connect to server\n");
		return 1;
	}

	int server_rvgpu_wm_sock = create_server_socket(UHMI_RVGPU_WM_SOCK);
	if (server_rvgpu_wm_sock < 0) {
		fprintf(stderr, "Failed to create server socket\n");
	}

	pthread_mutex_init(&layout_mutex, NULL);
	while (1) {
		int rvgpu_wm_fd = connect_to_client(server_rvgpu_wm_sock);
		struct thread_params *tp = (struct thread_params *)calloc(
			1, sizeof(struct thread_params));
		tp->rvgpu_wm_fd = rvgpu_wm_fd;
		tp->rvgpu_server_fd = sock;
		pthread_t layout_loop_thread;
		pthread_create(&layout_loop_thread, NULL, layout_loop, tp);
	}
	return 0;
}
