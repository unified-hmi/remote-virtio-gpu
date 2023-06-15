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
#include <dlfcn.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/poll.h>
#include <unistd.h>

#include <rvgpu-proxy/gpu/rvgpu-gpu-device.h>
#include <rvgpu-proxy/gpu/rvgpu-input-device.h>
#include <rvgpu-proxy/rvgpu-proxy.h>

#include <librvgpu/rvgpu-plugin.h>
#include <librvgpu/rvgpu-protocol.h>

#include <rvgpu-generic/rvgpu-sanity.h>
#include <rvgpu-generic/rvgpu-utils.h>

static void usage(void)
{
	static const char program_name[] = "rvgpu-proxy";

	info("Usage: %s [options]\n", program_name);
	info("\t-c capset\tspecify capset file (default: %s)\n", CAPSET_PATH);
	info("\t-s scanout\tspecify scanout in form WxH@X,Y (default: %ux%u@0,0)\n",
	     DEFAULT_WIDTH, DEFAULT_HEIGHT);
	info("\t-f rate\t\tspecify virtual framerate (default: disabled)\n");
	info("\t-i index\tspecify index 'n' for device /dev/dri/card<n>\n");
	info("\t-n\t\tserver:port for connecting (max 4 hosts, default: %s:%s)\n",
	     RVGPU_DEFAULT_HOSTNAME, RVGPU_DEFAULT_PORT);
	info("\t-h\t\tshow this message\n");
}

