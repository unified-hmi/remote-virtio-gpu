// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
 */

#include <linux/completion.h>
#include <linux/eventfd.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>

#include "virtio_lo_device.h"

/* The alignment to use between consumer and producer parts of vring.
 * Currently hardcoded to the page size. */
#define VIRTIO_LO_VRING_ALIGN PAGE_SIZE

#define to_virtio_lo_driver(_virt_dev)                                         \
	container_of(_virt_dev, struct virtio_lo_driver, vdev)

#define to_virtio_lo_device(_virt_dev) (to_virtio_lo_driver(_virt_dev)->device)

struct virtio_lo_driver {
	struct virtio_device vdev;
	struct platform_device *pdev;

	struct virtio_lo_device *device;

	/* Array of queues */
	struct virtqueue **queues;
};

/* Configuration interface */

static u64 vl_get_features(struct virtio_device *vdev)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);

	dev_notice(&vdev->dev, "get features %llx", vl_dev->features);

	return vl_dev->features;
}

static int vl_finalize_features(struct virtio_device *vdev)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);
	vring_transport_features(vdev);
	vl_dev->features = vdev->features;
	dev_notice(&vdev->dev, "finalize features %llx", vl_dev->features);
	return 0;
}

static void vl_get(struct virtio_device *vdev, unsigned offset, void *buf,
		   unsigned len)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);
	dev_notice(&vdev->dev, "get config");

	if (offset >= vl_dev->config_size ||
	    offset + len > vl_dev->config_size) {
		dev_err(&vdev->dev, "reading beyond config space");
		memset(buf, 0, len);
	} else {
		virtio_lo_config_get(vl_dev, offset, buf, len);
	}
}

static void vl_set(struct virtio_device *vdev, unsigned offset, const void *buf,
		   unsigned len)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);
	dev_notice(&vdev->dev, "set config");

	if (offset >= vl_dev->config_size ||
	    offset + len > vl_dev->config_size) {
		dev_err(&vdev->dev, "writing beyond config space");
	} else {
		virtio_lo_config_set(vl_dev, offset, buf, len);
		virtio_lo_config_device(vl_dev);
	}
}

static u32 vl_generation(struct virtio_device *vdev)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);
	u32 ret;

	ret = (u32)virtio_lo_config_generation(vl_dev);
	dev_notice(&vdev->dev, "generation %d", ret);
	return ret;
}

static u8 vl_get_status(struct virtio_device *vdev)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);
	u8 ret;
	unsigned long flags;

	spin_lock_irqsave(&vl_dev->status_lock, flags);
	ret = vl_dev->status;
	spin_unlock_irqrestore(&vl_dev->status_lock, flags);
	dev_notice(&vdev->dev, "get_status %x", ret);
	return ret;
}

static void vl_set_status(struct virtio_device *vdev, u8 status)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);
	unsigned long flags;

	/* We should never be setting status to 0. */
	dev_notice(&vdev->dev, "set_status %x", status);
	BUG_ON(status == 0);

	spin_lock_irqsave(&vl_dev->status_lock, flags);
	vl_dev->status = status;
	spin_unlock_irqrestore(&vl_dev->status_lock, flags);
	if (status & VIRTIO_CONFIG_S_DRIVER_OK) {
		dev_notice(&vdev->dev, "init complete");
		complete(&vl_dev->init_done);
	}
}

static void vl_reset(struct virtio_device *vdev)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);
	unsigned long flags;

	dev_notice(&vdev->dev, "reset");
	spin_lock_irqsave(&vl_dev->status_lock, flags);
	vl_dev->status = 0;
	vl_dev->features = vl_dev->device_features;
	spin_unlock_irqrestore(&vl_dev->status_lock, flags);
}

/* Transport interface */

/* the notify function used when creating a virt queue */
static bool vl_notify(struct virtqueue *vq)
{
	struct virtio_lo_driver *vl_driv = vq->priv;
	virtio_lo_kick_device(vl_driv->device, vq->index);
	return true;
}

void virtio_lo_kick_driver(struct platform_device *pdev, int qidx)
{
	struct virtio_lo_driver *vl_driv;

	vl_driv = platform_get_drvdata(pdev);
	if (qidx >= 0) {
		vring_interrupt(0, vl_driv->queues[qidx]);
	} else {
		struct virtio_lo_device *vl_dev = vl_driv->device;
		unsigned i;
		for (i = 0; i < vl_dev->nqueues; i++) {
			vring_interrupt(0, vl_driv->queues[i]);
		}
	}
}

void virtio_lo_config_driver(struct platform_device *pdev)
{
	struct virtio_lo_driver *vl_driv = platform_get_drvdata(pdev);
	virtio_config_changed(&vl_driv->vdev);
}

static void vl_del_vqs(struct virtio_device *vdev)
{
	struct virtio_lo_driver *vl_driver = to_virtio_lo_driver(vdev);
	struct virtio_lo_device *vl_dev = vl_driver->device;
	unsigned i;

	dev_notice(&vdev->dev, "deleting queues");

	for (i = 0; i < vl_dev->nqueues; i++) {
		struct virtqueue *vq = vl_driver->queues[i];
		vring_del_virtqueue(vq);
	}
}

