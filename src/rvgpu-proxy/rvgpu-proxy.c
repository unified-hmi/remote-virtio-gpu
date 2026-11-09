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
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libudev.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <librvgpu/rvgpu-plugin.h>
#include <librvgpu/rvgpu-protocol.h>

#include <rvgpu-proxy/gpu/rvgpu-gpu-device.h>
#include <rvgpu-proxy/gpu/rvgpu-input-device.h>
#include <rvgpu-proxy/rvgpu-proxy.h>

#include <rvgpu-generic/rvgpu-sanity.h>
#include <rvgpu-utils/rvgpu-utils.h>

/* Global flag for graceful shutdown - set by signal handler */
static volatile sig_atomic_t g_shutdown_requested;

/* Signal handler - only sets the flag, nothing else */
static void shutdown_signal_handler(int sig)
{
	(void)sig;
	g_shutdown_requested = 1;
}

/**
 * @brief Find PIDs of processes using the specified device and store them
 * @param target_stat - stat of the target device
 * @param my_pid - our own pid to exclude
 * @param pids - array to store PIDs (caller must provide)
 * @param max_pids - maximum number of PIDs to store
 * @return number of processes found
 */
static int find_dri_user_pids(const struct stat *target_stat, pid_t my_pid,
			     pid_t *pids, int max_pids)
{
	int count = 0;
	DIR *proc_dir = opendir("/proc");

	if (!proc_dir)
		return 0;

	struct dirent *proc_entry;

	while ((proc_entry = readdir(proc_dir)) != NULL && count < max_pids) {
		pid_t pid = (pid_t)atoi(proc_entry->d_name);

		if (pid <= 0 || pid == my_pid)
			continue;

		char fd_dir_path[64];

		snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd",
			 pid);

		DIR *fd_dir = opendir(fd_dir_path);

		if (!fd_dir)
			continue;

		struct dirent *fd_entry;

		while ((fd_entry = readdir(fd_dir)) != NULL) {
			char fd_path[128];
			struct stat fd_stat;

			snprintf(fd_path, sizeof(fd_path), "%s/%s",
				 fd_dir_path, fd_entry->d_name);
			if (stat(fd_path, &fd_stat) == 0 &&
			    fd_stat.st_dev == target_stat->st_dev &&
			    fd_stat.st_ino == target_stat->st_ino) {
				pids[count++] = pid;
				break;
			}
		}
		closedir(fd_dir);
	}
	closedir(proc_dir);
	return count;
}

/**
 * @brief Check if a process is still running
 * @param pid - process ID to check
 * @return true if process exists, false otherwise
 */
static bool process_exists(pid_t pid)
{
	return kill(pid, 0) == 0 || errno == EPERM;
}

/**
 * @brief Check if a process still has threads in the kernel
 *
 * Even after SIGKILL, threads may linger in D state (e.g., DRM cleanup).
 * /proc/<pid> exists until all threads have fully exited.
 */
static bool process_has_threads(pid_t pid)
{
	char path[64];

	snprintf(path, sizeof(path), "/proc/%d", pid);
	return access(path, F_OK) == 0;
}

/**
 * @brief Wait for a list of processes to terminate
 * @param pids - array of PIDs
 * @param count - number of PIDs
 * @param timeout_ms - maximum time to wait in milliseconds
 * @return number of processes still running after timeout
 */
static int wait_for_processes(pid_t *pids, int count, int timeout_ms)
{
	const int poll_interval_ms = 50;
	int elapsed = 0;

	while (elapsed < timeout_ms) {
		int still_running = 0;

		for (int i = 0; i < count; i++) {
			if (pids[i] > 0 && process_exists(pids[i]))
				still_running++;
		}
		if (still_running == 0)
			return 0;

		usleep(poll_interval_ms * 1000);
		elapsed += poll_interval_ms;
	}

	/* Count remaining processes */
	int remaining = 0;

	for (int i = 0; i < count; i++) {
		if (pids[i] > 0 && process_exists(pids[i]))
			remaining++;
	}
	return remaining;
}

/**
 * @brief Count how many external processes have the given DRM device open
 * @param devnode - DRM device path (e.g. "/dev/dri/card0")
 * @return number of other processes using the device
 */
static int count_dri_users(const char *devnode)
{
	struct stat st;
	pid_t pids[64];

	if (!devnode || stat(devnode, &st) < 0)
		return 0;

	return find_dri_user_pids(&st, getpid(), pids, 64);
}

