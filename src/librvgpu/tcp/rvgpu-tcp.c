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
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <librvgpu/rvgpu-plugin.h>
#include <librvgpu/rvgpu.h>

struct poll_entries {
	struct pollfd *ses_timer;
	struct pollfd *recon_timer;
	struct pollfd *cmd_host;
	struct pollfd *cmd_pipe_in;
	struct pollfd *res_host;
	struct pollfd *res_pipe_in;
};

struct conninfo {
	struct addrinfo *servinfo, *p;
};

static int reconnect_single(struct vgpu_host *host);

static int reconnect_next(struct conninfo *ci)
{
	int fd;

	if (ci->p != NULL)
		ci->p = ci->p->ai_next;

	if (ci->p == NULL)
		ci->p = ci->servinfo;

	fd = socket(ci->p->ai_family, ci->p->ai_socktype | SOCK_NONBLOCK,
		    ci->p->ai_protocol);
	if (fd == -1)
		return -1;

	int keepalive = 1;

	if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive,
		       sizeof(int)) != 0)
		err(1, "setsockopt SO_KEEPALIVE");

	int keepidle = 1;

	if (setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, &keepidle,
		       sizeof(int)) != 0)
		err(1, "setsockopt TCP_KEEPIDLE");

	int keepintvl = 1;

	if (setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, &keepintvl,
		       sizeof(int)) != 0)
		err(1, "setsockopt TCP_KEEPINTVL");

	int keepcnt = 1;

	if (setsockopt(fd, SOL_TCP, TCP_KEEPCNT, &keepcnt,
		       sizeof(int)) != 0)
		err(1, "setsockopt TCP_KEEPCNT");

	int nodelay = 1;

	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay,
		       sizeof(int)) != 0) {
		err(1, "setsockopt TCP_NODELAY");
	}

	if (connect(fd, ci->p->ai_addr, ci->p->ai_addrlen) != -1 ||
	    errno != EINPROGRESS) {
		close(fd);
		fd = -1;
	}

	return fd;
}

static int wait_scanouts_init(struct ctx_priv *ctx)
{
	struct timespec end;
	uint16_t timeo_sec = 10;
	int timeout;

	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += timeo_sec;

	do {
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		timeout = (int)((end.tv_sec - now.tv_sec) * 1000 +
				(end.tv_nsec - now.tv_nsec) / 1000000);
		if (ctx->inited_scanout_num == ctx->scanout_num)
			return 0;
	} while (timeout > 0);

	return -1;
}

static void connect_hosts(struct vgpu_host *conn, uint16_t count, uint16_t timeo_s)
{
	struct pollfd pfds[MAX_HOSTS];
	struct conninfo cinfo[MAX_HOSTS];
	bool wait_more;
	struct timespec end;

	clock_gettime(CLOCK_MONOTONIC, &end);
	end.tv_sec += timeo_s;

	for (unsigned int i = 0; i < count; i++) {
		struct addrinfo hints = {
			.ai_family = AF_INET,
			.ai_socktype = SOCK_STREAM,
			.ai_protocol = IPPROTO_TCP,
		};
		int res;

		conn[i].sock = -1;
		pfds[i].fd = -1;
		pfds[i].events = POLLOUT;
		cinfo[i] = (struct conninfo){ NULL, NULL };

		res = getaddrinfo(conn[i].tcp->ip, conn[i].tcp->port, &hints,
				  &cinfo[i].servinfo);
		if (res != 0) {
			warnx("getaddrinfo %s", gai_strerror(res));
			continue;
		}
		pfds[i].fd = reconnect_next(&cinfo[i]);
	}

	do {
		struct timespec now;
		int res, timeout;

		wait_more = false;
		clock_gettime(CLOCK_MONOTONIC, &now);
		timeout = (int)((end.tv_sec - now.tv_sec) * 1000 +
				(end.tv_nsec - now.tv_nsec) / 1000000);
		if (timeout < 0)
			break;

		res = poll(pfds, count, timeout);
		if (res == -1)
			continue;
		for (unsigned int i = 0; i < count; i++) {
			if (pfds[i].fd == -1)
				continue;
			if (pfds[i].revents & POLLOUT) {
				int soerr = 0;
				socklen_t len = sizeof(soerr);

				if (getsockopt(pfds[i].fd, SOL_SOCKET, SO_ERROR,
					       &soerr, &len) == 0 &&
				    soerr == 0) {
					conn[i].sock = pfds[i].fd;
					pfds[i].fd = -1;
				} else {
					close(pfds[i].fd);
					pfds[i].fd = reconnect_next(&cinfo[i]);
					wait_more = true;
				}
			} else {
				wait_more = true;
			}
		}
	} while (wait_more);

	for (unsigned int i = 0; i < count; i++) {
		if (!cinfo[i].servinfo)
			continue;
		if (pfds[i].fd != -1)
			close(pfds[i].fd);
		freeaddrinfo(cinfo[i].servinfo);
	}
}

