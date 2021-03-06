/*-
 * Copyright (c) 2011, Bryan Venteicher <bryanv@daemoninthecloset.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * $FreeBSD: src/sys/dev/virtio/virtqueue.h,v 1.2 2012/04/14 05:48:04 grehan Exp $
 */

#ifndef _VIRTIO_VIRTQUEUE_H
#define _VIRTIO_VIRTQUEUE_H

#include <sys/types.h>
#include <sys/serialize.h>

struct virtqueue;
struct sglist;

/* Support for indirect buffer descriptors. */
#define VIRTIO_RING_F_INDIRECT_DESC	(1 << 28)

/* The guest publishes the used index for which it expects an interrupt
 * at the end of the avail ring. Host should ignore the avail->flags field.
 * The host publishes the avail index for which it expects a kick
 * at the end of the used ring. Guest should ignore the used->flags field.
 */
#define VIRTIO_RING_F_EVENT_IDX		(1 << 29)

/* Device callback for a virtqueue interrupt. */
typedef int virtqueue_intr_t(void *);

#define VIRTQUEUE_MAX_NAME_SZ	32

/* One for each virtqueue the device wishes to allocate. */
struct vq_alloc_info {
	char		   vqai_name[VIRTQUEUE_MAX_NAME_SZ];
	int		   vqai_maxindirsz;
	virtqueue_intr_t  *vqai_intr;
	void		  *vqai_intr_arg;
	struct virtqueue **vqai_vq;
};

#define VQ_ALLOC_INFO_INIT(_i,_nsegs,_intr,_arg,_vqp,_str,...) do {	\
	ksnprintf((_i)->vqai_name, VIRTQUEUE_MAX_NAME_SZ, _str,		\
	    ##__VA_ARGS__);						\
	(_i)->vqai_maxindirsz = (_nsegs);				\
	(_i)->vqai_intr = (_intr);					\
	(_i)->vqai_intr_arg = (_arg);					\
	(_i)->vqai_vq = (_vqp);						\
} while (0)

uint64_t virtqueue_filter_features(uint64_t features);

int	 virtqueue_alloc(device_t dev, uint16_t queue, uint16_t size,
	     int align, vm_paddr_t highaddr, struct vq_alloc_info *info,
	     struct virtqueue **vqp);
void	*virtqueue_drain(struct virtqueue *vq, int *last);
void	 virtqueue_free(struct virtqueue *vq);
int	 virtqueue_reinit(struct virtqueue *vq, uint16_t size);

int	 virtqueue_intr(struct virtqueue *vq);
int	 virtqueue_enable_intr(struct virtqueue *vq);
int	 virtqueue_postpone_intr(struct virtqueue *vq);
void	 virtqueue_disable_intr(struct virtqueue *vq);

/* Get physical address of the virtqueue ring. */
vm_paddr_t virtqueue_paddr(struct virtqueue *vq);

int	 virtqueue_full(struct virtqueue *vq);
int	 virtqueue_empty(struct virtqueue *vq);
int	 virtqueue_size(struct virtqueue *vq);
int	 virtqueue_nused(struct virtqueue *vq);
void	 virtqueue_notify(struct virtqueue *vq, lwkt_serialize_t);
void	 virtqueue_dump(struct virtqueue *vq);

int	 virtqueue_enqueue(struct virtqueue *vq, void *cookie,
	     struct sglist *sg, int readable, int writable);
void	*virtqueue_dequeue(struct virtqueue *vq, uint32_t *len);
void	*virtqueue_poll(struct virtqueue *vq, uint32_t *len);

#endif /* _VIRTIO_VIRTQUEUE_H */
