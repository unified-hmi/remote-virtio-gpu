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
#include <limits.h>
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
#include <sys/syscall.h>
#include <sys/sysmacros.h>

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

/** One DRM minor of a card: its path, and the device it actually is. */
struct drm_node {
	char path[64];
	dev_t rdev;
};

/** A process found holding one of those nodes. */
struct dri_user {
	pid_t pid;
	int fd;			/* pidfd, or -1 where the kernel has none */
	char cmd[128];
};

static int pidfd_open_compat(pid_t pid)
{
#ifdef SYS_pidfd_open
	return (int)syscall(SYS_pidfd_open, pid, 0);
#else
	(void)pid;
	errno = ENOSYS;
	return -1;
#endif
}

static int pidfd_signal_compat(int pidfd, int sig)
{
#ifdef SYS_pidfd_send_signal
	return (int)syscall(SYS_pidfd_send_signal, pidfd, sig, NULL, 0);
#else
	(void)pidfd;
	(void)sig;
	errno = ENOSYS;
	return -1;
#endif
}

/**
 * @brief Read a process command line into buf, blanks between arguments
 * @param pid - process ID to read
 * @param buf - output buffer, empty string if the process is already gone
 * @param len - size of buf
 */
static void read_proc_cmdline(pid_t pid, char *buf, size_t len)
{
	char path[64];
	int fd;
	ssize_t n;

	buf[0] = '\0';
	snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return;

	n = read(fd, buf, len - 1);
	close(fd);
	if (n <= 0) {
		buf[0] = '\0';
		return;
	}

	buf[n] = '\0';
	for (ssize_t i = 0; i < n - 1; i++) {
		if (buf[i] == '\0')
			buf[i] = ' ';
	}
}

/**
 * @brief Find PIDs of processes using the specified device and store them
 * @param rdev - device number of the target node
 * @param my_pid - our own pid to exclude
 * @param pids - array to store PIDs (caller must provide)
 * @param max_pids - maximum number of PIDs to store
 * @return number of processes found
 */
