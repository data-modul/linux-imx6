/*
 * i.MX IPUv3 vdicr driver
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

#include <linux/platform_device.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-dma-contig.h>

#include "imx-ipu.h"
#include <video/imx-ipu-v3.h>

#define MIN_W 32
#define MIN_H 32
#define MAX_W 4096
#define MAX_H 4096
#define DIM_ALIGN_MASK 0x08 /* 8-alignment for dimensions */

/* Flags that indicate a format can be used for capture/output */
#define MEM2MEM_CAPTURE	(1 << 0)
#define MEM2MEM_OUTPUT	(1 << 1)

#define MEM2MEM_NAME		"imx-ipuv3-vdic"

/* Per queue */
#define MEM2MEM_DEF_NUM_BUFS	VIDEO_MAX_FRAME
/* In bytes, per queue */
#define MEM2MEM_VID_MEM_LIMIT	(64 * 1024 * 1024)

enum {
	V4L2_M2M_SRC = 0,
	V4L2_M2M_DST = 1,
};

struct ipu_vdic_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	*vfd;
	struct device		*dev;
	struct ipu_soc		*ipu;

	atomic_t		num_inst;
	spinlock_t		irqlock;

	struct v4l2_m2m_dev	*m2m_dev;
	struct ipuv3_channel	*ipu_ch[4];
};

/* Per-queue, driver-specific private data */
struct ipu_vdic_q_data {
	struct v4l2_pix_format	cur_fmt;
};

struct ipu_vdic_ctx {
	struct ipu_vdic_dev	*ipu_vdic;

	struct v4l2_m2m_ctx	*m2m_ctx;
	struct vb2_alloc_ctx	*alloc_ctx;
	struct ipu_vdic_q_data	q_data[2];
	struct mutex		lock;

	struct vb2_buffer *in_p, *in;
};

static struct ipu_vdic_q_data *get_q_data(struct ipu_vdic_ctx *ctx, enum v4l2_buf_type type)
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

static void job_abort(void *priv)
{
}

static void ipu_vdic_lock(void *priv)
{
	struct ipu_vdic_ctx *ctx = priv;
	mutex_lock(&ctx->lock);
}

static void ipu_vdic_unlock(void *priv)
{
	struct ipu_vdic_ctx *ctx = priv;
	mutex_unlock(&ctx->lock);
}