static int reconnect_single(struct vgpu_host *host)
{
	struct conninfo cinfo;
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = IPPROTO_TCP,
	};
	int sockfd = -1;

	cinfo = (struct conninfo){ NULL, NULL };

	int res = getaddrinfo(host->tcp->ip, host->tcp->port, &hints,
			      &cinfo.servinfo);
	if (res != 0) {
		warnx("getaddrinfo %s", gai_strerror(res));
		return -1;
	}

	struct pollfd pfd = {
		.fd = -1,
		.events = POLLOUT,
	};

	pfd.fd = reconnect_next(&cinfo);

	poll(&pfd, 1, 10);

	if (pfd.revents & POLLOUT) {
		int soerr = 0;
		socklen_t len = sizeof(soerr);

		if (getsockopt(pfd.fd, SOL_SOCKET, SO_ERROR, &soerr, &len) ==
			    0 &&
		    soerr == 0) {
			sockfd = pfd.fd;
		} else {
			close(pfd.fd);
			pfd.fd = -1;
		}
	}

	if (sockfd == -1) {
		close(pfd.fd);
		pfd.fd = -1;
	}

	if (host->pfd)
		host->pfd->fd = sockfd;

	return sockfd;
}

static void close_conn(struct vgpu_host *vhost)
{
	if (vhost->pfd) {
		if (vhost->pfd->fd > 0) {
			close(vhost->pfd->fd);
			vhost->pfd->fd = -1;
			vhost->pfd->events = 0;
			vhost->pfd->revents = 0;
		}
	}
	vhost->state = HOST_DISCONNECTED;
}

static void reconnect_all(struct vgpu_host *vhost[], unsigned int count)
{
	for (unsigned int i = 0; i < count; i++) {
		if (vhost[i]->state != HOST_RECONNECTED) {
			close_conn(vhost[i]);
			reconnect_single(vhost[i]);
		}
		vhost[i]->state = HOST_CONNECTED;
	}
}

static int init_timer(void)
{
	int timer = timerfd_create(CLOCK_MONOTONIC, 0);

	assert(timer != -1);
	return timer;
}


static void set_timer(int timerfd, unsigned int msec)
{
	struct itimerspec ts = { .it_value = { msec / 1000,
					       (msec % 1000) * 1000000 } };

	if (timerfd_settime(timerfd, 0, &ts, NULL) == -1)
		warn("Failed to set timerfd");
}

void rvgpu_ctx_wait(struct ctx_priv *ctx, enum reset_state state)
{
	pthread_mutex_lock(&ctx->reset.lock);
	while (ctx->reset.state != state)
		pthread_cond_wait(&ctx->reset.cond, &ctx->reset.lock);
	pthread_mutex_unlock(&ctx->reset.lock);
}

void rvgpu_ctx_wakeup(struct ctx_priv *ctx)
{
	pthread_mutex_lock(&ctx->reset.lock);
	pthread_cond_signal(&ctx->reset.cond);
	pthread_mutex_unlock(&ctx->reset.lock);
}

