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

#ifndef RVGPU_H
#define RVGPU_H

#include <stdbool.h>
#include <arpa/inet.h>
#include <sys/queue.h>

#include <librvgpu/rvgpu-plugin.h>

#define MAX_HOSTS 16

#define SOCKET_NUM 2
#define TIMERS_CNT 2

#define PIPE_SIZE (8 * 1024 * 1024)
#define PIPE_READ (0)
#define PIPE_WRITE (1)

struct gpu_reset {
	enum reset_state state;
	pthread_mutex_t lock;
	pthread_cond_t cond;
};

struct conn_pipes {
	int rcv_pipe[2];
	int snd_pipe[2];
};

enum host_state {
	HOST_NONE,
	HOST_CONNECTED,
	HOST_DISCONNECTED,
	HOST_RECONNECTED,
};

struct vgpu_host {
	struct tcp_host *tcp;
	struct pollfd *pfd;
	int host_p[2];
	int vpgu_p[2];
	int sock;
	enum host_state state;
};

struct ctx_priv {
	pthread_t tid;
	uint16_t inited_scanout_num;
	uint16_t scanout_num;
	bool interrupted;
	struct vgpu_host cmd[MAX_HOSTS];
	struct vgpu_host res[MAX_HOSTS];
	uint16_t cmd_count;
	uint16_t res_count;
	struct gpu_reset reset;
	pthread_mutex_t lock;
	struct rvgpu_scanout *sc[MAX_HOSTS];
	struct rvgpu_ctx_arguments args;
	void (*gpu_reset_cb)(struct rvgpu_ctx *ctx,
			     enum reset_state state); /**< reset callback */
	LIST_HEAD(res_head, rvgpu_res) reslist;
};

struct sc_priv {
	struct conn_pipes pipes[SOCKET_NUM];
	struct rvgpu_scanout_arguments *args;
	bool activated;
};

/** @brief Init a remote virtio gpu context
 *
 *  @param ctx pointer to the rvgpu context
 *  @param state GPU reset state
 *  @param args arguments needed to initialize a remote virtio gpu context
 *  @param gpu_reset_cb callback function to process the GPU reset state
 *
 *  @return 0 on success
 *         -1 on error
 */
int rvgpu_ctx_init(struct rvgpu_ctx *ctx, struct rvgpu_ctx_arguments args,
		   void (*gpu_reset_cb)(struct rvgpu_ctx *ctx,
					enum reset_state state));

/** @brief Destroy a remote virtio gpu context
 *
 *  @param ctx pointer to the rvgpu context
 *
 *  @return void
 */
void rvgpu_ctx_destroy(struct rvgpu_ctx *ctx);

/** @brief Lock ctx and wait for appropriate GPU reset state
 *
 *  @param ctx pointer to the rvgpu context
 *  @param state GPU reset state
 *
 *  @return void
 */
void rvgpu_ctx_wait(struct ctx_priv *ctx, enum reset_state state);

/** @brief Wakeup a ctx from rvgpu_ctx_wait function
 *
 *  @param ctx pointer to the rvgpu context
 *  @param state GPU reset state
 *
 *  @return void
 */
void rvgpu_ctx_wakeup(struct ctx_priv *ctx);

/** @brief Poll for ctx events
 *
 *  @param ctx pointer to the rvgpu context
 *  @param p type of pipe. virtio or resource pipe
 *  @param timeo poll timeout
 *  @param events poll events
 *  @param revents poll revents
 *
 *  @return number of poll revents
 */
int rvgpu_ctx_poll(struct rvgpu_ctx *ctx, enum pipe_type p, int timeo,
		   short int *events, short int *revents);

/** @brief Transfer the virtio stream to remote targets
 *
 *  @param ctx pointer to the rvgpu context
 *  @param buf pointer to virtio buffer
 *  @param len size of data
 *
 *  @return 0 on success
 *  @return errno on error
 */
int rvgpu_ctx_send(struct rvgpu_ctx *ctx, const void *buf, size_t len);

/** @brief transfer a remote virtio gpu resource to target
 *
 *  @param ctx pointer to the rvgpu context
 *  @param t resource transfer container
 *  @param res remote virtio gpu resource
 *
 *  @return 0 on success
 *  @return -1 on error
 */
int rvgpu_ctx_transfer_to_host(struct rvgpu_ctx *ctx,
			       const struct rvgpu_res_transfer *t,
			       const struct rvgpu_res *res);

/** @brief Get a remote virtio gpu resource
 *
 *  @param ctx pointer to the rvgpu context
 *  @param resource_id resource identificator
 *
 *  @return pointer to resource
 */
struct rvgpu_res *rvgpu_ctx_res_find(struct rvgpu_ctx *ctx,
				     uint32_t resource_id);

/** @brief Destroy a remote virtio gpu resource
 *
 *  @param ctx pointer to the rvgpu context
 *  @param resource_id resource identificator
 *
 *  @return void
 */
void rvgpu_ctx_res_destroy(struct rvgpu_ctx *ctx, uint32_t resource_id);

/** @brief Create a remote virtio gpu resource
 *
 *  @param ctx pointer to the rvgpu context
 *  @param info resource info
 *  @param resource_id resource identificator
 *
 *  @return 0 on success
 *  @return -1 on error
 */
int rvgpu_ctx_res_create(struct rvgpu_ctx *ctx,
			 const struct rvgpu_res_info *info,
			 uint32_t resource_id);

/** @brief Initialize a remote target
 *
 *  @param ctx pointer to the rvgpu context
 *  @param scanout pointer to remote target
 *  @param args arguments needed to initialize a remote target
 *
 *  @return 0 on success
 *  @return -1 on error
 */
int rvgpu_init(struct rvgpu_ctx *ctx, struct rvgpu_scanout *scanout,
	       struct rvgpu_scanout_arguments args);

/** @brief Destroy a remote target
 *
 *  @param ctx pointer to the rvgpu context
 *  @param scanout pointer to remote target
 *
 *  @return void
 */
void rvgpu_destroy(struct rvgpu_ctx *ctx, struct rvgpu_scanout *scanout);

/** @brief Receive data of the specified length from a remote target
 *
 *  @param scanout pointer to remote target
 *  @param p type of data. virtio command or resource
 *  @param buf pointer to virtio buffer
 *  @param len size of the virtio buffer
 *
 *  @return size of received data
 *  @return errno on error
 */
int rvgpu_recv_all(struct rvgpu_scanout *scanout, enum pipe_type p, void *buf,
		   size_t len);

/** @brief Receive data from a remote target
 *
 *  @param scanout pointer to remote target
 *  @param p type of data. virtio command or resource
 *  @param buf pointer to virtio buffer
 *  @param len size of the virtio buffer
 *
 *  @return size of received data
 *  @return errno on error
 */
int rvgpu_recv(struct rvgpu_scanout *scanout, enum pipe_type p, void *buf,
	       size_t len);

/** @brief Send data to a remote target
 *
 *  @param scanout pointer to remote target
 *  @param p type of pipe. virtio or resource pipe
 *  @param buf pointer to virtio buffer
 *  @param size of data
 *
 *  @return size of sent data
 *  @return errno on error
 */
int rvgpu_send(struct rvgpu_scanout *scanout, enum pipe_type p, const void *buf,
	       size_t len);

/** @brief Process the GPU reset state
 *
 *  @param ctx pointer to the rvgpu context
 *  @param state GPU reset state
 *
 *  @return void
 */
void rvgpu_frontend_reset_state(struct rvgpu_ctx *ctx, enum reset_state state);

void *thread_conn_tcp(void *arg);

#endif /* RVGPU_H */