static struct virtqueue *vl_setup_vq(struct virtio_device *vdev, unsigned index,
				     void (*callback)(struct virtqueue *vq),
				     const char *name, bool ctx)
{
	struct virtio_lo_device *vl_dev = to_virtio_lo_device(vdev);
	struct virtio_lo_vq_info *info;
	struct virtqueue *vq;

	if (!name)
		return NULL;
	if (index > vl_dev->nqueues)
		return NULL;
	info = &vl_dev->queues[index];
	dev_notice(&vdev->dev, "creating queue %d", index);

	/* Create the vring */
	vq = vring_create_virtqueue(index, info->maxsize, VIRTIO_LO_VRING_ALIGN,
				    vdev, true, true, ctx, vl_notify, callback,
				    name);
	if (!vq) {
		return ERR_PTR(-ENOMEM);
	}

	virtio_lo_set_queue(vl_dev, index, virtqueue_get_vring_size(vq),
			    virtqueue_get_desc_addr(vq),
			    virtqueue_get_avail_addr(vq),
			    virtqueue_get_used_addr(vq));

	vq->priv = to_virtio_lo_driver(vdev);
	return vq;
}

static int vl_find_vqs(struct virtio_device *vdev, unsigned nvqs,
		       struct virtqueue *vqs[], vq_callback_t *callbacks[],
		       const char *const names[], const bool *ctx,
		       struct irq_affinity *desc)
{
	struct virtio_lo_driver *vl_driver = to_virtio_lo_driver(vdev);
	unsigned i;

	for (i = 0; i < nvqs; ++i) {
		vqs[i] = vl_setup_vq(vdev, i, callbacks[i], names[i],
				     ctx ? ctx[i] : false);
		if (IS_ERR(vqs[i])) {
			vl_del_vqs(vdev);
			return PTR_ERR(vqs[i]);
		}
		vl_driver->queues[i] = vqs[i];
	}

	return 0;
}

static const char *vl_bus_name(struct virtio_device *vdev)
{
	struct virtio_lo_driver *vl_driver = to_virtio_lo_driver(vdev);

	return vl_driver->pdev->name;
}

static const struct virtio_config_ops virtio_lo_config_ops = {
	.get = vl_get,
	.set = vl_set,
	.generation = vl_generation,
	.get_status = vl_get_status,
	.set_status = vl_set_status,
	.reset = vl_reset,
	.find_vqs = vl_find_vqs,
	.del_vqs = vl_del_vqs,
	.get_features = vl_get_features,
	.finalize_features = vl_finalize_features,
	.bus_name = vl_bus_name,
};

static void virtio_lo_release_dev_empty(struct device *_d)
{
}

/* Platform device */

static int virtio_lo_probe(struct platform_device *pdev)
{
	struct virtio_lo_driver *vl_driv;
	struct virtio_lo_device *device;

	device = *(struct virtio_lo_device **)dev_get_platdata(&pdev->dev);
	if (!device) {
		dev_err(&pdev->dev, "no platform data");

		return -EINVAL;
	}

	vl_driv = devm_kzalloc(&pdev->dev, sizeof(*vl_driv), GFP_KERNEL);
	if (!vl_driv) {
		dev_err(&pdev->dev, "no memory");
		return -ENOMEM;
	}

	vl_driv->device = device;
	vl_driv->pdev = pdev;

#ifdef CONFIG_VIRTIO_LO_DEVICE_INDEX
	vl_driv->vdev.card_index = device->card_index;
#endif /* CONFIG_VIRTIO_LO_DEVICE_INDEX */
	vl_driv->vdev.dev.parent = &pdev->dev;
	vl_driv->vdev.dev.release = virtio_lo_release_dev_empty;
	vl_driv->vdev.config = &virtio_lo_config_ops;
	vl_driv->pdev = pdev;
	vl_driv->queues = devm_kcalloc(&pdev->dev, device->nqueues,
				       sizeof(struct virtqueue), GFP_KERNEL);
	if (!vl_driv->queues) {
		dev_err(&pdev->dev, "no memory");
		return -ENOMEM;
	}

	vl_driv->vdev.id.device = device->device_id;
	vl_driv->vdev.id.vendor = device->vendor_id;
	device->pdev = pdev;

	platform_set_drvdata(pdev, vl_driv);

	return register_virtio_device(&vl_driv->vdev);
}

static int virtio_lo_remove(struct platform_device *pdev)
{
	struct virtio_lo_driver *vl_driv = platform_get_drvdata(pdev);

	unregister_virtio_device(&vl_driv->vdev);
	return 0;
}

/* Platform driver */
static struct of_device_id virtio_lo_match[] = {
	{
		.compatible = "virtio,lo",
	},
	{},
};
MODULE_DEVICE_TABLE(of, virtio_lo_match);

static struct platform_driver virtio_lo_driver_ops = {
	.probe = virtio_lo_probe,
	.remove = virtio_lo_remove,
	.driver =
		{
			.name = "virtio-lo",
			.of_match_table = virtio_lo_match,
		},
};

static int __init virtio_lo_init(void)
{
	int err = virtio_lo_device_init();
	if (err)
		return err;
	else
		return platform_driver_register(&virtio_lo_driver_ops);
}

static void __exit virtio_lo_exit(void)
{
	virtio_lo_device_exit();
	platform_driver_unregister(&virtio_lo_driver_ops);
}

module_init(virtio_lo_init);
module_exit(virtio_lo_exit);

MODULE_DESCRIPTION("Platform bus driver for loopback virtio devices");
MODULE_LICENSE("GPL");
