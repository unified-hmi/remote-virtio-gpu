// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
 */

#ifndef _UAPI__VIRTIO_LO_H
#define _UAPI__VIRTIO_LO_H

#include <linux/ioctl.h>
#include <linux/types.h>

struct virtio_lo_qinfo {
	__s32 kickfd; /* IN */
	__u32 size; /* IN/OUT */
	__u64 desc; /* OUT */
	__u64 avail; /* OUT */
	__u64 used; /* OUT */
};

struct virtio_lo_devinfo {
	__u32 idx; /* OUT */
	__u32 device_id; /* IN */
	__u32 vendor_id; /* IN */
	__u32 nqueues; /* IN */
	__u64 features; /* IN/OUT */
	__u32 config_size; /* IN */
	__s32 config_kick; /* IN */
	__s32 card_index; /* IN */
	__u32 padding; /* IN */
	__u8 *config; /* IN/OUT */
	struct virtio_lo_qinfo *qinfo; /* IN/OUT */
};

struct virtio_lo_config {
	__u32 idx; /* IN */
	__u32 offset; /* IN */
	__u32 len; /* IN */
	__u8 *config; /* IN/OUT */
};

/* if qidx == -1, all the queues are notified */
struct virtio_lo_kick {
	__u32 idx; /* IN */
	__s32 qidx; /* IN */
};

/* ioctls for virtio_lo */
#define VIRTIO_LOIO 0x50

/* ioctl for creating virtio device */
#define VIRTIO_LO_ADDDEV _IOWR(VIRTIO_LOIO, 1, struct virtio_lo_devinfo)
#define VIRTIO_LO_DELDEV _IOW(VIRTIO_LOIO, 2, unsigned)

/* ioctls for configuration */
/* get config for device */
#define VIRTIO_LO_GCONF _IOR(VIRTIO_LOIO, 20, struct virtio_lo_config)
/* set config for device */
#define VIRTIO_LO_SCONF _IOW(VIRTIO_LOIO, 21, const struct virtio_lo_config)

/* ioctls for kicking driver */
#define VIRTIO_LO_KICK _IOW(VIRTIO_LOIO, 30, const struct virtio_lo_config)

#endif /* _UAPI__VIRTIO_LO_H */
