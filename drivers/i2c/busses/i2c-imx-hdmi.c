/*
 * I2C master driver for iMX6 HDMI DDC bus
 *
 * Copyright (C) 2013-2014 Mentor Graphics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

/*
 * According to the iMX6 Reference Manual only two types of transactions
 * are suported by HDMI I2C Master Interface in Normal Mode:
 *
 * A) one byte data write transaction (I2C spec 2 bytes transmission):
 *    master   S|slave addr[6:0]|0|     |slave reg[7:0]|     |data[7:0]|     |P
 *    slave                       | Ack |              | Ack |         | Ack |
 *
 * B) one byte data read transaction (I2C spec write/read combined format):
 *    master   S|slave addr[6:0]|0|     |slave reg[7:0]|     | ...
 *    slave                       | Ack |              | Ack | ...
 *
 *    master   ... Sr|slave addr[6:0]|1|     |         | Ack |P
 *    slave    ...                     | Ack |data[7:0]|
 *
 *
 * The technical limitations of the iMX6 HDMI E-DDC bus does not allow
 * to call it an I2C compatible bus, however relativery large subset of
 * I2C transactions can be decomposed into aforementioned data read/write
 * operations and many I2C devices correctly support those operations (and
 * of course it is possible to read EDID blob of a connected HDMI monitor),
 * so practically it makes sense to have an I2C bus driver utilizing
 * Linux I2C framework potentialities.
 */

#include <linux/clk.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>

/* iMX6 HDMI shared registers */
#define HDMI_IH_I2CM_STAT0			0x0105
#define HDMI_IH_MUTE_I2CM_STAT0			0x0185

/* HDMI_IH_I2CM_STAT0 / HDMI_IH_MUTE_I2CM_STAT0 register bits */
#define HDMI_IH_I2CM_STAT0_ERROR		BIT(0)
#define HDMI_IH_I2CM_STAT0_DONE			BIT(1)

/* iMX6 HDMI I2C master (E-DDC) register offsets */
#define HDMI_I2CM_SLAVE				0x7E00
#define HDMI_I2CM_ADDRESS			0x7E01
#define HDMI_I2CM_DATAO				0x7E02
#define HDMI_I2CM_DATAI				0x7E03
#define HDMI_I2CM_OPERATION			0x7E04
#define HDMI_I2CM_INT				0x7E05
#define HDMI_I2CM_CTLINT			0x7E06
#define HDMI_I2CM_DIV				0x7E07
#define HDMI_I2CM_SEGADDR			0x7E08
#define HDMI_I2CM_SOFTRSTZ			0x7E09
#define HDMI_I2CM_SEGPTR			0x7E0A
#define HDMI_I2CM_SS_SCL_HCNT_1_ADDR		0x7E0B
#define HDMI_I2CM_SS_SCL_HCNT_0_ADDR		0x7E0C
#define HDMI_I2CM_SS_SCL_LCNT_1_ADDR		0x7E0D
#define HDMI_I2CM_SS_SCL_LCNT_0_ADDR		0x7E0E
#define HDMI_I2CM_FS_SCL_HCNT_1_ADDR		0x7E0F
#define HDMI_I2CM_FS_SCL_HCNT_0_ADDR		0x7E10
#define HDMI_I2CM_FS_SCL_LCNT_1_ADDR		0x7E11
#define HDMI_I2CM_FS_SCL_LCNT_0_ADDR		0x7E12

/* HDMI_I2CM_OPERATION register bits */
#define HDMI_I2CM_OPERATION_READ		BIT(0)
#define HDMI_I2CM_OPERATION_READ_EXT		BIT(1)
#define HDMI_I2CM_OPERATION_WRITE		BIT(4)

/* HDMI_I2CM_INT register bits */
#define HDMI_I2CM_INT_DONE_MASK			BIT(2)
#define HDMI_I2CM_INT_DONE_POL			BIT(3)

/* HDMI_I2CM_CTLINT register bits */
#define HDMI_I2CM_CTLINT_ARB_MASK		BIT(2)
#define HDMI_I2CM_CTLINT_ARB_POL		BIT(3)
#define HDMI_I2CM_CTLINT_NAC_MASK		BIT(6)
#define HDMI_I2CM_CTLINT_NAC_POL		BIT(7)

#define IMX_HDMI_I2C_STANDARD_FREQ		100000
#define IMX_HDMI_I2C_FAST_FREQ			400000

struct imx_hdmi_i2c {
	struct i2c_adapter	adap;
	struct device		*dev;

	void __iomem		*base;
	struct clk		*isfr_clk;
	struct clk		*iahb_clk;

	spinlock_t		lock;
	u8			stat;
	struct completion	cmp;

