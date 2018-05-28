/*
 * This is a mem2mem driver for the Freescale i.MX5/6 SOC. It carries out
 * color-space conversion, downsizing, resizing, and rotation transformations
 * on input buffers using the IPU Image Converter's Post-Processing task.
 *
 * Based on mem2mem_testdev.c by Pawel Osciak.
 *
 * Copyright (c) 2012-2013 Mentor Graphics Inc.
 * Steve Longerbeam <steve_longerbeam@mentor.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/log2.h>

#include <linux/platform_device.h>
#include <linux/interrupt.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-dma-contig.h>
#include <video/imx-ipu-v3.h>
#include <video/imx-ipu-image-convert.h>
#include <media/imx.h>

MODULE_DESCRIPTION("i.MX5/6 Post-Processing mem2mem device");
MODULE_AUTHOR("Steve Longerbeam <steve_longerbeam@mentor.com>");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

static int instrument;
module_param(instrument, int, 0);
MODULE_PARM_DESC(instrument, "1 = enable conversion time measurement");

/* Flags that indicate a format can be used for capture/output */
#define MEM2MEM_CAPTURE BIT(0)
#define MEM2MEM_OUTPUT  BIT(1)

#define MEM2MEM_NAME		"imx-m2m"

/* Per queue */
#define MEM2MEM_DEF_NUM_BUFS	VIDEO_MAX_FRAME
/* In bytes, per queue */
#define MEM2MEM_VID_MEM_LIMIT	SZ_256M

#define dprintk(dev, fmt, arg...) \
	v4l2_dbg(1, 1, &dev->v4l2_dev, "%s: " fmt, __func__, ## arg)

struct m2mx_ctx;

struct m2mx_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	*vfd;
	struct device           *ipu_dev;
	struct ipu_soc          *ipu;

	struct mutex		dev_mutex;

	struct v4l2_m2m_dev	*m2m_dev;
};

struct m2mx_ctx {
	struct v4l2_fh          fh;
	struct m2mx_dev	*dev;

	struct ipu_ic           *ic;
	struct ipu_image_convert_ctx *ic_ctx;

	/* The rotation controls */
	struct v4l2_ctrl_handler ctrl_hdlr;
	int                     rotation; /* degrees */
	bool                    hflip;
	bool                    vflip;

	/* derived from rotation, hflip, vflip controls */
	enum ipu_rotate_mode    rot_mode;

	/* has the IC image converter been preapred */
	bool                    image_converter_ready;

	/* Abort requested by m2m */
	bool			aborting;

	/* Source and destination image data */
	struct ipu_image  image[2];

	/* for instrumenting */
	struct timespec   start;
};

#define fh_to_ctx(p) container_of(p, struct m2mx_ctx, fh)

enum {
	V4L2_M2M_SRC = 0,
	V4L2_M2M_DST = 1,
};

static const struct v4l2_ctrl_config m2mx_std_ctrl[] = {
	{
		.id	= V4L2_CID_HFLIP,
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.name	= "Horizontal Flip",
		.def	= 0,
		.min	= 0,
		.max	= 1,
		.step	= 1,
	}, {
		.id	= V4L2_CID_VFLIP,
		.type	= V4L2_CTRL_TYPE_BOOLEAN,
		.name	= "Vertical Flip",
		.def	= 0,
		.min	= 0,
		.max	= 1,
		.step	= 1,
	}, {
		.id	= V4L2_CID_ROTATE,
		.type	= V4L2_CTRL_TYPE_INTEGER,
		.name	= "Rotation",
		.def	= 0,
		.min	= 0,
		.max	= 270,
		.step	= 90,
	},
};

#define M2MX_NUM_STD_CONTROLS ARRAY_SIZE(m2mx_std_ctrl)

static struct ipu_image *get_image_data(struct m2mx_ctx *ctx,
					enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &ctx->image[V4L2_M2M_SRC];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &ctx->image[V4L2_M2M_DST];
	default:
		break;
	}
	return NULL;
}

