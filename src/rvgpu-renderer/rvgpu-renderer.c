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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <rvgpu-generic/rvgpu-sanity.h>
#include <rvgpu-generic/rvgpu-utils.h>
#include <rvgpu-renderer/renderer/rvgpu-egl.h>
#include <rvgpu-renderer/rvgpu-renderer.h>

#include <linux/virtio_gpu.h>

static void usage(void)
{
	static const char program_name[] = "rvgpu-renderer";

	info("Usage: %s [options]\n", program_name);
	info("\t-a\t\tenable translucent mode on Wayland\n");
	info("\t-B color\tcolor of initial screen in RGBA format");
	info("(0xRRGGBBAA, default is 0x%08x)\n", BACKEND_COLOR);
	info("\t-c capset\tdump capset into file\n");
	info("\t-s scanout\tdisplay specified scanout\n");
	info("\t-b box\t\toverride scanout box (format WxH@X,Y)\n");
	info("\t-i ID\t\tset scanout window ID (for IVI shell)\n");
	info("\t-g card\t\tuse GBM mode on card (/dev/dri/cardN)\n");
	info("\t-S seat\t\tspecify seat for input in GBM mode\n");
	info("\t-f\t\tRun in fullscreen mode\n");
	info("\t-p port\t\tport for listening (default: %u)\n",
	     RVGPU_DEFAULT_PORT);
	info("\t-v\t\tRun in vsync mode (eglSwapInterval 1)\n");
	info("\t-h\t\tShow this message\n");

	info("\nNote:\n");
	info("\tThe alpha component of 'B' option is ignored in the following cases:\n");
	info("\t- 'a' option is not used.\n");
	info("\t- 'g' option is used.\n");

	info("\n\tSince 's' option specifies scanout of 'b' and 'i' options,\n");
	info("\tsetting it before 'b' and 'i' options is mandatory,\n");
	info("\te.g., rvgpu-renderer -s 0 -b 1920x1080@0,0 -i 9000 -p 55667.\n");

	info("\n\t'-b' option is used to crop out an area from the application's drawing area at rvgpu-renderer side.\n");
	info("\tIf omitted, an initial 100x100 surface will be created when connected from rvgpu-proxy.\n");
	info("\tThis initial surface will be expanded to the full size of application's draw area later.\n");

	info("\n\t'g' option is for using GBM mode, which scales to full screen according to 'b' option.\n");

	info("\n\t'f' option scales to full screen according to 'b' option.\n");
}

/* Signal handler to reap zombie processes */
static void wait_for_child(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

static FILE *listen_conn(uint16_t port_nr, int *res_socket)
{
	int sock, newsock = -1;
	struct sigaction sa;
	struct sockaddr_in server_addr = { 0 };
	int reuseaddr = 1; /* True */
	int fin_wait = 1; /* FIN wait timeout */
	pid_t pid = 0;

	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == -1)
		err(1, "socket");

	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuseaddr,
		       sizeof(int)) == -1) {
		err(1, "setsockopt");
	}

	if (setsockopt(sock, SOL_TCP, TCP_LINGER2, &fin_wait, sizeof(int)) ==
	    -1) {
		err(1, "setsockopt");
	}

	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	server_addr.sin_port = htons(port_nr);

	if (bind(sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) ==
	    -1) {
		err(1, "bind");
	}

	if (listen(sock, BACKLOG) == -1)
		err(1, "listen");

	sa.sa_handler = wait_for_child;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	if (sigaction(SIGCHLD, &sa, NULL) == -1)
		err(1, "sigaction");

	while (1) {
		newsock = accept4(sock, NULL, NULL, SOCK_NONBLOCK);
		if (newsock == -1)
			err(1, "accept");

		*res_socket = accept4(sock, NULL, NULL, SOCK_NONBLOCK);
		if (*res_socket == -1)
			err(1, "accept");

		if (pid > 0) {
			kill(pid, SIGTERM);
			/*
			 * sleep for 100 ms until all child resources
			 * will be freed
			 */
			usleep(100 * 1000);
		}

		pid = fork();
		switch (pid) {
		case 0: /* In child process */
		{
			FILE *ret;

			close(sock);
			dup2(newsock, 0);
			ret = fdopen(newsock, "w");
			setvbuf(ret, NULL, _IOFBF, BUFSIZ);
			return ret;
		}
		case -1: /* fork failed */
			err(1, "fork");
		default: /* Parent process */
			if (newsock > 0)
				close(newsock);
			if (*res_socket > 0)
				close(*res_socket);
		}
	}

	close(sock);
	return NULL;
}

