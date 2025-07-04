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

#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>

int create_server_socket(const char *domain)
{
	struct sockaddr_un un;
	int sock_fd, ret;

	sock_fd = socket(PF_UNIX, SOCK_STREAM, 0);
	if (sock_fd < 0) {
		fprintf(stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
		return -1;
	}

	memset(&un, 0, sizeof(un));
	un.sun_family = AF_UNIX;
	strcpy(un.sun_path, domain);

	//Unix domain socket abstract namespace
	un.sun_path[0] = '\0';
	strncpy(&un.sun_path[1], domain, sizeof(un.sun_path) - 2);

	int size = offsetof(struct sockaddr_un, sun_path) + strlen(domain) + 1;
	ret = bind(sock_fd, (struct sockaddr *)&un, size);
	if (ret < 0) {
		fprintf(stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
		close(sock_fd);
		return -1;
	}

	ret = listen(sock_fd, 1);
	if (ret < 0) {
		fprintf(stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
		close(sock_fd);
		return -1;
	}

	return sock_fd;
}

int connect_to_client(int socket)
{
	struct sockaddr_un conn_addr = { 0 };
	socklen_t conn_addr_len = sizeof(conn_addr);
	int fd;

	fd = accept(socket, (struct sockaddr *)&conn_addr, &conn_addr_len);
	if (fd < 0) {
		fprintf(stderr, "ERR: %s(%d)\n", __FILE__, __LINE__);
		return -1;
	}

	return fd;
}

int connect_to_server(const char *domain)
{
	int sock;
	struct sockaddr_un addr;

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sock < 0) {
		perror("socket");
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	addr.sun_path[0] = '\0';
	strncpy(&addr.sun_path[1], domain, sizeof(addr.sun_path) - 2);

	int addr_len =
		offsetof(struct sockaddr_un, sun_path) + strlen(domain) + 1;

	if (connect(sock, (struct sockaddr *)&addr, addr_len) < 0) {
		perror("connect");
		close(sock);
		return -1;
	}

	return sock;
}
