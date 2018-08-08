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
#include <linux/sched.h>
#include <linux/slab.h>

#include <linux/platform_device.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-ctrls.h>
#include <media/videobuf2-dma-contig.h>
#include <video/imx-ipu-v3.h>
#include <video/imx-ipu-image-convert.h>

#include "../imx-ipu.h"

/* Flags that indicate a format can be used for capture/output */
#define MEM2MEM_CAPTURE BIT(0)
#define MEM2MEM_OUTPUT  BIT(1)

#define MEM2MEM_NAME		"imx-ipuv3-scale"

#define MIN_W 16
#define MIN_H 16
#define MAX_W 4096
#define MAX_H 4096

struct m2mx_ctx;

struct m2mx_dev {
	struct v4l2_device	v4l2_dev;
	struct video_device	*vfd;
	struct device           *ipu_dev;
	struct ipu_soc          *ipu;

	struct mutex		dev_mutex;
	atomic_t		num_inst;

	struct v4l2_m2m_dev	*m2m_dev;
};

/* Per-queue, driver-specific private data */
struct mem2mem_q_data {
	struct v4l2_pix_format	cur_fmt;
	struct v4l2_rect	rect;
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
	struct mem2mem_q_data  q_data[2];

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

static struct mem2mem_q_data *get_q_data(struct m2mx_ctx *ctx,
					enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &ctx->q_data[V4L2_M2M_SRC];
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &ctx->q_data[V4L2_M2M_DST];
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
	if(ctx->ic_ctx)
		ipu_image_convert_abort(ctx->ic_ctx);
}

static void m2mx_device_run(void *priv)
{
	struct m2mx_ctx *ctx = priv;
	struct m2mx_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_buf, *dst_buf;
	struct ipu_image_convert_run *run;
	int ret;

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	run = kzalloc(sizeof(*run), GFP_KERNEL);
	if (!run)
		goto err;

	run->ctx = ctx->ic_ctx;
	run->in_phys = vb2_dma_contig_plane_dma_addr(&src_buf->vb2_buf, 0);
	run->out_phys = vb2_dma_contig_plane_dma_addr(&dst_buf->vb2_buf, 0);

	ret = ipu_image_convert_queue(run);
	if (ret < 0) {
		v4l2_err(ctx->dev->vfd->v4l2_dev,
				"%s: failed to queue: %d\n", __func__, ret);
		goto err;
	}

	return;
err:
	v4l2_m2m_buf_done(src_buf, VB2_BUF_STATE_ERROR);
	v4l2_m2m_buf_done(dst_buf, VB2_BUF_STATE_ERROR);
	v4l2_m2m_job_finish(dev->m2m_dev, ctx->fh.m2m_ctx);
}

static void m2mx_convert_complete(struct ipu_image_convert_run *run,
				  void *data)
{
	struct m2mx_ctx *ctx = data;
	struct m2mx_dev *dev = ctx->dev;
	struct vb2_v4l2_buffer *src_vb, *dst_vb;

	src_vb = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst_vb = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	dst_vb->vb2_buf.timestamp = src_vb->vb2_buf.timestamp;
	dst_vb->timecode = src_vb->timecode;

	v4l2_m2m_buf_done(src_vb ,run->status ?VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst_vb, run->status ?VB2_BUF_STATE_ERROR : VB2_BUF_STATE_DONE);

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
	strncpy(cap->bus_info, "platform:"MEM2MEM_NAME,
			sizeof(cap->bus_info) - 1);
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
	struct mem2mem_q_data *q_data;

	q_data = get_q_data(ctx, f->type);
	if (!q_data)
		return -EINVAL;

	f->fmt.pix = q_data->cur_fmt;

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

static int mem2mem_try_fmt(struct file *file, void *priv,
			   struct v4l2_format *f)
{
	struct m2mx_ctx *ctx = fh_to_ctx(priv);
	struct mem2mem_q_data *q_data = get_q_data(ctx, f->type);

	/*TODO: FIX ROTATION feature*/
/*	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {*/
		/*
		 * The IC burst reads 8 pixels at a time. Reading beyond the
		 * end of the line is usually acceptable. Those pixels are
		 * ignored, unless the IC has to write the scaled line in
		 * reverse.
		 */
/*		if (!ipu_rot_mode_is_irt(ctx->rot_mode) &&
		    ctx->rot_mode && IPU_ROT_BIT_HFLIP)
			walign = 3;
	} else {
		if (ipu_rot_mode_is_irt(ctx->rot_mode)) {
			switch (f->fmt.pix.pixelformat) {
			case V4L2_PIX_FMT_YUV420:
			case V4L2_PIX_FMT_YVU420:
			case V4L2_PIX_FMT_YUV422P:*/
				/*
				 * Align to 16x16 pixel blocks for planar 4:2:0
				 * chroma subsampled formats to guarantee
				 * 8-byte aligned line start addresses in the
				 * chroma planes.
				 */
/*				walign = 4;
				halign = 4;
				break;
			default:*/
				/*
				 * Align to 8x8 pixel IRT block size for all
				 * other formats.
				 */
/*				walign = 3;
				halign = 3;
				break;
			}
		} else {*/
			/*
			 * The IC burst writes 8 pixels at a time.
			 *
			 * TODO: support unaligned width with via
			 * V4L2_SEL_TGT_COMPOSE_PADDED.
			 */
/*			walign = 3;
		}
	}*/

	ipu_try_fmt(file, priv, f);

	if (f->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) {
		f->fmt.pix.colorspace = q_data->cur_fmt.colorspace;
		f->fmt.pix.ycbcr_enc = q_data->cur_fmt.ycbcr_enc;
		f->fmt.pix.xfer_func = q_data->cur_fmt.xfer_func;
		f->fmt.pix.quantization = q_data->cur_fmt.quantization;
	} else if (f->fmt.pix.colorspace == V4L2_COLORSPACE_DEFAULT) {
		f->fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;
		f->fmt.pix.ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
		f->fmt.pix.xfer_func = V4L2_XFER_FUNC_DEFAULT;
		f->fmt.pix.quantization = V4L2_QUANTIZATION_DEFAULT;
	}

	return 0;
}

static int mem2mem_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct mem2mem_q_data *q_data;
	struct m2mx_ctx *ctx = fh_to_ctx(priv);
	struct vb2_queue *vq;
	int ret;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq)) {
		v4l2_err(ctx->dev->vfd->v4l2_dev, "%s queue busy\n",
			 __func__);
		return -EBUSY;
	}

	q_data = get_q_data(ctx, f->type);

	ret = mem2mem_try_fmt(file, priv, f);
	if (ret < 0)
		return ret;

	q_data->cur_fmt.width = f->fmt.pix.width;
	q_data->cur_fmt.height = f->fmt.pix.height;
	q_data->cur_fmt.pixelformat = f->fmt.pix.pixelformat;
	q_data->cur_fmt.field = f->fmt.pix.field;
	q_data->cur_fmt.bytesperline = f->fmt.pix.bytesperline;
	q_data->cur_fmt.sizeimage = f->fmt.pix.sizeimage;

	/* Reset cropping/composing rectangle */
	q_data->rect.left = 0;
	q_data->rect.top = 0;
	q_data->rect.width = q_data->cur_fmt.width;
	q_data->rect.height = q_data->cur_fmt.height;

	if (f->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		/* Set colorimetry on the output queue */
		q_data->cur_fmt.colorspace = f->fmt.pix.colorspace;
		q_data->cur_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
		q_data->cur_fmt.xfer_func = f->fmt.pix.xfer_func;
		q_data->cur_fmt.quantization = f->fmt.pix.quantization;
		/* Propagate colorimetry to the capture queue */
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		q_data->cur_fmt.colorspace = f->fmt.pix.colorspace;
		q_data->cur_fmt.ycbcr_enc = f->fmt.pix.ycbcr_enc;
		q_data->cur_fmt.xfer_func = f->fmt.pix.xfer_func;
		q_data->cur_fmt.quantization = f->fmt.pix.quantization;
	}

	/*
	 * TODO: Setting colorimetry on the capture queue is currently not
	 * supported by the V4L2 API
	 */

	return 0;
}

