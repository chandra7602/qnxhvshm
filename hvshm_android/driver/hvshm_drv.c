#include <linux/bitops.h>
#include <linux/build_bug.h>
#include <linux/fs.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/io-64-nonatomic-lo-hi.h>
#include <linux/jiffies.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/property.h>
#include <linux/sizes.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "guest_shm.h"
#include "hvshm_proto.h"
#include "hvshm_uapi.h"

#define DRV_NAME             "hvshm"
#define HVSHM_DEFAULT_NAME   "qnx_android_shm"
#define HVSHM_DEFAULT_PAGES  240u
#define HVSHM_CLIENT_MASK    ((1u << GUEST_SHM_MAX_CLIENTS) - 1u)

#define FACT_REG(f)  offsetof(struct guest_shm_factory, f)
#define CTRL_REG(f)  offsetof(struct guest_shm_control, f)

struct hvshm_dev {
	struct miscdevice misc;
	void __iomem *factory;                 /* mapped for the module lifetime */
	char region_name[GUEST_SHM_MAX_NAME];
	u32 req_pages;
	int irq;

	void __iomem *ctrl;
	void *region;
	size_t region_bytes;
	u32 conn_idx;

	spinlock_t lock;                       /* protects the three masks below */
	u32 attach_list;
	u32 pend_conn;                         /* peers attached, not yet reported */
	u32 pend_disc;                         /* peers detached, not yet reported */
	wait_queue_head_t wq;

	struct mutex ring_lock;                /* serialises ring + channel state */
	struct hvshm_channel ch;
	bool bound;

	unsigned long flags;
#define HVSHM_F_OPEN 0
};

static int hvshm_status_to_errno(u32 status) {
	switch (status) {
        case GSS_NOMEM:          return -ENOMEM;
        case GSS_CLIENT_MAX:     return -EUSERS;
        case GSS_ILLEGAL_NAME:   return -EINVAL;
        case GSS_NO_PERMISSION:  return -EACCES;
        default:                 return -EIO;
	}
}

static int hvshm_attach(struct hvshm_dev *hd) {
	struct device *dev = hd->misc.parent;
	char name[GUEST_SHM_MAX_NAME] = {0}
	phys_addr_t ctrl_phys;
	u32 status, pages;
	int ret;

	memcpy(name, hd->region_name, strlen(hd->region_name));
	memcpy_toio(hd->factory + FACT_REG(name), name, sizeof(name));
	writel(hd->req_pages, hd->factory + FACT_REG(size));

	status = readl(hd->factory + FACT_REG(status));
	if (status != GSS_OK) {
		dev_err(dev, "attach '%s' failed, vdev status %u\n", hd->region_name, status);
		return hvshm_status_to_errno(status);
	}

	ctrl_phys = readq(hd->factory + FACT_REG(shmem));
	pages = readl(hd->factory + FACT_REG(size));
	if (!ctrl_phys || !pages) return -ENXIO;

	hd->ctrl = ioremap(ctrl_phys, SZ_4K);
	if (!hd->ctrl) return -ENOMEM;

	hd->conn_idx = readl(hd->ctrl + CTRL_REG(idx));
	if (hd->conn_idx >= GUEST_SHM_MAX_CLIENTS) {
		ret = -EIO;
		goto err_detach;
	}

	hd->region_bytes = (size_t)pages * SZ_4K;
	hd->region = memremap(ctrl_phys + SZ_4K, hd->region_bytes, MEMREMAP_WB);
	if (!hd->region) {
		ret = -ENOMEM;
		goto err_detach;
	}
	return 0;

err_detach:
	writel(0, hd->ctrl + CTRL_REG(detach));
	iounmap(hd->ctrl);
	hd->ctrl = NULL;
	return ret;
}

static void hvshm_detach(struct hvshm_dev *hd) {
	writel(0, hd->ctrl + CTRL_REG(detach));
	memunmap(hd->region);
	iounmap(hd->ctrl);
	hd->region = NULL;
	hd->ctrl = NULL;
	hd->bound = false;
}

