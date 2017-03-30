#ifndef __DRM_IPUV3_VOUT_H
#define __DRM_IPUV3_VOUT_H

struct ipu_vout_pdata {
	struct ipu_soc *ipu;
	struct ipu_dp *dp;
	struct ipuv3_channel *ipu_ch;
};

struct ipu_ovl_pdata {
	struct ipu_soc *ipu;
	struct ipu_dp *dp;
	struct ipuv3_channel *ipu_ch;
	int dma[2];
};

#endif /* __DRM_IPUV3_VOUT_H */
