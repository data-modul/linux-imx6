#ifndef __MEDIA_IMX_IPU_H
#define __MEDIA_IMX_IPU_H

#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-of.h>
#include <media/videobuf2-dma-contig.h>
#include <video/imx-ipu-v3.h>

enum codespace_sel {
	CS_SEL_YUV = 0,
	CS_SEL_RGB,
	CS_SEL_ANY,
};

struct imx_media_pixfmt {
	u32     fourcc;
	u32     codes[4];
	int     bpp;     /* total bpp */
	enum ipu_color_space cs;
	bool    planar;  /* is a planar format */
	bool    bayer;   /* is a raw bayer format */
	bool    ipufmt;  /* is one of the IPU internal formats */
};

int ipu_enum_fmt(struct file *file, void *fh,
		struct v4l2_fmtdesc *f);
int ipu_enum_fmt_rgb(struct file *file, void *fh,
		struct v4l2_fmtdesc *f);
int ipu_enum_fmt_yuv(struct file *file, void *fh,
		struct v4l2_fmtdesc *f);
const struct imx_media_pixfmt *ipu_find_fmt_rgb(unsigned int pixelformat);
const struct imx_media_pixfmt *ipu_find_fmt_yuv(unsigned int pixelformat);
int ipu_try_fmt(struct file *file, void *fh,
		struct v4l2_format *f);
int ipu_try_fmt_rgb(struct file *file, void *fh,
		struct v4l2_format *f);
int ipu_try_fmt_yuv(struct file *file, void *fh,
		struct v4l2_format *f);
int ipu_s_fmt(struct file *file, void *fh,
		struct v4l2_format *f, struct v4l2_pix_format *pix);
int ipu_s_fmt_rgb(struct file *file, void *fh,
		struct v4l2_format *f, struct v4l2_pix_format *pix);
int ipu_s_fmt_yuv(struct file *file, void *fh,
		struct v4l2_format *f, struct v4l2_pix_format *pix);
int ipu_g_fmt(struct v4l2_format *f, struct v4l2_pix_format *pix);
int ipu_enum_framesizes(struct file *file, void *fh,
			struct v4l2_frmsizeenum *fsize);

const struct imx_media_pixfmt *imx_media_find_format(u32 fourcc,
						     enum codespace_sel cs_sel,
						     bool allow_bayer);
#endif /* __MEDIA_IMX_IPU_H */