/**
 * @brief Find DRM card number matching the given vendor_id via DEVPATH
 * @param vendor_id - vendor_id used when creating the virtio-lo device
 * @return card number (>= 0) on success, -1 if not found
 */
static int find_drm_card_by_vendor(uint32_t vendor_id)
{
	struct udev *ud;
	struct udev_enumerate *en;
	struct udev_list_entry *devs, *e;
	int card = -1;

	ud = udev_new();
	if (!ud)
		return -1;

	en = udev_enumerate_new(ud);
	if (!en) {
		udev_unref(ud);
		return -1;
	}

	udev_enumerate_add_match_subsystem(en, "drm");
	udev_enumerate_scan_devices(en);
	devs = udev_enumerate_get_list_entry(en);

	udev_list_entry_foreach(e, devs) {
		const char *sp = udev_list_entry_get_name(e);
		struct udev_device *d =
			udev_device_new_from_syspath(ud, sp);
		if (!d)
			continue;

		const char *dn = udev_device_get_devnode(d);
		const char *seat = udev_device_get_property_value(
			d, "ID_SEAT");
		const char *devpath = udev_device_get_property_value(
			d, "DEVPATH");

		if (dn && strstr(dn, "/dev/dri/card") && seat &&
		    strcmp(seat, "seat_virtual") == 0 && devpath) {
			unsigned int dp_vid;

			if (get_virtio_suffix_hex(devpath, &dp_vid) == 0 &&
			    dp_vid == vendor_id) {
				const char *cn = strstr(dn, "card");

				if (cn)
					card = atoi(cn + 4);
			}
		}
		udev_device_unref(d);
		if (card >= 0)
			break;
	}

	udev_enumerate_unref(en);
	udev_unref(ud);
	return card;
}

/**
 * @brief Wait for DRM card node to appear while processing virtqueue
 * @param dev - gpu device
 * @param epoll_fd - epoll file descriptor for virtqueue events
 *
 * The kernel virtio-gpu driver probes asynchronously and needs
 * virtqueue responses before creating /dev/dri/ nodes.
 * @return card number (>= 0) on success, -1 on timeout
 */
static int wait_for_drm_card(struct gpu_device *dev, int epoll_fd)
{
	uint32_t vid = gpu_device_get_vendor_id(dev);
	const int max_wait_ms = 15000;
	struct timespec ts_start, ts_now;

	clock_gettime(CLOCK_MONOTONIC, &ts_start);

	for (;;) {
		struct epoll_event events[8];
		int n = epoll_wait(epoll_fd, events, ARRAY_SIZE(events), 100);

		for (int i = 0; i < n; i++) {
			if (events[i].data.u32 == PROXY_GPU_QUEUES)
				gpu_device_serve(dev);
		}

		int card = find_drm_card_by_vendor(vid);

		if (card >= 0)
			return card;

		clock_gettime(CLOCK_MONOTONIC, &ts_now);
		int elapsed_ms =
			(int)((ts_now.tv_sec - ts_start.tv_sec) * 1000 +
			      (ts_now.tv_nsec - ts_start.tv_nsec) / 1000000);
		if (elapsed_ms >= max_wait_ms) {
			warnx("udev: DRM device not found (timeout %dms,"
			      " vendor_id=0x%04x)", max_wait_ms, vid);
			return -1;
		}
	}
}

/**
 * @brief Ask the processes still holding our DRM card to let go of it
 * @param card_num - DRM card number (e.g. 2 for /dev/dri/card2)
 * @param out_pids - array to store signalled PIDs (caller provides)
 * @param max_pids - size of out_pids array
 * @return number of PIDs signalled (0 if none)
 */
