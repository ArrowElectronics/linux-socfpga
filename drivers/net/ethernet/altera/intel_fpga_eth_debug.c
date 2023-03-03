#include "altera_eth_dma.h"
#include "altera_msgdma.h"
#include "altera_msgdmahw.h"
#include "altera_msgdma_prefetcher.h"
#include "altera_msgdmahw_prefetcher.h"
#include "altera_utils.h"
#include "intel_fpga_eth_main.h"

static void xtile_prefetcher_reg_dump_tx(struct altera_dma_private *priv) {

	s32 ret;

	netdev_info(priv->dev, "TX PREFETCHER:\n");
	
	ret = csrrd32(priv->tx_pref_csr, msgdma_pref_csroffs(control));
	netdev_info(priv->dev, "\tCONTROL      	     | 0x%x\n", ret);

	ret = csrrd32(priv->tx_pref_csr, msgdma_pref_csroffs(status));
	netdev_info(priv->dev, "\tSTATUS 	     | 0x%x\n", ret);

  	ret = csrrd32(priv->tx_pref_csr, msgdma_pref_csroffs(next_desc_lo));
  	netdev_info(priv->dev, "\tNEXT DESC LOW  | 0x%x\n", ret);

}

static void xtile_prefetcher_reg_dump_rx(struct altera_dma_private *priv) {

        s32 ret;

        netdev_info(priv->dev, "RX PREFETCHER:\n");

        ret = csrrd32(priv->rx_pref_csr, msgdma_pref_csroffs(control));
        netdev_info(priv->dev, "\tCONTROL 	    | 0x%x\n", ret);

        ret = csrrd32(priv->rx_pref_csr, msgdma_pref_csroffs(status));
        netdev_info(priv->dev, "\tSTATUS  	    | 0x%x\n", ret);

        ret = csrrd32(priv->rx_pref_csr, msgdma_pref_csroffs(next_desc_lo));
        netdev_info(priv->dev, "\tNEXT DESC LOW  | 0x%x\n", ret);

}

static void xtile_dispatcher_reg_dump_tx(struct altera_dma_private *priv) { 

	s32 ret;

	netdev_info(priv->dev, "Tx DISPATCHER:\n");

  	ret = csrrd32(priv->tx_dma_csr, msgdma_csroffs(status));
 	netdev_info(priv->dev, "\tSTATUS   | 0x%x\n", ret);

  	ret = csrrd32(priv->tx_dma_csr, msgdma_csroffs(control));
 	netdev_info(priv->dev, "\tCONTROL  | 0x%x\n", ret);

}

static void xtile_dispatcher_reg_dump_rx(struct altera_dma_private *priv) {

        s32 ret;

        netdev_info(priv->dev, "Rx DISPATCHER:\n");

        ret = csrrd32(priv->rx_dma_csr, msgdma_csroffs(status));
        netdev_info(priv->dev, "\tSTATUS   | 0x%x\n", ret);

        ret = csrrd32(priv->rx_dma_csr, msgdma_csroffs(control));
        netdev_info(priv->dev, "\tCONTROL  | 0x%x\n", ret);

}

static void xtile_fifo_fill_level_tx(struct altera_dma_private *priv) {
	
	s32 ret;

	netdev_info(priv->dev, "Tx FILL LEVEL:\n");

	ret = csrrd32(priv->tx_dma_csr, msgdma_csroffs(rw_fill_level));

  	netdev_info(priv->dev, "\tWR FILL LEVEL  | 0x%x\n", MSGDMA_CSR_WR_FILL_LEVEL_GET(ret));
  	netdev_info(priv->dev, "\tRD FILL LEVEL  | 0x%x\n", MSGDMA_CSR_RD_FILL_LEVEL_GET(ret));

  	ret = MSGDMA_CSR_RESP_FILL_LEVEL_GET(csrrd32(priv->tx_dma_csr, msgdma_csroffs(resp_fill_level)));
  	netdev_info(priv->dev, "\tRSP FILL LEVEL | 0x%x\n", ret);
}

static void xtile_fifo_fill_level_rx(struct altera_dma_private *priv) {
	
	s32 ret;

	netdev_info(priv->dev, "Rx FILL LEVEL:\n");

	ret = csrrd32(priv->rx_dma_csr, msgdma_csroffs(rw_fill_level));

  	netdev_info(priv->dev, "\tWR FILL LEVEL  | 0x%x\n", MSGDMA_CSR_WR_FILL_LEVEL_GET(ret));
  	netdev_info(priv->dev, "\tRD FILL LEVEL  | 0x%x\n", MSGDMA_CSR_RD_FILL_LEVEL_GET(ret));

  	ret = MSGDMA_CSR_RESP_FILL_LEVEL_GET(csrrd32(priv->rx_dma_csr, 
					     msgdma_csroffs(resp_fill_level)));

  	netdev_info(priv->dev, "\tRSP FILL LEVEL | 0x%x\n", ret);
}