static void ipu_complete(void *priv, int err)
{
	struct ipu_vdic_dev *ipu_vdic = priv;
	struct ipu_vdic_ctx *ctx;
	struct vb2_buffer *dst_vb;
	unsigned long flags;

	ctx = v4l2_m2m_get_curr_priv(ipu_vdic->m2m_dev);

	if (!ctx) {
		dev_dbg(ipu_vdic->dev,
			"Instance released before the end of transaction\n");
		return;
	}

	spin_lock_irqsave(&ipu_vdic->irqlock, flags);

	if (ctx->in_p != ctx->in)
		v4l2_m2m_buf_done(ctx->in_p, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	ctx->in_p = ctx->in;

	dst_vb = v4l2_m2m_dst_buf_remove(ctx->m2m_ctx);
	dst_vb->v4l2_buf.timestamp = ctx->in->v4l2_buf.timestamp;
	dst_vb->v4l2_buf.timecode = ctx->in->v4l2_buf.timecode;
	v4l2_m2m_buf_done(dst_vb, err ? VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);

	spin_unlock_irqrestore(&ipu_vdic->irqlock, flags);

	v4l2_m2m_job_finish(ipu_vdic->m2m_dev, ctx->m2m_ctx);
}

static void device_run(void *priv)
{
	struct ipu_vdic_ctx *ctx = priv;
	struct ipu_vdic_dev *ipu_vdic = ctx->ipu_vdic;
	struct vb2_buffer *dst_buf;
	struct ipu_vdic_q_data *q_data;
	struct v4l2_pix_format *pix;
	struct ipu_image image_in[3];
	struct ipu_image image_out = {};
	int i;

	ctx->in = v4l2_m2m_src_buf_remove(ctx->m2m_ctx);

	if (!ctx->in_p)
		ctx->in_p = ctx->in;

	dst_buf = v4l2_m2m_next_dst_buf(ctx->m2m_ctx);

	q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	pix = &q_data->cur_fmt;

	for (i = 0; i < 3; i++) {
		struct ipu_image *image = &image_in[i];

		memset(image, 0, sizeof(*image));
		image->pix.width = pix->width;
		image->pix.height = pix->height / 2;
		image->pix.bytesperline = pix->bytesperline;
		image->pix.pixelformat = pix->pixelformat;
		image->rect.left = 0;
		image->rect.top = 0;
		image->rect.width = pix->width;
		image->rect.height = pix->height / 2;
	}
	if (pix->field == V4L2_FIELD_INTERLACED_BT) {
		for (i = 0; i < 3; i++)
			image_in[i].pix.bytesperline = pix->bytesperline * 2;

		image_in[0].phys = vb2_dma_contig_plane_dma_addr(ctx->in_p, 0);
		image_in[1].phys = vb2_dma_contig_plane_dma_addr(ctx->in, 0) +
			pix->bytesperline;
		image_in[2].phys = vb2_dma_contig_plane_dma_addr(ctx->in, 0);
	} else if (pix->field == V4L2_FIELD_INTERLACED_TB) {
		for (i = 0; i < 3; i++)
			image_in[i].pix.bytesperline = pix->bytesperline * 2;

		image_in[0].phys = vb2_dma_contig_plane_dma_addr(ctx->in_p, 0) +
			pix->bytesperline;
		image_in[1].phys = vb2_dma_contig_plane_dma_addr(ctx->in, 0);
		image_in[2].phys = vb2_dma_contig_plane_dma_addr(ctx->in, 0) +
			pix->bytesperline;
	} else {
		image_in[0].phys = vb2_dma_contig_plane_dma_addr(ctx->in_p, 0) +
			pix->bytesperline * pix->height / 2;
		image_in[1].phys = vb2_dma_contig_plane_dma_addr(ctx->in, 0);
		image_in[2].phys = vb2_dma_contig_plane_dma_addr(ctx->in, 0) +
			pix->bytesperline * pix->height / 2;
	}

	q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	pix = &q_data->cur_fmt;

	image_out.pix.width = pix->width;
	image_out.pix.height = pix->height;
	image_out.pix.bytesperline = pix->bytesperline;
	image_out.pix.pixelformat = pix->pixelformat;
	image_out.rect.left = 0;
	image_out.rect.top = 0;
	image_out.rect.width = pix->width;
	image_out.rect.height = pix->height;
	image_out.phys = vb2_dma_contig_plane_dma_addr(dst_buf, 0);

	ipu_image_deinterlace_convert(ipu_vdic->ipu, &image_in[0], &image_in[1], &image_in[2], &image_out,
		ipu_complete, ipu_vdic);
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

static int vidioc_g_fmt(struct ipu_vdic_ctx *ctx, struct v4l2_format *f)
{
	struct vb2_queue *vq;
	struct ipu_vdic_q_data *q_data;
	struct v4l2_pix_format *pix;

	vq = v4l2_m2m_get_vq(ctx->m2m_ctx, f->type);
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
	enum v4l2_field field = f->fmt.pix.field;
	int ret;

	switch (f->fmt.pix.field) {
	case V4L2_FIELD_SEQ_BT:
	case V4L2_FIELD_INTERLACED_BT:
		printk("%s: %s not supported yet\n", __func__,
		       v4l2_field_names[f->fmt.pix.field]);
		/* fallthrough */
	case V4L2_FIELD_ANY:
	case V4L2_FIELD_NONE:
	case V4L2_FIELD_TOP:
	case V4L2_FIELD_BOTTOM:
	case V4L2_FIELD_INTERLACED:
		field = V4L2_FIELD_INTERLACED_TB;
		break;
	case V4L2_FIELD_SEQ_TB:
	case V4L2_FIELD_INTERLACED_TB:
		break;
	}

	ret = ipu_try_fmt(file, priv, f);

	f->fmt.pix.field = field;

	return ret;
}

static int vidioc_s_fmt(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct ipu_vdic_q_data *q_data;
	struct vb2_queue *vq;
	struct ipu_vdic_ctx *ctx = priv;
	enum v4l2_field field = V4L2_FIELD_ANY;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	q_data = get_q_data(ctx, f->type);
	if (!q_data)
		return -EINVAL;

	if (vb2_is_busy(vq)) {
		v4l2_err(&ctx->ipu_vdic->v4l2_dev, "%s queue busy\n", __func__);
		return -EBUSY;
	}

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		vidioc_try_fmt_vid_out(file, priv, f);
		field = f->fmt.pix.field;
	}

	ret = ipu_s_fmt(file, priv, f, &q_data->cur_fmt);

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		f->fmt.pix.field = field;
		q_data->cur_fmt.field = field;
	}

	return ret;
}