static int terminate_dri_users(int card_num, pid_t *out_pids, int max_pids)
{
	char devnode[32];
	struct stat target_stat;
	pid_t my_pid = getpid();
	pid_t pids[64];
	int count, remaining, out_count;
	const int sigterm_timeout_ms = 5000;

	if (card_num < 0) {
		warnx("DRM card number unknown, skipping DRI cleanup");
		return 0;
	}

	snprintf(devnode, sizeof(devnode), "/dev/dri/card%d", card_num);

	if (stat(devnode, &target_stat) < 0) {
		warnx("DRI device %s not found", devnode);
		return 0;
	}

	warnx("Found DRI device: %s", devnode);

	/* Find processes using the device */
	count = find_dri_user_pids(&target_stat, my_pid, pids, 64);
	if (count == 0) {
		warnx("No processes using %s", devnode);
		return 0;
	}

	warnx("Found %d process(es) using %s, sending SIGTERM...", count,
	      devnode);

	/* Send SIGTERM to all processes */
	for (int i = 0; i < count; i++) {
		warnx("  Sending SIGTERM to PID %d", pids[i]);
		kill(pids[i], SIGTERM);
	}

	/* Wait for processes to terminate */
	remaining = wait_for_processes(pids, count, sigterm_timeout_ms);
	if (remaining == 0)
		warnx("All DRI users terminated gracefully");
	else
		/*
		 * Killing here costs more than it saves: the client dies
		 * before its virtio-gpu commands are answered and the driver
		 * then loses those vbufs ("Slab cache still has objects" when
		 * the device goes away). The drain below both wakes the client
		 * and escalates once it is clear nothing is moving.
		 */
		warnx("%d process(es) still hold the device, leaving it to the drain",
		      remaining);

	/*
	 * Wait for kernel to release DRM resources (GEM objects, etc.)
	 * Even after userspace processes terminate, the kernel needs time
	 * to clean up virtio-gpu-vbufs and other internal resources.
	 * The cleanup involves:
	 *   1. Closing file descriptors triggers drm_release()
	 *   2. drm_release() schedules GEM object destruction
	 *   3. virtio-gpu driver frees command buffers
	 *   4. Memory manager cleans up allocations
	 *
	 * Open the DRM device briefly to trigger any pending cleanup.
	 */
	{
		int drm_fd = open(devnode, O_RDWR | O_CLOEXEC);

		if (drm_fd >= 0) {
			/* Opening and closing DRM device triggers cleanup */
			fsync(drm_fd);
			close(drm_fd);
			warnx("Triggered DRM device cleanup via open/close");
		}
	}

	/*
	 * Note: No fixed sleep needed here. gpu_device_flush_pending()
	 * will poll until no new virtqueue requests arrive, which
	 * naturally waits for kernel DRM cleanup to complete.
	 */

	out_count = count < max_pids ? count : max_pids;
	for (int i = 0; i < out_count; i++)
		out_pids[i] = pids[i];
	return out_count;
}

/**
 * @brief Drain virtqueue while DRM cleanup proceeds
 *
 * When a DRI client (e.g., crosvm) is killed, its kernel-side DRM cleanup
 * (drm_release -> drm_gem_release -> virtio_gpu_gem_object_close ->
 * virtio_gpu_queue_ctrl_sgs) generates new virtqueue commands. If we stop
 * processing these commands before cleanup completes, both crosvm and
 * rvgpu-proxy enter unrecoverable D state:
 *
 *   crosvm:      virtio_gpu_queue_ctrl_sgs (virtqueue full, no consumer)
 *   rvgpu-proxy: synchronize_srcu via drm_dev_unplug (DRM readers remain)
 *
 * This function keeps draining the virtqueue until:
 *   1. All known killed PIDs have fully exited (/proc/<pid> gone)
 *   2. No external process has our DRM device open
 *   3. Virtqueue is idle for a stability period (500ms)
 *
 * The devnode scan (expensive) is throttled to once per second.
 */
