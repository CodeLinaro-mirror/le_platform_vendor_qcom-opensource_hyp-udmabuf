// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#define pr_fmt(fmt) "hyp-udmabuf: " fmt

#include <linux/device.h>
#include <linux/dma-buf.h>
#include <linux/dma-mapping.h>
#include <linux/highmem.h>
#include <linux/hyp_udmabuf.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/slab.h>

struct udmabuf {
	struct miscdevice *device;
	int dmabuf_fd;
	u64 size;
	u32 nregions;
	struct guest_mem_region *list;
};

static const u32 list_limit = 1024 * 1024 * 4; /* 4M page */

static int hyp_udmabuf_mmap(struct dma_buf *buf, struct vm_area_struct *vma)
{
	struct udmabuf *ubuf = buf->priv;
	struct guest_mem_region *region = ubuf->list;
	unsigned long start, len;
	int i, ret;

	start = vma->vm_start;
	len = vma->vm_end - vma->vm_start;

	if (!ubuf->nregions) {
		pr_err("GPA regions are not initialized\n");
		return -EINVAL;
	}

	for (i = 0; i < ubuf->nregions; i++) {
		if (vma->vm_end - start < region[i].len) {
			if (start < vma->vm_end) {
				len = vma->vm_end - start;
			} else {
				pr_err("Out of range, start = %lx, end = %lx\n",
						start, vma->vm_end);
				return -EINVAL;
			}
		} else {
			len = region[i].len;
		}

		ret = remap_pfn_range(vma, start,
				region[i].addr >> PAGE_SHIFT,
				len, vma->vm_page_prot);
		if (ret < 0) {
			pr_err("map pfn range failed\n");
			return ret;
		}
		pr_debug("Mapped addr 0x%llx size 0x%llx bytes\n",
				region[i].addr, len);
		start += len;
	}

	return 0;
}

static struct sg_table *hyp_udmabuf_get_sgt(struct device *dev, struct dma_buf *buf,
				     enum dma_data_direction direction)
{
	struct udmabuf *ubuf = buf->priv;
	unsigned int i;
	struct sg_table *sgt;
	struct scatterlist *sg;
	int ret;

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = sg_alloc_table(sgt, ubuf->nregions, GFP_KERNEL);
	if (ret < 0)
		goto err;
	for_each_sg(sgt->sgl, sg, sgt->nents, i) {
		struct guest_mem_region *region = &ubuf->list[i];
		sg_set_page(sg, phys_to_page(region->addr),
				region->len,
				offset_in_page(region->addr));
	}
	ret = dma_map_sgtable(dev, sgt, direction, 0);
	if (ret < 0)
		goto err_free_sgt;

	return sgt;
err_free_sgt:
	sg_free_table(sgt);
err:
	kfree(sgt);

	return ERR_PTR(ret);
}

static void hyp_udmabuf_put_sgt(struct device *dev, struct sg_table *sgt,
			 enum dma_data_direction direction)
{
	dma_unmap_sgtable(dev, sgt, direction, 0);
	sg_free_table(sgt);
	kfree(sgt);
}

static struct sg_table *hyp_udmabuf_map(struct dma_buf_attachment *at,
				    enum dma_data_direction direction)
{
	return hyp_udmabuf_get_sgt(at->dev, at->dmabuf, direction);
}

static void hyp_udmabuf_unmap(struct dma_buf_attachment *at,
			  struct sg_table *sgt,
			  enum dma_data_direction direction)
{
	return hyp_udmabuf_put_sgt(at->dev, sgt, direction);
}

static void hyp_udmabuf_release(struct dma_buf *buf)
{
	struct udmabuf *ubuf = buf->priv;

	pr_debug("Release dmabuf fd = %d\n", ubuf->dmabuf_fd);

	kvfree(ubuf->list);
	kfree(ubuf);
}

static const struct dma_buf_ops hyp_udmabuf_ops = {
	.cache_sgt_mapping = true,
	.map_dma_buf	   = hyp_udmabuf_map,
	.unmap_dma_buf	   = hyp_udmabuf_unmap,
	.release	   = hyp_udmabuf_release,
	.mmap		   = hyp_udmabuf_mmap,
};