static int vidioc_reqbufs(struct file *file, void *priv,
			  struct v4l2_requestbuffers *reqbufs)
{
	struct ipu_vdic_ctx *ctx = priv;

	return v4l2_m2m_reqbufs(file, ctx->m2m_ctx, reqbufs);
}

static int vidioc_querybuf(struct file *file, void *priv,
			   struct v4l2_buffer *buf)
{
	struct ipu_vdic_ctx *ctx = priv;

	return v4l2_m2m_querybuf(file, ctx->m2m_ctx, buf);
}

static int vidioc_qbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct ipu_vdic_ctx *ctx = priv;

	return v4l2_m2m_qbuf(file, ctx->m2m_ctx, buf);
}

static int vidioc_dqbuf(struct file *file, void *priv, struct v4l2_buffer *buf)
{
	struct ipu_vdic_ctx *ctx = priv;

	return v4l2_m2m_dqbuf(file, ctx->m2m_ctx, buf);
}

static int vidioc_create_bufs(struct file*file, void *priv,
			      struct v4l2_create_buffers *create)
{
	struct ipu_vdic_ctx *ctx = priv;

	return v4l2_m2m_create_bufs(file, ctx->m2m_ctx, create);
}

static int vidioc_streamon(struct file *file, void *priv,
			   enum v4l2_buf_type type)
{
	struct ipu_vdic_ctx *ctx = priv;

	return v4l2_m2m_streamon(file, ctx->m2m_ctx, type);
}

static int vidioc_streamoff(struct file *file, void *priv,
			    enum v4l2_buf_type type)
{
	struct ipu_vdic_ctx *ctx = priv;

	return v4l2_m2m_streamoff(file, ctx->m2m_ctx, type);
}

static int vidioc_enum_framesizes(struct file *file, void *fh,
				  struct v4l2_frmsizeenum *fsize)
{
	return ipu_enum_framesizes(file, fh, fsize);
}

static const struct v4l2_ioctl_ops ipu_vdic_ioctl_ops = {
	.vidioc_querycap	= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap	= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap	= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap	= vidioc_s_fmt,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out	= vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out	= vidioc_s_fmt,

	.vidioc_reqbufs		= vidioc_reqbufs,
	.vidioc_querybuf	= vidioc_querybuf,

	.vidioc_qbuf		= vidioc_qbuf,
	.vidioc_dqbuf		= vidioc_dqbuf,
	.vidioc_create_bufs	= vidioc_create_bufs,

	.vidioc_streamon	= vidioc_streamon,
	.vidioc_streamoff	= vidioc_streamoff,

	.vidioc_enum_framesizes = vidioc_enum_framesizes,
};


/*
 * Queue operations
 */

static int ipu_vdic_queue_setup(struct vb2_queue *vq, const struct v4l2_format *fmt,
		unsigned int *nbuffers,
		unsigned int *nplanes, unsigned int sizes[],
		void *alloc_ctxs[])
{
	struct ipu_vdic_ctx *ctx = vb2_get_drv_priv(vq);
	struct ipu_vdic_q_data *q_data;
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

	ctx->alloc_ctx = vb2_dma_contig_init_ctx(ctx->ipu_vdic->dev);
	if (IS_ERR(ctx->alloc_ctx))
		return PTR_ERR(ctx->alloc_ctx);

	alloc_ctxs[0] = ctx->alloc_ctx;

	dev_dbg(ctx->ipu_vdic->dev, "get %d buffer(s) of size %d each.\n", count, size);

	return 0;
}

