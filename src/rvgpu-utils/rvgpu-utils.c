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
#include <err.h>

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

ssize_t write_all(int fd, const void *buf, size_t count)
{
	const char *ptr = (const char *)buf;
	size_t remaining = count;
	ssize_t written;

	while (remaining > 0) {
		written = write(fd, ptr, remaining);
		if (written < 0) {
			if (errno == EINTR || errno == EAGAIN ||
			    errno == EWOULDBLOCK)
				continue;
			warn("Error while writing to socket");
			return -1;
		} else if (written == 0) {
			break;
		}
		ptr += written;
		remaining -= written;
	}

	return count - remaining;
}

ssize_t read_all(int fd, void *buf, size_t count)
{
	char *ptr = (char *)buf;
	size_t remaining = count;
	ssize_t received;

	while (remaining > 0) {
		received = read(fd, ptr, remaining);
		if (received < 0) {
			if (errno == EINTR || errno == EAGAIN ||
			    errno == EWOULDBLOCK)
				continue;
			warn("Error while reading from socket");
			return -1;
		} else if (received == 0) {
			/* EOF - connection closed by peer */
			break;
		}
		ptr += received;
		remaining -= received;
	}

	return count - remaining;
}

void send_str_with_size(int client_fd, const char *str)
{
	uint32_t buffer_size_hb = strlen(str) + 1;
	uint32_t buffer_size = htonl(buffer_size_hb);

	ssize_t sent_bytes =
		write_all(client_fd, &buffer_size, sizeof(buffer_size));
	if (sent_bytes != sizeof(buffer_size)) {
		perror("send_str_with_size size write");
		return;
	}

	sent_bytes = write_all(client_fd, str, buffer_size_hb);
	if (sent_bytes != (ssize_t)buffer_size_hb) {
		perror("send_str_with_size data write");
	}
}

// Need to free buffer after use
char *recv_str_all(int client_fd)
{
	uint32_t buffer_size_nb;

	ssize_t received_bytes =
		read_all(client_fd, &buffer_size_nb, sizeof(buffer_size_nb));
	if (received_bytes != sizeof(buffer_size_nb)) {
		return NULL;
	}

	uint32_t buffer_size = ntohl(buffer_size_nb);
	char *buffer = (char *)calloc(buffer_size + 1, sizeof(char));
	if (buffer == NULL) {
		return NULL;
	}

	received_bytes = read_all(client_fd, buffer, buffer_size);
	if (received_bytes != (ssize_t)buffer_size) {
		free(buffer);
		return NULL;
	}

	buffer[buffer_size] = '\0';

	return buffer;
}
