/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (c) 2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#ifndef _UAPI_LINUX_HYP_UDMABUF_H
#define _UAPI_LINUX_HYP_UDMABUF_H

#include <linux/types.h>
#include <linux/ioctl.h>

/**
 * struct guest_mem_region - Guest memory region
 * @addr: Start physical address(page aligned)
 * @len: Length of the region in bytes(page aligned except last region)
 */
struct guest_mem_region {
	__u64 addr;
	__u64 len;
};

/**
 * struct hyp_udmabuf_create - Arguments to create a dma-buf fd function
 * @flags: Mode flags for the dma-buf file(O_RDONLY,O_WRONLY,O_RDWR)
 * @count: Guest memory regions count
 * @resv: Reserverd fields
 * @fd: Created new dma-buf fd (output)
 * @size: Size of the new dma-buf (output)
 * @list: List of the guest memory regions
 */
struct hyp_udmabuf_create {
	__u32 flags;
	__u32 count;
	__u32 resv;
	__s32 fd;
	__u64 size;
	struct guest_mem_region list[];
};

/**
 * HYP_UDMABUF_CREATE - This ioctl turn GPA regions into dma-bufs
 *
 * Input: hyp_udmabuf_create structure with the required attributes.
 * Output: new dma-buf fd and size
 *
 * Return: 0 if success, -errno on failure
 */
#define HYP_UDMABUF_CREATE   _IOW('u', 0x42, struct hyp_udmabuf_create)

#endif /* _UAPI_LINUX_HYP_UDMABUF_H */
