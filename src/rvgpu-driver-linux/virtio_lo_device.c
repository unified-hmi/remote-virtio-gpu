// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022  Panasonic Automotive Systems, Co., Ltd.
 */

#include "virtio_lo_device.h"
#include <linux/atomic.h>
#include <linux/eventfd.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_lo.h>

static void virtio_lo_add_pdev(struct work_struct *work);

void vl_device_parent_release(struct device *dev)
{
}

static struct device vl_device_parent = {
	.init_name = "virtio-lo-bus",
	.release = vl_device_parent_release,
};

static atomic_t vilo_device_id;
static struct workqueue_struct *vilo_wq;

/* Structure allocated on each open call to handle all virtual devices
 * provided by userspace program */
struct virtio_lo_owner {
	atomic_t lastidx;
	spinlock_t lock;
	struct list_head devlist;
};

/* should be called with owner->lock held */
static struct virtio_lo_device *
virtio_owner_getdev_unlocked(struct virtio_lo_owner *owner, unsigned idx)
{
	struct virtio_lo_device *ret = NULL, *dev;
	list_for_each_entry (dev, &owner->devlist, devlist) {
		if (dev->idx == idx) {
			ret = dev;
			break;
		}
	}
	return ret;
}
static inline struct virtio_lo_device *
virtio_owner_getdev(struct virtio_lo_owner *owner, unsigned idx)
{
	unsigned long flags;
	struct virtio_lo_device *ret;

	spin_lock_irqsave(&owner->lock, flags);
	ret = virtio_owner_getdev_unlocked(owner, idx);
	spin_unlock_irqrestore(&owner->lock, flags);
	return ret;
}

static int virtio_lo_misc_device_open(struct inode *inode, struct file *file)
{
	struct virtio_lo_owner *owner;

	owner = kmalloc(sizeof(*owner), GFP_KERNEL);
	spin_lock_init(&owner->lock);
	INIT_LIST_HEAD(&owner->devlist);
	file->private_data = owner;
	return 0;
}

static void virtio_lo_device_release(struct virtio_lo_device *dev)
{
	unsigned long i;

	dev->status = 0;
	dev->device_features = 0;

	platform_device_unregister(dev->pdev);

	kfree(dev->config);
	dev->config = NULL;

	if (dev->config_kick) {
		eventfd_ctx_put(dev->config_kick);
	}

	for (i = 0; i < dev->nqueues; i++) {
		if (dev->queues[i].device_kick) {
			eventfd_ctx_put(dev->queues[i].device_kick);
		}
	}
	kfree(dev->queues);
	kfree(dev);
	dev_notice(&vl_device_parent, "device released\n");
}

static int virtio_lo_misc_device_release(struct inode *inode, struct file *file)
{
	if (file->private_data) {
		struct virtio_lo_owner *owner = file->private_data;
		unsigned long flags;
		spin_lock_irqsave(&owner->lock, flags);
		while (!list_empty(&owner->devlist)) {
			struct virtio_lo_device *dev =
				list_first_entry(&owner->devlist,
						 struct virtio_lo_device,
						 devlist);
			list_del(&dev->devlist);
			spin_unlock_irqrestore(&owner->lock, flags);
			virtio_lo_device_release(dev);
			spin_lock_irqsave(&owner->lock, flags);
		}
		spin_unlock_irqrestore(&owner->lock, flags);
		kfree(owner);
	}
	dev_notice(&vl_device_parent, "misc device released\n");
	return 0;
}

static int virtio_lo_misc_device_mmap(struct file *file,
				      struct vm_area_struct *vma)
{
	/* Remap-pfn-range will mark the range VM_IO */
	if (remap_pfn_range(vma, vma->vm_start, vma->vm_pgoff,
			    vma->vm_end - vma->vm_start, vma->vm_page_prot)) {
		return -EAGAIN;
	}
	return 0;
}

static void virtio_lo_add_pdev(struct work_struct *work)
{
	struct virtio_lo_device *dev =
		container_of(work, struct virtio_lo_device, init_work);
	unsigned id = atomic_fetch_add(1, &vilo_device_id);
	dev->pdev = platform_device_register_data(
		&vl_device_parent, "virtio-lo", id, &dev, sizeof(dev));
	complete_all(&dev->init_done);
}

static long vilo_ioctl_adddev(struct virtio_lo_owner *owner,
			      struct virtio_lo_devinfo __user *info)
{
	struct virtio_lo_devinfo di;
	struct virtio_lo_device *dev;
	struct virtio_lo_qinfo *qi;
	unsigned i;
	long ret = 0;
	unsigned long flags;

	if (copy_from_user(&di, info, sizeof(di))) {
		return -EFAULT;
	}

	dev = kcalloc(1, sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		return -ENOMEM;
	}

	spin_lock_init(&dev->config_lock);
	spin_lock_init(&dev->status_lock);

	dev->device_id = di.device_id;
	dev->vendor_id = di.vendor_id;
	dev->card_index = di.card_index;
	dev->nqueues = di.nqueues;
	dev->features = dev->device_features = di.features;