static int mem2mem_g_selection(struct file *file, void *priv,
			       struct v4l2_selection *s)
{
	struct m2mx_ctx *ctx = fh_to_ctx(priv);
	struct mem2mem_q_data *q_data;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
		break;
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_PADDED:
		if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;
		q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		break;
	default:
		return -EINVAL;
	}

	if (s->target == V4L2_SEL_TGT_CROP ||
	    s->target == V4L2_SEL_TGT_COMPOSE) {
		s->r = q_data->rect;
	} else {
		s->r.left = 0;
		s->r.top = 0;
		s->r.width = q_data->cur_fmt.width;
		s->r.height = q_data->cur_fmt.height;
	}

	return 0;
}

static int mem2mem_s_selection(struct file *file, void *priv,
			       struct v4l2_selection *s)
{
	struct m2mx_ctx *ctx = fh_to_ctx(priv);
	struct mem2mem_q_data *q_data;

	switch (s->target) {
	case V4L2_SEL_TGT_CROP:
		if (s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_COMPOSE:
		if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	if (s->type != V4L2_BUF_TYPE_VIDEO_CAPTURE ||
	    s->type != V4L2_BUF_TYPE_VIDEO_OUTPUT)
		return -EINVAL;

	q_data = get_q_data(ctx, s->type);

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

static int m2mx_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct m2mx_ctx *ctx = container_of(ctrl->handler,
					     struct m2mx_ctx, ctrl_hdlr);
	enum ipu_rotate_mode rot_mode;
	bool hflip, vflip;
	int rotation;
	int ret;

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
		return -EINVAL;
	}

	ret = ipu_degrees_to_rot_mode(&rot_mode, rotation, hflip, vflip);
	if (ret)
		return ret;
	

	if (rot_mode != ctx->rot_mode) {
		struct vb2_queue *cap_q;
		
		cap_q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
		
		/* can't change rotation mid-streaming */
		if (vb2_is_streaming(cap_q))
			return -EBUSY;
		
		ctx->rotation = rotation;
		ctx->hflip = hflip;
		ctx->vflip = vflip;
		ctx->rot_mode = rot_mode;
	}

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
	.vidioc_try_fmt_vid_cap	= mem2mem_try_fmt,
	.vidioc_s_fmt_vid_cap	= mem2mem_s_fmt,

	.vidioc_enum_fmt_vid_out = vidioc_enum_fmt_vid_out,
	.vidioc_g_fmt_vid_out	= vidioc_g_fmt_vid_out,
	.vidioc_try_fmt_vid_out	= mem2mem_try_fmt,
	.vidioc_s_fmt_vid_out	= mem2mem_s_fmt,

	.vidioc_g_selection	= mem2mem_g_selection,
	.vidioc_s_selection	= mem2mem_s_selection,

	.vidioc_reqbufs		= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf	= v4l2_m2m_ioctl_querybuf,

	.vidioc_qbuf		= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf		= v4l2_m2m_ioctl_dqbuf,
	.vidioc_expbuf		= v4l2_m2m_ioctl_expbuf,
	.vidioc_create_bufs	= v4l2_m2m_ioctl_create_bufs,

	.vidioc_streamon	= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff	= v4l2_m2m_ioctl_streamoff,
};

/*
 * Queue operations
 */

static int m2mx_queue_setup(struct vb2_queue *vq,
			     unsigned int *nbuffers, unsigned int *nplanes,
			     unsigned int sizes[], struct device *alloc_ctxs[])
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(vq);
	struct mem2mem_q_data *q_data;
	unsigned int  count = *nbuffers;
	struct v4l2_pix_format *pix;

