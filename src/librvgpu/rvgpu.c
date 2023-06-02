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

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/un.h>
#include <unistd.h>

#include <librvgpu/rvgpu-plugin.h>
#include <librvgpu/rvgpu.h>

const uint32_t rvgpu_backend_version = 1;

static void free_communic_pipes(struct rvgpu_scanout *scanout)
{
	struct sc_priv *sc_priv = (struct sc_priv *)scanout->priv;

	for (unsigned int i = 0; i < SOCKET_NUM; i++) {
		close(sc_priv->pipes[i].rcv_pipe[PIPE_READ]);
		close(sc_priv->pipes[i].rcv_pipe[PIPE_WRITE]);
		close(sc_priv->pipes[i].snd_pipe[PIPE_READ]);
		close(sc_priv->pipes[i].snd_pipe[PIPE_WRITE]);
	}
}

static int init_communic_pipes(struct rvgpu_scanout *scanout)
{
	struct sc_priv *sc_priv = (struct sc_priv *)scanout->priv;

	for (int i = 0; i < SOCKET_NUM; i++) {
		if (pipe2(sc_priv->pipes[i].rcv_pipe, 0) == -1) {
			perror("pipe creation error");
			return -1;
		}
		fcntl(sc_priv->pipes[i].rcv_pipe[0], F_SETPIPE_SZ, PIPE_SIZE);
		if (pipe2(sc_priv->pipes[i].snd_pipe, 0) == -1) {
			perror("pipe creation error");
			return -1;
		}
		fcntl(sc_priv->pipes[i].snd_pipe[0], F_SETPIPE_SZ, PIPE_SIZE);
	}
	return 0;
}

static int init_tcp_scanout(struct rvgpu_ctx *ctx, struct rvgpu_scanout *scanout,
		     struct rvgpu_scanout_arguments *args)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	struct sc_priv *sc_priv = (struct sc_priv *)scanout->priv;

	struct vgpu_host *cmd = &ctx_priv->cmd[ctx_priv->cmd_count];
	struct vgpu_host *res = &ctx_priv->res[ctx_priv->res_count];

	cmd->tcp = &args->tcp;
	cmd->host_p[PIPE_WRITE] = sc_priv->pipes[COMMAND].rcv_pipe[PIPE_WRITE];
	cmd->host_p[PIPE_READ] = sc_priv->pipes[COMMAND].snd_pipe[PIPE_READ];
	cmd->vpgu_p[PIPE_WRITE] = sc_priv->pipes[COMMAND].snd_pipe[PIPE_WRITE];
	cmd->vpgu_p[PIPE_READ] = sc_priv->pipes[COMMAND].rcv_pipe[PIPE_READ];
	ctx_priv->cmd_count++;

	res->tcp = &args->tcp;
	res->host_p[PIPE_WRITE] = sc_priv->pipes[RESOURCE].rcv_pipe[PIPE_WRITE];
	res->host_p[PIPE_READ] = sc_priv->pipes[RESOURCE].snd_pipe[PIPE_READ];
	res->vpgu_p[PIPE_WRITE] = sc_priv->pipes[RESOURCE].snd_pipe[PIPE_WRITE];
	res->vpgu_p[PIPE_READ] = sc_priv->pipes[RESOURCE].rcv_pipe[PIPE_READ];
	ctx_priv->res_count++;

	return 0;
}

int rvgpu_ctx_send(struct rvgpu_ctx *ctx, const void *buf, size_t len)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;

	for (unsigned int i = 0; i < ctx_priv->cmd_count; i++) {
		struct sc_priv *sc_priv =
			(struct sc_priv *)ctx_priv->sc[i]->priv;
		size_t offset = 0;

		if (!sc_priv->activated)
			return -EBUSY;

		while (offset < len) {
			ssize_t written = write(
				sc_priv->pipes[COMMAND].snd_pipe[PIPE_WRITE],
				(const char *)buf + offset, len - offset);
			if (written >= 0) {
				offset += (size_t)written;
			} else if (errno != EAGAIN) {
				warn("Error while writing to socket");
				return errno;
			}
		}
	}

	return 0;
}

int rvgpu_recv_all(struct rvgpu_scanout *scanout, enum pipe_type p, void *buf,
		   size_t len)
{
	struct sc_priv *sc_priv = (struct sc_priv *)scanout->priv;
	size_t offset = 0;

	if (!sc_priv->activated)
		return -EBUSY;

	while (offset < len) {
		ssize_t r = read(sc_priv->pipes[p].rcv_pipe[PIPE_READ],
				 (char *)buf + offset, len - offset);
		if (r > 0) {
			offset += (size_t)r;
		} else if (r == 0) {
			warnx("Connection was closed");
			return -1;
		} else if (errno != EAGAIN) {
			warn("Error while reading from socket");
			return -1;
		}
	}

	return offset;
}

int rvgpu_recv(struct rvgpu_scanout *scanout, enum pipe_type p, void *buf,
	       size_t len)
{
	struct sc_priv *sc_priv = (struct sc_priv *)scanout->priv;

	if (!sc_priv->activated)
		return -EBUSY;

	return read(sc_priv->pipes[p].rcv_pipe[PIPE_READ], buf, len);
}

