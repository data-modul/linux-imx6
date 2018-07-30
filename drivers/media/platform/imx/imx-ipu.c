/*
 * i.MX IPUv3 common v4l2 support
 *
 * Copyright (C) 2011 Pengutronix, Sascha Hauer <kernel@pengutronix.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <media/v4l2-common.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-ioctl.h>

#include "imx-ipu.h"


/* List of pixel formats for the subdevs. This must be a super-set of
 * the formats supported by the ipu image converter.
 */

static const struct imx_media_pixfmt yuv_formats[] = {
	{
		.fourcc	= V4L2_PIX_FMT_UYVY,
		.codes  = {
			MEDIA_BUS_FMT_UYVY8_2X8,
			MEDIA_BUS_FMT_UYVY8_1X16
		},
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 16,
	}, {
		.fourcc	= V4L2_PIX_FMT_YUYV,
		.codes  = {
			MEDIA_BUS_FMT_YUYV8_2X8,
			MEDIA_BUS_FMT_YUYV8_1X16
		},
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 16,
	},
	/***
	 * non-mbus YUV formats start here. NOTE! when adding non-mbus
	 * formats, NUM_NON_MBUS_YUV_FORMATS must be updated below.
	 ***/
	{
		.fourcc	= V4L2_PIX_FMT_YUV420,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 12,
		.planar = true,
	}, {
		.fourcc = V4L2_PIX_FMT_YVU420,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 12,
		.planar = true,
	}, {
		.fourcc = V4L2_PIX_FMT_YUV422P,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 16,
		.planar = true,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 12,
		.planar = true,
	}, {
		.fourcc = V4L2_PIX_FMT_NV16,
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 16,
		.planar = true,
	},
};

#define NUM_NON_MBUS_YUV_FORMATS 5
#define NUM_YUV_FORMATS ARRAY_SIZE(yuv_formats)
#define NUM_MBUS_YUV_FORMATS (NUM_YUV_FORMATS - NUM_NON_MBUS_YUV_FORMATS)