	q_data = get_q_data(ctx, vq->type);
	pix = &q_data->cur_fmt;

	*nplanes = 1;
	*nbuffers = count;
	sizes[0] = pix->sizeimage;

	dev_dbg(ctx->dev->ipu_dev, "get %d buffer(s) of size %d each.\n",
					count, pix->sizeimage);
	return 0;
}

static int m2mx_buf_prepare(struct vb2_buffer *vb)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct mem2mem_q_data *q_data;
	struct v4l2_pix_format *pix;
	unsigned int plane_size, payload;

	dev_dbg(ctx->dev->ipu_dev, "type: %d\n", vb->vb2_queue->type);

	q_data = get_q_data(ctx, vb->vb2_queue->type);
	pix = &q_data->cur_fmt;
	plane_size = pix->sizeimage;


	if (vb2_plane_size(vb, 0) < plane_size) {
		dev_dbg(ctx->dev->ipu_dev,
			"%s data will not fit into plane (%lu < %lu)\n",
			__func__, vb2_plane_size(vb, 0), (long)plane_size);
		return -EINVAL;
	}

	payload = pix->bytesperline * pix->height;
	if (pix->pixelformat == V4L2_PIX_FMT_YUV420 ||
		    pix->pixelformat == V4L2_PIX_FMT_YVU420 ||
		    pix->pixelformat == V4L2_PIX_FMT_NV12)
		payload = payload * 3 / 2;
	else if (pix->pixelformat == V4L2_PIX_FMT_YUV422P ||
			pix->pixelformat == V4L2_PIX_FMT_NV16)
		payload *= 2;

	vb2_set_plane_payload(vb, 0, payload);

	return 0;
}