static bool hvshm_sync_status(struct hvshm_dev *hd) {
	const u32 self = BIT(hd->conn_idx);
	unsigned long flags;
	u32 st, attach, notified, up, down;

	st = readl(hd->ctrl + CTRL_REG(status));
	attach = st >> GUEST_SHM_MAX_CLIENTS;
	notified = st & HVSHM_CLIENT_MASK;

	spin_lock_irqsave(&hd->lock, flags);
	up = attach & ~hd->attach_list & ~self;
	down = hd->attach_list & ~attach & ~self;
	hd->attach_list = attach;
	hd->pend_conn |= up;
	hd->pend_disc |= down;
	spin_unlock_irqrestore(&hd->lock, flags);

	return notified || up || down;
}

static irqreturn_t hvshm_irq(int irq, void *arg) {
	struct hvshm_dev *hd = arg;

	if (!hvshm_sync_status(hd)) return IRQ_NONE;
	wake_up_interruptible(&hd->wq);
	return IRQ_HANDLED;
}

static int hvshm_ensure_bound(struct hvshm_dev *hd) {
	int rc;

	if (hd->bound && hvshm_channel_alive(&hd->ch)) return 0;
	rc = hvshm_guest_attach(hd->region, hd->region_bytes, &hd->ch);
	hd->bound = (rc == 0);
	return rc;
}

static bool hvshm_rx_ready(struct hvshm_dev *hd) {
	struct hvshm_header *h = hd->region;

	if (HVSHM_LOAD_ACQ(&h->magic) != HVSHM_MAGIC) return false;
	return HVSHM_LOAD_ACQ(&h->h2g.head) != HVSHM_LOAD_RLX(&h->h2g.tail);
}

static bool hvshm_has_event(struct hvshm_dev *hd) {
	unsigned long flags;
	bool pending;

	spin_lock_irqsave(&hd->lock, flags);
	pending = hd->pend_conn || hd->pend_disc;
	spin_unlock_irqrestore(&hd->lock, flags);

	return pending || hvshm_rx_ready(hd);
}

static int hvshm_pop_message(struct hvshm_dev *hd, struct hvshm_event *ev) {
	u32 type, len;
	int rc, ret;

	mutex_lock(&hd->ring_lock);
	if (hvshm_ensure_bound(hd)) {
		ret = 0;                     /* host not up: nothing to deliver */
		goto out;
	}

	rc = hvshm_recv(&hd->ch, &type, ev->payload, sizeof(ev->payload), &len);
	if (rc == 0) {
		ev->type = HVSHM_EV_MESSAGE;
		ev->msg_type = type;
		ev->len = len;
		ret = 1;
	} else if (rc == -EAGAIN) {
		ret = 0;
	} else {
		hvshm_rx_flush(&hd->ch);     /* resync: drop what the host corrupted */
		ret = rc;
	}
out:
	mutex_unlock(&hd->ring_lock);
	return ret;
}

static int hvshm_take_event(struct hvshm_dev *hd, struct hvshm_event *ev) {
	unsigned long flags;
	u32 mask;
	int ret;

	memset(ev, 0, sizeof(*ev));
	ev->conn_idx = HVSHM_NO_CONN;

	spin_lock_irqsave(&hd->lock, flags);
	mask = hd->pend_conn;
	if (mask) {
		ev->type = HVSHM_EV_PEER_CONNECTED;
		ev->conn_idx = __ffs(mask);
		hd->pend_conn &= ~BIT(ev->conn_idx);
	}
	spin_unlock_irqrestore(&hd->lock, flags);
	if (mask) return 1;

	ret = hvshm_pop_message(hd, ev);
	if (ret) return ret;

	spin_lock_irqsave(&hd->lock, flags);
	mask = hd->pend_disc;
	if (mask) {
		ev->type = HVSHM_EV_PEER_DISCONNECTED;
		ev->conn_idx = __ffs(mask);
		hd->pend_disc &= ~BIT(ev->conn_idx);
	}
	spin_unlock_irqrestore(&hd->lock, flags);
	return mask ? 1 : 0;
}

static long hvshm_ioc_wait(struct hvshm_dev *hd, struct hvshm_wait_arg __user *uarg) {
	struct hvshm_event ev;
	u32 timeout_ms;
	long left = 0;
	int rc;

	if (get_user(timeout_ms, &uarg->timeout_ms)) return -EFAULT;
	if (timeout_ms != HVSHM_WAIT_FOREVER) left = msecs_to_jiffies(timeout_ms);

	for (;;) {
		rc = hvshm_take_event(hd, &ev);
		if (rc < 0) return rc;
		if (rc == 1) break;

		if (timeout_ms == 0) return -EAGAIN;
		if (timeout_ms == HVSHM_WAIT_FOREVER) {
			rc = wait_event_interruptible(hd->wq, hvshm_has_event(hd));
			if (rc) return rc;
		} else {
			left = wait_event_interruptible_timeout(hd->wq, hvshm_has_event(hd), left);
			if (left < 0) return left;
			if (left == 0) return -ETIMEDOUT;
		}
	}

	return copy_to_user(&uarg->ev, &ev, sizeof(ev)) ? -EFAULT : 0;
}