static void drain_during_dri_cleanup(struct gpu_device *dev,
				     pid_t *killed_pids, int nkilled,
				     int card_num, int max_wait_ms)
{
	char devnode[32];
	const int interval_ms = 10;
	const int stable_required = 50;        /* 500ms idle = done */
	const int devnode_scan_interval = 100;  /* 1s between /proc scans */
	const int progress_interval_ms = 5000;
	/*
	 * If drain has been idle for this long while killed_pids are still
	 * alive, the SIGTERM'd client is stuck (kernel D state or blocked in
	 * a compositor round-trip). Escalate to SIGKILL: the pending drain
	 * will wake the stuck ioctl, at which point the queued SIGKILL kills
	 * the process instantly, bypassing any userspace cleanup that would
	 * block again. Chosen to be longer than the worst-case time a normal
	 * app (or crosvm guest) needs to start emitting drm_release traffic.
	 */
	const int idle_before_kill_ms = 3000;
	/*
	 * A client that keeps rendering never lets the idle timer above fire,
	 * yet it is exactly the client that must be gone before the device is
	 * removed: anything it submits afterwards is a virtio-gpu vbuf the
	 * kernel can no longer reclaim ("Objects remaining in virtio-gpu-vbufs"
	 * on cache shutdown). Bound the wait so a busy client is killed too.
	 */
	const int kill_deadline_ms = 5000;
	int elapsed_ms = 0;
	int stable_count = 0;
	int idle_since_activity_ms = 0;
	int total_drained = 0;
	int last_progress_ms = 0;
	int loops_since_scan = devnode_scan_interval; /* scan on first pass */
	bool external_users_present = false;
	bool sigkill_sent = false;

	if (card_num >= 0)
		snprintf(devnode, sizeof(devnode), "/dev/dri/card%d",
			 card_num);

	warnx("Draining virtqueue during DRM cleanup (killed=%d)...",
	      nkilled);

	while (elapsed_ms < max_wait_ms) {
		int flushed = gpu_device_drain_once(dev);

		total_drained += flushed;

		/* Lightweight check: are killed processes still in kernel? */
		bool any_alive = false;

		for (int i = 0; i < nkilled; i++) {
			if (killed_pids[i] > 0 &&
			    process_has_threads(killed_pids[i])) {
				any_alive = true;
				break;
			}
		}

		/* Heavyweight check: any external DRM users? (throttled) */
		if (!any_alive && card_num >= 0) {
			loops_since_scan++;
			if (loops_since_scan >= devnode_scan_interval) {
				loops_since_scan = 0;
				external_users_present =
					count_dri_users(devnode) > 0;
			}
			if (external_users_present)
				any_alive = true;
		}

		if (!sigkill_sent && nkilled > 0 &&
		    (idle_since_activity_ms >= idle_before_kill_ms ||
		     elapsed_ms >= kill_deadline_ms)) {
			warnx("Escalating to SIGKILL (elapsed=%dms idle=%dms)",
			      elapsed_ms, idle_since_activity_ms);
			for (int i = 0; i < nkilled; i++) {
				if (killed_pids[i] > 0 &&
				    process_has_threads(killed_pids[i])) {
					warnx("  SIGKILL pid %d",
					      killed_pids[i]);
					kill(killed_pids[i], SIGKILL);
				}
			}
			sigkill_sent = true;
			/* give drm_release time to emit its cleanup */
			idle_since_activity_ms = 0;
		}

		if (flushed > 0) {
			idle_since_activity_ms = 0;
			stable_count = 0;
		} else if (any_alive) {
			idle_since_activity_ms += interval_ms;
			stable_count = 0;
		} else {
			stable_count++;
			if (stable_count >= stable_required)
				break;
		}

		if (elapsed_ms - last_progress_ms >= progress_interval_ms) {
			warnx("  Still draining: %dms elapsed, %d drained, "
			      "processes_alive=%s",
			      elapsed_ms, total_drained,
			      any_alive ? "yes" : "no");
			last_progress_ms = elapsed_ms;
		}

		usleep(interval_ms * 1000);
		elapsed_ms += interval_ms;
	}

	if (total_drained > 0)
		warnx("Drained %d requests during DRI cleanup", total_drained);

	if (elapsed_ms >= max_wait_ms)
		warnx("drain_during_dri_cleanup: timeout after %dms",
		      max_wait_ms);
	else
		warnx("DRI cleanup drain complete in %dms", elapsed_ms);
}