int main(int argc, char **argv)
{
	struct rvgpu_pr_state *pr;
	struct rvgpu_egl_state *egl;
	struct rvgpu_scanout_params sp[VIRTIO_GPU_MAX_SCANOUTS], *cp = &sp[0];
	struct rvgpu_pr_params pp = {
		.sp = sp,
		.nsp = VIRTIO_GPU_MAX_SCANOUTS,
	};
	struct rvgpu_egl_params e_params = {
		.clear_color = BACKEND_COLOR,
	};
	char *errstr = NULL;
	const char *carddev = NULL;
	const char *seat = "seat0";
	int w, h, x, y, opt, res, res_socket = 0;
	unsigned int res_id, scanout;
	uint16_t port_nr = RVGPU_DEFAULT_PORT;
	FILE *input_stream = stdout;
	bool fullscreen = false, vsync = false, translucent = false,
	     user_specified_scanouts = false;

	memset(sp, 0, sizeof(sp));
	memset(&pp, 0, sizeof(pp));

	while ((opt = getopt(argc, argv, "afhvi:c:s:S:b:B:p:g:")) != -1) {
		switch (opt) {
		case 'a':
			translucent = true;
			break;
		case 'B':
			e_params.clear_color = (unsigned int)sanity_strtonum(
						optarg, 0, 0xFFFFFFFF, &errstr);
			if (errstr != NULL){
				warnx("Background color should be in 0 - 0xFFFFFFFF\n");
				errx(1, "Invalid background color specified %s:%s",
				     optarg, errstr);				
			}
			break;

		case 'c':
			pp.capset = fopen(optarg, "w");
			if (pp.capset == NULL)
				err(1, "cannot open %s for writing", optarg);
			break;
		case 's':
			scanout = (unsigned int)sanity_strtonum(
				optarg, 0, VIRTIO_GPU_MAX_SCANOUTS - 1,
				&errstr);
			if (errstr != NULL) {
				warnx("Scanout number should be in [%u..%u]\n",
				      0, VIRTIO_GPU_MAX_SCANOUTS);
				errx(1, "Invalid scanout %s:%s", optarg,
				     errstr);
			}
			cp = &sp[scanout];
			cp->enabled = true;
			user_specified_scanouts = true;
			break;
		case 'b':
			if (sscanf(optarg, "%dx%d@%d,%d", &w, &h, &x, &y) != 4) {
				errx(1, "invalid scanout box %s", optarg);
			} else {
				if (w > 0 && h > 0 && x >= 0 && y >= 0){
					cp->box.w = (unsigned int)w;
					cp->box.h = (unsigned int)h;
					cp->box.x = (unsigned int)x;
					cp->box.y = (unsigned int)y;
				} else {
					errx(1, "invalid scanout configuration %s, width and height "
						"values must be greater than zero, x y position must be "
						"greater or equal zero", optarg);
				}
			}
			cp->boxed = true;
			break;
		case 'i':
			cp->id = (uint32_t)sanity_strtonum(
				optarg, 1, UINT32_MAX, &errstr);
			if (errstr != NULL)
				errx(1, "Invalid IVI id specified %s:%s",
				     optarg, errstr);
			break;
		case 'g':
			carddev = optarg;
			break;
		case 'S':
			seat = optarg;
			break;
		case 'f':
			fullscreen = true;
			break;
		case 'p':
			port_nr = (uint16_t)sanity_strtonum(optarg,
							     MIN_PORT_NUMBER,
							     MAX_PORT_NUMBER,
							     &errstr);
			if (errstr != NULL) {
				warnx("Port number should be in [%u..%u]\n",
				      MIN_PORT_NUMBER, MAX_PORT_NUMBER);
				errx(1, "Invalid port number %s:%s", optarg,
				     errstr);
			}
			break;
		case 'v':
			vsync = true;
			break;
		case 'h':
			usage();
			exit(EXIT_SUCCESS);
		default:
			usage();
			exit(EXIT_FAILURE);
		}
	}

	if (!user_specified_scanouts) {
		sp[0].enabled = true;
	}

	input_stream = listen_conn(port_nr, &res_socket);
	assert(input_stream);

	if (carddev == NULL)
		egl = rvgpu_wl_init(fullscreen, translucent, input_stream);
	else
		egl = rvgpu_gbm_init(carddev, seat, input_stream);

	egl->params = &e_params;

	pr = rvgpu_pr_init(egl, &pp, res_socket);

	for (unsigned int i = 0; i < VIRTIO_GPU_MAX_SCANOUTS; i++) {
		struct rvgpu_scanout *s = &egl->scanouts[i];

		s->scanout_id = i;
		s->params = sp[i];
		if (sp[i].enabled) {
			rvgpu_egl_create_scanout(egl, &egl->scanouts[i]);
			rvgpu_egl_draw(egl, &egl->scanouts[i], false);
		}
	}

	while ((res_id = rvgpu_pr_dispatch(pr))) {
		rvgpu_egl_drawall(egl, res_id, vsync);
	}

	if (pp.capset)
		fclose(pp.capset);

	rvgpu_pr_free(pr);
	rvgpu_egl_free(egl);
	fclose(input_stream);

	return EXIT_SUCCESS;
}
