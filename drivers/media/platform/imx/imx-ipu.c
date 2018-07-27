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

/*
 * These formats are in order of preference: interleaved YUV first,
 * because those are the most bandwidth efficient, followed by
 * chroma-interleaved formats, and planar formats last.
 * In each category, YUV 4:2:0 may be preferrable to 4:2:2 for bandwidth
 * reasons, if the IDMAC channel supports double read/write reduction
 * (all write channels, VDIC read channels).
 */
static struct ipu_fmt ipu_fmt_yuv[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.bytes_per_pixel = 2,
	}, {
		.fourcc = V4L2_PIX_FMT_UYVY,
		.bytes_per_pixel = 2,
	}, {
		.fourcc = V4L2_PIX_FMT_NV12,
		.bytes_per_pixel = 1,
	}, {
		.fourcc = V4L2_PIX_FMT_NV16,
		.bytes_per_pixel = 1,
	}, {
		.fourcc = V4L2_PIX_FMT_YUV420,
		.bytes_per_pixel = 1,
	}, {
		.fourcc = V4L2_PIX_FMT_YVU420,
		.bytes_per_pixel = 1,
	}, {
		.fourcc = V4L2_PIX_FMT_YUV422P,
		.bytes_per_pixel = 1,
	},
};

static struct ipu_fmt ipu_fmt_rgb[] = {
	{
		.fourcc = V4L2_PIX_FMT_RGB32,
		.bytes_per_pixel = 4,
	}, {
		.fourcc = V4L2_PIX_FMT_RGB24,
		.bytes_per_pixel = 3,
	}, {
		.fourcc = V4L2_PIX_FMT_BGR24,
		.bytes_per_pixel = 3,
	}, {
		.fourcc = V4L2_PIX_FMT_RGB565,
		.bytes_per_pixel = 2,
	},
	{
		.fourcc = V4L2_PIX_FMT_BGR32,
		.bytes_per_pixel = 4,
	},
};


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

static const struct imx_media_pixfmt ipu_yuv_formats[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUV32,
		.codes  = {MEDIA_BUS_FMT_AYUV8_1X32},
		.cs     = IPUV3_COLORSPACE_YUV,
		.bpp    = 32,
		.ipufmt = true,
	},
};

#define NUM_IPU_YUV_FORMATS ARRAY_SIZE(ipu_yuv_formats)

static const struct imx_media_pixfmt ipu_rgb_formats[] = {
	{
		.fourcc	= V4L2_PIX_FMT_RGB32,
		.codes  = {MEDIA_BUS_FMT_ARGB8888_1X32},
		.cs     = IPUV3_COLORSPACE_RGB,
		.bpp    = 32,
		.ipufmt = true,
	},
};

#define NUM_IPU_RGB_FORMATS ARRAY_SIZE(ipu_rgb_formats)


struct ipu_fmt *ipu_find_fmt_yuv(unsigned int pixelformat)
{
	struct ipu_fmt *fmt;
	int i;

	for (i = 0; i < ARRAY_SIZE(ipu_fmt_yuv); i++) {
		fmt = &ipu_fmt_yuv[i];
		if (fmt->fourcc == pixelformat)
			return fmt;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ipu_find_fmt_yuv);

struct ipu_fmt *ipu_find_fmt_rgb(unsigned int pixelformat)
{
	struct ipu_fmt *fmt;
	int i;

	for (i = 0; i < ARRAY_SIZE(ipu_fmt_rgb); i++) {
		fmt = &ipu_fmt_rgb[i];
		if (fmt->fourcc == pixelformat)
			return fmt;
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(ipu_find_fmt_rgb);

static struct ipu_fmt *ipu_find_fmt(unsigned long pixelformat)
{
	struct ipu_fmt *fmt;

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
	struct ipu_fmt *fmt;

	v4l_bound_align_image(&f->fmt.pix.width, 16, 4096, 3,
			      &f->fmt.pix.height, 16, 4096, 1, 0);

	f->fmt.pix.field = V4L2_FIELD_NONE;

	fmt = ipu_find_fmt(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	f->fmt.pix.bytesperline = f->fmt.pix.width * fmt->bytes_per_pixel;
	f->fmt.pix.sizeimage = f->fmt.pix.bytesperline * f->fmt.pix.height;
	if (fmt->fourcc == V4L2_PIX_FMT_YUV420 ||
	    fmt->fourcc == V4L2_PIX_FMT_YVU420 ||
	    fmt->fourcc == V4L2_PIX_FMT_NV12)
		f->fmt.pix.sizeimage = f->fmt.pix.sizeimage * 3 / 2;
	else if (fmt->fourcc == V4L2_PIX_FMT_YUV422P ||
		 fmt->fourcc == V4L2_PIX_FMT_NV16)
		f->fmt.pix.sizeimage *= 2;

	f->fmt.pix.priv = 0;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_try_fmt);

int ipu_try_fmt_rgb(struct file *file, void *fh,
		struct v4l2_format *f)
{
	struct ipu_fmt *fmt;

	fmt = ipu_find_fmt_rgb(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	return ipu_try_fmt(file, fh, f);
}
EXPORT_SYMBOL_GPL(ipu_try_fmt_rgb);

int ipu_try_fmt_yuv(struct file *file, void *fh,
		struct v4l2_format *f)
{
	struct ipu_fmt *fmt;

	fmt = ipu_find_fmt_yuv(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	return ipu_try_fmt(file, fh, f);
}
EXPORT_SYMBOL_GPL(ipu_try_fmt_yuv);

int ipu_enum_fmt_rgb(struct file *file, void *fh,
		struct v4l2_fmtdesc *f)
{
	struct ipu_fmt *fmt;

	if (f->index >= ARRAY_SIZE(ipu_fmt_rgb))
		return -EINVAL;

	fmt = &ipu_fmt_rgb[f->index];

	f->pixelformat = fmt->fourcc;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_enum_fmt_rgb);

int ipu_enum_fmt_yuv(struct file *file, void *fh,
		struct v4l2_fmtdesc *f)
{
	struct ipu_fmt *fmt;

	if (f->index >= ARRAY_SIZE(ipu_fmt_yuv))
		return -EINVAL;

	fmt = &ipu_fmt_yuv[f->index];

	f->pixelformat = fmt->fourcc;

	return 0;
}
EXPORT_SYMBOL_GPL(ipu_enum_fmt_yuv);

int ipu_enum_fmt(struct file *file, void *fh,
		struct v4l2_fmtdesc *f)
{
	struct ipu_fmt *fmt;
	int index = f->index;

	if (index >= ARRAY_SIZE(ipu_fmt_yuv)) {
		index -= ARRAY_SIZE(ipu_fmt_yuv);
		if (index >= ARRAY_SIZE(ipu_fmt_rgb))
			return -EINVAL;
		fmt = &ipu_fmt_rgb[index];
	} else {
		fmt = &ipu_fmt_yuv[index];
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
	struct ipu_fmt *fmt;

	fmt = ipu_find_fmt_rgb(f->fmt.pix.pixelformat);
	if (!fmt)
		return -EINVAL;

	return ipu_s_fmt(file, fh, f, pix);
}
EXPORT_SYMBOL_GPL(ipu_s_fmt_rgb);

int ipu_s_fmt_yuv(struct file *file, void *fh,
		struct v4l2_format *f, struct v4l2_pix_format *pix)
{
	struct ipu_fmt *fmt;

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
	struct ipu_fmt *fmt;

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
/*--------------------------------------------------------------------*/

static const
struct imx_media_pixfmt *__find_format(u32 fourcc,
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