/*
 * mem2mem callbacks
 */

static void m2mx_job_abort(void *priv)
{
	struct m2mx_ctx *ctx = priv;

	/* Will cancel the transaction in the next interrupt handler */
	ipu_image_convert_abort(ctx->ic_ctx);
	ctx->aborting = true;
}

static void m2mx_lock(void *priv)
{
	struct m2mx_ctx *ctx = priv;
	struct m2mx_dev *dev = ctx->dev;

	mutex_lock(&dev->dev_mutex);
}

static void m2mx_unlock(void *priv)
{
	struct m2mx_ctx *ctx = priv;
	struct m2mx_dev *dev = ctx->dev;

	mutex_unlock(&dev->dev_mutex);
}

static void m2mx_device_run(void *priv)
{
	struct m2mx_ctx *ctx = priv;
	struct m2mx_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct ipu_image_convert_run *run;
	dma_addr_t s_phys, d_phys;

	run = kzalloc(sizeof(*run), GFP_KERNEL);
	if (!run)
		return;

	run->ctx = ctx->ic_ctx;
	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	s_phys = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	d_phys = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);
	if (!s_phys || !d_phys) {
		v4l2_err(&dev->v4l2_dev,
			 "Acquiring kernel pointers to buffers failed\n");
		return;
	}

	run->in_phys = s_phys;
	run->out_phys = d_phys;

	if (instrument)
		getnstimeofday(&ctx->start);

	ipu_image_convert_queue(run);
}

static void m2mx_convert_complete(struct ipu_image_convert_run *run,
				  void *data)
{
	struct m2mx_ctx *ctx = data;
	struct m2mx_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;

	if (!ctx->aborting) {
		src_vb = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		dst_vb = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		v4l2_m2m_buf_done(src_vb, VB2_BUF_STATE_DONE);
		v4l2_m2m_buf_done(dst_vb, VB2_BUF_STATE_DONE);

		if (instrument) {
			struct timespec ts, diff;
			unsigned long interval;

			getnstimeofday(&ts);
			diff = timespec_sub(ts, ctx->start);
			interval = diff.tv_sec * 1000 * 1000 +
				diff.tv_nsec / 1000;
			v4l2_info(&ctx->dev->v4l2_dev,
				  "buf%d completed in %lu usec\n",
				  dst_vb->vb2_buf.index, interval);
		}
	}

	v4l2_m2m_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx);
}

/*
 * video ioctls
 */
static int vidioc_querycap(struct file *file, void *fh,
			   struct v4l2_capability *cap)
{
	strncpy(cap->driver, MEM2MEM_NAME, sizeof(cap->driver) - 1);
	strncpy(cap->card, MEM2MEM_NAME, sizeof(cap->card) - 1);
	cap->bus_info[0] = 0;
	cap->device_caps = V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	return 0;
}

static int enum_fmt(struct v4l2_fmtdesc *f, u32 type)
{
	u32 fourcc;
	int ret;

	ret = ipu_image_convert_enum_format(f->index, &fourcc);
	if (ret)
		return ret;

	f->pixelformat = fourcc;
	return 0;
}

static int vidioc_enum_fmt_vid_cap(struct file *file, void *fh,
				   struct v4l2_fmtdesc *f)
{
	return enum_fmt(f, MEM2MEM_CAPTURE);
}

static int vidioc_enum_fmt_vid_out(struct file *file, void *fh,
				   struct v4l2_fmtdesc *f)
{
	return enum_fmt(f, MEM2MEM_OUTPUT);
}

static int m2mx_g_fmt(struct m2mx_ctx *ctx, struct v4l2_format *f)
{
	struct vb2_queue *vq;
	struct ipu_image *image;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	image = get_image_data(ctx, f->type);
	if (!image)
		return -EINVAL;

	f->fmt.pix = image->pix;

	return 0;
}