static void disconnect(struct vgpu_host *vhost[], unsigned int cmd_cnt,
		unsigned int res_cnt, unsigned int idx)
{
	if (cmd_cnt) {
		if (idx < cmd_cnt)
			close_conn(vhost[idx]);
		else
			close_conn(vhost[idx - cmd_cnt]);
	}
	if (res_cnt) {
		if (idx < res_cnt)
			close_conn(vhost[idx + res_cnt]);
		else
			close_conn(vhost[idx]);
	}
}

static void process_reset_backend(struct rvgpu_ctx *ctx, enum reset_state state)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;

	if (ctx_priv->gpu_reset_cb)
		ctx_priv->gpu_reset_cb(ctx, state);
}

static void handle_reset(struct rvgpu_ctx *ctx, struct vgpu_host *vhost[],
		  unsigned int host_count)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;

	rvgpu_ctx_wait(ctx_priv, GPU_RESET_INITIATED);

	reconnect_all(vhost, host_count);

	ctx_priv->reset.state = GPU_RESET_NONE;
	if (ctx_priv->gpu_reset_cb)
		ctx_priv->gpu_reset_cb(ctx, GPU_RESET_NONE);
	/*
	 * FIXME: Without this delay, rvgpu-proxy will not
	 * wait for subscriber creation in rvgpu-renderer. This
	 * may lead rvgpu-renderer to miss resources from rvgpu-proxy.
	 */
	usleep(100 * 1000);
	rvgpu_ctx_wakeup(ctx_priv);
}

static bool sessions_hung(struct ctx_priv *ctx, struct vgpu_host *vhost[],
		   unsigned int *active_sessions, unsigned int count)
{
	bool ses_hung = false;

	for (unsigned int i = 0; i < count; i++) {
		if (vhost[i]->pfd) {
			struct pollfd *host_pfd = vhost[i]->pfd;

			if ((host_pfd->events & POLLOUT) &&
			    (host_pfd->fd > 0)) {
				disconnect(vhost, ctx->cmd_count,
					   ctx->res_count, i);
				ses_hung = true;
				*active_sessions -= 1;
			}
		}
	}
	return ses_hung;
}

static bool sessions_reconnect(struct rvgpu_ctx *ctx, struct vgpu_host *vhost[],
			int reconn_fd, unsigned int count)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	bool reconnected = true;

	for (unsigned int i = 0; i < count; i++) {
		if (vhost[i]->state == HOST_DISCONNECTED) {
			if (reconnect_single(vhost[i]) < 0) {
				reconnected = false;
				set_timer(reconn_fd,
					  ctx_priv->args.reconn_intv_ms);
			} else {
				vhost[i]->state = HOST_RECONNECTED;
			}
		}
	}
	if (reconnected) {
		handle_reset(ctx, vhost, count);
		set_timer(reconn_fd, 0);
	}
	return reconnected;
}

static unsigned int get_pointers(struct ctx_priv *ctx, struct pollfd *pfd,
			  struct pollfd **ses_timer,
			  struct pollfd **recon_timer, struct pollfd **cmd_host,
			  struct pollfd **cmd_pipe, struct pollfd **res_host,
			  struct pollfd **res_pipe)
{
	unsigned int pfd_count = 0;
	/*
	 * set pointers as following. Ex: for 2 targets
	 * 0 - session timer
	 * 1 - recconnect timer
	 * 2 - command host 0
	 * 3 - command host 1
	 * 4 - command pipe in 0
	 * 5 - command pipe in 1
	 * 6 - res host 0
	 * 7 - res host 1
	 * 8 - res pipe in 0
	 * 9 - res pipe in 1
	 */
	*ses_timer = &pfd[pfd_count];
	pfd_count++;
	*recon_timer = &pfd[pfd_count];
	pfd_count++;
	*cmd_host = &pfd[pfd_count];
	pfd_count += ctx->cmd_count;
	*cmd_pipe = &pfd[pfd_count];
	pfd_count += ctx->cmd_count;
	*res_host = &pfd[pfd_count];
	pfd_count += ctx->res_count;
	*res_pipe = &pfd[pfd_count];
	pfd_count += ctx->res_count;

