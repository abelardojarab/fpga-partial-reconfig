// Copyright (c) 2001-2017 Intel Corporation
//  
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//  
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
//  
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
// OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
// IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
// TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
// SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

/*
 * Driver for Altera Partial Reconfiguration IP Core
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Based on socfpga-a10.c Copyright (C) 2015-2016 Altera Corporation
 *  by Alan Tull <atull@opensource.altera.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "altera-pr-ip-core.h"
#include <linux/delay.h>
#include <linux/fpga/fpga-mgr.h>
#include <linux/module.h>

#define ALT_PR_DATA_OFST		0x00
#define ALT_PR_CSR_OFST			0x04
#define ALT_PR_VER_OFST			0x08
#define ALT_PR_POF_ID_OFST		0x0c

#define ALT_PR_CSR_PR_START		BIT(0)
#define ALT_PR_CSR_STATUS_SFT		2
#define ALT_PR_CSR_STATUS_MSK		(7 << ALT_PR_CSR_STATUS_SFT)
#define ALT_PR_CSR_STATUS_NRESET	(0 << ALT_PR_CSR_STATUS_SFT)
#define ALT_PR_CSR_STATUS_PR_ERR	(1 << ALT_PR_CSR_STATUS_SFT)
#define ALT_PR_CSR_STATUS_CRC_ERR	(2 << ALT_PR_CSR_STATUS_SFT)
#define ALT_PR_CSR_STATUS_BAD_BITS	(3 << ALT_PR_CSR_STATUS_SFT)
#define ALT_PR_CSR_STATUS_PR_IN_PROG	(4 << ALT_PR_CSR_STATUS_SFT)
#define ALT_PR_CSR_STATUS_PR_SUCCESS	(5 << ALT_PR_CSR_STATUS_SFT)

#define ALT_PR_VER_POF_ID		0xaa500003
#define ALT_PR_RBF_ID_OFST		(unsigned int)(71*sizeof(u32))

struct alt_pr_priv {
	void __iomem *reg_base;
};

static enum fpga_mgr_states alt_pr_fpga_state(struct fpga_manager *mgr)
{
	struct alt_pr_priv *priv = mgr->priv;
	const char *err = "unknown";
	enum fpga_mgr_states ret = FPGA_MGR_STATE_UNKNOWN;
	u32 val;

	val = readl(priv->reg_base + ALT_PR_CSR_OFST);

	val &= ALT_PR_CSR_STATUS_MSK;

	switch (val) {
	case ALT_PR_CSR_STATUS_NRESET:
		return FPGA_MGR_STATE_RESET;

	case ALT_PR_CSR_STATUS_PR_ERR:
		err = "pr error";
		ret = FPGA_MGR_STATE_WRITE_ERR;
		break;

	case ALT_PR_CSR_STATUS_CRC_ERR:
		err = "crc error";
		ret = FPGA_MGR_STATE_WRITE_ERR;
		break;

	case ALT_PR_CSR_STATUS_BAD_BITS:
		err = "bad bits";
		ret = FPGA_MGR_STATE_WRITE_ERR;
		break;

	case ALT_PR_CSR_STATUS_PR_IN_PROG:
		return FPGA_MGR_STATE_WRITE;

	case ALT_PR_CSR_STATUS_PR_SUCCESS:
		return FPGA_MGR_STATE_OPERATING;

	default:
		break;
	}

	dev_err(&mgr->dev, "encountered error code %d (%s) in %s()\n",
		val, err, __func__);
	return ret;
}

static int alt_pr_fpga_write_init(struct fpga_manager *mgr,
				  struct fpga_image_info *info,
				  const char *buf, size_t count)
{
	struct alt_pr_priv *priv = mgr->priv;
	u32 val;
	u32 *prbf;

	if (!(info->flags & FPGA_MGR_PARTIAL_RECONFIG)) {
		dev_err(&mgr->dev, "%s Partial Reconfiguration flag not set\n",
			__func__);
		return -EINVAL;
	}

	val = readl(priv->reg_base + ALT_PR_CSR_OFST);

	if (val & ALT_PR_CSR_PR_START) {
		dev_err(&mgr->dev,
			"%s Partial Reconfiguration already started\n",
		       __func__);
		return -EINVAL;
	}

	val = readl(priv->reg_base + ALT_PR_VER_OFST);

	if (val == ALT_PR_VER_POF_ID) {
		if (count < ALT_PR_RBF_ID_OFST) {
			dev_err(&mgr->dev, "%s count is too small %zu < %u\n",
				__func__,
				count, ALT_PR_RBF_ID_OFST);
			return -EINVAL;
		}

		prbf = (u32*)(buf + ALT_PR_RBF_ID_OFST);

		val = readl(priv->reg_base + ALT_PR_POF_ID_OFST);

		if (val) {
			if (val != *prbf) {
				dev_err(&mgr->dev,
					"%s POF ID does not match RBF %x != %x",
					__func__, val, *prbf);
				return -EINVAL;
			}
			dev_info(&mgr->dev, "%s POF ID matches RBF\n",
				 __func__);
		} else
			dev_info(&mgr->dev, "POF ID check disabled\n");
	}

	writel(val | ALT_PR_CSR_PR_START, priv->reg_base + ALT_PR_CSR_OFST);

	return 0;
}

static int alt_pr_fpga_write(struct fpga_manager *mgr, const char *buf,
			     size_t count)
{
	struct alt_pr_priv *priv = mgr->priv;
	u32 *buffer_32 = (u32 *)buf;
	size_t i = 0;

	if (count <= 0)
		return -EINVAL;

	/* Write out the complete 32-bit chunks */
	while (count >= sizeof(u32)) {
		writel(buffer_32[i++], priv->reg_base);
		count -= sizeof(u32);
	}

	/* Write out remaining non 32-bit chunks */
	switch (count) {
	case 3:
		writel(buffer_32[i++] & 0x00ffffff, priv->reg_base);
		break;
	case 2:
		writel(buffer_32[i++] & 0x0000ffff, priv->reg_base);
		break;
	case 1:
		writel(buffer_32[i++] & 0x000000ff, priv->reg_base);
		break;
	case 0:
		break;
	default:
		/* This will never happen */
		return -EFAULT;
	}

	if (alt_pr_fpga_state(mgr) == FPGA_MGR_STATE_WRITE_ERR)
		return -EIO;

	return 0;
}

