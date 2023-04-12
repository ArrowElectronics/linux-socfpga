// SPDX-License-Identifier: GPL-2.0
/* Intel FPGA HSSI SS debugfs
 * Copyright (C) 2022 Intel Corporation. All rights reserved
 *
	 * Contributors:
	 *   Subhransu S. Prusty
 *
 */
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/debugfs.h>
#include "altera_utils.h"
#include "intel_fpga_hssiss.h"

struct hssiss_dbg_read_data {
	u32 dr_grp; /* get_hss_profile */
	u32 profile; /* get_hss_profile */
	u32 data; /* port_data for mac_stat, data for link_status, fw_version and csr */
	u32 max_tx_frame_size;
	u32 max_rx_frame_size;
};

struct hssiss_dbg {
	struct platform_device *pdev;
	struct dentry *dbgfs;
	enum hssiss_salcmd sal_cmd;
	struct hssiss_dbg_read_data read;
};

/*
 * hssiss_dbgfs_csr_read() - hssiss debugfs-node csr read callback
 */
static ssize_t hssiss_dbgfs_csr_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	char buf[10];
	int size;

	size = snprintf(buf, sizeof(buf), "%x\n", d->read.data);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);
}

/*
 * hssiss_dbgfs_csr_write() - hssiss debugfs-node csr write callback
 * for read:
 * 	echo "ch type offset word" > hssi_reg
 * for write:
 * 	echo "ch type offset word data" > hssi_reg
 *
 * word: 1 for word read/write, 0 for byte read/write
 */
static ssize_t hssiss_dbgfs_csr_write(struct file *filep, const char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct get_set_csr_data data;
	char *buf;
	int word, type, ch, ret;
	u32 offset, val;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0)
		goto free_buf;
	buf[count] = 0;

	/* Parse the values */
	ret = sscanf(buf, "%d %d %x %d %x", &ch, &type, &offset, &word, &val);

	if (ret < 4) {
		ret = -EINVAL;
		goto free_buf;
	}

 	data.ch = ch;
	data.reg_type = type;
	data.offs = offset;
	data.word = word ? true:false;
	data.data = val;

	if (ret == 4) {
		ret = hssiss_execute_sal_cmd(pdev, SAL_GET_CSR, &data);
		if (ret == 0)
			d->read.data= data.data;
	} else {
		ret = hssiss_execute_sal_cmd(pdev, SAL_SET_CSR, &data);
	}

free_buf:
	kfree(buf);
	return (ret < 0 ? ret : count);
}

/*
 * hssiss_dbgfs_sal_read() - hssiss debugfs-node SAL read callback
 * Note: Except get/set csr. Use get/set csr dbgfs to read csr registers.
 */
static ssize_t hssiss_dbgfs_sal_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	char buf[100];
	int size;

	switch(d->sal_cmd) {
	case SAL_GET_HSSI_PROFILE:
		size = scnprintf(buf, sizeof(buf),
				"dr_grp: %x profile: %x",
				d->read.dr_grp, d->read.profile);
		break;
	case SAL_READ_MAC_STAT:
		size = scnprintf(buf, sizeof(buf), "%x", d->read.data);
		break;
	case SAL_GET_MTU:
		size = scnprintf(buf, sizeof(buf),
				"max_tx_frame_size: %x \
				max_rx_frame_size:%x",
				d->read.max_tx_frame_size,
				d->read.max_rx_frame_size);
		break;
	case SAL_NCSI_GET_LINK_STS:
		size = scnprintf(buf, sizeof(buf), "%x", d->read.data);
		break;
	case SAL_FW_VERSION:
		size = scnprintf(buf, sizeof(buf), "%x", d->read.data);
		break;
	default:
		size = scnprintf(buf, sizeof(buf), "No command in progress\n");
		break;
	}

	return simple_read_from_buffer(ubuf, count, offp, buf, size);
}

/*
 * hssiss_dbgfs_sal_write() - hssiss debugfs-node sal write callback
 * Note: Except get/set csr. Use get/set csr dbgfs to read csr registers.
 */