	return pfd_count;
}

static unsigned int set_pfd(struct ctx_priv *ctx, struct vgpu_host *vhost[],
		     struct pollfd *pfd, struct poll_entries *p_entry)
{
	unsigned int pfd_count =
		get_pointers(ctx, pfd, &p_entry->ses_timer,
			     &p_entry->recon_timer, &p_entry->cmd_host,
			     &p_entry->cmd_pipe_in, &p_entry->res_host,
			     &p_entry->res_pipe_in);
	/* Timer to detect hung sessions */
	p_entry->ses_timer->fd = init_timer();
	p_entry->ses_timer->events = POLLIN;

	/* Timer to do the reconnection attempts */
	p_entry->recon_timer->fd = init_timer();
	p_entry->recon_timer->events = POLLIN;

	/* set command pfd */
	for (unsigned int i = 0; i < ctx->cmd_count; i++) {
		p_entry->cmd_host[i].fd = ctx->cmd[i].sock;
		p_entry->cmd_host[i].events = POLLIN;
		vhost[i] = &ctx->cmd[i];
		vhost[i]->pfd = &p_entry->cmd_host[i];
	}

	/* set command pipe pfd */
	for (unsigned int i = 0; i < ctx->cmd_count; i++) {
		p_entry->cmd_pipe_in[i].fd = ctx->cmd[i].host_p[PIPE_READ];
		p_entry->cmd_pipe_in[i].events = POLLIN;
	}

	/* set resource pfd */
	for (unsigned int i = 0; i < ctx->res_count; i++) {
		p_entry->res_host[i].fd = ctx->res[i].sock;
		p_entry->res_host[i].events = POLLIN;
		vhost[i + ctx->cmd_count] = &ctx->res[i];
		vhost[i + ctx->cmd_count]->pfd = &p_entry->res_host[i];
	}

	/* set resource pipe pfd */
	for (unsigned int i = 0; i < ctx->res_count; i++) {
		p_entry->res_pipe_in[i].fd = ctx->res[i].host_p[PIPE_READ];
		p_entry->res_pipe_in[i].events = POLLIN;
	}

	return pfd_count;
}

unsigned int handle_host_comm(struct rvgpu_ctx *ctx, struct vgpu_host *vhost[],
			      struct poll_entries *p_entry, int devnull)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	struct pollfd *cmd_host = p_entry->cmd_host;
	struct pollfd *pipe_in = p_entry->cmd_pipe_in;
	unsigned int sent = 0;

	for (unsigned int i = 0; i < ctx_priv->cmd_count; i++) {
		if (cmd_host[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			disconnect(vhost, ctx_priv->cmd_count,
				   ctx_priv->res_count, i);
			process_reset_backend(ctx, GPU_RESET_TRUE);
			set_timer(p_entry->recon_timer->fd,
				  ctx_priv->args.reconn_intv_ms);
		}
		if (cmd_host[i].revents & POLLOUT) {
			int ret = splice(pipe_in[i].fd, NULL, cmd_host[i].fd,
					 NULL, PIPE_SIZE, 0);
			if (ret == -1) {
				warnx("wr: spile error %s fd %d",
				      strerror(errno), cmd_host[i].fd);
				disconnect(vhost, ctx_priv->cmd_count,
					   ctx_priv->res_count, i);
				process_reset_backend(ctx, GPU_RESET_TRUE);
				set_timer(p_entry->recon_timer->fd,
					  ctx_priv->args.reconn_intv_ms);
			}
			cmd_host[i].events &= ~POLLOUT;
			sent++;
		}
		if (cmd_host[i].revents & POLLIN) {
			int ret = splice(cmd_host[i].fd, NULL,
					 ctx_priv->cmd[i].host_p[PIPE_WRITE],
					 NULL, PIPE_SIZE, 0);
			if (ret == -1) {
				warnx("rd: spile error %s", strerror(errno));
				disconnect(vhost, ctx_priv->cmd_count,
					   ctx_priv->res_count, i);
				process_reset_backend(ctx, GPU_RESET_TRUE);
				set_timer(p_entry->recon_timer->fd,
					  ctx_priv->args.reconn_intv_ms);
			}
		}
		if (cmd_host[i].fd < 0) {
			splice(pipe_in[i].fd, NULL, devnull, NULL, PIPE_SIZE,
			       SPLICE_F_NONBLOCK);
		}
	}
	return sent;
}