static int vidioc_g_fmt_vid_out(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return m2mx_g_fmt(ctx, f);
}

static int vidioc_g_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return m2mx_g_fmt(ctx, f);
}

static int m2mx_try_fmt(struct m2mx_ctx *ctx, struct v4l2_format *f)
{
	struct ipu_image test_in, test_out;
	struct ipu_image *pin, *pout;

	pin = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	pout = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		test_out.pix = f->fmt.pix;
		test_in = *pin;
	} else {
		test_in.pix = f->fmt.pix;
		test_out = *pout;
	}

	ipu_image_convert_adjust(&test_in, &test_out, ctx->rot_mode);

	f->fmt.pix = (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) ?
		test_out.pix : test_in.pix;

	return 0;
}

static int vidioc_try_fmt_vid_cap(struct file *file, void *fh,
				  struct v4l2_format *f)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return m2mx_try_fmt(ctx, f);
}

static int vidioc_try_fmt_vid_out(struct file *file, void *fh,
				  struct v4l2_format *f)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return m2mx_try_fmt(ctx, f);
}

static int m2mx_s_fmt(struct m2mx_ctx *ctx, struct v4l2_format *f)
{
	struct m2mx_dev *dev = ctx->dev;
	struct ipu_image test_in, test_out;
	struct ipu_image *pin, *pout;
	struct vb2_queue *vq;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (!vq)
		return -EINVAL;

	if (vb2_is_busy(vq)) {
		v4l2_err(&dev->v4l2_dev, "%s: queue busy\n", __func__);
		return -EBUSY;
	}

	pin = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	pout = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		test_out.pix = f->fmt.pix;
		test_in = *pin;
	} else {
		test_in.pix = f->fmt.pix;
		test_out = *pout;
	}

	ipu_image_convert_adjust(&test_in, &test_out, ctx->rot_mode);

	f->fmt.pix = (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) ?
		test_out.pix : test_in.pix;
	*pin = test_in;
	*pout = test_out;

	return 0;
}

static int vidioc_s_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return m2mx_s_fmt(ctx, f);
}

static int vidioc_s_fmt_vid_out(struct file *file, void *fh,
				struct v4l2_format *f)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return m2mx_s_fmt(ctx, f);
}

static int vidioc_reqbufs(struct file *file, void *fh,
			  struct v4l2_requestbuffers *reqbufs)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return v4l2_m2m_reqbufs(file, ctx->fh.m2m_ctx, reqbufs);
}

static int vidioc_querybuf(struct file *file, void *fh,
			   struct v4l2_buffer *buf)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return v4l2_m2m_querybuf(file, ctx->fh.m2m_ctx, buf);
}

static int vidioc_qbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return v4l2_m2m_qbuf(file, ctx->fh.m2m_ctx, buf);
}

static int vidioc_dqbuf(struct file *file, void *fh, struct v4l2_buffer *buf)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return v4l2_m2m_dqbuf(file, ctx->fh.m2m_ctx, buf);
}

static int vidioc_expbuf(struct file *file, void *fh,
			 struct v4l2_exportbuffer *eb)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return v4l2_m2m_expbuf(file, ctx->fh.m2m_ctx, eb);
}

static int vidioc_streamon(struct file *file, void *fh,
			   enum v4l2_buf_type type)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return v4l2_m2m_streamon(file, ctx->fh.m2m_ctx, type);
}

static int vidioc_streamoff(struct file *file, void *fh,
			    enum v4l2_buf_type type)
{
	struct m2mx_ctx *ctx = fh_to_ctx(fh);

	return v4l2_m2m_streamoff(file, ctx->fh.m2m_ctx, type);
}