static void m2mx_buf_queue(struct vb2_buffer *vb)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, to_vb2_v4l2_buffer(vb));
}

static void ipu_image_from_q_data(struct ipu_image *im,
		struct mem2mem_q_data *q_data)
{
	im->pix.width = q_data->cur_fmt.width;
	im->pix.height = q_data->cur_fmt.height;
	im->pix.bytesperline = q_data->cur_fmt.bytesperline;
	im->pix.pixelformat = q_data->cur_fmt.pixelformat;
	im->rect = q_data->rect;
}

static int m2mx_start_streaming(struct vb2_queue *q, unsigned int count)
{
	const enum ipu_ic_task ic_task = IC_TASK_POST_PROCESSOR;
	struct m2mx_ctx *ctx = vb2_get_drv_priv(q);
	struct m2mx_dev *priv = ctx->dev;
	struct ipu_soc *ipu = priv->ipu;
	struct mem2mem_q_data *q_data;
	struct vb2_queue *other_q;
	struct ipu_image in, out;


	other_q = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			(q->type == V4L2_BUF_TYPE_VIDEO_CAPTURE) ?
			  V4L2_BUF_TYPE_VIDEO_OUTPUT :
			  V4L2_BUF_TYPE_VIDEO_CAPTURE);
	if (!vb2_is_streaming(other_q))
		return 0;

	if (ctx->ic_ctx) {
		v4l2_warn(ctx->dev->vfd->v4l2_dev, "removing old ICC\n");
		ipu_image_convert_unprepare(ctx->ic_ctx);
	}

	q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_OUTPUT);
	ipu_image_from_q_data(&in, q_data);

	q_data = get_q_data(ctx, V4L2_BUF_TYPE_VIDEO_CAPTURE);
	ipu_image_from_q_data(&out, q_data);

	ctx->ic_ctx = ipu_image_convert_prepare(ipu, ic_task, &in, &out,
						ctx->rot_mode,
						m2mx_convert_complete, ctx);
	if (IS_ERR(ctx->ic_ctx)) {
		struct vb2_v4l2_buffer *buf;
		int ret = PTR_ERR(ctx->ic_ctx);

		ctx->ic_ctx = NULL;
		v4l2_err(ctx->dev->vfd->v4l2_dev, "%s: error %d\n",
			 __func__, ret);
		while ((buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_QUEUED);
		while ((buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_QUEUED);
		return ret;
	}

	return 0;
}

static void m2mx_stop_streaming(struct vb2_queue *q)
{
	struct m2mx_ctx *ctx = vb2_get_drv_priv(q);
	struct vb2_v4l2_buffer *buf;

	if (ctx->ic_ctx) {
		ipu_image_convert_unprepare(ctx->ic_ctx);
		ctx->ic_ctx = NULL;
	}

	if (q->type == V4L2_BUF_TYPE_VIDEO_OUTPUT) {
		while ((buf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	} else {
		while ((buf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx)))
			v4l2_m2m_buf_done(buf, VB2_BUF_STATE_ERROR);
	}
}

static struct vb2_ops m2mx_qops = {
	.queue_setup	 = m2mx_queue_setup,
	.buf_prepare	 = m2mx_buf_prepare,
	.buf_queue	 = m2mx_buf_queue,
	.wait_prepare	 = vb2_ops_wait_prepare,
	.wait_finish	 = vb2_ops_wait_finish,
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
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->ops = &m2mx_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->dev_mutex;
	src_vq->dev = ctx->dev->ipu_dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	memset(dst_vq, 0, sizeof(*dst_vq));
	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->ops = &m2mx_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->dev_mutex;
	dst_vq->dev = ctx->dev->ipu_dev;

	return vb2_queue_init(dst_vq);
}


#define DEFAULT_WIDTH	720
#define DEFAULT_HEIGHT	576
static const struct mem2mem_q_data mem2mem_q_data_default = {
	.cur_fmt = {
		.width = DEFAULT_WIDTH,
		.height = DEFAULT_HEIGHT,
		.pixelformat = V4L2_PIX_FMT_YUV420,
		.field = V4L2_FIELD_NONE,
		.bytesperline = DEFAULT_WIDTH,
		.sizeimage = DEFAULT_WIDTH * DEFAULT_HEIGHT * 3 / 2,
		.colorspace = V4L2_COLORSPACE_SRGB,
	},
	.rect = {
		.width = DEFAULT_WIDTH,
		.height = DEFAULT_HEIGHT,
	},
};


/*
 * File operations
 */
static int m2mx_open(struct file *file)
{
	struct m2mx_dev *dev = video_drvdata(file);
	struct m2mx_ctx *ctx = NULL;
	int ret = 0;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
	}

	ctx->rot_mode = IPU_ROTATE_NONE;
	
	v4l2_fh_init(&ctx->fh, dev->vfd);
	file->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);
	ctx->dev = dev;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(dev->m2m_dev, ctx, &queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto error_fh;
	}

	ret = m2mx_init_controls(ctx);
	if (ret)
		goto error_m2m_ctx;

	ctx->fh.ctrl_handler = &ctx->ctrl_hdlr;
	ctx->q_data[V4L2_M2M_SRC] = mem2mem_q_data_default;
	ctx->q_data[V4L2_M2M_DST] = mem2mem_q_data_default;

	atomic_inc(&dev->num_inst);

	dev_dbg(dev->ipu_dev, "Created instance %p, m2m_ctx: %p\n", ctx,
			ctx->fh.m2m_ctx);
	return 0;

error_m2m_ctx:
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
error_fh:
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int m2mx_release(struct file *file)
{
	struct m2mx_dev *dev = video_drvdata(file);
	struct m2mx_ctx *ctx = fh_to_ctx(file->private_data);

	dev_dbg(dev->ipu_dev, "Releasing instance %p\n", ctx);

	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);

	atomic_dec(&dev->num_inst);
	return 0;
}