static void *input_thread_func(void *param)
{
	struct input_device *inpdev = (struct input_device *)param;
	struct rvgpu_input_header hdr;

	while (input_read(inpdev, &hdr, sizeof(hdr), &hdr.src) > 0) {
		struct rvgpu_input_event uev[hdr.evnum];
		ssize_t len = sizeof(uev[0]) * hdr.evnum;

		if (input_read(inpdev, uev, len, NULL) != len)
			break;
		input_device_serve(inpdev, &hdr, uev);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	struct gpu_device *dev;
	struct input_device *inpdev;
	struct rvgpu_backend *rvgpu_be = NULL;
	int w, h, x , y;

	struct gpu_device_params params = {
		.framerate = 0u,
		.mem_limit = VMEM_DEFAULT_MB,
		.card_index = -1,
		.num_scanouts = 0u,
		.dpys = { { .r = { .x = 0,
				   .y = 0,
				   .width = DEFAULT_WIDTH,
				   .height = DEFAULT_HEIGHT },
			    .flags = 1,
			    .enabled = 1 } },
	};

	struct host_conn servers = {
		.host_cnt = 0,
		.conn_tmt_s = RVGPU_DEFAULT_CONN_TMT_S,
		.reconn_intv_ms = RVGPU_RECONN_INVL_MS,
		.active = true,
	};

	pthread_t input_thread;
	char path[64];
	FILE *oomFile;
	int lo_fd, epoll_fd, res, opt, capset = -1;
	char *ip, *port, *errstr = NULL;

	while ((opt = getopt(argc, argv, "hi:n:M:c:R:f:s:")) != -1) {
		switch (opt) {
		case 'c':
			capset = open(optarg, O_RDONLY);
			if (capset == -1)
				err(1, "open %s", optarg);
			break;
		case 'i':
			params.card_index =
				(int)sanity_strtonum(optarg, CARD_INDEX_MIN,
						     CARD_INDEX_MAX - 1,
						     &errstr);
			if (errstr != NULL) {
				warnx("Card index should be in [%u..%u]\n",
				      CARD_INDEX_MIN, CARD_INDEX_MAX - 1);
				errx(1, "Invalid card index %s:%s", optarg,
				     errstr);
			}

			snprintf(path, sizeof(path), "/dev/dri/card%d",
				 params.card_index);
			res = access(path, F_OK);
			if (res == 0)
				errx(1, "device %s exists", path);
			else if (errno != ENOENT)
				err(1, "error while checking device %s", path);
			break;
		case 'M':
			params.mem_limit = (unsigned int)sanity_strtonum(
				optarg, VMEM_MIN_MB, VMEM_MAX_MB, &errstr);
			if (errstr != NULL) {
				warnx("Memory limit should be in [%u..%u]\n",
				      VMEM_MIN_MB, VMEM_MAX_MB);
				errx(1, "Invalid memory limit %s:%s", optarg,
				     errstr);
			}
			break;
		case 'f':
			params.framerate = (unsigned long)sanity_strtonum(
				optarg, FRAMERATE_MIN, FRAMERATE_MAX, &errstr);
			if (errstr != NULL) {
				warnx("Framerate should be in [%u..%u]\n",
				      FRAMERATE_MIN, FRAMERATE_MAX);
				errx(1, "Invalid framerate %s:%s", optarg,
				     errstr);
			}
			break;
		case 's':
			if (params.num_scanouts >= VIRTIO_GPU_MAX_SCANOUTS) {
				errx(1, "too many scanouts, max is %d",
				     VIRTIO_GPU_MAX_SCANOUTS);
			}
			if (sscanf(optarg, "%dx%d@%d,%d", &w, &h, &x, &y) == 4u) {
				if (w > 0 && h > 0 && x >= 0 && y >= 0){
					params.dpys[params.num_scanouts].r.width = (uint32_t)w;
					params.dpys[params.num_scanouts].r.height = (uint32_t)h;
					params.dpys[params.num_scanouts].r.x = (uint32_t)x;
					params.dpys[params.num_scanouts].r.y = (uint32_t)y;				
					params.dpys[params.num_scanouts].enabled = 1;
					params.dpys[params.num_scanouts].flags = 1;
					params.num_scanouts++;
				} else {
					errx(1, "invalid scanout configuration %s, width and height "
						"values must be greater than zero, x y position must be "
						"greater or equal zero", optarg);
				}
			
			} else {
				errx(1, "invalid scanout configuration %s",
				     optarg);
			}
			break;
		case 'n':
			ip = strtok(optarg, ":");
			if (ip == NULL) {
				warnx("Pass a valid IPv4 address and port\n");
				err(1, "Incorrect format for server:port");
			}
			port = strtok(NULL, "");
			if (port == NULL)
				port = RVGPU_DEFAULT_PORT;

			if (servers.host_cnt == MAX_HOSTS) {
				errx(1, "Only upto %d hosts are supported.",
				     MAX_HOSTS);
			}

			servers.hosts[servers.host_cnt].hostname = ip;
			servers.hosts[servers.host_cnt].portnum = port;
			servers.host_cnt++;
			break;
		case 'R':
			servers.conn_tmt_s = (unsigned int)sanity_strtonum(
				optarg, RVGPU_MIN_CONN_TMT_S,
				RVGPU_MAX_CONN_TMT_S, &errstr);
			if (errstr != NULL) {
				warnx("Conn timeout should be in [%u..%u]\n",
				      RVGPU_MIN_CONN_TMT_S,
				      RVGPU_MAX_CONN_TMT_S);
				errx(1, "Invalid conn timeout %s:%s", optarg,
				     errstr);
			}
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		default:
			usage();
			exit(EXIT_FAILURE);
		}
	}

	if (capset == -1) {
		capset = open(CAPSET_PATH, O_RDONLY);
		if (capset == -1)
			err(1, "%s", CAPSET_PATH);
	}

	if (servers.host_cnt == 0) {
		servers.hosts[0].hostname = RVGPU_DEFAULT_HOSTNAME;
		servers.hosts[0].portnum = RVGPU_DEFAULT_PORT;
		servers.host_cnt = 1;
	}

	rvgpu_be = init_backend_rvgpu(&servers);
	assert(rvgpu_be);

	lo_fd = open(VIRTIO_LO_PATH, O_RDWR);
	if (lo_fd == -1)
		err(1, "%s", VIRTIO_LO_PATH);

	epoll_fd = epoll_create(1);
	if (epoll_fd == -1)
		err(1, "epoll_create");

	if (params.num_scanouts == 0)
		params.num_scanouts = 1;

	/* change oom_score_adj to be very less likely killed */
	oomFile = fopen("/proc/self/oom_score_adj", "w");
	if (oomFile == NULL) {
		err(1, "fopen /proc/self/oom_score_adj");
	} else {
		fprintf(oomFile, "%d", -1000);
		fclose(oomFile);
	}

	dev = gpu_device_init(lo_fd, epoll_fd, PROXY_GPU_CONFIG,
			      PROXY_GPU_QUEUES, capset, &params, rvgpu_be);

	if (!dev)
		err(1, "gpu device init");

	inpdev = input_device_init(rvgpu_be);
	if (!inpdev)
		err(1, "input device init");
	if (pthread_create(&input_thread, NULL, input_thread_func, inpdev) !=
	    0) {
		err(1, "input thread create");
	}

	/* do the main_cycle */
	for (;;) {
		int i, n;
		struct epoll_event events[8];

		n = epoll_wait(epoll_fd, events, ARRAY_SIZE(events), -1);

		for (i = 0; i < n; i++) {
			switch (events[i].data.u32) {
			case PROXY_GPU_CONFIG:
				gpu_device_config(dev);
				break;
			case PROXY_GPU_QUEUES:
				gpu_device_serve(dev);
				break;
			default:
				errx(1, "Uknown event!");
			}
		}
	}

	gpu_device_free(dev);
	input_device_free(inpdev);

	close(epoll_fd);
	close(lo_fd);
	close(capset);

	return EXIT_SUCCESS;
}