	dev->config_size = di.config_size;
	dev->config = kmalloc(dev->config_size, GFP_KERNEL);
	if (!dev->config) {
		ret = -ENOMEM;
		goto err_dev;
	}
	dev->config_kick = eventfd_ctx_fdget(di.config_kick);

	if (copy_from_user(dev->config, di.config, di.config_size)) {
		ret = -EFAULT;
		goto err_conf;
	}

	qi = kcalloc(dev->nqueues, sizeof(*qi), GFP_KERNEL);
	if (!qi) {
		ret = -ENOMEM;
		goto err_conf;
	}
	if (copy_from_user(qi, di.qinfo, dev->nqueues * sizeof(*qi))) {
		ret = -EFAULT;
		goto err_qi;
	}
	dev->queues = kcalloc(dev->nqueues, sizeof(*dev->queues), GFP_KERNEL);
	if (!dev->queues) {
		ret = -ENOMEM;
		goto err_qi;
	}

	for (i = 0; i < dev->nqueues; i++) {
		dev->queues[i].maxsize = qi[i].size;
		if (qi[i].kickfd != -1) {
			dev->queues[i].device_kick =
				eventfd_ctx_fdget(qi[i].kickfd);
		}
	}

	dev->idx = atomic_fetch_add(1, &owner->lastidx);

	/* everything is OK, create driver part platform device */
	init_completion(&dev->init_done);

	INIT_WORK(&dev->init_work, virtio_lo_add_pdev);

	queue_work(vilo_wq, &dev->init_work);

	wait_for_completion(&dev->init_done);

	if (!(dev->status & VIRTIO_CONFIG_S_DRIVER_OK)) {
		dev_notice(&vl_device_parent,
			   "virtio lo device initialization failed\n");
		ret = -ENOENT;
		goto err_queues;
	}

	for (i = 0; i < dev->nqueues; i++) {
		qi[i].size = dev->queues[i].size;
		qi[i].desc = dev->queues[i].desc;
		qi[i].avail = dev->queues[i].avail;
		qi[i].used = dev->queues[i].used;
	}
	if (copy_to_user(di.qinfo, qi, dev->nqueues * sizeof(*qi))) {
		ret = -EFAULT;
	}
	if (copy_to_user(&info->idx, &dev->idx, sizeof(dev->idx))) {
		ret = -EFAULT;
	}
	if (copy_to_user(&info->features, &dev->features,
			 sizeof(dev->features))) {
		ret = -EFAULT;
	}
	if (copy_to_user(di.config, dev->config, dev->config_size)) {
		ret = -EFAULT;
	}
	spin_lock_irqsave(&owner->lock, flags);
	list_add(&dev->devlist, &owner->devlist);
	spin_unlock_irqrestore(&owner->lock, flags);

	kfree(qi);
	return ret;
err_queues:
	kfree(dev->queues);
err_qi:
	kfree(qi);
err_conf:
	kfree(dev->config);
err_dev:
	kfree(dev);
	return ret;
}

static long vilo_ioctl_deldev(struct virtio_lo_owner *owner, unsigned idx)
{
	unsigned long flags;
	long ret = -ENOENT;
	struct virtio_lo_device *dev;

	spin_lock_irqsave(&owner->lock, flags);

	dev = virtio_owner_getdev_unlocked(owner, idx);
	if (dev) {
		list_del(&dev->devlist);
		virtio_lo_device_release(dev);
		ret = 0;
	}
	spin_unlock_irqrestore(&owner->lock, flags);
	return ret;
}

static long vilo_ioctl_getconf(struct virtio_lo_owner *owner,
			       struct virtio_lo_config __user *conf)
{
	struct virtio_lo_config c;
	struct virtio_lo_device *dev;
	void *mem;
	long ret = 0;
	if (copy_from_user(&c, conf, sizeof(c)))
		return -EFAULT;
	dev = virtio_owner_getdev(owner, c.idx);
	if (!dev) {
		return -ENOENT;
	}
	if (c.offset >= dev->config_size ||
	    c.offset + c.len > dev->config_size) {
		return -EINVAL;
	}
	mem = kmalloc(c.len, GFP_KERNEL);
	if (!mem) {
		return -ENOMEM;
	}
	virtio_lo_config_get(dev, c.offset, mem, c.len);
	if (copy_to_user(c.config, mem, c.len)) {
		ret = -EFAULT;
	}
	kfree(mem);

	return ret;
}

static long vilo_ioctl_setconf(struct virtio_lo_owner *owner,
			       const struct virtio_lo_config __user *conf)
{
	struct virtio_lo_config c;
	struct virtio_lo_device *dev;
	void *mem;
	long ret;
	if (copy_from_user(&c, conf, sizeof(c)))
		return -EFAULT;
	dev = virtio_owner_getdev(owner, c.idx);
	if (!dev) {
		return -ENOENT;
	}
	if (c.offset >= dev->config_size ||
	    c.offset + c.len > dev->config_size) {
		return -EINVAL;
	}

	mem = kmalloc(c.len, GFP_KERNEL);
	if (!mem) {
		return -ENOMEM;
	}
	if (copy_from_user(mem, c.config, c.len)) {
		ret = -EFAULT;
	} else {
		virtio_lo_config_set(dev, c.offset, mem, c.len);
		virtio_lo_config_driver(dev->pdev);
		ret = 0;
	}
	kfree(mem);
	return ret;
}