static const struct v4l2_file_operations m2mx_fops = {
	.owner		= THIS_MODULE,
	.open		= m2mx_open,
	.release	= m2mx_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static struct video_device m2mx_videodev = {
	.name		= MEM2MEM_NAME,
	.fops		= &m2mx_fops,
	.ioctl_ops	= &m2mx_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release,
	.vfl_dir	= VFL_DIR_M2M,
	.tvnorms	= V4L2_STD_NTSC | V4L2_STD_PAL | V4L2_STD_SECAM,
	.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static struct v4l2_m2m_ops m2m_ops = {
	.device_run	= m2mx_device_run,
	.job_abort	= m2mx_job_abort,
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

static u64 vout_dmamask = ~(u32)0;

static int m2mx_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct m2mx_dev *dev;
	struct video_device *vfd;
	int ret;

	pdev->dev.dma_mask = &vout_dmamask;
	pdev->dev.coherent_dma_mask = DMA_BIT_MASK(32);

	dev = devm_kzalloc(&pdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	ret = v4l2_device_register(&pdev->dev, &dev->v4l2_dev);
	if (ret)
		return ret;

	/* get our IPU */
	dev->ipu = m2mx_get_ipu(dev, node);
	if (IS_ERR(dev->ipu)) {
		v4l2_err(&dev->v4l2_dev, "could not get ipu\n");
		ret = PTR_ERR(dev->ipu);
		goto unreg_dev;
	}

	mutex_init(&dev->dev_mutex);
	atomic_set(&dev->num_inst, 0);

	vfd = video_device_alloc();
	if (!vfd) {
		return -ENOMEM;
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
MODULE_DESCRIPTION("i.MX IPUv3 mem2mem scaler/CSC driver");
MODULE_AUTHOR("Sascha Hauer <s.hauer@pengutronix.de>");
MODULE_LICENSE("GPL");