static int alt_pr_fpga_write_complete(struct fpga_manager *mgr,
				      struct fpga_image_info *info)
{
	u32 i;

	for (i = 0; i < info->config_complete_timeout_us; i++) {
		switch (alt_pr_fpga_state(mgr)) {
		case FPGA_MGR_STATE_WRITE_ERR:
			return -EIO;

		case FPGA_MGR_STATE_OPERATING:
			dev_info(&mgr->dev,
				 "successful partial reconfiguration\n");
			return 0;

		default:
			break;
		}
		udelay(1);
	}
	dev_err(&mgr->dev, "timed out waiting for write to complete\n");
	return -ETIMEDOUT;
}

static const struct fpga_manager_ops alt_pr_ops = {
	.state = alt_pr_fpga_state,
	.write_init = alt_pr_fpga_write_init,
	.write = alt_pr_fpga_write,
	.write_complete = alt_pr_fpga_write_complete,
};

int alt_pr_probe(struct device *dev, void __iomem *reg_base)
{
	struct alt_pr_priv *priv;
	u32 val;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->reg_base = reg_base;

	val = readl(priv->reg_base + ALT_PR_CSR_OFST);

	dev_info(dev, "%s status=%d start=%d ver=0x%x id=0x%x\n", __func__,
		 (val & ALT_PR_CSR_STATUS_MSK) >> ALT_PR_CSR_STATUS_SFT,
		 (int)(val & ALT_PR_CSR_PR_START),
		 readl(priv->reg_base + ALT_PR_VER_OFST),
		 readl(priv->reg_base + ALT_PR_POF_ID_OFST));

	return fpga_mgr_register(dev, dev_name(dev), &alt_pr_ops, priv);
}
EXPORT_SYMBOL_GPL(alt_pr_probe);

int alt_pr_remove(struct device *dev)
{
	dev_dbg(dev, "%s\n", __func__);

	fpga_mgr_unregister(dev);

	return 0;
}
EXPORT_SYMBOL_GPL(alt_pr_remove);

MODULE_AUTHOR("Matthew Gerlach <matthew.gerlach@linux.intel.com>");
MODULE_DESCRIPTION("Altera Partial Reconfiguration IP Core");
MODULE_LICENSE("GPL v2");