static long vilo_ioctl_kick(struct virtio_lo_owner *owner,
			    const struct virtio_lo_kick __user *kick)
{
	struct virtio_lo_kick k;
	struct virtio_lo_device *dev;
	if (copy_from_user(&k, kick, sizeof(k)))
		return -EFAULT;
	dev = virtio_owner_getdev(owner, k.idx);
	if (!dev) {
		return -ENOENT;
	}
	if (k.qidx >= dev->nqueues) {
		return -EINVAL;
	}
	virtio_lo_kick_driver(dev->pdev, k.qidx);

	return 0;
}

void virtio_lo_kick_device(struct virtio_lo_device *dev, int qidx)
{
	if (qidx >= 0 && qidx < dev->nqueues) {
		if (dev->queues[qidx].device_kick) {
			eventfd_signal(dev->queues[qidx].device_kick, 1);
		}
	} else {
		unsigned i;
		for (i = 0; i < dev->nqueues; i++) {
			if (dev->queues[i].device_kick) {
				eventfd_signal(dev->queues[i].device_kick, 1);
			}
		}
	}
}

void virtio_lo_set_queue(struct virtio_lo_device *dev, unsigned qidx, u32 size,
			 u64 desc, u64 avail, u64 used)
{
	struct virtio_lo_vq_info *info = &dev->queues[qidx];
	dev_notice(&vl_device_parent,
		   "setting queue addr %u size %u\n"
		   "\tdesc  %016llx\n"
		   "\tavail %016llx\n"
		   "\tused  %016llx\n",
		   qidx, size, desc, avail, used);
	info->size = size;
	info->desc = desc;
	info->avail = avail;
	info->used = used;
}

void virtio_lo_config_device(struct virtio_lo_device *dev)
{
	if (dev->config_kick) {
		eventfd_signal(dev->config_kick, 1);
	}
}

void virtio_lo_config_get(struct virtio_lo_device *dev, unsigned offset,
			  void *buf, unsigned len)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->config_lock, flags);
	memcpy(buf, dev->config + offset, len);
	spin_unlock_irqrestore(&dev->config_lock, flags);
}

unsigned virtio_lo_config_generation(struct virtio_lo_device *dev)
{
	unsigned long flags;
	unsigned ret;
	spin_lock_irqsave(&dev->config_lock, flags);
	ret = dev->generation;
	spin_unlock_irqrestore(&dev->config_lock, flags);
	return ret;
}

void virtio_lo_config_set(struct virtio_lo_device *dev, unsigned offset,
			  const void *buf, unsigned len)
{
	unsigned long flags;
	spin_lock_irqsave(&dev->config_lock, flags);
	memcpy(dev->config + offset, buf, len);
	dev->generation++;
	spin_unlock_irqrestore(&dev->config_lock, flags);
}

static long virtio_lo_misc_device_ioctl(struct file *file, unsigned int cmd,
					unsigned long arg)
{
	long ret;
	struct virtio_lo_owner *owner =
		(struct virtio_lo_owner *)file->private_data;
	void __user *argp = (void __user *)arg;

	if (!owner) {
		return -ENOTTY;
	}

	if (_IOC_TYPE(cmd) != VIRTIO_LOIO) {
		return -ENOTTY;
	}

	switch (cmd) {
	case VIRTIO_LO_ADDDEV:
		ret = vilo_ioctl_adddev(owner, argp);
		break;
	case VIRTIO_LO_DELDEV:
		ret = vilo_ioctl_deldev(owner, arg);
		break;
	case VIRTIO_LO_GCONF:
		ret = vilo_ioctl_getconf(owner, argp);
		break;
	case VIRTIO_LO_SCONF:
		ret = vilo_ioctl_setconf(owner, argp);
		break;
	case VIRTIO_LO_KICK:
		ret = vilo_ioctl_kick(owner, argp);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

static struct file_operations virtio_lo_misc_device_fops = {
	.owner = THIS_MODULE,
	.open = virtio_lo_misc_device_open,
	.unlocked_ioctl = virtio_lo_misc_device_ioctl,
	.mmap = virtio_lo_misc_device_mmap,
	.release = virtio_lo_misc_device_release
};

static struct miscdevice virtio_lo_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "virtio-lo",
	.fops = &virtio_lo_misc_device_fops
};

int __init virtio_lo_device_init(void)
{
	vilo_wq = create_singlethread_workqueue("virtio-lo");
	if (!vilo_wq) {
		return -ENOMEM;
	}
	if (device_register(&vl_device_parent)) {
		destroy_workqueue(vilo_wq);
	}

	return misc_register(&virtio_lo_misc_device);
}

void __exit virtio_lo_device_exit(void)
{
	flush_workqueue(vilo_wq);
	destroy_workqueue(vilo_wq);
	device_unregister(&vl_device_parent);
	misc_deregister(&virtio_lo_misc_device);
}
