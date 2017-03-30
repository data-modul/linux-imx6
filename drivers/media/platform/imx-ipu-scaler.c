/*
 * i.MX IPUv3 scaler driver
 *
 * Copyright (C) 2011 Sascha Hauer, Pengutronix
 *
 * based on the mem2mem test driver
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/version.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <video/imx-ipu-v3.h>

#include <linux/platform_device.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "imx-ipu.h"

#define MIN_W 32
#define MIN_H 32
#define MAX_W 4096
#define MAX_H 4096
#define DIM_ALIGN_MASK 0x08 /* 8-alignment for dimensions */

/* Flags that indicate a format can be used for capture/output */
#define MEM2MEM_CAPTURE	(1 << 0)
#define MEM2MEM_OUTPUT	(1 << 1)

#define MEM2MEM_NAME		"imx-ipuv3-scale"

/* Per queue */
#define MEM2MEM_DEF_NUM_BUFS	VIDEO_MAX_FRAME
/* In bytes, per queue */
#define MEM2MEM_VID_MEM_LIMIT	(64 * 1024 * 1024)

#define fh_to_ctx(__fh)	container_of(__fh, struct ipu_scale_ctx, fh)

enum {
	V4L2_M2M_SRC = 0,
	V4L2_M2M_DST = 1,
};

struct ipu_scale_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	*vfd;
	struct device		*dev;
	struct ipu_soc		*ipu;

	atomic_t		num_inst;
	spinlock_t		irqlock;

	struct v4l2_m2m_dev	*m2m_dev;
	struct mutex		dev_mutex;
};

/* Per-queue, driver-specific private data */
struct ipu_scale_q_data {
	struct v4l2_pix_format	cur_fmt;
	struct v4l2_rect	rect;
};

struct ipu_scale_ctx {
	struct ipu_scale_dev	*ipu_scaler;

	struct v4l2_fh		fh;
	struct vb2_alloc_ctx	*alloc_ctx;
	struct ipu_scale_q_data	q_data[2];
	struct work_struct	work;
	struct completion	completion;
	struct work_struct	skip_run;
	int			error;
	int			aborting;
};

static struct ipu_scale_q_data *get_q_data(struct ipu_scale_ctx *ctx, enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &ctx->q_data[V4L2_M2M_SRC];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &ctx->q_data[V4L2_M2M_DST];
	default:
		BUG();
	}
	return NULL;
}

/*
 * mem2mem callbacks
 */

/**
 * job_ready() - check whether an instance is ready to be scheduled to run
 */
static int job_ready(void *priv)
{
	struct ipu_scale_ctx *ctx = priv;

	if (ctx->aborting)
		return 0;

	if (v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) < 1
	    || v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx) < 1) {
		dev_dbg(ctx->ipu_scaler->dev, "Not enough buffers available\n");
		return 0;
	}

	return 1;
}

static void job_abort(void *priv)
{
	struct ipu_scale_ctx *ctx = priv;

	ctx->aborting = 1;
}

static void ipu_complete(void *priv, int err)
{
	struct ipu_scale_dev *ipu_scaler = priv;
	struct ipu_scale_ctx *curr_ctx;

	curr_ctx = v4l2_m2m_get_curr_priv(ipu_scaler->m2m_dev);

	if (NULL == curr_ctx) {
		dev_dbg(ipu_scaler->dev,
			"Instance released before the end of transaction\n");
		return;
	}

	curr_ctx->error = err;
	complete(&curr_ctx->completion);
}

static void device_run(void *priv)
{
	struct ipu_scale_ctx *ctx = priv;

	schedule_work(&ctx->work);
}