	u8			slave_reg;
	bool			is_regaddr;

	u32			clk_hz;
};

static void hdmi_writeb(struct imx_hdmi_i2c *pd, unsigned int reg, u8 value)
{
	dev_dbg(pd->dev, "write: reg 0x%04x, val 0x%02x\n", reg, value);
	writeb_relaxed(value, pd->base + reg);
}

static u8 hdmi_readb(struct imx_hdmi_i2c *pd, unsigned int reg)
{
	u8 value;

	value = readb_relaxed(pd->base + reg);
	dev_dbg(pd->dev, "read: reg 0x%04x, val 0x%02x\n", reg, value);

	return value;
}

/* Initialize iMX6 HDMI DDC I2CM controller */
static void imx_hdmi_hwinit(struct imx_hdmi_i2c *pd)
{
	unsigned long flags;

	spin_lock_irqsave(&pd->lock, flags);

	/* Set Mode speed */
	if (pd->clk_hz == IMX_HDMI_I2C_FAST_FREQ) /* Set Fast Mode speed */
		hdmi_writeb(pd, HDMI_I2CM_DIV, 0x0b);
	else /* Set Standard Mode speed */
		hdmi_writeb(pd, HDMI_I2CM_DIV, 0x03);

	/* Software reset */
	hdmi_writeb(pd, HDMI_I2CM_SOFTRSTZ, 0x00);

	/* Set done, not acknowledged and arbitration interrupt polarities */
	hdmi_writeb(pd, HDMI_I2CM_INT, HDMI_I2CM_INT_DONE_POL);
	hdmi_writeb(pd, HDMI_I2CM_CTLINT,
		    HDMI_I2CM_CTLINT_NAC_POL | HDMI_I2CM_CTLINT_ARB_POL);

	/* Clear DONE and ERROR interrupts */
	hdmi_writeb(pd, HDMI_IH_I2CM_STAT0,
		    HDMI_IH_I2CM_STAT0_ERROR | HDMI_IH_I2CM_STAT0_DONE);

	/* Mute DONE and ERROR interrupts */
	hdmi_writeb(pd, HDMI_IH_MUTE_I2CM_STAT0,
		    HDMI_IH_I2CM_STAT0_ERROR | HDMI_IH_I2CM_STAT0_DONE);

	spin_unlock_irqrestore(&pd->lock, flags);
}

static irqreturn_t imx_hdmi_i2c_isr(int irq, void *dev)
{
	struct imx_hdmi_i2c *pd = dev;
	unsigned long flags;

	spin_lock_irqsave(&pd->lock, flags);

	pd->stat = hdmi_readb(pd, HDMI_IH_I2CM_STAT0);
	if (!pd->stat) {
		spin_unlock_irqrestore(&pd->lock, flags);
		return IRQ_NONE;
	}

	dev_dbg(pd->dev, "irq: 0x%02x, addr: 0x%02x, reg: 0x%02x\n",
		pd->stat, hdmi_readb(pd, HDMI_I2CM_SLAVE),
		hdmi_readb(pd, HDMI_I2CM_ADDRESS));

	hdmi_writeb(pd, HDMI_IH_I2CM_STAT0, pd->stat);
	complete(&pd->cmp);

	spin_unlock_irqrestore(&pd->lock, flags);

	return IRQ_HANDLED;
}

static int xfer_read(struct imx_hdmi_i2c *pd, unsigned char *buf, int length)
{
	int stat;
	unsigned long flags;

	spin_lock_irqsave(&pd->lock, flags);

	if (!pd->is_regaddr) {
		dev_dbg(pd->dev, "set read register address to 0\n");
		pd->slave_reg = 0x00;
		pd->is_regaddr = true;
	}

	while (length--) {
		hdmi_writeb(pd, HDMI_I2CM_ADDRESS, pd->slave_reg++);
		hdmi_writeb(pd, HDMI_I2CM_OPERATION, HDMI_I2CM_OPERATION_READ);
		pd->stat = 0;

		spin_unlock_irqrestore(&pd->lock, flags);

		stat = wait_for_completion_interruptible_timeout(&pd->cmp,
								 HZ / 10);
		if (!stat)
			return -ETIMEDOUT;
		if (stat < 0)
			return stat;

		spin_lock_irqsave(&pd->lock, flags);

		/* Check for error condition on the bus */
		if (pd->stat & HDMI_IH_I2CM_STAT0_ERROR) {
			spin_unlock_irqrestore(&pd->lock, flags);
			return -EIO;
		}

		*buf++ = hdmi_readb(pd, HDMI_I2CM_DATAI);
	}

	spin_unlock_irqrestore(&pd->lock, flags);

	return 0;
}