static const struct imx_media_pixfmt rgb_formats[] = {
	{
		.fourcc	= V4L2_PIX_FMT_RGB565,
		.codes  = {MEDIA_BUS_FMT_RGB565_2X8_LE},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 16,
	}, {
		.fourcc	= V4L2_PIX_FMT_RGB24,
		.codes  = {
			MEDIA_BUS_FMT_RGB888_1X24,
			MEDIA_BUS_FMT_RGB888_2X12_LE
		},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 24,
	}, {
		.fourcc	= V4L2_PIX_FMT_RGB32,
		.codes  = {MEDIA_BUS_FMT_ARGB8888_1X32},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 32,
		.ipufmt = true,
	},
	/*** raw bayer and grayscale formats start here ***/
	{
		.fourcc = V4L2_PIX_FMT_SBGGR8,
		.codes  = {MEDIA_BUS_FMT_SBGGR8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SGBRG8,
		.codes  = {MEDIA_BUS_FMT_SGBRG8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SGRBG8,
		.codes  = {MEDIA_BUS_FMT_SGRBG8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SRGGB8,
		.codes  = {MEDIA_BUS_FMT_SRGGB8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_SBGGR16,
		.codes  = {
			MEDIA_BUS_FMT_SBGGR10_1X10,
			MEDIA_BUS_FMT_SBGGR12_1X12,
			MEDIA_BUS_FMT_SBGGR14_1X14,
			MEDIA_BUS_FMT_SBGGR16_1X16
		},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 16,
		.bayer  = true,
	}, {
		.fourcc = V4L2_PIX_FMT_GREY,
		.codes = {MEDIA_BUS_FMT_Y8_1X8},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 8,
		.bayer  = true,
	},
	/***
	 * non-mbus RGB formats start here. NOTE! when adding non-mbus
	 * formats, NUM_NON_MBUS_RGB_FORMATS must be updated below.
	 ***/
	{
		.fourcc	= V4L2_PIX_FMT_BGR24,
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 24,
	}, {
		.fourcc	= V4L2_PIX_FMT_BGR32,
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 32,
	},
};

#define NUM_NON_MBUS_RGB_FORMATS 2
#define NUM_RGB_FORMATS ARRAY_SIZE(rgb_formats)
#define NUM_MBUS_RGB_FORMATS (NUM_RGB_FORMATS - NUM_NON_MBUS_RGB_FORMATS)


const struct imx_media_pixfmt *ipu_find_fmt_yuv(unsigned int pixelformat)
{
	const struct imx_media_pixfmt *fmt;
	int i;

	for (i = 0; i < ARRAY_SIZE(yuv_formats); i++) {
		fmt = &yuv_formats[i];
		if (fmt->fourcc == pixelformat)
			return fmt;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ipu_find_fmt_yuv);

const struct imx_media_pixfmt *ipu_find_fmt_rgb(unsigned int pixelformat)
{
	const struct imx_media_pixfmt *fmt;
	int i;

	for (i = 0; i < ARRAY_SIZE(rgb_formats); i++) {
		fmt = &rgb_formats[i];
		if (fmt->fourcc == pixelformat)
			return fmt;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ipu_find_fmt_rgb);

static const struct imx_media_pixfmt *ipu_find_fmt(unsigned long pixelformat)
{
	const struct imx_media_pixfmt *fmt;

	fmt = ipu_find_fmt_yuv(pixelformat);
	if (fmt)
		return fmt;
	fmt = ipu_find_fmt_rgb(pixelformat);

	return fmt;
}
EXPORT_SYMBOL_GPL(ipu_find_fmt);

int ipu_try_fmt(struct file *file, void *fh,
		struct v4l2_format *f)
{
	const struct imx_media_pixfmt *fmt;
	unsigned int walign, halign;
	u32 stride;

	fmt = imx_media_find_format(f->fmt.pix.pixelformat, CS_SEL_ANY, false);
	if (!fmt) {
		f->fmt.pix.pixelformat = V4L2_PIX_FMT_RGB32;
		fmt = imx_media_find_format(V4L2_PIX_FMT_RGB32, CS_SEL_RGB,
				false);
	}

	/*
	 * Horizontally/vertically chroma subsampled formats must have even
	 * width/height.
	*/
	switch (f->fmt.pix.pixelformat) {
		case V4L2_PIX_FMT_YUV420:
		case V4L2_PIX_FMT_YVU420:
		case V4L2_PIX_FMT_NV12:
			walign = 1;
			halign = 1;
			break;
		case V4L2_PIX_FMT_YUV422P:
		case V4L2_PIX_FMT_NV16:
			walign = 1;
			halign = 0;
			break;
		default:
			walign = 3;
			halign = 0;
			break;
	}

	v4l_bound_align_image(&f->fmt.pix.width, 16, 4096, walign,
			      &f->fmt.pix.height, 16, 4096, halign, 0);

	stride = fmt->planar ? f->fmt.pix.width
		    : (f->fmt.pix.width * fmt->bpp) >> 3;
	switch (f->fmt.pix.pixelformat) {
		case V4L2_PIX_FMT_YUV420:
		case V4L2_PIX_FMT_YVU420:
		case V4L2_PIX_FMT_YUV422P:
			stride = round_up(stride, 16);
			break;
		default:
			stride = round_up(stride, 8);
			break;
	}

	f->fmt.pix.field = V4L2_FIELD_NONE;
	f->fmt.pix.bytesperline = stride;
	f->fmt.pix.sizeimage = fmt->planar ?
			       (stride * f->fmt.pix.height * fmt->bpp) >> 3 :
			       stride * f->fmt.pix.height;

	f->fmt.pix.priv = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_try_fmt);

int ipu_try_fmt_rgb(struct file *file, void *fh,
		struct v4l2_format *f)
{
	const struct imx_media_pixfmt *fmt;

	fmt = ipu_find_fmt_rgb(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	return ipu_try_fmt(file, fh, f);
}
EXPORT_SYMBOL_GPL(ipu_try_fmt_rgb);

int ipu_try_fmt_yuv(struct file *file, void *fh,
		struct v4l2_format *f)
{
	const struct imx_media_pixfmt *fmt;

	fmt = ipu_find_fmt_yuv(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	return ipu_try_fmt(file, fh, f);
}
EXPORT_SYMBOL_GPL(ipu_try_fmt_yuv);

int ipu_enum_fmt_rgb(struct file *file, void *fh,
		struct v4l2_fmtdesc *f)
{
	const struct imx_media_pixfmt *fmt;

	if (f->index >= ARRAY_SIZE(rgb_formats))
		return -EINVAL;

	fmt = &rgb_formats[f->index];

	f->pixelformat = fmt->fourcc;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_enum_fmt_rgb);

int ipu_enum_fmt_yuv(struct file *file, void *fh,
		struct v4l2_fmtdesc *f)
{
	const struct imx_media_pixfmt *fmt;

	if (f->index >= ARRAY_SIZE(yuv_formats))
		return -EINVAL;

	fmt = &yuv_formats[f->index];

	f->pixelformat = fmt->fourcc;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_enum_fmt_yuv);

int ipu_enum_fmt(struct file *file, void *fh,
		struct v4l2_fmtdesc *f)
{
	const struct imx_media_pixfmt *fmt;
	int index = f->index;

	if (index >= ARRAY_SIZE(yuv_formats)) {
		index -= ARRAY_SIZE(yuv_formats);
		if (index >= ARRAY_SIZE(rgb_formats))
			return -EINVAL;
		fmt = &rgb_formats[index];
	} else {
		fmt = &yuv_formats[index];
	}

	f->pixelformat = fmt->fourcc;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_enum_fmt);

int ipu_s_fmt(struct file *file, void *fh,
		struct v4l2_format *f, struct v4l2_pix_format *pix)
{
	int ret;

	ret = ipu_try_fmt(file, fh, f);
	if (ret)
		return ret;

	*pix = f->fmt.pix;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_s_fmt);

int ipu_s_fmt_rgb(struct file *file, void *fh,
		struct v4l2_format *f, struct v4l2_pix_format *pix)
{
	const struct imx_media_pixfmt *fmt;

	fmt = ipu_find_fmt_rgb(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	return ipu_s_fmt(file, fh, f, pix);
}
EXPORT_SYMBOL_GPL(ipu_s_fmt_rgb);

int ipu_s_fmt_yuv(struct file *file, void *fh,
		struct v4l2_format *f, struct v4l2_pix_format *pix)
{
	const struct imx_media_pixfmt *fmt;

	fmt = ipu_find_fmt_yuv(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	return ipu_s_fmt(file, fh, f, pix);
}
EXPORT_SYMBOL_GPL(ipu_s_fmt_yuv);

int ipu_g_fmt(struct v4l2_format *f, struct v4l2_pix_format *pix)
{

	f->fmt.pix = *pix;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_g_fmt);

int ipu_enum_framesizes(struct file *file, void *fh,
			struct v4l2_frmsizeenum *fsize)
{
	const struct imx_media_pixfmt *fmt;

	if (fsize->index != 0)
		return -EINVAL;

	fmt = ipu_find_fmt(fsize->pixel_format);
	if (!fmt)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_CONTINUOUS;
	fsize->stepwise.min_width = 1;
	fsize->stepwise.min_height = 1;
	fsize->stepwise.max_width = 4096;
	fsize->stepwise.max_height = 4096;
	fsize->stepwise.step_width = fsize->stepwise.step_height = 1;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_enum_framesizes);


static const struct imx_media_pixfmt *__find_format(u32 fourcc,
				       u32 code,
				       bool allow_non_mbus,
				       bool allow_bayer,
				       const struct imx_media_pixfmt *array,
				       u32 array_size)
{
	const struct imx_media_pixfmt *fmt;
	int i, j;

	for (i = 0; i < array_size; i++) {
		fmt = &array[i];

		if ((!allow_non_mbus && !fmt->codes[0]) ||
		    (!allow_bayer && fmt->bayer))
			continue;

		if (fourcc && fmt->fourcc == fourcc)
			return fmt;

		if (!code)
			continue;

		for (j = 0; fmt->codes[j]; j++) {
			if (code == fmt->codes[j])
				return fmt;
		}
	}
	return NULL;
}

static const struct imx_media_pixfmt *find_format(u32 fourcc,
						  u32 code,
						  enum codespace_sel cs_sel,
						  bool allow_non_mbus,
						  bool allow_bayer)
{
	const struct imx_media_pixfmt *ret;

	switch (cs_sel) {
	case CS_SEL_YUV:
		return __find_format(fourcc, code, allow_non_mbus, allow_bayer,
				     yuv_formats, NUM_YUV_FORMATS);
	case CS_SEL_RGB:
		return __find_format(fourcc, code, allow_non_mbus, allow_bayer,
				     rgb_formats, NUM_RGB_FORMATS);
	case CS_SEL_ANY:
		ret = __find_format(fourcc, code, allow_non_mbus, allow_bayer,
				    yuv_formats, NUM_YUV_FORMATS);
		if (ret)
			return ret;
		return __find_format(fourcc, code, allow_non_mbus, allow_bayer,
				     rgb_formats, NUM_RGB_FORMATS);
	default:
		return NULL;
	}
}


const struct imx_media_pixfmt *
imx_media_find_format(u32 fourcc, enum codespace_sel cs_sel, bool allow_bayer)
{
	return find_format(fourcc, 0, cs_sel, true, allow_bayer);
}
EXPORT_SYMBOL_GPL(imx_media_find_format);

MODULE_LICENSE("GPL");