static void ipu_scaler_work(struct work_struct *work)
{
	struct ipu_scale_ctx *ctx = container_of(work, struct ipu_scale_ctx, work);
	struct ipu_scale_dev *ipu_scaler = ctx->ipu_scaler;
	struct vb2_buffer *src_buf, *dst_buf;
	struct ipu_scale_q_data *q_data;
	struct v4l2_pix_format *pix;
	struct ipu_image in, out;
	int err = -ETIMEDOUT;
	unsigned long flags;

	/*
	 * If streamoff dequeued all buffers before we could get the lock,
	 * just bail out immediately.
	 */
	if (!v4l2_m2m_num_src_bufs_ready(ctx->fh.m2m_ctx) ||
		!v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx)) {
		WARN_ON(1);
		schedule_work(&ctx->skip_run);
		return;
	}

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	pix = &q_data->cur_fmt;

	in.pix.width = pix->width;
	in.pix.height = pix->height;
	in.pix.bytesperline = pix->bytesperline;
	in.pix.pixelformat = pix->pixelformat;
	in.rect = q_data->rect;
	in.phys = vb2_dma_contig_plane_dma_addr(src_buf, 0);

	q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	pix = &q_data->cur_fmt;

	out.pix.width = pix->width;
	out.pix.height = pix->height;
	out.pix.bytesperline = pix->bytesperline;
	out.pix.pixelformat = pix->pixelformat;
	out.rect = q_data->rect;
	out.phys = vb2_dma_contig_plane_dma_addr(dst_buf, 0);

	ipu_image_convert(ipu_scaler->ipu, &in, &out, ipu_complete, ipu_scaler);

	if (!wait_for_completion_timeout(&ctx->completion,
					 msecs_to_jiffies(300))) {
		dev_err(ipu_scaler->dev,
			"Timeout waiting for scaling result\n");
		err = -ETIMEDOUT;
	} else {
		err = ctx->error;
	}

	src_buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	dst_buf->v4l2_buf.timestamp = src_buf->v4l2_buf.timestamp;
	dst_buf->v4l2_buf.timecode = src_buf->v4l2_buf.timecode;

	spin_lock_irqsave(&ipu_scaler->irqlock, flags);
	v4l2_m2m_buf_done(src_buf, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst_buf, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	spin_unlock_irqrestore(&ipu_scaler->irqlock, flags);

	v4l2_m2m_job_finish(ipu_scaler->m2m_dev, ctx->fh.m2m_ctx);
}

/*
 * video ioctls
 */
static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	strncpy(cap->driver, MEM2MEM_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, MEM2MEM_NAME, sizeof(cap->card) - 1);
	strncpy(cap->bus_info, "platform:" MEM2MEM_NAME, sizeof(cap->bus_info) - 1);
	/*
	 * This is only a mem-to-mem video device. The capture and output
	 * device capability flags are left for backward compatibility and
	 * are scheduled for removal.
	 */
	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_VIDEO_OUTPUT |
			   V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return ipu_enum_fmt(file, priv, f);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *priv,
				   struct v4l2_fmtdesc *f)
{
	return ipu_enum_fmt(file, priv, f);
}

static int vidioc_g_fmt(struct ipu_scale_ctx *ctx, struct v4l2_format *f)
{
	struct vb2_queue *vq;
	struct ipu_scale_q_data *q_data;
	struct v4l2_pix_format *pix;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	pix = &q_data->cur_fmt;

	return ipu_g_fmt(f, pix);
}

static int vidioc_g_fmt_vid_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(priv, f);
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	return vidioc_g_fmt(priv, f);
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	return ipu_try_fmt(file, priv, f);
}

static int vidioc_try_fmt_vid_out(struct file *file, void *priv,
				  struct v4l2_format *f)
{
	f->fmt.pix.width &= ~0x7;
	return ipu_try_fmt(file, priv, f);
}

static int vidioc_s_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct ipu_scale_q_data *q_data;
	struct vb2_queue *vq;
	struct ipu_scale_ctx *ctx = fh_to_ctx(priv);
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	if (!q_data)
		return -EINVAL;

	if (vb2_is_busy(vq)) {
		v4l2_err(&ctx->ipu_scaler->v4l2_dev, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	ret = ipu_s_fmt(file, priv, f, &q_data->cur_fmt);
	if (ret < 0)
		return ret;

	/* Reset cropping/composing rectangle */
	q_data->rect.left = 0;
	q_data->rect.top = 0;
	q_data->rect.width = q_data->cur_fmt.width;
	q_data->rect.height = q_data->cur_fmt.height;

	return 0;
}

static int vidioc_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct ipu_scale_ctx *ctx = fh_to_ctx(priv);
	struct ipu_scale_q_data *q_data;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = q_data->cur_fmt.width;
		s->r.height = q_data->cur_fmt.height;
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = q_data->cur_fmt.width;
		s->r.height = q_data->cur_fmt.height;
		break;
	case V4L2_SEL_TGT_COMPOSE:
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		s->r = q_data->rect;
		break;
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = q_data->cur_fmt.width;
		s->r.height = q_data->cur_fmt.height;
		break;
	}

	return 0;
}

