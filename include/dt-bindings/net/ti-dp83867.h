/*
 * Device Tree constants for the Texas Instruments DP83867 PHY
 *
 * Author: Dan Murphy <dmurphy@ti.com>
 *
 * Copyright:   (C) 2015 Texas Instruments, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _DT_BINDINGS_TI_DP83867_H
#define _DT_BINDINGS_TI_DP83867_H

/* PHY CTRL bits */
#define DP83867_PHYCR_FIFO_DEPTH_3_B_NIB	0x00
#define DP83867_PHYCR_FIFO_DEPTH_4_B_NIB	0x01
#define DP83867_PHYCR_FIFO_DEPTH_6_B_NIB	0x02
#define DP83867_PHYCR_FIFO_DEPTH_8_B_NIB	0x03

/* RGMIIDCTL internal delay for rx and tx */
#define	DP83867_RGMIIDCTL_250_PS	0x0
#define	DP83867_RGMIIDCTL_500_PS	0x1
#define	DP83867_RGMIIDCTL_750_PS	0x2
#define	DP83867_RGMIIDCTL_1_NS		0x3
#define	DP83867_RGMIIDCTL_1_25_NS	0x4
#define	DP83867_RGMIIDCTL_1_50_NS	0x5
#define	DP83867_RGMIIDCTL_1_75_NS	0x6
#define	DP83867_RGMIIDCTL_2_00_NS	0x7
#define	DP83867_RGMIIDCTL_2_25_NS	0x8
#define	DP83867_RGMIIDCTL_2_50_NS	0x9
#define	DP83867_RGMIIDCTL_2_75_NS	0xa
#define	DP83867_RGMIIDCTL_3_00_NS	0xb
#define	DP83867_RGMIIDCTL_3_25_NS	0xc
#define	DP83867_RGMIIDCTL_3_50_NS	0xd
#define	DP83867_RGMIIDCTL_3_75_NS	0xe
#define	DP83867_RGMIIDCTL_4_00_NS	0xf

/* CLK_OUT source */
#define	DP83867_CLK_OUT_CHAN_D_TX_CLK			0x0B	/* 01011  Channel D transmit clock */
#define	DP83867_CLK_OUT_CHAN_C_TX_CLK			0x0A	/* 01010  Channel C transmit clock             */
#define	DP83867_CLK_OUT_CHAN_B_TX_CLK			0x09	/* 01001  Channel B transmit clock             */
#define	DP83867_CLK_OUT_CHAN_A_TX_CLK			0x08	/* 01000  Channel A transmit clock             */
#define	DP83867_CLK_OUT_CHAN_D_RX_CLK_DIV_5		0x07	/* 00111  Channel D receive clock divided by 5 */
#define	DP83867_CLK_OUT_CHAN_C_RX_CLK_DIV_5		0x06	/* 00110  Channel C receive clock divided by 5 */
#define	DP83867_CLK_OUT_CHAN_B_RX_CLK_DIV_5		0x05	/* 00101  Channel B receive clock divided by 5 */
#define	DP83867_CLK_OUT_CHAN_A_RX_CLK_DIV_5		0x04	/* 00100  Channel A receive clock divided by 5 */
#define	DP83867_CLK_OUT_CHAN_D_RX_CLK			0x03	/* 00011  Channel D receive clock              */
#define	DP83867_CLK_OUT_CHAN_C_RX_CLK			0x02	/* 00010  Channel C receive clock              */
#define	DP83867_CLK_OUT_CHAN_B_RX_CLK			0x01	/* 00001  Channel B receive clock              */
#define	DP83867_CLK_OUT_CHAN_A_RX_CLK			0x00	/* 00000  Channel A receive clock              */
#define	DP83867_CLK_OUT_MASK					0x1F	/* clock field mask */
#define	DP83867_CLK_OUT_SHIFT					8		/* clock field shift count */

#endif
