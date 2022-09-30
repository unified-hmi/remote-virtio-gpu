// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
 */

#ifndef _VIRTIO_LO_DEVICE_H
#define _VIRTIO_LO_DEVICE_H

#include <linux/completion.h>
#include <linux/kref.h>
#include <linux/list.h>
#include <linux/types.h>
#include <linux/workqueue.h>

struct virtio_lo_vq_info {
	unsigned maxsize;
	unsigned size;
	u64 desc;
	u64 avail;
	u64 used;
	struct eventfd_ctx *device_kick;
};

struct virtio_lo_device {
	unsigned idx;
	u32 device_id;
	u32 vendor_id;
	int card_index;

	struct platform_device *pdev;

	u64 device_features;

	struct completion init_done;
	struct work_struct init_work;

	/* State machine */
	spinlock_t status_lock;
	u8 status;
	u64 features;

	spinlock_t config_lock;
	unsigned generation;
	unsigned config_size;
	void *config;
	struct eventfd_ctx *config_kick;

	unsigned nqueues;
	struct virtio_lo_vq_info *queues;
	struct list_head devlist;
};

/* interaction between driver and device */

/* Queue management */

/** Forward queue addresses */
void virtio_lo_set_queue(struct virtio_lo_device *dev, unsigned qidx, u32 size,
			 u64 desc, u64 avail, u64 used);
/** Queue kick device -> driver */
void virtio_lo_kick_driver(struct platform_device *pdev, int qidx);

/** Queue kick driver -> device */
void virtio_lo_kick_device(struct virtio_lo_device *dev, int qidx);

/* Config routines */
/** Config change device -> driver */
void virtio_lo_config_driver(struct platform_device *pdev);

/** Config change driver -> device */
void virtio_lo_config_device(struct virtio_lo_device *dev);

/** Reading the configuration */
void virtio_lo_config_get(struct virtio_lo_device *dev, unsigned offset,
			  void *buf, unsigned len);
/** Returns current config generation */
unsigned virtio_lo_config_generation(struct virtio_lo_device *dev);

/** Writing the configuration */
void virtio_lo_config_set(struct virtio_lo_device *dev, unsigned offset,
			  const void *buf, unsigned len);

int __init virtio_lo_device_init(void);
void __exit virtio_lo_device_exit(void);

#endif /* _VIRTIO_LO_DEVICE_H */
