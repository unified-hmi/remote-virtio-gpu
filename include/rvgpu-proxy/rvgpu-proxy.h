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

#ifndef RVGPU_PROXY_H
#define RVGPU_PROXY_H

#include <librvgpu/rvgpu-plugin.h>

#define RVGPU_MIN_CONN_TMT_S 1u
#define RVGPU_MAX_CONN_TMT_S 100u
#define RVGPU_DEFAULT_CONN_TMT_S 100u

#define RVGPU_RECONN_INVL_MS 500u

#define DEFAULT_WIDTH 800u
#define DEFAULT_HEIGHT 600u

#define CARD_INDEX_MIN 0
#define CARD_INDEX_MAX 64

#define VMEM_MIN_MB 0
#define VMEM_DEFAULT_MB 0
#define VMEM_MAX_MB 4096u

#define FRAMERATE_MIN 1u
#define FRAMERATE_MAX 120u

#define RVGPU_DEFAULT_HOSTNAME "127.0.0.1"
#define RVGPU_DEFAULT_PORT "55667"

#define CAPSET_PATH "/etc/virgl.capset"
#define VIRTIO_LO_PATH "/dev/virtio-lo"

enum { PROXY_GPU_CONFIG, PROXY_GPU_QUEUES, PROXY_INPUT_EVENT };

struct host_server {
	char *hostname;
	char *portnum;
};

struct host_conn {
	struct host_server hosts[MAX_HOSTS];
	unsigned int host_cnt;
	unsigned int conn_tmt_s;
	unsigned int reconn_intv_ms;
	bool active;
};

#endif /* RVGPU_PROXY_H */