static int ipu_vdic_buf_prepare(struct vb2_buffer *vb)
{
	struct ipu_vdic_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct ipu_vdic_q_data *q_data;
	struct v4l2_pix_format *pix;

	dev_dbg(ctx->ipu_vdic->dev, "type: %d\n", vb->vb2_queue->type);

	q_data = get_q_data(ctx, vb->vb2_queue->type);
	pix = &q_data->cur_fmt;

	if (vb2_plane_size(vb, 0) < pix->sizeimage) {
		dev_dbg(ctx->ipu_vdic->dev,
				"%s data will not fit into plane (%lu < %lu)\n",
				__func__, vb2_plane_size(vb, 0), (long)pix->sizeimage);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, pix->sizeimage);

	return 0;
}

static void ipu_vdic_buf_queue(struct vb2_buffer *vb)
{
	struct ipu_vdic_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->m2m_ctx, vb);
}

static void ipu_vdic_wait_prepare(struct vb2_queue *q)
{
	struct ipu_vdic_ctx *ctx = vb2_get_drv_priv(q);
	ipu_vdic_unlock(ctx);
}

static void ipu_vdic_wait_finish(struct vb2_queue *q)
{
	struct ipu_vdic_ctx *ctx = vb2_get_drv_priv(q);
	ipu_vdic_lock(ctx);
}

static struct vb2_ops ipu_vdic_qops = {
	.queue_setup	 = ipu_vdic_queue_setup,
	.buf_prepare	 = ipu_vdic_buf_prepare,
	.buf_queue	 = ipu_vdic_buf_queue,
	.wait_prepare	 = ipu_vdic_wait_prepare,
	.wait_finish	 = ipu_vdic_wait_finish,
};

static int queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct ipu_vdic_ctx *ctx = priv;
	int ret;

	memset(src_vq, 0, sizeof(*src_vq));
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &ipu_vdic_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_type = V4L2_BUF_FLAG_TIMESTAMP_COPY;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	memset(dst_vq, 0, sizeof(*dst_vq));
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &ipu_vdic_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_type = V4L2_BUF_FLAG_TIMESTAMP_COPY;

	return vb2_queue_init(dst_vq);
}

/*
 * File operations
 */
static int ipu_vdic_open(struct file *file)
{
	struct ipu_vdic_dev *ipu_vdic = video_drvdata(file);
	struct ipu_vdic_ctx *ctx = NULL;
	const int width = 720;
	const int height = 576;
	int i;

	ctx = kzalloc(sizeof *ctx, GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	file->private_data = ctx;
	ctx->ipu_vdic = ipu_vdic;
	mutex_init(&ctx->lock);

	ctx->m2m_ctx = v4l2_m2m_ctx_init(ipu_vdic->m2m_dev, ctx, &queue_init);

	if (IS_ERR(ctx->m2m_ctx)) {
		int ret = PTR_ERR(ctx->m2m_ctx);

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
	}

	atomic_inc(&ipu_vdic->num_inst);

	dev_dbg(ipu_vdic->dev, "Created instance %p, m2m_ctx: %p\n",
			ctx, ctx->m2m_ctx);

	return 0;
}

static int ipu_vdic_release(struct file *file)
{
	struct ipu_vdic_dev *ipu_vdic = video_drvdata(file);
	struct ipu_vdic_ctx *ctx = file->private_data;

	dev_dbg(ipu_vdic->dev,"Releasing instance %p\n", ctx);

	v4l2_m2m_ctx_release(ctx->m2m_ctx);
	kfree(ctx);

	atomic_dec(&ipu_vdic->num_inst);

	return 0;
}

static unsigned int ipu_vdic_poll(struct file *file,
				 struct poll_table_struct *wait)
{
	struct ipu_vdic_ctx *ctx = file->private_data;

	return v4l2_m2m_poll(file, ctx->m2m_ctx, wait);
}

static int ipu_vdic_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct ipu_vdic_ctx *ctx = file->private_data;

	return v4l2_m2m_mmap(file, ctx->m2m_ctx, vma);
}

