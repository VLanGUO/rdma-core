/*
 * Copyright (c) 2006 Chelsio, Inc. All rights reserved.
 * Copyright (c) 2006 Open Grid Computing, Inc. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#if HAVE_CONFIG_H
#  include <config.h>
#endif				/* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <pthread.h>

#ifdef HAVE_SYSFS_LIBSYSFS_H
#include <sysfs/libsysfs.h>
#endif

#ifndef HAVE_IBV_READ_SYSFS_FILE
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#include "iwch.h"
#include "iwch-abi.h"

#define PCI_VENDOR_ID_CHELSIO		0x1425
#define PCI_DEVICE_ID_CHELSIO_PE9000_2C	0x0020
#define PCI_DEVICE_ID_CHELSIO_T302E	0x0021
#define PCI_DEVICE_ID_CHELSIO_T310E	0x0022
#define PCI_DEVICE_ID_CHELSIO_T310X	0x0023
#define PCI_DEVICE_ID_CHELSIO_T302X	0x0024

#define HCA(v, d, t) \
	{ .vendor = PCI_VENDOR_ID_##v,			\
	  .device = PCI_DEVICE_ID_CHELSIO_##d,		\
	  .type = CHELSIO_##t }

struct {
	unsigned vendor;
	unsigned device;
	enum iwch_hca_type type;
} hca_table[] = {
	HCA(CHELSIO, PE9000_2C, CXGB3),
	HCA(CHELSIO, T302E, CXGB3),
	HCA(CHELSIO, T302X, CXGB3),
	HCA(CHELSIO, T310E, CXGB3),
	HCA(CHELSIO, T310X, CXGB3),
};

static struct ibv_context_ops iwch_ctx_ops = {
	.query_device = iwch_query_device,
	.query_port = iwch_query_port,
	.alloc_pd = iwch_alloc_pd,
	.dealloc_pd = iwch_free_pd,
	.reg_mr = iwch_reg_mr,
	.dereg_mr = iwch_dereg_mr,
	.create_cq = iwch_create_cq,
	.resize_cq = iwch_resize_cq,
	.poll_cq = iwch_poll_cq,
	.destroy_cq = iwch_destroy_cq,
	.create_srq = iwch_create_srq,
	.modify_srq = iwch_modify_srq,
	.destroy_srq = iwch_destroy_srq,
	.create_qp = iwch_create_qp,
	.modify_qp = iwch_modify_qp,
	.destroy_qp = iwch_destroy_qp,
	.create_ah = iwch_create_ah,
	.destroy_ah = iwch_destroy_ah,
	.attach_mcast = iwch_attach_mcast,
	.detach_mcast = iwch_detach_mcast
};

static struct ibv_context *iwch_alloc_context(struct ibv_device *ibdev,
					      int cmd_fd)
{
	struct iwch_context *context;
	struct ibv_get_context cmd;
	struct iwch_alloc_ucontext_resp resp;

	context = malloc(sizeof *context);
	if (!context)
		return NULL;

	context->ibv_ctx.cmd_fd = cmd_fd;

	if (ibv_cmd_get_context(&context->ibv_ctx, &cmd, sizeof cmd,
				&resp.ibv_resp, sizeof resp))
		goto err_free;

	context->ibv_ctx.device = ibdev;
	context->ibv_ctx.ops = iwch_ctx_ops;
	context->ibv_ctx.ops.req_notify_cq = iwch_arm_cq;
	context->ibv_ctx.ops.cq_event = NULL;
	context->ibv_ctx.ops.post_send = iwch_post_send;
	context->ibv_ctx.ops.post_recv = iwch_post_recv;
	context->ibv_ctx.ops.post_srq_recv = iwch_post_srq_recv;

	return &context->ibv_ctx;
err_free:
	free(context);
	return NULL;
}

static void iwch_free_context(struct ibv_context *ibctx)
{
	struct iwch_context *context = to_iwch_ctx(ibctx);

	free(context);
}

static struct ibv_device_ops iwch_dev_ops = {
	.alloc_context = iwch_alloc_context,
	.free_context = iwch_free_context
};

struct ibv_device *ibv_driver_init(const char *uverbs_sys_path,
				   int abi_version)
{
	char value[8];
	struct iwch_device *dev;
	unsigned vendor, device;
	int i;

	if (ibv_read_sysfs_file(uverbs_sys_path, "device/vendor",
				value, sizeof value) < 0)
		return NULL;
	sscanf(value, "%i", &vendor);

	if (ibv_read_sysfs_file(uverbs_sys_path, "device/device",
				value, sizeof value) < 0)
		return NULL;
	sscanf(value, "%i", &device);

	for (i = 0; i < sizeof hca_table / sizeof hca_table[0]; ++i)
		if (vendor == hca_table[i].vendor &&
		    device == hca_table[i].device)
			goto found;

	return NULL;

found:
	dev = malloc(sizeof *dev);
	if (!dev) {
		return NULL;
	}

	dev->ibv_dev.ops = iwch_dev_ops;
	dev->hca_type = hca_table[i].type;
	dev->page_size = sysconf(_SC_PAGESIZE);

	return &dev->ibv_dev;
}

#ifdef HAVE_SYSFS_LIBSYSFS_H
struct ibv_device *openib_driver_init(struct sysfs_class_device *sysdev)
{
	int abi_ver = 0;
	char value[8];

	if (ibv_read_sysfs_file(sysdev->path, "abi_version",
				value, sizeof value) > 0)
		abi_ver = strtol(value, NULL, 10);

	return ibv_driver_init(sysdev->path, abi_ver);
}
#endif /* HAVE_SYSFS_LIBSYSFS_H */
