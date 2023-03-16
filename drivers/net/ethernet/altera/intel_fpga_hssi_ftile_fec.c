// SPDX-License-Identifier: GPL-2.0
/* Intel FPGA E-tile Forward Error Correction (FEC) Linux driver
 * Copyright (C) 2020-2022 Intel Corporation. All rights reserved.
 *
 * Contributors:
 *   Joyce Ooi
 */

#include <linux/bitops.h>
#include <linux/if_vlan.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/of_net.h>
#include <linux/of_platform.h>
#include <linux/phy.h>
#include <linux/platform_device.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/phylink.h>
//#include "intel_fpga_eth_main.h"
#include "altera_eth_dma.h"
#include "intel_fpga_eth_ftile.h"
#include "intel_fpga_eth_hssi_itf.h"

#define MAX_COUNT_OFFSET		64000



void ftile_ui_adjustments(struct timer_list *t);



/* Calculate Unit Interval Adjustments */
void ftile_ui_adjustments(struct timer_list *t)
{
	intel_fpga_xtile_eth_private *priv = from_timer(priv, t, fec_timer);
	struct platform_device *pdev = priv->pdev_hssi;
	u32 chan = priv->tile_chan;
	static u64 start_jiffies;
	static u8 initial_done = 0;
	static u64 tx_tam_initial, rx_tam_initial;
	static u32 tx_tam_count_initial, rx_tam_count_initial;
	u32 tx_tam_l_initial, tx_tam_h_initial, ptp_tx_uim_tam_info1;
	u32 rx_tam_l_initial, rx_tam_h_initial, ptp_rx_uim_tam_info1;
	u32 tx_tam_l_nth, tx_tam_h_nth, tx_tam_count_nth;
	u32 rx_tam_l_nth, rx_tam_h_nth, rx_tam_count_nth;
	u64 tx_tam_nth, rx_tam_nth;
	u32 tx_tam_interval = 0, rx_tam_interval = 0;
	u32 ui_value, tx_tam_count, rx_tam_count;
	u8  tx_tam_valid, rx_tam_valid;
	u64 tx_tam_delta, rx_tam_delta;
	u64 tx_ui, rx_ui;

	if (!initial_done) {
		/* Set tam_snapshot to 1 to take the first snapshot of the Time of
		 * Alignment marker (TAM)
		 */
		 hssi_set_bit(pdev, HSSI_ETH_RECONFIG, chan,  eth_soft_csroffs(ptp_uim_tam_snapshot),
			    ETH_TX_TAM_SNAPSHOT | ETH_RX_TAM_SNAPSHOT,true);

		/* Read snapshotted initial TX TAM and counter values */
		tx_tam_l_initial = hssi_csrrd32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_tx_uim_tam_info0));
		ptp_tx_uim_tam_info1 = hssi_csrrd32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_tx_uim_tam_info1));
		tx_tam_h_initial = ptp_tx_uim_tam_info1 & ETH_TX_TAM_HI_NS;
		tx_tam_initial = ((u64)tx_tam_h_initial << 32) | tx_tam_l_initial;
		tx_tam_count_initial = (ptp_tx_uim_tam_info1 & ETH_TX_TAM_CNT_MASK) >> ETH_TX_TAM_CNT_SHIFT;
		tx_tam_valid = (ptp_tx_uim_tam_info1 & ETH_TX_TAM_VALID) ? 1 : 0;

		/* Read snapshotted initial RX TAM and counter values */
		rx_tam_l_initial = hssi_csrrd32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_rx_uim_tam_info0));
		ptp_rx_uim_tam_info1 = hssi_csrrd32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_rx_uim_tam_info1));
		rx_tam_h_initial = ptp_rx_uim_tam_info1 & ETH_RX_TAM_HI_NS;
		rx_tam_initial = ((u64)rx_tam_h_initial << 32) | rx_tam_l_initial;
		rx_tam_count_initial = (ptp_rx_uim_tam_info1 & ETH_RX_TAM_CNT_MASK) >> ETH_RX_TAM_CNT_SHIFT;
		rx_tam_valid = (ptp_rx_uim_tam_info1 & ETH_RX_TAM_VALID) ? 1 : 0;

		/* Clear snapshot */
		hssi_clear_bit(pdev, HSSI_ETH_RECONFIG, chan,  eth_soft_csroffs(ptp_uim_tam_snapshot),
			      ETH_TX_TAM_SNAPSHOT | ETH_RX_TAM_SNAPSHOT,true);
		if (!rx_tam_valid || !tx_tam_valid) {
			dev_warn(priv->device, "%s: Initial rx_tam_valid=%u tx_tam_valid=%u\n" , __func__, rx_tam_valid, tx_tam_valid);
			goto ui_init_restart;
		}
	} else {
		/* Wait for a few TAM interval */
		// udelay(5300);

		/* Request snapshot of Nth TX TAM and RX TAM */
		 hssi_set_bit(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_uim_tam_snapshot),
			    ETH_TX_TAM_SNAPSHOT | ETH_RX_TAM_SNAPSHOT,true);

		/* Read snapshotted of Nth TX TAM and counter values */
		tx_tam_l_nth =  hssi_csrrd32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_tx_uim_tam_info0));
		ptp_tx_uim_tam_info1 =  hssi_csrrd32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_tx_uim_tam_info1));
		tx_tam_h_nth = ptp_tx_uim_tam_info1 & ETH_TX_TAM_HI_NS;
		tx_tam_nth = ((u64)tx_tam_h_nth << 32) | tx_tam_l_nth;
		tx_tam_count_nth = (ptp_tx_uim_tam_info1 & ETH_TX_TAM_CNT_MASK) >> ETH_TX_TAM_CNT_SHIFT;
		tx_tam_valid = (ptp_tx_uim_tam_info1 & ETH_TX_TAM_VALID) ? 1 : 0;

		/* Read snapshotted of Nth RX TAM and counter values */
		rx_tam_l_nth =  hssi_csrrd32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_rx_uim_tam_info0));
		ptp_rx_uim_tam_info1 =  hssi_csrrd32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_rx_uim_tam_info1));
		rx_tam_h_nth = ptp_rx_uim_tam_info1 & ETH_RX_TAM_HI_NS;
		rx_tam_nth = ((u64)rx_tam_h_nth << 32) | rx_tam_l_nth;
		rx_tam_count_nth = (ptp_rx_uim_tam_info1 & ETH_RX_TAM_CNT_MASK) >> ETH_RX_TAM_CNT_SHIFT;
		rx_tam_valid = (ptp_rx_uim_tam_info1 & ETH_RX_TAM_VALID) ? 1 : 0;

		/* Clear snapshot */
		 hssi_clear_bit(pdev, HSSI_ETH_RECONFIG, chan, eth_soft_csroffs(ptp_uim_tam_snapshot),
			      ETH_TX_TAM_SNAPSHOT | ETH_RX_TAM_SNAPSHOT,true);
		if ((get_jiffies_64() - start_jiffies) > HZ) {
			dev_warn(priv->device, "%s: 1st to Nth snapshot takes more than 1 second\n" , __func__);
			goto ui_restart;
		} else if (!rx_tam_valid || !tx_tam_valid) {
			dev_warn(priv->device, "%s: Nth rx_tam_valid=%u tx_tam_valid=%u\n" , __func__, rx_tam_valid, tx_tam_valid);
			goto ui_restart;
		}

		/* Calculate new UI value */
		/* Reference Time (TAM) interval = AM interval * Unit interval of serial bit
		 * AM interval for No FEC for 10/25GbE: TX = 5406720, RX = 168960
		 * AM interval for KR-FEC for 25GbE: TX = 5406720, RX = 5406720
		 * Values from doc. table 92
		 */
		/* Step 7a Get TAM interval */
		switch (0) {
		case INTEL_FPGA_FTILE_ETH_RATE_10G_25G:  // 10G / 25G
			if (!strcasecmp(priv->fec_type, "kr-fec")) {
				tx_tam_interval = 5406720;
				rx_tam_interval = 5406720;
			} else if (!strcasecmp(priv->fec_type, "no-fec")) {
				tx_tam_interval = 5406720;
				rx_tam_interval = 168960;
			}
			break;
		case INTEL_FPGA_FTILE_ETH_RATE_50G:      // 50G
			if (!strcasecmp(priv->fec_type, "no-fec")) {
				tx_tam_interval = 4325376;
				rx_tam_interval = 4325376;
			} else { // KR-FEC / KP-FEC / LL-FEC
				tx_tam_interval = 5406720;
				rx_tam_interval = 5406720;
			}
			break;
		case INTEL_FPGA_FTILE_ETH_RATE_40G_100G: // 40G / 100G
		case INTEL_FPGA_FTILE_ETH_RATE_200G:     // 200G
			tx_tam_interval = 21626880;
			rx_tam_interval = 21626880;
			break;
		case INTEL_FPGA_FTILE_ETH_RATE_400G:     // 400G
			tx_tam_interval = 43253760;
			rx_tam_interval = 43253760;
			break;
		default:
			dev_warn(priv->device, "%s: Eth rate %u unknown\n", __func__, 0);
			break;
		}

		/* Calculate time elapsed */
		if (tx_tam_nth <= tx_tam_initial)
			tx_tam_delta = tx_tam_nth + 0x3B9ACA000000UL - tx_tam_initial; // 10^9 ns = 0x3B9ACA000000
		else
			tx_tam_delta = tx_tam_nth - tx_tam_initial;

		if (rx_tam_nth <= rx_tam_initial)
			rx_tam_delta = rx_tam_nth + 0x3B9ACA000000UL - rx_tam_initial; // 10^9 ns = 0x3B9ACA000000
		else
			rx_tam_delta = rx_tam_nth - rx_tam_initial;
		dev_info(priv->device, "%s tx_tam_initial:0x%llx tx_tam_nth:0x%llx tx_tam_delta:0x%llx\n", __func__, tx_tam_initial, tx_tam_nth, tx_tam_delta);
		dev_info(priv->device, "%s rx_tam_initial:0x%llx rx_tam_nth:0x%llx rx_tam_delta:0x%llx\n", __func__, rx_tam_initial, rx_tam_nth, rx_tam_delta);

		// TBD add other PHY modes and ui_value for those...
		switch (priv->phy_iface) {
		case PHY_INTERFACE_MODE_10GKR:
		case PHY_INTERFACE_MODE_10GBASER:
			ui_value = INTEL_FPGA_FTILE_UI_VALUE_10G;
			break;
		case PHY_INTERFACE_MODE_25GKR:
			ui_value = INTEL_FPGA_FTILE_UI_VALUE_25G;
			break;
		default:
			ui_value = 0; //invalid value
		}

		/* Step 7c Calculate TAM count value */
		if (tx_tam_count_nth <= tx_tam_count_initial)
			tx_tam_count = (tx_tam_count_nth + (1 << 15)) - tx_tam_count_initial;
		else
			tx_tam_count = tx_tam_count_nth - tx_tam_count_initial;
		if (rx_tam_count_nth <= rx_tam_count_initial)
			rx_tam_count = (rx_tam_count_nth + (1 << 15)) - rx_tam_count_initial;
		else
			rx_tam_count = rx_tam_count_nth - rx_tam_count_initial;
		dev_info(priv->device, "%s tx_tam_count_initial:0x%08x tx_tam_count_nth:0x%08x tx_tam_count:0x%08x\n", __func__, tx_tam_count_initial, tx_tam_count_nth, tx_tam_count);
		dev_info(priv->device, "%s rx_tam_count_initial:0x%08x rx_tam_count_nth:0x%08x rx_tam_count:0x%08x\n", __func__, rx_tam_count_initial, rx_tam_count_nth, rx_tam_count);

		/* Step 7d Calculate UI value */
		// Make sure the format is {4-bit nanoseconds, 28-bit fractional nanoseconds}
		tx_ui = (tx_tam_delta << 12) / (((u64)tx_tam_count * tx_tam_interval) / 1/*priv->num_lanes*/); // priv->num_lanes=PL
		rx_ui = (rx_tam_delta << 12) / (((u64)rx_tam_count * rx_tam_interval) /1/* priv->num_lanes*/); // priv->num_lanes=PL
		dev_info(priv->device, "%s tx_ui:0x%08llx rx_ui:0x%08llx\n", __func__, tx_ui, rx_ui);

		// check new tx_ui / rx_ui against min./max. ui_value
		if (tx_ui > 0x9EE420 || tx_ui < 0x9EDC00) {
			dev_warn(priv->device, "%s: TX UI value (0x%llX) is not within "
			       "0x9EDC00 to 0x9EE420 range\n", __func__, tx_ui);
			goto ui_restart;
		}
		if (rx_ui > 0x9EE420 || rx_ui < 0x9EDC00) {
			dev_warn(priv->device, "%s: RX UI value (0x%llX) is not within "
			       "0x9EDC00 to 0x9EE420 range\n", __func__, rx_ui);
			goto ui_restart;
		}
		hssi_csrwr32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_mac_ptp_csroffs(0, tx_ptp_ui),tx_ui);
		hssi_csrwr32_ba(pdev, HSSI_ETH_RECONFIG, chan, eth_mac_ptp_csroffs(0, rx_ptp_ui),rx_ui);
	}
ui_restart:
	if (initial_done) {
		tx_tam_initial       = tx_tam_nth;
		rx_tam_initial       = rx_tam_nth;
		tx_tam_count_initial = tx_tam_count_nth;
		rx_tam_count_initial = rx_tam_count_nth;
	} else
		initial_done = 1;
	start_jiffies = get_jiffies_64();
ui_init_restart:
	mod_timer(&priv->fec_timer, jiffies + msecs_to_jiffies(500));

}

MODULE_LICENSE("GPL");
