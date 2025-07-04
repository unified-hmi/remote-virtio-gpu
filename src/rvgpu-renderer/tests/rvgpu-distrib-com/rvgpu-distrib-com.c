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
#include <time.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <poll.h>

#include <jansson.h>

#include <rvgpu-utils/rvgpu-utils.h>
#include <rvgpu-renderer/compositor/rvgpu-connection.h>
#include <rvgpu-renderer/tests/rvgpu-wm.h>

char *read_json_from_stdin()
{
	size_t buffer_size = 1024;
	size_t json_str_size = 0;
	char *json_str = malloc(buffer_size);
	if (json_str == NULL) {
		fprintf(stderr, "Failed to allocate memory\n");
		return NULL;
	}

	int c;
	while ((c = getchar()) != EOF) {
		if (json_str_size + 1 >= buffer_size) {
			buffer_size *= 2;
			json_str = realloc(json_str, buffer_size);
			if (json_str == NULL) {
				fprintf(stderr,
					"Failed to reallocate memory\n");
				return NULL;
			}
		}
		json_str[json_str_size++] = (char)c;
	}
	json_str[json_str_size] = '\0';

	return json_str;
}

int get_int_from_surfaces(json_t *json_obj, int id, char *key)
{
	size_t index;
	int val = 0;
	json_t *value;
	json_t *json_surfaces = json_object_get(json_obj, "surfaces");
	json_array_foreach(json_surfaces, index, value)
	{
		json_t *json_id = json_object_get(value, "id");
		int layout_id = json_integer_value(json_id);
		if (layout_id == id) {
			json_t *json_value = json_object_get(value, key);
			val = json_integer_value(json_value);
			break;
		}
	}
	return val;
}

double get_double_from_surfaces(json_t *json_obj, int id, char *key)
{
	size_t index;
	double val = 0.0;
	json_t *value;
	json_t *json_surfaces = json_object_get(json_obj, "surfaces");
	json_array_foreach(json_surfaces, index, value)
	{
		json_t *json_id = json_object_get(value, "id");
		int layout_id = json_integer_value(json_id);
		if (layout_id == id) {
			json_t *json_value = json_object_get(value, key);
			if (json_is_real(json_value)) {
				val = json_real_value(json_value);
			} else if (json_is_integer(json_value)) {
				val = (double)json_integer_value(json_value);
			} else {
				fprintf(stderr,
					"Error: '%s' is not a real number or integer.\n",
					key);
			}
			break;
		}
	}
	return val;
}

void update_and_send_layout(int sock, json_t *json_obj, int id, double x,
			    double y, double w, double h)
{
	int ret;
	size_t index;
	json_t *value;
	json_t *json_surfaces = json_object_get(json_obj, "surfaces");
	json_array_foreach(json_surfaces, index, value)
	{
		json_t *json_id = json_object_get(value, "id");
		int layout_id = json_integer_value(json_id);
		if (layout_id == id) {
			if (x != -1) {
				json_object_set(value, "dst_x", json_real(x));
			}
			if (y != -1) {
				json_object_set(value, "dst_y", json_real(y));
			}
			if (w != -1) {
				json_object_set(value, "dst_w", json_real(w));
			}
			if (h != -1) {
				json_object_set(value, "dst_h", json_real(h));
			}
			break;
		}
	}
	send_str_with_size(sock, json_dumps(json_obj, 0));
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
				read(sock, buffer, sizeof(buffer) - 1);
			buffer[bytes_read] = '\0';
			//printf("rvgpu_distrib_com return: %s\n", buffer);
			break;
		}
	}
}