static int find_dri_user_pids(dev_t rdev, pid_t my_pid,
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
		snprintf(fd_dir_path, sizeof(fd_dir_path), "/proc/%d/fd", pid);

		DIR *fd_dir = opendir(fd_dir_path);
		if (!fd_dir)
			continue;

		struct dirent *fd_entry;
		while ((fd_entry = readdir(fd_dir)) != NULL) {
			char fd_path[PATH_MAX];
			struct stat fd_stat;

			if (fd_entry->d_name[0] == '.')
				continue;

			snprintf(fd_path, sizeof(fd_path), "%s/%s", fd_dir_path,
				 fd_entry->d_name);
			/*
			 * st_rdev is the device; st_dev and st_ino name only one
			 * directory entry of it, and so miss an fd opened through
			 * by-path, through a bind mount, or before the node was
			 * last recreated.
			 */
			if (stat(fd_path, &fd_stat) == 0 &&
			    S_ISCHR(fd_stat.st_mode) &&
			    fd_stat.st_rdev == rdev) {
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
 * @brief Check if a process still has threads (via /proc/<pid>)
 * @param pid - process ID to check
 * @return true if /proc/<pid> still exists
 */
static bool process_has_threads(pid_t pid)
{
	char path[64];

	snprintf(path, sizeof(path), "/proc/%d", pid);
	return access(path, F_OK) == 0;
}

/**
 * @brief Check whether a process we found earlier has exited
 * @param u - the process, holding a pidfd where one could be opened
 * @return true once it is gone
 */
static bool dri_user_gone(const struct dri_user *u)
{
	struct pollfd pfd = { .fd = u->fd, .events = POLLIN };

	if (u->fd < 0)
		return !process_exists(u->pid);

	return poll(&pfd, 1, 0) > 0;
}

/**
 * @brief Wait for a list of processes to terminate
 * @param users - array of processes
 * @param count - number of entries in users
 * @param timeout_ms - maximum time to wait in milliseconds
 * @return number of processes still running after timeout
 */
static int wait_for_dri_users(struct dri_user *users, int count, int timeout_ms)
{
	const int poll_interval_ms = 50;
	int elapsed = 0;
	int remaining;

	for (;;) {
		remaining = 0;
		for (int i = 0; i < count; i++) {
			if (!dri_user_gone(&users[i]))
				remaining++;
		}
		if (remaining == 0 || elapsed >= timeout_ms)
			return remaining;

		usleep(poll_interval_ms * 1000);
		elapsed += poll_interval_ms;
	}
}

/**
 * @brief List every DRM node of a card, the card node first
 *
 * Reads the minors the kernel itself lists under the device. This is sysfs,
 * not udev, so it needs no daemon and no libudev; and it is keyed on the
 * device number rather than the card name, so a renamed or renumbered node
 * does not throw it off. There is deliberately no fallback that derives the
 * render minor from the card minor: those two numbering spaces are unrelated,
 * and on a machine whose first free card minor is 0 the arithmetic lands on
 * renderD128, which belongs to whichever GPU came up first.
 *
 * @param card_num - DRM card number
 * @param nodes - output array (caller provides)
 * @param max_nodes - size of nodes array
 * @return number of nodes found, 0 if the card is already gone
 */
static int find_drm_nodes(int card_num, struct drm_node *nodes, int max_nodes)
{
	char sys_path[128];
	struct stat st;
	DIR *dp;
	struct dirent *de;
	int count;

	if (card_num < 0 || max_nodes < 1)
		return 0;

	snprintf(nodes[0].path, sizeof(nodes[0].path), "/dev/dri/card%d",
		 card_num);
	if (stat(nodes[0].path, &st) < 0 || !S_ISCHR(st.st_mode))
		return 0;

	nodes[0].rdev = st.st_rdev;
	count = 1;

	snprintf(sys_path, sizeof(sys_path), "/sys/dev/char/%u:%u/device/drm",
		 major(st.st_rdev), minor(st.st_rdev));
	dp = opendir(sys_path);
	if (!dp)
		return count;

	while ((de = readdir(dp)) != NULL && count < max_nodes) {
		struct stat node_st;

		if (de->d_name[0] == '.')
			continue;

		snprintf(nodes[count].path, sizeof(nodes[count].path),
			 "/dev/dri/%.31s", de->d_name);
		if (stat(nodes[count].path, &node_st) < 0 ||
		    !S_ISCHR(node_st.st_mode) ||
		    node_st.st_rdev == nodes[0].rdev)
			continue;

		nodes[count].rdev = node_st.st_rdev;
		count++;
	}
	closedir(dp);

	return count;
}

/**
 * @brief Count DRI users across card and all render nodes
 * @param card_num - DRM card number
 * @return total number of external processes using any DRI node
 */
static int count_all_dri_users(int card_num)
{
	struct drm_node nodes[8];
	pid_t pids[64];
	pid_t my_pid = getpid();
	int nnodes = find_drm_nodes(card_num, nodes, (int)ARRAY_SIZE(nodes));
	int total = 0;

	for (int i = 0; i < nnodes; i++)
		total += find_dri_user_pids(nodes[i].rdev, my_pid, pids,
					    (int)ARRAY_SIZE(pids));

	return total;
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
	struct drm_node nodes[8];
	struct dri_user users[64];
	pid_t my_pid = getpid();
	int nnodes, count = 0, remaining, out_count;
	const int sigterm_timeout_ms = 5000;

	if (card_num < 0) {
		warnx("DRM card number unknown, skipping DRI cleanup");
		return 0;
	}

	nnodes = find_drm_nodes(card_num, nodes, (int)ARRAY_SIZE(nodes));
	if (nnodes == 0) {
		warnx("DRI device for card%d not found", card_num);
		return 0;
	}

	for (int ni = 0; ni < nnodes && count < (int)ARRAY_SIZE(users); ni++) {
		pid_t npids[64];
		int n = find_dri_user_pids(nodes[ni].rdev, my_pid, npids,
					   (int)ARRAY_SIZE(npids));

		if (n == (int)ARRAY_SIZE(npids))
			warnx("More than %d users of %s, ignoring the rest",
			      n, nodes[ni].path);

		for (int j = 0; j < n && count < (int)ARRAY_SIZE(users); j++) {
			bool dup = false;

			for (int k = 0; k < count; k++) {
				if (users[k].pid == npids[j]) {
					dup = true;
					break;
				}
			}
			if (dup)
				continue;

			users[count].pid = npids[j];
			/*
			 * Taken now, while this pid is still known to be the
			 * process we found, so that one recycled before the
			 * signal goes out cannot be hit in its place.
			 */
			users[count].fd = pidfd_open_compat(npids[j]);
			read_proc_cmdline(npids[j], users[count].cmd,
					  sizeof(users[count].cmd));
			warnx("  %s held by pid %d (%s)", nodes[ni].path,
			      users[count].pid,
			      users[count].cmd[0] ? users[count].cmd : "gone");
			count++;
		}
	}

	if (count == 0)
		return 0;

	warnx("Asking %d process(es) to release card%d...", count, card_num);
	for (int i = 0; i < count; i++) {
		if (users[i].fd < 0 ||
		    pidfd_signal_compat(users[i].fd, SIGTERM) < 0)
			kill(users[i].pid, SIGTERM);
	}

	remaining = wait_for_dri_users(users, count, sigterm_timeout_ms);
	if (remaining == 0)
		warnx("All DRI users released the device");
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

	for (int i = 0; i < count; i++) {
		if (users[i].fd >= 0)
			close(users[i].fd);
	}

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
		int drm_fd = open(nodes[0].path, O_RDWR | O_CLOEXEC);

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
		out_pids[i] = users[i].pid;
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
					count_all_dri_users(card_num) > 0;
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
	if (rvgpu_be == NULL)
		errx(1, "failed to init rvgpu backend");

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
	 * period is signalled, so a process that merely opened the node in
	 * passing is never disturbed.
	 *
	 * The drain also runs when our card was never found: the device
	 * removal still blocks on clients we failed to identify.
	 */
	{
		pid_t killed_pids[64];
		int nkilled = 0;
		const int dri_grace_ms = 3000;
		const int dri_cleanup_ms = 30000;

		drain_during_dri_cleanup(dev, NULL, 0, drm_card_num,
					 dri_grace_ms);

		if (count_all_dri_users(drm_card_num) > 0)
			nkilled = terminate_dri_users(drm_card_num, killed_pids,
						      64);

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