static ssize_t hssiss_dbgfs_sal_write(struct file *filep, const char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	char *buf;
	u32 cmd;
	int ret;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0)
		goto free_buf;
	buf[count] = 0;

	/* Parse SAL command */
	ret = sscanf(buf, "%x", &cmd);
	if (!ret) {
		ret = -EINVAL;
		goto free_buf;
	}

	d->sal_cmd = cmd;

	/* Parse and prepare data for command */
	switch(cmd) {
	case SAL_GET_HSSI_PROFILE:
	case SAL_SET_HSSI_PROFILE:
	{
		struct get_set_dr_data data;

		ret = sscanf(buf, "%x %x %x %u", &cmd, &data.dr_grp, &data.profile, &data.port);
		if (ret != 4) {
			ret = -EINVAL;
			goto free_buf;
		}

		ret = hssiss_execute_sal_cmd(pdev, cmd, &data);

		d->read.dr_grp = data.dr_grp;
		d->read.profile = data.profile;

		break;
	}
	case SAL_READ_MAC_STAT:
	{
		struct read_mac_stat_data data;
		int lsb;

		ret = sscanf(buf, "%x %x %x %d", &cmd, &data.port_data, &data.type, &lsb);
		if (ret != 4) {
			ret = -EINVAL;
			goto free_buf;
		}

		data.lsb = (lsb) ? true : false;

		ret = hssiss_execute_sal_cmd(pdev, cmd, &data);

		d->read.data = data.port_data;

		break;
	}
	case SAL_GET_MTU:
	{
		struct get_mtu_data data;

		ret = sscanf(buf, "%x %u %hu %hu",
				&cmd, &data.port, &data.max_tx_frame_size,
				&data.max_rx_frame_size);
		if (ret != 4) {
			ret = -EINVAL;
			goto free_buf;
		}

		ret = hssiss_execute_sal_cmd(pdev, cmd, &data);
		d->read.max_tx_frame_size = data.max_tx_frame_size;
		d->read.max_rx_frame_size = data.max_rx_frame_size;

		break;
	}
	case SAL_RESET_MAC_STAT:
	{
		struct reset_mac_stat_data data;
		int tx, rx;

		ret = sscanf(buf, "%x %u %d %d", &cmd, &data.port, &tx, &rx);
		if (ret != 4) {
			ret = -EINVAL;
			goto free_buf;
		}

		data.tx = tx ? true : false;
		data.rx = rx ? true : false;

		ret = hssiss_execute_sal_cmd(pdev, cmd, &data);

		break;
	}
	case SAL_NCSI_GET_LINK_STS:
	{
		union ncsi_link_status_data data;

		ret = sscanf(buf, "%x %x", &cmd, &data.full);
		if (ret != 2) {
			ret = -EINVAL;
			goto free_buf;
		}

		ret = hssiss_execute_sal_cmd(pdev, cmd, &data);
		d->read.data = data.full;

		break;
	}
	case SAL_FW_VERSION:
	{
		u32 data;
		ret = hssiss_execute_sal_cmd(pdev, cmd, &data);
		d->read.data = data;

		break;
	}
	case SAL_DISABLE_LOOPBACK:
	case SAL_ENABLE_LOOPBACK:
	{
		u32 data = 0;
		sscanf(buf, "%x %x", &cmd, &data);
		ret = hssiss_execute_sal_cmd(pdev, cmd, &data);
		break;

	}
	default:
		ret = -EINVAL;
		break;
	}

free_buf:
	kfree(buf);
	return (ret < 0 ? ret : count);
}

#define BUF_SIZE	PAGE_SIZE
/*
 * hssiss_dbgfs_readme_read() - hssiss debugfs-node readme read callback
 */