static int m2mx_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct m2mx_ctx *ctx = container_of(ctrl->handler,
					     struct m2mx_ctx, ctrl_hdlr);
	struct m2mx_dev *dev = ctx->dev;
	enum ipu_rotate_mode rot_mode;
	struct ipu_image *in, *out;
	struct vb2_queue *vq;
	bool hflip, vflip;
	int rotation;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (!vq)
		return -EINVAL;

	/* can't change rotation mid-streaming */
	if (vb2_is_streaming(vq)) {
		v4l2_err(&dev->v4l2_dev, "%s: not allowed while streaming\n",
			 __func__);
		return -EBUSY;
	}

	in = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	out = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	rotation = ctx->rotation;
	hflip = ctx->hflip;
	vflip = ctx->vflip;

	switch (ctrl->id) {
	case V4L2_CID_HFLIP:
		hflip = (ctrl->val == 1);
		break;
	case V4L2_CID_VFLIP:
		vflip = (ctrl->val == 1);
		break;
	case V4L2_CID_ROTATE:
		rotation = ctrl->val;
		break;
	default:
		v4l2_err(&dev->v4l2_dev, "Invalid control\n");
		return -EINVAL;
	}

	ret = ipu_degrees_to_rot_mode(&rot_mode, rotation, hflip, vflip);
	if (ret)
		return ret;

	/*
	 * make sure this rotation will work with current src/dest
	 * rectangles before setting
	 */
	ret = ipu_image_convert_verify(in, out, rot_mode);
	if (ret)
		return ret;

	ctx->rotation = rotation;
	ctx->hflip = hflip;
	ctx->vflip = vflip;
	ctx->rot_mode = rot_mode;

	return 0;
}

static const struct v4l2_ctrl_ops m2mx_ctrl_ops = {
	.s_ctrl = m2mx_s_ctrl,
};

static int m2mx_init_controls(struct m2mx_ctx *ctx)
{
	struct v4l2_ctrl_handler *hdlr = &ctx->ctrl_hdlr;
	const struct v4l2_ctrl_config *c;
	int i, ret;

	v4l2_ctrl_handler_init(hdlr, M2MX_NUM_STD_CONTROLS);

	for (i = 0; i < M2MX_NUM_STD_CONTROLS; i++) {
		c = &m2mx_std_ctrl[i];
		v4l2_ctrl_new_std(hdlr, &m2mx_ctrl_ops,
				  c->id, c->min, c->max, c->step, c->def);
	}

	if (hdlr->error) {
		ret = hdlr->error;
		goto free_ctrls;
	}

	ctx->fh.ctrl_handler = hdlr;
	ret = v4l2_ctrl_handler_setup(hdlr);
	if (ret)
		goto free_ctrls;

	return 0;

free_ctrls:
	v4l2_ctrl_handler_free(hdlr);
	return ret;
}

static const struct v4l2_ioctl_ops m2mx_ioctl_ops = {
	.vidioc_querycap	= vidioc_querycap,

	.vidioc_enum_fmt_vid_cap = vidioc_enum_fmt_vid_cap,
	.vidioc_g_fmt_vid_cap	= vidioc_g_fmt_vid_cap,
	.vidioc_try_fmt_vid_cap	= vidioc_try_fmt_vid_cap,
	.vidioc_s_fmt_vid_cap	= vidioc_s_fmt_vid_cap,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out	= vidioc_try_fmt_vid_out,
	.vidioc_s_fmt_vid_out	= vidioc_s_fmt_vid_out,

	.vidioc_reqbufs		= vidioc_reqbufs,
	.vidioc_querybuf	= vidioc_querybuf,

	.vidioc_qbuf		= vidioc_qbuf,
	.vidioc_dqbuf		= vidioc_dqbuf,
	.vidioc_expbuf		= vidioc_expbuf,

	.vidioc_streamon	= vidioc_streamon,
	.vidioc_streamoff	= vidioc_streamoff,
};

/*
 * Queue operations
 */