static const struct v4l2_file_operations ipu_vdic_fops = {
	.owner		= THIS_MODULE,
	.open		= ipu_vdic_open,
	.release	= ipu_vdic_release,
	.poll		= ipu_vdic_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= ipu_vdic_mmap,
};

static struct video_device ipu_vdic_videodev = {
	.name		= MEM2MEM_NAME,
	.fops		= &ipu_vdic_fops,
	.ioctl_ops	= &ipu_vdic_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.vfl_dir	= VFL_DIR_M2M,
};

static struct v4l2_m2m_ops m2m_ops = {
	.device_run	= device_run,
	.job_abort	= job_abort,
};

static u64 vout_dmamask = ~(u32)0;

static int ipu_vdic_probe(struct platform_device *pdev)
{
	struct ipu_vdic_dev *ipu_vdic;
	struct video_device *vfd;
	struct ipu_soc *ipu = dev_get_drvdata(pdev->dev.parent);
	int ret;

	pdev->dev.dma_mask = &vout_dmamask;
	pdev->dev.coherent_dma_mask = 0xffffffff;

	ipu_vdic = devm_kzalloc(&pdev->dev, sizeof(*ipu_vdic), GFP_KERNEL);
	if (!ipu_vdic)
		return -ENOMEM;

	ipu_vdic->ipu = ipu;
	ipu_vdic->dev = &pdev->dev;

	spin_lock_init(&ipu_vdic->irqlock);

	ret = v4l2_device_register(&pdev->dev, &ipu_vdic->v4l2_dev);
	if (ret)
		goto free_dev;

	atomic_set(&ipu_vdic->num_inst, 0);

	vfd = video_device_alloc();
	if (!vfd) {
		dev_err(ipu_vdic->dev, "Failed to allocate video device\n");
		ret = -ENOMEM;
		goto unreg_dev;
	}

	*vfd = ipu_vdic_videodev;
	vfd->v4l2_dev = &ipu_vdic->v4l2_dev;

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		dev_err(ipu_vdic->dev, "Failed to register video device\n");
		goto rel_vdev;
	}

	video_set_drvdata(vfd, ipu_vdic);
	snprintf(vfd->name, sizeof(vfd->name), "%s", ipu_vdic_videodev.name);
	ipu_vdic->vfd = vfd;
	dev_dbg(ipu_vdic->dev, "Device registered as /dev/video%d\n", vfd->num);

	platform_set_drvdata(pdev, ipu_vdic);

	ipu_vdic->m2m_dev = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(ipu_vdic->m2m_dev)) {
		dev_err(ipu_vdic->dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(ipu_vdic->m2m_dev);
		goto err_m2m;
	}

	return 0;

	v4l2_m2m_release(ipu_vdic->m2m_dev);
err_m2m:
	video_unregister_device(ipu_vdic->vfd);
rel_vdev:
	video_device_release(vfd);
unreg_dev:
	v4l2_device_unregister(&ipu_vdic->v4l2_dev);
free_dev:

	return ret;
}

static int ipu_vdic_remove(struct platform_device *pdev)
{
	struct ipu_vdic_dev *ipu_vdic =
		(struct ipu_vdic_dev *)platform_get_drvdata(pdev);

	v4l2_m2m_release(ipu_vdic->m2m_dev);
	video_unregister_device(ipu_vdic->vfd);
	v4l2_device_unregister(&ipu_vdic->v4l2_dev);
	kfree(ipu_vdic);

	return 0;
}

static struct platform_driver ipu_vdic_pdrv = {
	.probe		= ipu_vdic_probe,
	.remove		= ipu_vdic_remove,
	.driver		= {
		.name	= "imx-ipuv3-vdic",
		.owner	= THIS_MODULE,
	},
};

static void __exit ipu_vdic_exit(void)
{
	platform_driver_unregister(&ipu_vdic_pdrv);
}

static int __init ipu_vdic_init(void)
{
	return  platform_driver_register(&ipu_vdic_pdrv);
}

module_init(ipu_vdic_init);
module_exit(ipu_vdic_exit);

MODULE_DESCRIPTION("Virtual device for mem2mem framework testing");
MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_LICENSE("GPL");