static ssize_t hssiss_dbgfs_readme_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	char *buf;
	int ret;

	buf = kzalloc(BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = scnprintf(buf, BUF_SIZE, "get_csr: to read byte data:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo \"ch type offset 0\" > hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcat hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "get_csr: to read word data:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo \"ch type offset 1\" > hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcat hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "set_csr: to write byte data:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo \"ch type offset 0 data\" > hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "set_csr: to write word data:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo \"ch type offset 1 data\" > hssi_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Execute sal command:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo \"cmd x y z\" > sal\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcmd: SAL command\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tx, y, z: SAL command specific data\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcat sal\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Execute direct SAL command:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo <ctrladdr reg_data> > ctrladdr\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tfor write: echo <wr reg_data> > wr\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\techo <cmdsts reg_data> > cmdsts\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tto check ack or err: cat cmdsts\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tto read data: cat rd\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Execute direct register access:\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tfor wr: echo <baseaddr offset direct val> > direct_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tfor rd: echo <baseaddr offset direct> > direct_reg\n");
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "\tcat direct_reg\n");

	ret = simple_read_from_buffer(ubuf, count, offp, buf, ret);

	kfree(buf);
	return ret;
}

/*
 * hssiss_dbgfs_dumpcsr_read() - hssiss debugfs-node dumpcsr read callback
 */
static ssize_t hssiss_dbgfs_dumpcsr_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	unsigned int csr_addroff = priv->csr_addroff;
	void __iomem *base = priv->sscsr;
	char *buf;
	int ret;
	int i;
	u32 val;

	buf = kzalloc(BUF_SIZE, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = scnprintf(buf, BUF_SIZE, "Dumping device feature registers\n");
	for (i = 0; i < 10; i++)
		ret += scnprintf(buf + ret, BUF_SIZE - ret, "\t%x: %x\n",
				(i * 4), csrrd32(base, (i * 4)));

	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Dumping other CSR registers\n");

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_VER);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_VER: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_COMMON_FEATURE_LIST);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_COMMON_FEATURE_LIST: %x\n", val);

	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Dumping port attributes\n");
	for (i = 0; i < 15; i++) {
		if (priv->ver == HSSISS_FTILE) {
			val = csrrd32(base, HSSISS_CSR_INTER_ATTRIB_PORT_FTILE + (i * 4));
		} else {
			val = csrrd32_withoffset(base, csr_addroff,
				HSSISS_CSR_INTER_ATTRIB_PORT + (i * 4));
		}
		ret += scnprintf(buf + ret, BUF_SIZE - ret, "\t%x: %x\n", i, val);
	}

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_CMDSTS);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_CMDSTS: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_CTRLADDR);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_CTRLADDR: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_RD_DATA);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_RD_DATA: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_WR_DATA);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_WR_DATA: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_GMII_TX_LATENCY);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_GMII_TX_LATENCY: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_GMII_RX_LATENCY);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_GMII_RX_LATENCY: %x\n", val);

	ret += scnprintf(buf + ret, BUF_SIZE - ret, "Dumping port status\n");
	for (i = 0; i < 15; i++) {
		if (priv->ver == HSSISS_FTILE) {
			val = csrrd32(base, HSSISS_CSR_ETH_PORT_STS_FTILE + (i * 4));
		} else {
			val = csrrd32_withoffset(base, csr_addroff,
				HSSISS_CSR_ETH_PORT_STS + (i * 4));
		}
		ret += scnprintf(buf + ret, BUF_SIZE - ret, "\t%x: %x\n", i, val);
	}

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_TSE_CTRL);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_TSE_CTRL: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_DBG_CTRL);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_DBG_CTRL: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_HOTPLUG_DBG_CTRL);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_HOTPLUG_DBG_CTRL: %x\n", val);

	val = csrrd32_withoffset(base, csr_addroff, HSSISS_CSR_HOTPLUG_DBG_STS);
	ret += scnprintf(buf + ret, BUF_SIZE - ret, "HSSISS_CSR_HOTPLUG_DBG_STS: %x\n", val);

	ret = simple_read_from_buffer(ubuf, count, offp, buf, ret);

	kfree(buf);
	return ret;
}