static void usage(void)
{
	static const char program_name[] = "rvgpu-proxy";

	info("Usage: %s [options]\n", program_name);
	info("\t-c capset\tspecify capset file (default: %s)\n", CAPSET_PATH);
	info("\t-s scanout\tspecify scanout in form WxH@X,Y (default: %ux%u@0,0)\n",
	     DEFAULT_WIDTH, DEFAULT_HEIGHT);
	info("\t-f rate\t\tspecify virtual framerate (default: disabled)\n");
	info("\t-i id\tspecify rvgpu surface id\n");
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
		.rvgpu_surface_id = "no",
	};

	pthread_t input_thread;
	FILE *oomFile;
	int lo_fd, epoll_fd, opt, capset = -1;
	char *ip, *port, *errstr = NULL;

	while ((opt = getopt(argc, argv, "hi:n:M:c:R:f:s:")) != -1) {
		switch (opt) {
		case 'c':
			capset = open(optarg, O_RDONLY);
			if (capset == -1)
				err(1, "open %s", optarg);
			break;
		case 'i':
			servers.rvgpu_surface_id = optarg;
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

	/*
	 * Serialize vendor_id assignment across proxy instances.
	 * next_drm_card_mex() (called inside gpu_device_init) scans
	 * existing virtual DRM cards to pick the next vendor_id.
	 * Without a lock, simultaneous proxies all see 0 cards and
	 * pick the same vendor_id, making find_drm_card_by_vendor()
	 * return the same card for all of them.  Hold the lock until
	 * wait_for_drm_card() confirms our card is visible in udev.
	 */
#define VENDOR_LOCK_PATH "/tmp/.rvgpu-vendor-lock"
	int vendor_lock_fd = open(VENDOR_LOCK_PATH,
				  O_CREAT | O_RDWR | O_CLOEXEC, 0666);
	if (vendor_lock_fd < 0)
		err(1, "open %s", VENDOR_LOCK_PATH);
	if (flock(vendor_lock_fd, LOCK_EX) < 0)
		err(1, "flock %s", VENDOR_LOCK_PATH);

	dev = gpu_device_init(lo_fd, epoll_fd, capset, &params, rvgpu_be);
	if (!dev)
		err(1, "gpu device init");

	int drm_card_num = wait_for_drm_card(dev, epoll_fd);

	flock(vendor_lock_fd, LOCK_UN);
	close(vendor_lock_fd);

	inpdev = input_device_init(rvgpu_be);
	if (!inpdev)
		err(1, "input device init");
	if (pthread_create(&input_thread, NULL, input_thread_func, inpdev) !=
	    0) {
		err(1, "input thread create");
	}

	/* Setup signal handlers for graceful shutdown */
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = shutdown_signal_handler;
	sa.sa_flags = 0; /* Do not use SA_RESTART so epoll_wait gets interrupted */
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGTERM, &sa, NULL) < 0)
		warn("Failed to set SIGTERM handler");
	if (sigaction(SIGINT, &sa, NULL) < 0)
		warn("Failed to set SIGINT handler");

	/* do the main_cycle */
	while (!g_shutdown_requested) {
		int i, n;
		struct epoll_event events[8];

		/* Use timeout to periodically check shutdown flag */
		n = epoll_wait(epoll_fd, events, ARRAY_SIZE(events), 500);
		if (n < 0) {
			if (errno == EINTR) {
				/* Interrupted by signal, check shutdown flag */
				continue;
			}
			warn("epoll_wait error");
			break;
		}

		for (i = 0; i < n; i++) {
			switch (events[i].data.u32) {
			case PROXY_GPU_CONFIG:
				gpu_device_config(dev);
				break;
			case PROXY_GPU_QUEUES:
				gpu_device_serve(dev);
				break;
			default:
				warnx("Unknown event: %u",
				      events[i].data.u32);
				break;
			}
		}
	}

	warnx("Shutdown requested, cleaning up...");

	/*
	 * Brief drain before killing DRI users.
	 * Apps may be stuck in kernel virtio-gpu ioctls (D state)
	 * waiting for virtqueue responses.  Draining a few requests
	 * unblocks those ioctls so the apps can receive SIGTERM.
	 */
	{
		int pre_drained = 0;

		for (int i = 0; i < 50; i++) {
			int n = gpu_device_drain_once(dev);

			pre_drained += n;
			if (n == 0)
				break;
		}
		if (pre_drained > 0)
			warnx("Pre-drain: flushed %d pending requests",
			      pre_drained);
	}

	/*
	 * Give the DRI users a chance to leave of their own accord before
	 * asking them to. Draining is what unblocks the ioctls they are
	 * sleeping in, and a client that is already shutting down closes the
	 * device once it wakes. Only what is still holding it after the grace
	 * period is signalled.
	 */
	{
		pid_t killed_pids[64];
		int nkilled;
		const int dri_grace_ms = 3000;
		const int dri_cleanup_ms = 30000;

		drain_during_dri_cleanup(dev, NULL, 0, drm_card_num,
					 dri_grace_ms);

		nkilled = terminate_dri_users(drm_card_num, killed_pids, 64);

		/*
		 * Drain virtqueue while DRM cleanup proceeds.
		 * Always runs regardless of nkilled: crosvm may have been
		 * killed externally before our shutdown, and its kernel DRM
		 * cleanup threads may still be generating virtqueue commands.
		 */
		drain_during_dri_cleanup(dev, killed_pids, nkilled,
					 drm_card_num, dri_cleanup_ms);
	}

	/*
	 * Flush pending virtqueue requests with error responses.
	 * This allows the kernel virtio-gpu driver to free its vbufs
	 * before we close the virtio-lo device.
	 */
	gpu_device_flush_pending(dev);

	/*
	 * Stop input thread BEFORE gpu_device_free().
	 * input_thread uses librvgpu.so code, so it must be stopped
	 * before dlclose() is called in gpu_device_free().
	 */
	pthread_cancel(input_thread);
	pthread_join(input_thread, NULL);

	/* Now safe to free gpu device (includes dlclose) */
	gpu_device_free(dev);

	input_device_free(inpdev);

	close(epoll_fd);
	close(lo_fd);
	close(capset);

	return EXIT_SUCCESS;
}