static int vidioc_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *s)
{
	struct ipu_scale_ctx *ctx = fh_to_ctx(priv);
	struct ipu_scale_q_data *q_data;
	struct vb2_queue *vq;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, s->type);
	if (!vq)
		return -EINVAL;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
		break;
	case V4L2_SEL_TGT_COMPOSE:
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		break;
	default:
		return -EINVAL;
	}

	/* The input's frame width to the IC must be a multiple of 8 pixels
	 * When performing resizing the frame width must be multiple of burst
	 * size - 8 or 16 pixels as defined by CB#_BURST_16 parameter.
	 */
	if (s->flags & V4L2_SEL_FLAG_GE)
		s->r.width = round_up(s->r.width, 8);
	if (s->flags & V4L2_SEL_FLAG_LE)
		s->r.width = round_down(s->r.width, 8);
	s->r.width = clamp_t(unsigned int, s->r.width, 8,
			     round_down(q_data->cur_fmt.width, 8));
	s->r.height = clamp_t(unsigned int, s->r.height, 1,
			      q_data->cur_fmt.height);
	s->r.left = clamp_t(unsigned int, s->r.left, 0,
			    q_data->cur_fmt.width - s->r.width);
	s->r.top = clamp_t(unsigned int, s->r.top, 0,
			   q_data->cur_fmt.height - s->r.height);

	/* V4L2_SEL_FLAG_KEEP_CONFIG is only valid for subdevices */
	q_data->rect = s->r;

	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *fsize)
{
	return ipu_enum_framesizes(file, fh, fsize);
}

static const struct v4l2_ioctl_ops ipu_scale_ioctl_ops = {
	.vidioc_querycap	= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap	= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap	= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap	= vidioc_s_fmt,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out	= vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out	= vidioc_s_fmt,

	.vidioc_g_selection	= vidioc_g_selection,
	.vidioc_s_selection	= vidioc_s_selection,

	.vidioc_reqbufs		= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf	= v4l2_m2m_ioctl_querybuf,

	.vidioc_qbuf		= v4l2_m2m_ioctl_qbuf,
	.vidioc_expbuf		= v4l2_m2m_ioctl_expbuf,
	.vidioc_dqbuf		= v4l2_m2m_ioctl_dqbuf,
	.vidioc_create_bufs	= v4l2_m2m_ioctl_create_bufs,

	.vidioc_streamon	= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff	= v4l2_m2m_ioctl_streamoff,

	.vidioc_enum_framesizes = vidioc_enum_framesizes,
};

static void ipu_scale_skip_run(struct work_struct *work)
{
	struct ipu_scale_ctx *ctx = container_of(work, struct ipu_scale_ctx, skip_run);

	v4l2_m2m_job_finish(ctx->ipu_scaler->m2m_dev, ctx->fh.m2m_ctx);
}


/*
 * Queue operations
 */

static int ipu_scale_queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
		unsigned int *nbuffers,
		unsigned int *nplanes, unsigned int sizes[],
		void *alloc_ctxs[])
{
	struct ipu_scale_ctx *ctx = vb2_get_drv_priv(vq);
	struct ipu_scale_q_data *q_data;
	unsigned int size, count = *nbuffers;
	struct v4l2_pix_format *pix;

	q_data = get_q_data(ctx, vq->type);
	pix = &q_data->cur_fmt;

	size = pix->sizeimage;

	while (size * count > MEM2MEM_VID_MEM_LIMIT)
		(count)--;

	*nplanes = 1;
	*nbuffers = count;
	sizes[0] = size;

	ctx->alloc_ctx = vb2_dma_contig_init_ctx(ctx->ipu_scaler->dev);
	if (IS_ERR(ctx->alloc_ctx))
		return PTR_ERR(ctx->alloc_ctx);

	alloc_ctxs[0] = ctx->alloc_ctx;

	dev_dbg(ctx->ipu_scaler->dev, "get %d buffer(s) of size %d each.\n", count, size);

	return 0;
}