static int m2mx_queue_setup(struct vb2_queue *vq,
			     unsigned int *nbuffers, unsigned int *nplanes,
			     unsigned int sizes[], struct device *alloc_ctxs[])
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(vq);
	struct ipu_image *image;
	unsigned int count = *nbuffers;

	image = get_image_data(ctx, vq->type);
	if (!image)
		return -EINVAL;

	while (image->pix.sizeimage * count > MEM2MEM_VID_MEM_LIMIT)
		count--;

	*nplanes = 1;
	*nbuffers = count;
	sizes[0] = image->pix.sizeimage;

	return 0;
}

static int m2mx_buf_prepare(struct vb2_buffer *vb)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct m2mx_dev *dev = ctx->dev;
	struct ipu_image *image;

	image = get_image_data(ctx, vb->vb2_queue->type);
	if (!image)
		return -EINVAL;

	if (vb2_plane_size(vb, 0) < image->pix.sizeimage) {
		v4l2_err(&dev->v4l2_dev,
			 "%s: data will not fit into plane (%lu < %lu)\n",
			 __func__, vb2_plane_size(vb, 0),
			 (long)image->pix.sizeimage);
		return -EINVAL;
	}

	vb2_set_plane_payload(vb, 0, image->pix.sizeimage);

	return 0;
}

static void m2mx_buf_queue(struct vb2_buffer *vb)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void m2mx_wait_prepare(struct vb2_queue *q)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(q);

	m2mx_unlock(ctx);
}

static void m2mx_wait_finish(struct vb2_queue *q)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(q);

	m2mx_lock(ctx);
}

static int m2mx_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(q);
	struct ipu_image *pin, *pout;

	/*
	 * mem2mem requires streamon at both sides, capture and output,
	 * and we only want to prepare the ipu-ic image converter once.
	 */
	if (ctx->image_converter_ready)
		return 0;

	pin = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	pout = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);

	ctx->ic_ctx = ipu_image_convert_prepare(ctx->dev->ipu, IC_TASK_POST_PROCESSOR, pin, pout,
						ctx->rot_mode,
						m2mx_convert_complete, ctx);
	if (IS_ERR(ctx->ic_ctx))
		return PTR_ERR(ctx->ic_ctx);

	ctx->image_converter_ready = true;
	return 0;
}

static void m2mx_stop_streaming(struct vb2_queue *q)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(q);

	if (ctx->image_converter_ready) {
		ipu_image_convert_unprepare(ctx->ic_ctx);
		ctx->image_converter_ready = false;
	}
}

static struct vb2_ops m2mx_qops = {
	.queue_setup	 = m2mx_queue_setup,
	.buf_prepare	 = m2mx_buf_prepare,
	.buf_queue	 = m2mx_buf_queue,
	.wait_prepare	 = m2mx_wait_prepare,
	.wait_finish	 = m2mx_wait_finish,
	.start_streaming = m2mx_start_streaming,
	.stop_streaming  = m2mx_stop_streaming,
};

static int queue_init(void *priv, struct vb2_queue *src_vq,
		      struct vb2_queue *dst_vq)
{
	struct m2mx_ctx *ctx = priv;
	int ret;

	memset(src_vq, 0, sizeof(*src_vq));
	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &m2mx_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->dev = ctx->dev->ipu_dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	memset(dst_vq, 0, sizeof(*dst_vq));
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &m2mx_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->dev = ctx->dev->ipu_dev;

	return vb2_queue_init(dst_vq);
}

/*
 * File operations
 */