static long hvshm_ioc_send(struct hvshm_dev *hd, const struct hvshm_send_arg __user *uarg) {
	struct hvshm_send_arg a;
	u32 host_idx, mask;
	int rc;

	if (copy_from_user(&a, uarg, sizeof(a))) return -EFAULT;
	if (a.len > HVSHM_MSG_MAX_PAYLOAD) return -EMSGSIZE;

	mutex_lock(&hd->ring_lock);
	rc = hvshm_ensure_bound(hd);
	if (!rc) rc = hvshm_send(&hd->ch, a.msg_type, a.payload, a.len);
	host_idx = hd->bound ? hd->ch.hdr->host_idx : HVSHM_NO_CONN;
	mutex_unlock(&hd->ring_lock);
	if (rc) return rc;

	/* Wake only the host if we know its index, otherwise everybody else. */
	mask = (host_idx < GUEST_SHM_MAX_CLIENTS) ? BIT(host_idx) : ~0u;
	writel(mask, hd->ctrl + CTRL_REG(notify));
	return 0;
}

static long hvshm_ioc_info(struct hvshm_dev *hd, struct hvshm_info __user *uarg) {
	struct hvshm_info info = { 0 };
	unsigned long flags;
	u32 host_idx = HVSHM_NO_CONN;

	info.region_bytes = hd->region_bytes;
	info.conn_idx = hd->conn_idx;
	info.host_idx = HVSHM_NO_CONN;

	spin_lock_irqsave(&hd->lock, flags);
	info.attach_mask = hd->attach_list;
	spin_unlock_irqrestore(&hd->lock, flags);

	mutex_lock(&hd->ring_lock);
	if (!hvshm_ensure_bound(hd)) {
		host_idx = hd->ch.hdr->host_idx;
		info.ring_bytes = hd->ch.rx.size;
	}
	mutex_unlock(&hd->ring_lock);

	if (host_idx < GUEST_SHM_MAX_CLIENTS) {
		info.host_idx = host_idx;
		info.host_online = !!(info.attach_mask & BIT(host_idx));
	}
	return copy_to_user(uarg, &info, sizeof(info)) ? -EFAULT : 0;
}

static long hvshm_ioctl(struct file *filp, unsigned int cmd, unsigned long arg) {
	struct hvshm_dev *hd = filp->private_data;
	void __user *uarg = (void __user *)arg;

	switch (cmd) {
		case HVSHM_IOC_GET_INFO:   return hvshm_ioc_info(hd, uarg);
		case HVSHM_IOC_WAIT_EVENT: return hvshm_ioc_wait(hd, uarg);
		case HVSHM_IOC_SEND:       return hvshm_ioc_send(hd, uarg);
		default:                   return -ENOTTY;
	}
}

static __poll_t hvshm_poll(struct file *filp, poll_table *wait) {
	struct hvshm_dev *hd = filp->private_data;

	poll_wait(filp, &hd->wq, wait);
	return hvshm_has_event(hd) ? (EPOLLIN | EPOLLRDNORM) : 0;
}

static int hvshm_open(struct inode *inode, struct file *filp) {
	struct hvshm_dev *hd = container_of(filp->private_data, struct hvshm_dev, misc);
	int ret;

	if (test_and_set_bit(HVSHM_F_OPEN, &hd->flags)) return -EBUSY;                 /* one owner of the protocol */

	ret = hvshm_attach(hd);
	if (ret) goto err_clear;

	/*
	 * Start from "only we are attached"; the first sync below reports every
	 * peer that is already connected as a synthetic PEER_CONNECTED event.
	 */
	spin_lock_irq(&hd->lock);
	hd->attach_list = BIT(hd->conn_idx);
	hd->pend_conn = 0;
	hd->pend_disc = 0;
	spin_unlock_irq(&hd->lock);

	ret = request_irq(hd->irq, hvshm_irq, IRQF_SHARED, DRV_NAME, hd);
	if (ret) goto err_detach;
	hvshm_sync_status(hd);

	/* Drop whatever the host queued for a previous session. */
	mutex_lock(&hd->ring_lock);
	hd->bound = false;
	if (!hvshm_ensure_bound(hd)) hvshm_rx_flush(&hd->ch);
	mutex_unlock(&hd->ring_lock);

	filp->private_data = hd;
	return nonseekable_open(inode, filp);

err_detach:
	hvshm_detach(hd);
err_clear:
	clear_bit(HVSHM_F_OPEN, &hd->flags);
	return ret;
}