static ssize_t hssiss_dbgfs_ctrladdr_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	char buf[10];
	u32 val;
	int size;

	val = csrrd32_withoffset(priv->sscsr, priv->csr_addroff,
					HSSISS_CSR_CTRLADDR);

	size = snprintf(buf, sizeof(buf), "%x\n", val);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);

}

static ssize_t hssiss_dbgfs_ctrladdr_write(struct file *filep, const char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	u32 val;
	char *buf;
	int ret;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0) {
		kfree(buf);
		return -EIO;
	}
	buf[count] = 0;

	/* Parse the values */
	ret = sscanf(buf, "%x", &val);
	kfree(buf);
	if (ret < 1)
		return -EINVAL;

	csrwr32_withoffset(val, priv->sscsr, priv->csr_addroff,
					HSSISS_CSR_CTRLADDR);

	return count;
}

static ssize_t hssiss_dbgfs_cmdsts_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	char buf[10];
	u32 val;
	int size;

	val = csrrd32_withoffset(priv->sscsr, priv->csr_addroff,
					HSSISS_CSR_CMDSTS);

	size = snprintf(buf, sizeof(buf), "%x\n", val);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);

}

static ssize_t hssiss_dbgfs_cmdsts_write(struct file *filep, const char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	u32 val;
	char *buf;
	int ret;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0) {
		kfree(buf);
		return -EIO;
	}
	buf[count] = 0;

	/* Parse the values */
	ret = sscanf(buf, "%x", &val);
	kfree(buf);
	if (ret < 1)
		return -EINVAL;

	csrwr32_withoffset(val, priv->sscsr, priv->csr_addroff,
					HSSISS_CSR_CMDSTS);

	return count;
}

static ssize_t hssiss_dbgfs_wr_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	char buf[10];
	u32 val;
	int size;

	val = csrrd32_withoffset(priv->sscsr, priv->csr_addroff,
					HSSISS_CSR_WR_DATA);

	size = snprintf(buf, sizeof(buf), "%x\n", val);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);

}

static ssize_t hssiss_dbgfs_wr_write(struct file *filep, const char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	u32 val;
	char *buf;
	int ret;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0) {
		kfree(buf);
		return -EIO;
	}
	buf[count] = 0;

	/* Parse the values */
	ret = sscanf(buf, "%x", &val);
	kfree(buf);
	if (ret < 1)
		return -EINVAL;

	csrwr32_withoffset(val, priv->sscsr, priv->csr_addroff,
					HSSISS_CSR_WR_DATA);

	return count;
}

static ssize_t hssiss_dbgfs_rd_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	char buf[10];
	u32 val;
	int size;

	val = csrrd32_withoffset(priv->sscsr, priv->csr_addroff,
					HSSISS_CSR_RD_DATA);

	size = snprintf(buf, sizeof(buf), "%x\n", val);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);

}

static ssize_t hssiss_dbgfs_rd_write(struct file *filep, const char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	u32 val;
	char *buf;
	int ret;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0) {
		kfree(buf);
		return -EIO;
	}
	buf[count] = 0;

	/* Parse the values */
	ret = sscanf(buf, "%x", &val);
	kfree(buf);
	if (ret < 1)
		return -EINVAL;

	csrwr32_withoffset(val, priv->sscsr, priv->csr_addroff,
					HSSISS_CSR_RD_DATA);

	return count;
}

static ssize_t hssiss_dbgfs_direct_reg_read(struct file *filep, char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	char buf[10];
	int size;

	size = snprintf(buf, sizeof(buf), "%x\n", d->read.data);

	return simple_read_from_buffer(ubuf, count, offp, buf, size);

}