int rvgpu_send(struct rvgpu_scanout *scanout, enum pipe_type p, const void *buf,
	       size_t len)
{
	struct sc_priv *sc_priv = (struct sc_priv *)scanout->priv;

	if (!sc_priv->activated)
		return -EBUSY;

	int rc = write(sc_priv->pipes[p].snd_pipe[PIPE_WRITE], buf, len);
	/*
	 * During reset gpu procedure pipe may be full and since it's in
	 * NONBLOCKING mode, write will return EAGAIN. Backend device
	 * should ignore this.
	 */
	return ((rc != len) && (errno == EAGAIN)) ? len : rc;
}

int rvgpu_init(struct rvgpu_ctx *ctx, struct rvgpu_scanout *scanout,
	       struct rvgpu_scanout_arguments args)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	struct sc_priv *sc_priv;
	int rc;

	sc_priv = (struct sc_priv *)calloc(1, sizeof(*sc_priv));
	assert(sc_priv);

	sc_priv->args = calloc(1, sizeof(args));
	memcpy(sc_priv->args, &args, sizeof(args));
	scanout->priv = sc_priv;
	ctx_priv->sc[ctx_priv->inited_scanout_num] = scanout;

	pthread_mutex_lock(&ctx_priv->lock);

	rc = init_communic_pipes(scanout);
	if (rc) {
		perror("Failed to init communication pipes");
		return -1;
	}

	rc = init_tcp_scanout(ctx, scanout, sc_priv->args);
	if (rc) {
		perror("Failed to init TCP scanout");
		goto error;
	}

	sc_priv->activated = true;
	ctx_priv->inited_scanout_num++;

	pthread_mutex_unlock(&ctx_priv->lock);

	return 0;

error:
	free_communic_pipes(scanout);
	pthread_mutex_unlock(&ctx_priv->lock);

	return -1;
}

void rvgpu_destroy(struct rvgpu_ctx *ctx, struct rvgpu_scanout *scanout)
{
	if (scanout) {
		free_communic_pipes(scanout);
		free(scanout->priv);
		scanout->priv = NULL;
	}
}

void rvgpu_frontend_reset_state(struct rvgpu_ctx *ctx, enum reset_state state)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;

	ctx_priv->reset.state = state;
}

int rvgpu_ctx_poll(struct rvgpu_ctx *ctx, enum pipe_type p, int timeo,
		   short int *events, short int *revents)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	struct pollfd pfd[MAX_HOSTS];
	int ret = 0;

	if (p == COMMAND) {
		for (unsigned int i = 0; i < ctx_priv->cmd_count; i++) {
			struct vgpu_host *cmd = &ctx_priv->cmd[i];

			if (events[i] & POLLIN) {
				pfd[i].fd = cmd->vpgu_p[PIPE_READ];
				pfd[i].events = POLLIN;
			} else if (events[i] & POLLOUT) {
				pfd[i].fd = cmd->vpgu_p[PIPE_WRITE];
				pfd[i].events = POLLOUT;
			}
		}
		ret = poll(pfd, ctx_priv->cmd_count, timeo);
		for (unsigned int i = 0; i < ctx_priv->cmd_count; i++)
			revents[i] = pfd[i].revents;

		return ret;
	}

	if (p == RESOURCE) {
		for (unsigned int i = 0; i < ctx_priv->res_count; i++) {
			struct vgpu_host *res = &ctx_priv->res[i];

			if (events[i] & POLLIN) {
				pfd[i].fd = res->vpgu_p[PIPE_READ];
				pfd[i].events = POLLIN;
			} else if (events[i] & POLLOUT) {
				pfd[i].fd = res->vpgu_p[PIPE_WRITE];
				pfd[i].events = POLLOUT;
			}
		}
		ret = poll(pfd, ctx_priv->res_count, timeo);
		for (unsigned int i = 0; i < ctx_priv->res_count; i++)
			revents[i] = pfd[i].revents;

		return ret;
	}

	return ret;
}

int rvgpu_ctx_init(struct rvgpu_ctx *ctx, struct rvgpu_ctx_arguments args,
		   void (*gpu_reset_cb)(struct rvgpu_ctx *ctx,
					enum reset_state state))
{
	struct ctx_priv *ctx_priv =
		(struct ctx_priv *)calloc(1, sizeof(*ctx_priv));
	assert(ctx_priv);

	pthread_mutexattr_t attr;

	pthread_mutexattr_init(&attr);
	if (pthread_mutex_init(&ctx_priv->lock, &attr)) {
		perror("pthread_mutex_init failed");
		return -1;
	}
	if (pthread_cond_init(&ctx_priv->reset.cond, NULL)) {
		perror("pthread_cond_init");
		return -1;
	}
	if (pthread_mutex_init(&ctx_priv->reset.lock, NULL)) {
		perror("pthread_mutex_init");
		return -1;
	}

	ctx->priv = ctx_priv;
	ctx->scanout_num = args.scanout_num;
	ctx_priv->scanout_num = args.scanout_num;
	ctx_priv->gpu_reset_cb = gpu_reset_cb;
	memcpy(&ctx_priv->args, &args, sizeof(args));

	if (pthread_create(&ctx_priv->tid, NULL, thread_conn_tcp, ctx)) {
		perror("TCP thread creation error");
		return -1;
	}

	return 0;
}

void rvgpu_ctx_destroy(struct rvgpu_ctx *ctx)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;

	ctx_priv->interrupted = true;
}