static int ipu_scale_buf_prepare(struct vb2_buffer *vb)
{
	struct ipu_scale_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct ipu_scale_q_data *q_data;
	struct v4l2_pix_format *pix;

	dev_dbg(ctx->ipu_scaler->dev, "type: %d\n", vb->vb2_queue->type);

	q_data = get_q_data(ctx, vb->vb2_queue->type);
	pix = &q_data->cur_fmt;

	if (vb2_plane_size(vb, 0) < pix->sizeimage) {
		dev_dbg(ctx->ipu_scaler->dev,
				"%s data will not fit into plane (%lu < %lu)\n",
				__func__, vb2_plane_size(vb, 0), (long)pix->sizeimage);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, pix->sizeimage);

	return 0;
}

static void ipu_scale_buf_queue(struct vb2_buffer *vb)
{
	struct ipu_scale_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vb);
}

static struct vb2_ops ipu_scale_qops = {
	.queue_setup	 = ipu_scale_queue_setup,
	.buf_prepare	 = ipu_scale_buf_prepare,
	.buf_queue	 = ipu_scale_buf_queue,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
};

static int queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct ipu_scale_ctx *ctx = priv;
	int ret;

	memset(src_vq, 0, sizeof(*src_vq));
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &ipu_scale_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_type = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->ipu_scaler->dev_mutex;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	memset(dst_vq, 0, sizeof(*dst_vq));
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &ipu_scale_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_type = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->ipu_scaler->dev_mutex;

	return vb2_queue_init(dst_vq);
}

/*
 * File operations
 */
static int ipu_scale_open(struct file *file)
{
	struct ipu_scale_dev *ipu_scaler = video_drvdata(file);
	struct ipu_scale_ctx *ctx = NULL;
	const int width = 720;
	const int height = 576;
	int i;

	ctx = kzalloc(sizeof *ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	INIT_WORK(&ctx->skip_run, ipu_scale_skip_run);
	INIT_WORK(&ctx->work, ipu_scaler_work);
	init_completion(&ctx->completion);
	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);
	ctx->ipu_scaler = ipu_scaler;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(ipu_scaler->m2m_dev, ctx, &queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		int ret = PTR_ERR(ctx->fh.m2m_ctx);

		kfree(ctx);
		return ret;
	}

	for (i = 0; i < 2; i++) {
		ctx->q_data[i].cur_fmt.width = width;
		ctx->q_data[i].cur_fmt.height = height;
		ctx->q_data[i].cur_fmt.bytesperline = width;
		ctx->q_data[i].cur_fmt.pixelformat = V4L2_PIX_FMT_YUV420;
		ctx->q_data[i].cur_fmt.sizeimage = width * height * 3 / 2;
		ctx->q_data[i].cur_fmt.colorspace = V4L2_COLORSPACE_REC709;
		ctx->q_data[i].rect.left = 0;
		ctx->q_data[i].rect.top = 0;
		ctx->q_data[i].rect.width = width;
		ctx->q_data[i].rect.height = height;
	}

	atomic_inc(&ipu_scaler->num_inst);

	dev_dbg(ipu_scaler->dev, "Created instance %p, m2m_ctx: %p\n",
			ctx, ctx->fh.m2m_ctx);

	return 0;
}

static int ipu_scale_release(struct file *file)
{
	struct ipu_scale_dev *ipu_scaler = video_drvdata(file);
	struct ipu_scale_ctx *ctx = fh_to_ctx(file->private_data);

	dev_dbg(ipu_scaler->dev,"Releasing instance %p\n", ctx);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	atomic_dec(&ipu_scaler->num_inst);

	return 0;
}

