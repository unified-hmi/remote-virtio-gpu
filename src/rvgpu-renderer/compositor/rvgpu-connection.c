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
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/un.h>
#include <rvgpu-utils/rvgpu-utils.h>

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

static void close_fd_array(int *fds, size_t count)
{
	if (!fds)
		return;
	for (size_t i = 0; i < count; ++i) {
		if (fds[i] >= 0)
			close(fds[i]);
	}
}

int *multiple_connects_to_server(const char *domain, int num_fds)
{
	if (!domain || num_fds <= 0) {
		errno = EINVAL;
		return NULL;
	}

	int *fds = (int *)calloc((size_t)num_fds, sizeof(int));
	if (!fds)
		return NULL;
	for (int i = 0; i < num_fds; ++i)
		fds[i] = -1;

	fds[0] = connect_to_server(domain);
	if (fds[0] < 0) {
		free(fds);
		return NULL;
	}

	if (send_int(fds[0], num_fds) < 0) {
		close(fds[0]);
		free(fds);
		return NULL;
	}

	for (int i = 1; i < num_fds; ++i) {
		fds[i] = connect_to_server(domain);
		if (fds[i] < 0) {
			close_fd_array(fds, (size_t)i);
			free(fds);
			return NULL;
		}
	}
	return fds;
}

int *connect_to_multiple_clients(int server_socket_fd, int *out_count)
{
	if (server_socket_fd < 0 || !out_count) {
		errno = EINVAL;
		return NULL;
	}

	int fd0 = connect_to_client(server_socket_fd);
	if (fd0 < 0)
		return NULL;

	int num_fds = 0;
	if (recv_int(fd0, &num_fds) < 0 || num_fds <= 0) {
		close(fd0);
		errno = (num_fds <= 0) ? EINVAL : errno;
		return NULL;
	}

	int *fds = (int *)calloc((size_t)num_fds, sizeof(int));
	if (!fds) {
		close(fd0);
		return NULL;
	}
	for (int i = 0; i < num_fds; ++i)
		fds[i] = -1;
	fds[0] = fd0;

	for (int i = 1; i < num_fds; ++i) {
		int fd = connect_to_client(server_socket_fd);
		if (fd < 0) {
			close_fd_array(fds, (size_t)i);
			free(fds);
			return NULL;
		}
		fds[i] = fd;
	}

	*out_count = num_fds;
	return fds;
}