static int hvshm_release(struct inode *inode, struct file *filp) {
	struct hvshm_dev *hd = filp->private_data;

	free_irq(hd->irq, hd);
	hvshm_detach(hd);                      /* peers see PEER_DISCONNECTED */
	clear_bit(HVSHM_F_OPEN, &hd->flags);
	return 0;
}

static const struct file_operations hvshm_fops = {
	.owner          = THIS_MODULE,
	.open           = hvshm_open,
	.release        = hvshm_release,
	.poll           = hvshm_poll,
	.unlocked_ioctl = hvshm_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
	.llseek         = no_llseek,
};

static int hvshm_probe(struct platform_device *pdev) {
	struct device *dev = &pdev->dev;
	const char *name = HVSHM_DEFAULT_NAME;
	struct hvshm_dev *hd;
	struct resource *res;
	int ret;

	BUILD_BUG_ON(HVSHM_EVENT_PAYLOAD_MAX != HVSHM_MSG_MAX_PAYLOAD);

	if (PAGE_SIZE != SZ_4K) return -EOPNOTSUPP;            /* vdev page granule is 4 KiB */

	hd = devm_kzalloc(dev, sizeof(*hd), GFP_KERNEL);
	if (!hd) return -ENOMEM;
	spin_lock_init(&hd->lock);
	mutex_init(&hd->ring_lock);
	init_waitqueue_head(&hd->wq);

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	hd->irq = platform_get_irq(pdev, 0);
	if (!res || hd->irq < 0) return hd->irq < 0 ? hd->irq : -EINVAL;

	hd->req_pages = HVSHM_DEFAULT_PAGES;
	device_property_read_string(dev, "hvshm,region-name", &name);
	device_property_read_u32(dev, "hvshm,size-pages", &hd->req_pages);
	if (strlen(name) >= GUEST_SHM_MAX_NAME) return -ENAMETOOLONG;
	strscpy(hd->region_name, name, sizeof(hd->region_name));

	/* Factory page: trapped/emulated registers -> device mapping. */
	hd->factory = devm_ioremap(dev, res->start, resource_size(res));
	if (!hd->factory) return -ENOMEM;
	if (readq(hd->factory + FACT_REG(signature)) != GUEST_SHM_SIGNATURE) {
		dev_err(dev, "factory signature mismatch, wrong address?\n");
		return -ENODEV;
	}

	hd->misc.minor  = MISC_DYNAMIC_MINOR;
	hd->misc.name   = DRV_NAME;
	hd->misc.fops   = &hvshm_fops;
	hd->misc.mode   = 0660;
	hd->misc.parent = dev;
	ret = misc_register(&hd->misc);
	if (ret) return ret;

	platform_set_drvdata(pdev, hd);
	dev_info(dev, "ready: region '%s', irq %d (attaches on open)\n", hd->region_name, hd->irq);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 11, 0)
static void hvshm_remove(struct platform_device *pdev) {
	misc_deregister(&((struct hvshm_dev *)platform_get_drvdata(pdev))->misc);
}
#else
static int hvshm_remove(struct platform_device *pdev) {
	misc_deregister(&((struct hvshm_dev *)platform_get_drvdata(pdev))->misc);
	return 0;
}
#endif

static const struct of_device_id hvshm_of_match[] = {
	{ .compatible = "hvshm,guest-shm" },
	{ }
};
MODULE_DEVICE_TABLE(of, hvshm_of_match);

static struct platform_driver hvshm_driver = {
	.probe  = hvshm_probe,
	.remove = hvshm_remove,
	.driver = {
		.name = DRV_NAME,
		.of_match_table = hvshm_of_match,
	},
};
module_platform_driver(hvshm_driver);

MODULE_DESCRIPTION("Guest driver for the QNX Hypervisor shmem vdev");
MODULE_LICENSE("GPL");
MODULE_VERSION("2.0");