static ssize_t hssiss_dbgfs_direct_reg_write(struct file *filep, const char __user *ubuf,
				   size_t count, loff_t *offp)
{
	struct hssiss_dbg *d = filep->private_data;
	struct platform_device *pdev = d->pdev;
	struct hssiss_private *priv = platform_get_drvdata(pdev);
	u32 base, offset, val;
	char *buf;
	int ret, direct;

	/* Copy data from User-space */
	buf = kmalloc(count + 1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = simple_write_to_buffer(buf, count, offp, ubuf, count);
	if (ret < 0) {
		kfree(buf);
		return -EIO;
	}
	buf[count] = 0;

	/* Parse the values */
	ret = sscanf(buf, "%x %x %d %x", &base, &offset, &direct, &val);
	kfree(buf);
	if (ret < 3)
		return -EINVAL;

	if (ret == 4) {
		if (direct) {
			csrwr32_withoffset(val, priv->sscsr + base,
						0, offset);
		} else {
			csrwr32_withoffset(val, priv->sscsr + base,
					priv->csr_addroff, offset);

		}
	}
	else {
		if (direct) {
			d->read.data = csrrd32_withoffset(priv->sscsr + base,
							0, offset);
		} else {
			d->read.data = csrrd32_withoffset(priv->sscsr + base,
					priv->csr_addroff, offset);
		}
	}


	return count;
}

static const struct file_operations ctrladdr_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = hssiss_dbgfs_ctrladdr_write,
	.read = hssiss_dbgfs_ctrladdr_read
};

static const struct file_operations cmdsts_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = hssiss_dbgfs_cmdsts_write,
	.read = hssiss_dbgfs_cmdsts_read
};

static const struct file_operations csr_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = hssiss_dbgfs_csr_write,
	.read = hssiss_dbgfs_csr_read
};

static const struct file_operations wr_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = hssiss_dbgfs_wr_write,
	.read = hssiss_dbgfs_wr_read
};

static const struct file_operations rd_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = hssiss_dbgfs_rd_write,
	.read = hssiss_dbgfs_rd_read
};

static const struct file_operations sal_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = hssiss_dbgfs_sal_write,
	.read = hssiss_dbgfs_sal_read
};

static const struct file_operations readme_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hssiss_dbgfs_readme_read
};

static const struct file_operations dumpcsr_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = hssiss_dbgfs_dumpcsr_read
};

static const struct file_operations direct_reg_dbgfs_ops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.write = hssiss_dbgfs_direct_reg_write,
	.read = hssiss_dbgfs_direct_reg_read
};

struct hssiss_dbg *hssiss_dbgfs_init(struct platform_device *pdev)
{
	struct hssiss_dbg *d;

	d = devm_kzalloc(&pdev->dev, sizeof(*d), GFP_KERNEL);
	if (!d)
		return NULL;

	d->pdev = pdev;

	d->dbgfs = debugfs_create_dir("hssiss_dbg", NULL);

	debugfs_create_file("csr", S_IRUGO | S_IWUGO, d->dbgfs, d, &csr_dbgfs_ops);
	debugfs_create_file("ctrladdr", S_IRUGO | S_IWUGO, d->dbgfs, d, &ctrladdr_dbgfs_ops);
	debugfs_create_file("cmdsts", S_IRUGO | S_IWUGO, d->dbgfs, d, &cmdsts_dbgfs_ops);
	debugfs_create_file("wr", S_IRUGO | S_IWUGO, d->dbgfs, d, &wr_dbgfs_ops);
	debugfs_create_file("rd", S_IRUGO | S_IWUGO, d->dbgfs, d, &rd_dbgfs_ops);
	debugfs_create_file("sal", S_IRUGO | S_IWUGO, d->dbgfs, d, &sal_dbgfs_ops);
	debugfs_create_file("dumpcsr", 0444, d->dbgfs, d, &dumpcsr_dbgfs_ops);
	debugfs_create_file("readme", 0444, d->dbgfs, d, &readme_dbgfs_ops);
	debugfs_create_file("direct_reg", S_IRUGO | S_IWUGO, d->dbgfs, d, &direct_reg_dbgfs_ops);

	return d;
}

void hssiss_dbgfs_remove(struct hssiss_dbg *d)
{
	debugfs_remove_recursive(d->dbgfs);
}