static int xfer_write(struct imx_hdmi_i2c *pd, unsigned char *buf, int length)
{
	int stat;
	unsigned long flags;

	spin_lock_irqsave(&pd->lock, flags);

	if (!pd->is_regaddr) {
		if (length) {
			/* Use the first write byte as register address */
			pd->slave_reg = buf[0];
			length--;
			buf++;
		} else {
			dev_dbg(pd->dev, "set write register address to 0\n");
			pd->slave_reg = 0x00;
		}
		pd->is_regaddr = true;
	}

	while (length--) {
		hdmi_writeb(pd, HDMI_I2CM_DATAO, *buf++);
		hdmi_writeb(pd, HDMI_I2CM_ADDRESS, pd->slave_reg++);
		hdmi_writeb(pd, HDMI_I2CM_OPERATION, HDMI_I2CM_OPERATION_WRITE);
		pd->stat = 0;

		spin_unlock_irqrestore(&pd->lock, flags);

		stat = wait_for_completion_interruptible_timeout(&pd->cmp,
								 HZ / 10);
		if (!stat)
			return -ETIMEDOUT;
		if (stat < 0)
			return stat;

		spin_lock_irqsave(&pd->lock, flags);

		/* Check for error condition on the bus */
		if (pd->stat & HDMI_IH_I2CM_STAT0_ERROR) {
			spin_unlock_irqrestore(&pd->lock, flags);
			return -EIO;
		}
	}

	spin_unlock_irqrestore(&pd->lock, flags);

	return 0;
}

static int i2c_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct imx_hdmi_i2c *pd = i2c_get_adapdata(adap);

	int i, ret;
	u8 addr;
	unsigned long flags;

	dev_dbg(pd->dev, "xfer: num: %d, addr: 0x%x\n", num, msgs[0].addr);

	spin_lock_irqsave(&pd->lock, flags);

	hdmi_writeb(pd, HDMI_IH_MUTE_I2CM_STAT0, 0x00);

	/* Set slave device address from the first transaction */
	addr = msgs[0].addr;
	hdmi_writeb(pd, HDMI_I2CM_SLAVE, addr);

	/* Set slave device register address on transfer */
	pd->is_regaddr = false;

	spin_unlock_irqrestore(&pd->lock, flags);

	for (i = 0; i < num; i++) {
		dev_dbg(pd->dev, "xfer: num: %d/%d, len: %d, flags: 0x%x\n",
			i + 1, num, msgs[i].len, msgs[i].flags);

		if (msgs[i].addr != addr) {
			dev_warn(pd->dev,
				 "unsupported transaction, changed slave address\n");
			ret = -EOPNOTSUPP;
			break;
		}

		if (msgs[i].len == 0) {
			dev_dbg(pd->dev,
				 "unsupported transaction %d/%d, no data\n",
				 i + 1, num);
			ret = -EOPNOTSUPP;
			break;
		}

		if (msgs[i].flags & I2C_M_RD)
			ret = xfer_read(pd, msgs[i].buf, msgs[i].len);
		else
			ret = xfer_write(pd, msgs[i].buf, msgs[i].len);

		if (ret < 0)
			break;
	}

	if (!ret)
		ret = num;

	spin_lock_irqsave(&pd->lock, flags);

	/* Mute DONE and ERROR interrupts */
	hdmi_writeb(pd, HDMI_IH_MUTE_I2CM_STAT0,
		    HDMI_IH_I2CM_STAT0_ERROR | HDMI_IH_I2CM_STAT0_DONE);

	spin_unlock_irqrestore(&pd->lock, flags);

	return ret;
}

static u32 i2c_func(struct i2c_adapter *adapter)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static struct i2c_algorithm imx_hdmi_algorithm = {
	.master_xfer	= i2c_xfer,
	.functionality	= i2c_func,
};

static int imx_hdmi_i2c_get_frequency(struct platform_device *pdev,
				    struct imx_hdmi_i2c *pd)
{
	struct device_node *np = pdev->dev.of_node;
	u32 freq;
	int err;

	err = of_property_read_u32(np, "clock-frequency", &freq);
	if (err) {
		freq = IMX_HDMI_I2C_STANDARD_FREQ;
		dev_dbg(&pdev->dev, "using default frequency %u\n", freq);
	}
	else if (freq != IMX_HDMI_I2C_STANDARD_FREQ || freq != IMX_HDMI_I2C_FAST_FREQ)
	{
		freq = IMX_HDMI_I2C_STANDARD_FREQ;
		dev_dbg(&pdev->dev, "using default frequency %u\n", freq);
	}
	pd->clk_hz = freq;

	return 0;
}