static long hyp_udmabuf_create(struct miscdevice *device,
			   struct hyp_udmabuf_create *head,
			   struct guest_mem_region *list)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	struct udmabuf *ubuf = NULL;
	struct dma_buf *buf;
	int ret = -EINVAL, dmabuf_fd = -1;
	u32 i;
	u64 guest_size = 0;

	if (head->flags != O_RDONLY &&
			head->flags != O_WRONLY &&
			head->flags != O_RDWR) {
		pr_err("Invalid mode flags %u\n", head->flags);
		return -EINVAL;
	}

	ubuf = kzalloc(sizeof(*ubuf), GFP_KERNEL);
	if (!ubuf)
		return -ENOMEM;

	head->fd = -1;
	head->size = 0;
	ubuf->nregions = head->count;
	ubuf->list = list;
	for (i = 0 ; i < head->count; i++) {
		struct guest_mem_region *region = &list[i];

		/* TODO: check memory regions against GVM memory space */
		if (PAGE_ALIGNED(region->addr) &&
			(PAGE_ALIGNED(region->len) ||
			 (i == head->count - 1))) {
			/* allow unaligned bytes in last page */
			guest_size += region->len;
			pr_debug("Create region[%d] paddr 0x%llx size 0x%llx\n",
					i, region->addr, region->len);
		} else {
			pr_err("Invalid memory region paddr 0x%llx size 0x%llx\n",
					region->addr,
					region->len);
			ret = -EINVAL;
			goto err;
		}
	}

	exp_info.ops  = &hyp_udmabuf_ops;
	exp_info.size = PAGE_ALIGN(guest_size);
	exp_info.priv = ubuf;
	exp_info.flags = head->flags;

	ubuf->device = device;
	buf = dma_buf_export(&exp_info);
	if (IS_ERR(buf)) {
		ret = PTR_ERR(buf);
		goto err;
	}

	dmabuf_fd = dma_buf_fd(buf, O_CLOEXEC);
	if (dmabuf_fd < 0) {
		pr_err("Get dmabuf fd failed\n");
		ubuf->list = NULL;
		dma_buf_put(buf);
		return dmabuf_fd;
	}
	ubuf->size = head->size = guest_size;
	ubuf->dmabuf_fd = head->fd = dmabuf_fd;
	pr_debug("Exported dmabuf fd = %d size = 0x%llx bytes\n",
			dmabuf_fd, guest_size);

	return 0;
err:
	kfree(ubuf);

	return ret;
}

static long hyp_udmabuf_ioctl_create(struct file *filp, unsigned long arg)
{
	struct hyp_udmabuf_create head;
	struct guest_mem_region *list;
	int ret = -EINVAL;
	u32 lsize;

	if (copy_from_user(&head, (void __user *)arg, sizeof(head)))
		return -EFAULT;

	if (!head.count || head.count > list_limit) {
		pr_err("GPA regions %u out of range[1, %u]\n",
				head.count, list_limit);
		return -EINVAL;
	}

	lsize = sizeof(struct guest_mem_region) * head.count;
	list = kvzalloc(lsize, GFP_KERNEL);
	if (!list)
		return -ENOMEM;

	if (copy_from_user(list,
			(void __user *)(arg + sizeof(head)), lsize)) {
		ret = -EFAULT;
		goto err;
	}

	ret = hyp_udmabuf_create(filp->private_data, &head, list);
	if (ret)
		goto err;

	if (copy_to_user((void __user *)arg, &head, sizeof(head))) {
		ret = -EFAULT;
		goto err;
	}

	return 0;
err:
	kvfree(list);

	return ret;
}

static long hyp_udmabuf_ioctl(struct file *filp, unsigned int ioctl,
			  unsigned long arg)
{
	long ret;

	switch (ioctl) {
	case HYP_UDMABUF_CREATE:
		ret = hyp_udmabuf_ioctl_create(filp, arg);
		break;

	default:
		return -ENOTTY;
	}

	return ret;
}

static const struct file_operations hyp_udmabuf_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl = hyp_udmabuf_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl   = hyp_udmabuf_ioctl,
#endif
};

static struct miscdevice hyp_udmabuf_misc = {
	.minor          = MISC_DYNAMIC_MINOR,
	.name           = "hyp_udmabuf",
	.fops           = &hyp_udmabuf_fops,
};

static int __init hyp_udmabuf_init(void)
{
	int ret;

	ret = misc_register(&hyp_udmabuf_misc);
	if (ret < 0) {
		pr_err("Could not initialize udmabuf device\n");
		return ret;
	}

	ret = dma_coerce_mask_and_coherent(hyp_udmabuf_misc.this_device,
					   DMA_BIT_MASK(64));
	if (ret < 0) {
		pr_err("Could not setup DMA mask for udmabuf device\n");
		misc_deregister(&hyp_udmabuf_misc);
		return ret;
	}

	pr_info("module initialized\n");

	return 0;
}

static void __exit hyp_udmabuf_exit(void)
{
	misc_deregister(&hyp_udmabuf_misc);
	pr_info("module exited\n");
}

module_init(hyp_udmabuf_init)
module_exit(hyp_udmabuf_exit)

MODULE_IMPORT_NS(DMA_BUF);
MODULE_LICENSE("GPL v2");