int main(int argc, char **argv)
{
	int opt = 0;
	int id = -1;
	double x = -1;
	double y = -1;
	double w = -1;
	double h = -1;
	uint32_t duration_ms = 0;
	int target_fps;
	double frame_time_ms = 0;
	while ((opt = getopt(argc, argv, "i:x:y:w:h:d:")) != -1) {
		switch (opt) {
		case 'i':
			id = atoi(optarg);
			break;
		case 'x':
			x = atoi(optarg);
			break;
		case 'y':
			y = atoi(optarg);
			break;
		case 'w':
			w = atoi(optarg);
			break;
		case 'h':
			h = atoi(optarg);
			break;
		case 'd':
			duration_ms = atoi(optarg);
			break;
		case 'f':
			target_fps = atoi(optarg);
			if (target_fps > 0) {
				frame_time_ms = 1000.0 / (double)target_fps;
			}
			break;
		}
	}
	int sock = connect_to_server(UHMI_RVGPU_WM_SOCK);
	if (sock < 0) {
		fprintf(stderr, "Failed to connect to server\n");
		return 1;
	}

	struct pollfd fds[1];
	fds[0].fd = 0;
	fds[0].events = POLLIN;

	int ret = poll(fds, 1, -1);
	if (ret < 0) {
		perror("poll");
		close(sock);
		return 1;
	}

	if (fds[0].revents & POLLIN) {
		char *json_str = read_json_from_stdin();
		if (json_str == NULL) {
			fprintf(stderr, "Failed to read JSON from stdin\n");
			close(sock);
			return 1;
		}
		json_error_t error;
		json_t *json_obj = json_loads(json_str, 0, &error);
		if (!json_obj) {
			fprintf(stderr, "error: %s\n", error.text);
			free(json_str);
			return 0;
		}

		int frame_cnt = 0;
		double base_x = get_double_from_surfaces(json_obj, id, "dst_x");
		double base_y = get_double_from_surfaces(json_obj, id, "dst_y");
		double base_w = get_double_from_surfaces(json_obj, id, "dst_w");
		double base_h = get_double_from_surfaces(json_obj, id, "dst_h");
		printf("base params %.3f, %.3f, %.3f, %.3f\n", base_x, base_y,
		       base_w, base_h);
		if (id != -1 &&
		    ((x != -1 && y != -1) || (w != -1 || h != -1))) {
			struct timespec start, end;
			double elapsed_ms;
			double next_x, next_y, next_w, next_h;
			if (x == -1) {
				x = base_x;
			}
			if (y == -1) {
				y = base_y;
			}
			if (w == -1) {
				w = base_w;
			}
			if (h == -1) {
				h = base_h;
			}
			printf("target params %.3f, %.3f, %.3f, %.3f\n", x, y,
			       w, h);
			clock_gettime(CLOCK_MONOTONIC, &start);
			while (1) {
				clock_gettime(CLOCK_MONOTONIC, &end);
				elapsed_ms =
					(end.tv_sec - start.tv_sec) * 1000.0 +
					(end.tv_nsec - start.tv_nsec) / 1e6;
				if (elapsed_ms > duration_ms) {
					break;
				}

				next_x = base_x + ((double)(x - base_x) *
						   (elapsed_ms / duration_ms));
				next_y = base_y + ((double)(y - base_y) *
						   (elapsed_ms / duration_ms));
				next_w = base_w + ((double)(w - base_w) *
						   (elapsed_ms / duration_ms));
				next_h = base_h + ((double)(h - base_h) *
						   (elapsed_ms / duration_ms));
				printf("next params %.3f, %.3f, %.3f, %.3f\n",
				       next_x, next_y, next_w, next_h);
				update_and_send_layout(sock, json_obj, id,
						       next_x, next_y, next_w,
						       next_h);
				frame_cnt++;
				struct timespec sleep_time;
				sleep_time.tv_sec = 0;
				sleep_time.tv_nsec =
					(long)(frame_time_ms * 1000000);
				nanosleep(&sleep_time, NULL);
			}
			update_and_send_layout(sock, json_obj, id, x, y, w, h);
			frame_cnt++;
		} else {
			update_and_send_layout(sock, json_obj, id, base_x,
					       base_y, base_w, base_h);
			frame_cnt++;
		}
		printf("duration_ms: %d, frame_cnt: %d, FPS: %d\n", duration_ms,
		       frame_cnt, frame_cnt / (duration_ms / 1000));
	}
	close(sock);
	return 0;
}