static int m2mx_open(struct file *file)
{
	struct m2mx_dev *dev = video_drvdata(file);
	struct ipu_image *in, *out;
	struct m2mx_ctx *ctx;
	int ret = 0;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto unlock;
	}
	ctx->dev = dev;

	ctx->ic = ipu_ic_get(dev->ipu, IC_TASK_POST_PROCESSOR);
	if (IS_ERR(ctx->ic)) {
		v4l2_err(&dev->v4l2_dev, "could not get IC PP\n");
		ret = PTR_ERR(ctx->ic);
		goto error_free;
	}

	v4l2_fh_init(&ctx->fh, dev->vfd);
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, &queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto error_fh;
	}

	/*
	 * set some defaults for output and capture image formats.
	 * default for both is 640x480 RGB565.
	 */
	in = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	out = get_image_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	in->pix.width = out->pix.width = 640;
	in->pix.height = out->pix.height = 480;
	in->pix.pixelformat = V4L2_PIX_FMT_RGB565;
	out->pix.pixelformat = V4L2_PIX_FMT_RGB565;
	in->pix.sizeimage =
		(in->pix.width * in->pix.height * 16) >> 3;
	out->pix.sizeimage =
		(out->pix.width * out->pix.height * 16) >> 3;

	ret = m2mx_init_controls(ctx);
	if (ret)
		goto error_m2m_ctx;

	mutex_unlock(&dev->dev_mutex);
	return 0;

error_m2m_ctx:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
error_fh:
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	ipu_ic_put(ctx->ic);
error_free:
	kfree(ctx);
unlock:
	mutex_unlock(&dev->dev_mutex);
	return ret;
}

static int m2mx_release(struct file *file)
{
	struct m2mx_dev *dev = video_drvdata(file);
	struct m2mx_ctx *ctx = fh_to_ctx(file->private_data);

	mutex_lock(&dev->dev_mutex);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_ctrl_handler_free(&ctx->ctrl_hdlr);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	ipu_ic_put(ctx->ic);
	kfree(ctx);

	mutex_unlock(&dev->dev_mutex);
	return 0;
}

static unsigned int m2mx_poll(struct file *file,
			       struct poll_table_struct *wait)
{
	struct m2mx_dev *dev = video_drvdata(file);
	struct m2mx_ctx *ctx = fh_to_ctx(file->private_data);
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ret = v4l2_m2m_poll(file, ctx->fh.m2m_ctx, wait);

	mutex_unlock(&dev->dev_mutex);
	return ret;
}

static int m2mx_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct m2mx_dev *dev = video_drvdata(file);
	struct m2mx_ctx *ctx = fh_to_ctx(file->private_data);
	int ret;

	if (mutex_lock_interruptible(&dev->dev_mutex))
		return -ERESTARTSYS;

	ret = v4l2_m2m_mmap(file, ctx->fh.m2m_ctx, vma);

	mutex_unlock(&dev->dev_mutex);
	return ret;
}

static const struct v4l2_file_operations m2mx_fops = {
	.owner		= THIS_MODULE,
	.open		= m2mx_open,
	.release	= m2mx_release,
	.poll		= m2mx_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= m2mx_mmap,
};

static struct video_device m2mx_videodev = {
	.name		= MEM2MEM_NAME,
	.fops		= &m2mx_fops,
	.ioctl_ops	= &m2mx_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.vfl_dir	= VFL_DIR_M2M,
};

static struct v4l2_m2m_ops m2m_ops = {
	.device_run	= m2mx_device_run,
	.job_abort	= m2mx_job_abort,
	.lock		= m2mx_lock,
	.unlock		= m2mx_unlock,
};

static int of_dev_node_match(struct device *dev, void *data)
{
	return dev->of_node == data;
}

static struct ipu_soc *m2mx_get_ipu(struct m2mx_dev *dev,
				     struct device_node *node)
{
	struct device_node *ipu_node;
	struct device *ipu_dev;
	struct ipu_soc *ipu;

	ipu_node = of_parse_phandle(node, "ipu", 0);
	if (!ipu_node) {
		v4l2_err(&dev->v4l2_dev, "missing ipu phandle!\n");
		return ERR_PTR(-EINVAL);
	}

	ipu_dev = bus_find_device(&platform_bus_type, NULL,
				  ipu_node, of_dev_node_match);
	of_node_put(ipu_node);

