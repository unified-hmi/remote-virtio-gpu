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
#include <stdint.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <arpa/inet.h>
#include <poll.h>

int send_int(int fd, int value)
{
	ssize_t bytes_sent = write(fd, &value, sizeof(value));
	if (bytes_sent != sizeof(value)) {
		perror("send_int write");
		return -1;
	}
	return 0;
}

int recv_int(int fd, int *value)
{
	ssize_t bytes_received = read(fd, value, sizeof(*value));
	if (bytes_received != sizeof(*value)) {
		perror("read");
		return -1;
	}
	return 0;
}

void send_str_with_size(int client_fd, const char *str)
{
	uint32_t buffer_size_hb = strlen(str) + 1;
	uint32_t buffer_size = htonl(buffer_size_hb);

	ssize_t sent_bytes =
		write(client_fd, &buffer_size, sizeof(buffer_size));
	if (sent_bytes == -1) {
		perror("send_str_with_size size write");
		return;
	}

	sent_bytes = write(client_fd, str, buffer_size_hb);
	if (sent_bytes == -1) {
		perror("send_str_with_size data write");
	}
}

// Need to free buffer after use
char *recv_str_all(int client_fd)
{
	ssize_t received_bytes;
	uint32_t buffer_size_nb;

	received_bytes =
		read(client_fd, &buffer_size_nb, sizeof(buffer_size_nb));
	if (received_bytes <= 0) {
		close(client_fd);
		return NULL;
	}

	uint32_t buffer_size = ntohl(buffer_size_nb);
	char *buffer = (char *)calloc(buffer_size + 1, sizeof(char));
	if (buffer == NULL) {
		close(client_fd);
		return NULL;
	}

	ssize_t total_received_bytes = 0;
	struct pollfd fds;
	fds.fd = client_fd;
	fds.events = POLLIN;

	while (total_received_bytes < buffer_size) {
		int poll_result = poll(&fds, 1, -1);
		if (poll_result == -1) {
			perror("poll");
			free(buffer);
			return NULL;
		}

		if (fds.revents & POLLIN) {
			received_bytes =
				read(client_fd, buffer + total_received_bytes,
				     buffer_size - total_received_bytes);
			if (received_bytes < 0) {
				perror("read");
				free(buffer);
				return NULL;
			} else if (received_bytes == 0) {
				// Connection closed by the peer
				break;
			}

			total_received_bytes += received_bytes;
		}
	}

	buffer[total_received_bytes] = '\0';

	return buffer;
}