static void xtile_process_seq_no_tx(struct altera_dma_private *priv) {
	
	netdev_info(priv->dev, "Tx PROD/CONS     | 0x%x/0x%x\n", priv->tx_prod, priv->tx_cons);
}

static void xtile_seq_no_dump_tx(struct altera_dma_private *priv) {

	s32 ret;

	ret = csrrd32(priv->tx_dma_csr, msgdma_csroffs(rw_seq_num));
  	netdev_info(priv->dev, "Tx READ SEQ NO   | 0x%x\n", (ret & 0x0000fffff));
  	netdev_info(priv->dev, "Tx WRITE SEQ NO  | 0x%x\n", (ret & 0xffff0000)>>16);
}

static void xtile_seq_no_dump_rx(struct altera_dma_private *priv) {

	s32 ret;

	ret = csrrd32(priv->rx_dma_csr, msgdma_csroffs(rw_seq_num));
  	netdev_info(priv->dev, "Rx READ SEQ NO   | 0x%x\n", (ret & 0x0000fffff));
  	netdev_info(priv->dev, "Rx WRITE SEQ NO  | 0x%x\n", (ret & 0xffff0000)>>16);
}

static u64 timestamp_to_ns(struct msgdma_pref_extended_desc *desc)
{
        u64 ns = 0;
        u64 second;
        u32 tmp;

        tmp = desc->timestamp_96b[0] >> 16;
        tmp |= (desc->timestamp_96b[1] << 16);

        second = desc->timestamp_96b[2];
        second <<= 16;
        second |= ((desc->timestamp_96b[1] & 0xffff0000) >> 16);

        ns = second * NSEC_PER_SEC + tmp;

        return ns;
}

static void xtile_unprocess_desc_tx(struct altera_dma_private *priv) {

	u32 index;
	u32 desc_ringsize = priv->tx_ring_size * 2;

	for (index = 0; index < desc_ringsize; index++) {
		if ( priv->pref_txdesc[index].desc_control & 
				MSGDMA_PREF_DESC_CTL_OWNED_BY_HW) {
			netdev_info(priv->dev, 
				    "desc:%d: bytes %x ts %lld\n",
				    index,
				    priv->pref_txdesc[index].bytes_transferred,
				    timestamp_to_ns(&priv->pref_txdesc[index]));
		}
	}
}

static void xtile_dma_regs(struct altera_dma_private *priv)
{

	xtile_seq_no_dump_tx(priv);
	xtile_process_seq_no_tx(priv);
	xtile_fifo_fill_level_tx(priv);
	xtile_dispatcher_reg_dump_tx(priv);
	xtile_prefetcher_reg_dump_tx(priv);
	
	xtile_seq_no_dump_rx(priv);
	xtile_dispatcher_reg_dump_rx(priv);
	xtile_prefetcher_reg_dump_rx(priv);
	xtile_fifo_fill_level_rx(priv);
}

static ssize_t show_msgdma_reg_dump(struct device *dev,
                struct device_attribute *attr, char *buf)
{

        struct platform_device *pdev = to_platform_device(dev);
        struct net_device *ndev = platform_get_drvdata(pdev);
        struct intel_fpga_etile_eth_private *priv=  netdev_priv(ndev);

        xtile_dma_regs(&priv->dma_priv);

        return sprintf(buf, "%x", 1);
}

static ssize_t show_msgdma_tx_desc_dump(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct platform_device *pdev = to_platform_device(dev);
        struct net_device *ndev = platform_get_drvdata(pdev);
        struct intel_fpga_etile_eth_private *priv=  netdev_priv(ndev);

	xtile_unprocess_desc_tx(&priv->dma_priv);
	
	return sprintf(buf, "%x", 1);
}

static DEVICE_ATTR(msgdma_reg_dump, 0644, show_msgdma_reg_dump, NULL);
static DEVICE_ATTR(msgdma_tx_desc_dump, 0644, show_msgdma_tx_desc_dump, NULL);

static struct attribute *msgdma_sysfs_attrs[] = {
        &dev_attr_msgdma_reg_dump.attr,
	&dev_attr_msgdma_tx_desc_dump.attr,
        NULL
};

static const struct attribute_group msgdma_attr_group = {
        .attrs = msgdma_sysfs_attrs,
};

const struct attribute_group *msgdma_attr_groups[] = {
        &msgdma_attr_group,
        NULL
};