unsigned int handle_host_res(struct rvgpu_ctx *ctx, struct vgpu_host *vhost[],
			     struct poll_entries *p_entry, int devnull)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	struct pollfd *res_host = p_entry->res_host;
	struct pollfd *pipe_in = p_entry->res_pipe_in;
	unsigned int sent = 0;

	for (unsigned int i = 0; i < ctx_priv->res_count; i++) {
		if (res_host[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			disconnect(vhost, ctx_priv->cmd_count,
				   ctx_priv->res_count, i);
			process_reset_backend(ctx, GPU_RESET_TRUE);
			set_timer(p_entry->recon_timer->fd,
				  ctx_priv->args.reconn_intv_ms);
		}
		if (res_host[i].revents & POLLOUT &&
		    pipe_in[i].revents & POLLIN) {
			int ret = splice(pipe_in[i].fd, NULL, res_host[i].fd,
					 NULL, PIPE_SIZE, 0);
			if (ret == -1) {
				warnx("wr: spile error %s", strerror(errno));
				disconnect(vhost, ctx_priv->cmd_count,
					   ctx_priv->res_count, i);
				process_reset_backend(ctx, GPU_RESET_TRUE);
				set_timer(p_entry->recon_timer->fd,
					  ctx_priv->args.reconn_intv_ms);
			}
			sent++;
		}
		if (res_host[i].revents & POLLIN) {
			int ret = splice(res_host[i].fd, NULL,
					 ctx_priv->res[i].host_p[PIPE_WRITE],
					 NULL, PIPE_SIZE, 0);
			if (ret == -1) {
				disconnect(vhost, ctx_priv->cmd_count,
					   ctx_priv->res_count, i);
				process_reset_backend(ctx, GPU_RESET_TRUE);
				set_timer(p_entry->recon_timer->fd,
					  ctx_priv->args.reconn_intv_ms);
			}
		}
		if (res_host[i].fd < 0) {
			splice(pipe_in[i].fd, NULL, devnull, NULL, PIPE_SIZE,
			       SPLICE_F_NONBLOCK);
		}
	}
	return sent;
}

static void in_out_events(struct rvgpu_ctx *ctx, struct poll_entries *p_entry,
		   int cmd_count, int res_count)
{
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;

	/* Handle Virtio-GPU commands */
	for (int i = 0; i < cmd_count; i++) {
		if (p_entry->cmd_pipe_in[i].revents & POLLIN) {
			p_entry->cmd_pipe_in[i].events &= ~POLLIN;
			p_entry->cmd_host[i].events |= POLLOUT;
		}
	}

	for (int i = 0; i < cmd_count; i++) {
		if (p_entry->cmd_host[i].revents & POLLOUT) {
			splice(p_entry->cmd_pipe_in[i].fd, NULL,
			       p_entry->cmd_host[i].fd, NULL, PIPE_SIZE, 0);
			p_entry->cmd_host[i].events &= ~POLLOUT;
			p_entry->cmd_pipe_in[i].events |= POLLIN;
		}
	}

	for (int i = 0; i < cmd_count; i++) {
		if (p_entry->cmd_host[i].revents & POLLIN) {
			splice(p_entry->cmd_host[i].fd, NULL,
			       ctx_priv->cmd[i].host_p[PIPE_WRITE], NULL,
			       PIPE_SIZE, 0);
		}
	}

	/* Handle resources and fences */
	for (int i = 0; i < res_count; i++) {
		if (p_entry->res_pipe_in[i].revents & POLLIN) {
			p_entry->res_pipe_in[i].events &= ~POLLIN;
			p_entry->res_host[i].events |= POLLOUT;
		}
	}

	for (int i = 0; i < res_count; i++) {
		if (p_entry->res_host[i].revents & POLLOUT) {
			splice(p_entry->res_pipe_in[i].fd, NULL,
			       p_entry->res_host[i].fd, NULL, PIPE_SIZE, 0);
			p_entry->res_host[i].events &= ~POLLOUT;
			p_entry->res_pipe_in[i].events |= POLLIN;
		}
	}

	for (int i = 0; i < res_count; i++) {
		if (p_entry->res_host[i].revents & POLLIN) {
			splice(p_entry->res_host[i].fd, NULL,
			       ctx_priv->res[i].host_p[PIPE_WRITE], NULL,
			       PIPE_SIZE, 0);
		}
	}
}