static const struct v4l2_file_operations ipu_scale_fops = {
	.owner		= THIS_MODULE,
	.open		= ipu_scale_open,
	.release	= ipu_scale_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static struct video_device ipu_scale_videodev = {
	.name		= MEM2MEM_NAME,
	.fops		= &ipu_scale_fops,
	.ioctl_ops	= &ipu_scale_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.vfl_dir	= VFL_DIR_M2M,
};

static struct v4l2_m2m_ops m2m_ops = {
	.device_run	= device_run,
	.job_ready	= job_ready,
	.job_abort	= job_abort,
};

static u64 vout_dmamask = ~(u32)0;

static int ipu_scale_probe(struct platform_device *pdev)
{
	struct ipu_scale_dev *ipu_scaler;
	struct video_device *vfd;
	struct ipu_soc *ipu = dev_get_drvdata(pdev->dev.parent);
	int ret;

	pdev->dev.dma_mask = &vout_dmamask;
	pdev->dev.coherent_dma_mask = 0xffffffff;

	ipu_scaler = kzalloc(sizeof *ipu_scaler, GFP_KERNEL);
	if (!ipu_scaler)
		return -ENOMEM;

	ipu_scaler->ipu = ipu;
	ipu_scaler->dev = &pdev->dev;

	spin_lock_init(&ipu_scaler->irqlock);
	mutex_init(&ipu_scaler->dev_mutex);

	ret = v4l2_device_register(&pdev->dev, &ipu_scaler->v4l2_dev);
	if (ret)
		goto free_dev;

	atomic_set(&ipu_scaler->num_inst, 0);

	vfd = video_device_alloc();
	if (!vfd) {
		dev_err(ipu_scaler->dev, "Failed to allocate video device\n");
		ret = -ENOMEM;
		goto unreg_dev;
	}

	*vfd = ipu_scale_videodev;
	vfd->lock = &ipu_scaler->dev_mutex;
	vfd->v4l2_dev = &ipu_scaler->v4l2_dev;

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		dev_err(ipu_scaler->dev, "Failed to register video device\n");
		goto rel_vdev;
	}

	video_set_drvdata(vfd, ipu_scaler);
	snprintf(vfd->name, sizeof(vfd->name), "%s", ipu_scale_videodev.name);
	ipu_scaler->vfd = vfd;
	dev_dbg(ipu_scaler->dev, "Device registered as /dev/video%d\n",
		vfd->num);

	platform_set_drvdata(pdev, ipu_scaler);

	ipu_scaler->m2m_dev = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(ipu_scaler->m2m_dev)) {
		dev_err(ipu_scaler->dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(ipu_scaler->m2m_dev);
		goto err_m2m;
	}

	return 0;

	v4l2_m2m_release(ipu_scaler->m2m_dev);
err_m2m:
	video_unregister_device(ipu_scaler->vfd);
rel_vdev:
	video_device_release(vfd);
unreg_dev:
	v4l2_device_unregister(&ipu_scaler->v4l2_dev);
free_dev:
	kfree(ipu_scaler);

	return ret;
}

static int ipu_scale_remove(struct platform_device *pdev)
{
	struct ipu_scale_dev *ipu_scaler =
		(struct ipu_scale_dev *)platform_get_drvdata(pdev);

	v4l2_m2m_release(ipu_scaler->m2m_dev);
	video_unregister_device(ipu_scaler->vfd);
	v4l2_device_unregister(&ipu_scaler->v4l2_dev);
	kfree(ipu_scaler);

	return 0;
}

static struct platform_driver ipu_scale_pdrv = {
	.probe		= ipu_scale_probe,
	.remove		= ipu_scale_remove,
	.driver		= {
		.name	= "imx-ipuv3-scaler",
		.owner	= THIS_MODULE,
	},
};

static void __exit ipu_scale_exit(void)
{
	platform_driver_unregister(&ipu_scale_pdrv);
}

static int __init ipu_scale_init(void)
{
	return  platform_driver_register(&ipu_scale_pdrv);
}

module_init(ipu_scale_init);
module_exit(ipu_scale_exit);

MODULE_DESCRIPTION("Virtual device for mem2mem framework testing");
MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_LICENSE("GPL");