static int imx_hdmi_i2c_probe(struct platform_device *pdev)
{
	struct imx_hdmi_i2c *pd;
	struct i2c_adapter *adap;
	struct resource *res;
	int ret, irq;

	pd = devm_kzalloc(&pdev->dev, sizeof(struct imx_hdmi_i2c), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;
	pd->dev = &pdev->dev;

	/*
	 * iMX6 HDMI controller memory region is shared among device drivers,
	 * therefore do not ioremap_resource() it exclusively.
	 */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	pd->base = devm_ioremap(pd->dev, res->start, resource_size(res));
	if (IS_ERR(pd->base)) {
		dev_err(pd->dev, "unable to ioremap: %ld\n", PTR_ERR(pd->base));
		return PTR_ERR(pd->base);
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(pd->dev, "no irq resource\n");
		return irq;
	}

	imx_hdmi_i2c_get_frequency(pdev, pd);


	ret = devm_request_irq(pd->dev, irq, imx_hdmi_i2c_isr,
			       IRQF_SHARED, "imx hdmi i2c", pd);
	if (ret < 0) {
		dev_err(pd->dev, "unable to request irq %d: %d\n", irq, ret);
		return ret;
	}

	pd->isfr_clk = devm_clk_get(pd->dev, "isfr");
	if (IS_ERR(pd->isfr_clk)) {
		ret = PTR_ERR(pd->isfr_clk);
		dev_err(pd->dev, "unable to get HDMI isfr clk: %d\n", ret);
		return ret;
	}

	ret = clk_prepare_enable(pd->isfr_clk);
	if (ret) {
		dev_err(pd->dev, "cannot enable HDMI isfr clock: %d\n", ret);
		return ret;
	}

	pd->iahb_clk = devm_clk_get(pd->dev, "iahb");
	if (IS_ERR(pd->iahb_clk)) {
		ret = PTR_ERR(pd->iahb_clk);
		dev_err(pd->dev, "unable to get HDMI iahb clk: %d\n", ret);
		goto err_isfr;
	}

	ret = clk_prepare_enable(pd->iahb_clk);
	if (ret) {
		dev_err(pd->dev, "cannot enable HDMI iahb clock: %d\n", ret);
		goto err_isfr;
	}

	spin_lock_init(&pd->lock);
	init_completion(&pd->cmp);

	adap = &pd->adap;
	adap->algo = &imx_hdmi_algorithm;
	adap->dev.parent = pd->dev;
	adap->nr = pdev->id;
	adap->dev.of_node = pdev->dev.of_node;
	i2c_set_adapdata(adap, pd);
	strlcpy(adap->name, pdev->name, sizeof(adap->name));

	ret = i2c_add_numbered_adapter(adap);
	if (ret) {
		dev_err(pd->dev, "cannot add numbered adapter\n");
		goto err_register;
	}

	platform_set_drvdata(pdev, pd);

	/* reset I2C controller */
	imx_hdmi_hwinit(pd);

	dev_info(pd->dev, "registered %s bus driver\n", adap->name);

	return 0;

 err_register:
	clk_disable_unprepare(pd->iahb_clk);
 err_isfr:
	clk_disable_unprepare(pd->isfr_clk);

	return ret;
}

static int imx_hdmi_i2c_remove(struct platform_device *pdev)
{
	struct imx_hdmi_i2c *pd = platform_get_drvdata(pdev);

	platform_set_drvdata(pdev, NULL);

	clk_disable_unprepare(pd->iahb_clk);
	clk_disable_unprepare(pd->isfr_clk);

	i2c_del_adapter(&pd->adap);

	return 0;
}

static const struct of_device_id imx_hdmi_ddc_dt_ids[] = {
	{ .compatible = "fsl,imx6q-hdmi-ddc", },
	{ },
};
MODULE_DEVICE_TABLE(of, imx_hdmi_ddc_dt_ids);

static struct platform_driver imx_hdmi_i2c_driver = {
	.driver		= {
		.name	= "imx_hdmi_i2c",
		.owner	= THIS_MODULE,
		.of_match_table = imx_hdmi_ddc_dt_ids,
	},
	.probe		= imx_hdmi_i2c_probe,
	.remove		= imx_hdmi_i2c_remove,
};

static int __init imx_hdmi_i2c_init(void)
{
	return platform_driver_register(&imx_hdmi_i2c_driver);
}

static void __exit imx_hdmi_i2c_exit(void)
{
	platform_driver_unregister(&imx_hdmi_i2c_driver);
}

subsys_initcall(imx_hdmi_i2c_init);
module_exit(imx_hdmi_i2c_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("iMX6 HDMI DDC I2C master bus driver");
MODULE_AUTHOR("Vladimir Zapolskiy <vladimir_zapolskiy@mentor.com>");