void *thread_conn_tcp(void *arg)
{
	struct rvgpu_ctx *ctx = (struct rvgpu_ctx *)arg;
	struct ctx_priv *ctx_priv = (struct ctx_priv *)ctx->priv;
	struct rvgpu_ctx_arguments *conn_args = &ctx_priv->args;
	struct vgpu_host *vhost[MAX_HOSTS];
	struct pollfd pfd[MAX_HOSTS * SOCKET_NUM + TIMERS_CNT];
	struct poll_entries p_entry;
	unsigned int pfd_count;
	int devnull;

	devnull = open("/dev/null", O_WRONLY);
	assert(devnull != -1);

	if (wait_scanouts_init(ctx_priv)) {
		warnx("Scanouts hasn't been initialized. Exiting");
		return NULL;
	}

	connect_hosts(ctx_priv->cmd, ctx_priv->cmd_count,
		      conn_args->conn_tmt_s);
	connect_hosts(ctx_priv->res, ctx_priv->res_count,
		      conn_args->conn_tmt_s);

	pfd_count = set_pfd(ctx_priv, vhost, pfd, &p_entry);
	assert(pfd_count < MAX_HOSTS * SOCKET_NUM);

	unsigned int act_ses = ctx_priv->cmd_count;
	unsigned int host_count = ctx_priv->cmd_count + ctx_priv->res_count;

	while (!ctx_priv->interrupted) {
		/* Poll for timers, hosts and input pipes */
		poll(pfd, pfd_count, -1);

		/* Check for hung sessions */
		if (p_entry.ses_timer->revents == POLLIN) {
			if (sessions_hung(ctx_priv, vhost, &act_ses,
					  host_count)) {
				process_reset_backend(ctx, GPU_RESET_TRUE);
				set_timer(p_entry.recon_timer->fd,
					  conn_args->reconn_intv_ms);
			}
			set_timer(p_entry.ses_timer->fd, 0);
		}
		/* Try to reconnect */
		if (p_entry.recon_timer->revents == POLLIN) {
			if (sessions_reconnect(ctx, vhost,
					       p_entry.recon_timer->fd,
					       host_count)) {
				set_timer(p_entry.recon_timer->fd, 0);
				set_timer(p_entry.ses_timer->fd, 0);
				act_ses = ctx_priv->cmd_count;
			}
		}

		in_out_events(ctx, &p_entry, ctx_priv->cmd_count,
			      ctx_priv->res_count);
	}

	/* Release resources */
	for (unsigned int i = 0; i < ctx_priv->cmd_count; i++)
		close(ctx_priv->cmd[i].sock);
	for (unsigned int i = 0; i < ctx_priv->res_count; i++)
		close(ctx_priv->res[i].sock);
	close(p_entry.recon_timer->fd);
	close(p_entry.ses_timer->fd);
	close(devnull);
	return NULL;
}