	if (!ipu_dev) {
		v4l2_err(&dev->v4l2_dev, "failed to find ipu device!\n");
		return ERR_PTR(-ENODEV);
	}

	device_lock(ipu_dev);

	if (!ipu_dev->driver || !try_module_get(ipu_dev->driver->owner)) {
		ipu = ERR_PTR(-EPROBE_DEFER);
		v4l2_warn(&dev->v4l2_dev, "IPU driver not loaded\n");
		device_unlock(ipu_dev);
		goto dev_put;
	}

	dev->ipu_dev = ipu_dev;
	ipu = dev_get_drvdata(ipu_dev);

	device_unlock(ipu_dev);
	return ipu;
dev_put:
	put_device(ipu_dev);
	return ipu;
}

static void m2mx_put_ipu(struct m2mx_dev *dev)
{
	if (!IS_ERR_OR_NULL(dev->ipu_dev)) {
		module_put(dev->ipu_dev->driver->owner);
		put_device(dev->ipu_dev);
	}
}

static int m2mx_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct m2mx_dev *dev;
	struct video_device *vfd;
	int ret;

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);

	/* get our IPU */
	dev->ipu = m2mx_get_ipu(dev, node);
	if (IS_ERR(dev->ipu)) {
		v4l2_err(&dev->v4l2_dev, "could not get ipu\n");
		ret = PTR_ERR(dev->ipu);
		goto unreg_dev;
	}

	mutex_init(&dev->dev_mutex);

	vfd = video_device_alloc();
	if (!vfd) {
		v4l2_err(&dev->v4l2_dev, "Failed to allocate video device\n");
		ret = -ENOMEM;
		goto unreg_dev;
	}

	*vfd = m2mx_videodev;
	vfd->v4l2_dev = &dev->v4l2_dev;
	vfd->lock = &dev->dev_mutex;

	dev->m2m_dev = v4l2_m2m_init(&m2m_ops);
	if (IS_ERR(dev->m2m_dev)) {
		v4l2_err(&dev->v4l2_dev, "Failed to init mem2mem device\n");
		ret = PTR_ERR(dev->m2m_dev);
		video_device_release(vfd);
		goto unreg_dev;
	}

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&dev->v4l2_dev, "Failed to register video device\n");
		video_device_release(vfd);
		goto rel_m2m;
	}

	video_set_drvdata(vfd, dev);
	snprintf(vfd->name, sizeof(vfd->name), "%s", m2mx_videodev.name);
	dev->vfd = vfd;
	v4l2_info(&dev->v4l2_dev,
		  "Device registered as /dev/video%d, on ipu%d\n",
		  vfd->num, ipu_get_num(dev->ipu));

	platform_set_drvdata(pdev, dev);

	return 0;

rel_m2m:
	v4l2_m2m_release(dev->m2m_dev);
unreg_dev:
	v4l2_device_unregister(&dev->v4l2_dev);
	return ret;
}

static int m2mx_remove(struct platform_device *pdev)
{
	struct m2mx_dev *dev =
		(struct m2mx_dev *)platform_get_drvdata(pdev);

	v4l2_info(&dev->v4l2_dev, "Removing " MEM2MEM_NAME "\n");
	m2mx_put_ipu(dev);
	v4l2_m2m_release(dev->m2m_dev);
	video_unregister_device(dev->vfd);

	v4l2_device_unregister(&dev->v4l2_dev);

	return 0;
}

static const struct of_device_id m2mx_dt_ids[] = {
	{ .compatible = "fsl,imx-video-mem2mem" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, m2mx_dt_ids);

static struct platform_driver m2mx_pdrv = {
	.probe		= m2mx_probe,
	.remove		= m2mx_remove,
	.driver		= {
		.name	= MEM2MEM_NAME,
		.owner	= THIS_MODULE,
		.of_match_table	= m2mx_dt_ids,
	},
};

module_platform_driver(m2mx_pdrv);
