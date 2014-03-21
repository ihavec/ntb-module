/*
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 *   redistributing this file, you may do so under either license.
 *
 *   GPL LICENSE SUMMARY
 *
 *   Copyright(c) 2012 Intel Corporation. All rights reserved.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2012 Intel Corporation. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copy
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Intel PCIe NTB Network Linux driver
 *
 * Contact Information:
 * Jon Mason <jon.mason@intel.com>
 */
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/ntb.h>

#define NTB_NETDEV_VER	"0.7"

MODULE_DESCRIPTION(KBUILD_MODNAME);
MODULE_VERSION(NTB_NETDEV_VER);
MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Intel Corporation");

static int num_qps = 1;
module_param(num_qps, uint, 0644);
MODULE_PARM_DESC(num_qps, "Number of NTB transport connections");

struct ntb_netdev {
	struct list_head list;
	struct pci_dev *pdev;
	struct net_device *ndev;
	struct ntb_transport_qp **qp;
};

#define	NTB_TX_TIMEOUT_MS	1000
#define	NTB_RXQ_SIZE		100

static LIST_HEAD(dev_list);

static void ntb_netdev_event_handler(void *data, int status)
{
	struct net_device *ndev = data;
	struct ntb_netdev *dev = netdev_priv(ndev);
	int i;

	switch (status) {
	case NTB_LINK_DOWN:
		netif_carrier_off(ndev);
		break;
	case NTB_LINK_UP:
		for (i = 0; i < num_qps; i++)
			if (!ntb_transport_link_query(dev->qp[i]))
				return;

		netif_carrier_on(ndev);
		break;
	default:
		netdev_warn(ndev, "Unsupported event type %d\n", status);
	}
}

static void ntb_netdev_rx_handler(struct ntb_transport_qp *qp, void *qp_data,
				  void *data, int len)
{
	struct net_device *ndev = qp_data;
	struct sk_buff *skb;
	int rc;
	u16 rx_queue;

	skb = data;
	if (!skb)
		return;

	rx_queue = ntb_transport_qp_num(qp);

	netdev_dbg(ndev, "%s: %d byte payload received on qp %d\n",
		   __func__, len, rx_queue);

	skb_put(skb, len);
	skb->protocol = eth_type_trans(skb, ndev);
	skb->ip_summed = CHECKSUM_NONE;
	skb_record_rx_queue(skb, rx_queue);

	if (netif_rx(skb) == NET_RX_DROP) {
		ndev->stats.rx_errors++;
		ndev->stats.rx_dropped++;
	} else {
		ndev->stats.rx_packets++;
		ndev->stats.rx_bytes += len;
	}

	skb = netdev_alloc_skb(ndev, ndev->mtu + ETH_HLEN);
	if (!skb) {
		ndev->stats.rx_errors++;
		ndev->stats.rx_frame_errors++;
		return;
	}

	rc = ntb_transport_rx_enqueue(qp, skb, skb->data, ndev->mtu + ETH_HLEN);
	if (rc) {
		dev_kfree_skb(skb);
		ndev->stats.rx_errors++;
		ndev->stats.rx_fifo_errors++;
	}
}

static void ntb_netdev_tx_handler(struct ntb_transport_qp *qp, void *qp_data,
				  void *data, int len)
{
	struct net_device *ndev = qp_data;
	struct sk_buff *skb;

	skb = data;
	if (!skb || !ndev)
		return;

	if (len > 0) {
		ndev->stats.tx_packets++;
		ndev->stats.tx_bytes += skb->len;
	} else {
		ndev->stats.tx_errors++;
		ndev->stats.tx_aborted_errors++;
	}

	dev_kfree_skb(skb);
}

static netdev_tx_t ntb_netdev_start_xmit(struct sk_buff *skb,
					 struct net_device *ndev)
{
	struct ntb_netdev *dev = netdev_priv(ndev);
	struct netdev_queue *txq;
	int rc, qp_num;

	qp_num = skb->queue_mapping;
	txq = netdev_get_tx_queue(ndev, qp_num);

	netdev_dbg(ndev, "%s: transmitting %d byte payload on qp %d\n",
		   __func__, skb->len, qp_num);

	rc = ntb_transport_tx_enqueue(dev->qp[qp_num], skb, skb->data,
				      skb->len);
	if (rc)
		goto err;

	return NETDEV_TX_OK;

err:
	ndev->stats.tx_dropped++;
	ndev->stats.tx_errors++;
	return NETDEV_TX_BUSY;
}

static int ntb_netdev_open(struct net_device *ndev)
{
	struct ntb_netdev *dev = netdev_priv(ndev);
	struct sk_buff *skb;
	int rc, i, len, qp_num;

	netif_carrier_off(ndev);

	/* Add some empty rx bufs */
	for (qp_num = 0; qp_num < num_qps; qp_num++)
		for (i = 0; i < NTB_RXQ_SIZE; i++) {
			skb = netdev_alloc_skb(ndev, ndev->mtu + ETH_HLEN);
			if (!skb) {
				rc = -ENOMEM;
				goto err;
			}

			rc = ntb_transport_rx_enqueue(dev->qp[qp_num], skb,
						      skb->data,
						      ndev->mtu + ETH_HLEN);
			if (rc == -EINVAL)
				goto err;
		}

	for (qp_num = 0; qp_num < num_qps; qp_num++)
		ntb_transport_link_up(dev->qp[qp_num]);

	return 0;

err:
	for (qp_num = 0; qp_num < num_qps; qp_num++)
		while ((skb = ntb_transport_rx_remove(dev->qp[qp_num], &len)))
			dev_kfree_skb(skb);
	return rc;
}

static int ntb_netdev_close(struct net_device *ndev)
{
	struct ntb_netdev *dev = netdev_priv(ndev);
	struct sk_buff *skb;
	int len, qp_num;

	for (qp_num = 0; qp_num < num_qps; qp_num++)
		ntb_transport_link_down(dev->qp[qp_num]);

	for (qp_num = 0; qp_num < num_qps; qp_num++)
		while ((skb = ntb_transport_rx_remove(dev->qp[qp_num], &len)))
			dev_kfree_skb(skb);

	return 0;
}

static int ntb_netdev_change_mtu(struct net_device *ndev, int new_mtu)
{
	struct ntb_netdev *dev = netdev_priv(ndev);
	struct sk_buff *skb;
	int len, qpn, rc;

	for (qpn = 0; qpn < num_qps; qpn++)
		if (new_mtu > ntb_transport_max_size(dev->qp[qpn]) - ETH_HLEN)
			return -EINVAL;

	if (!netif_running(ndev)) {
		ndev->mtu = new_mtu;
		return 0;
	}

	/* Bring down the link and dispose of posted rx entries */
	for (qpn = 0; qpn < num_qps; qpn++)
		ntb_transport_link_down(dev->qp[qpn]);

	if (ndev->mtu < new_mtu) {
		int i;

		for (qpn = 0; qpn < num_qps; qpn++) {
			for (i = 0;
			     (skb = ntb_transport_rx_remove(dev->qp[qpn],
							    &len));
			     i++)
				dev_kfree_skb(skb);

			for (; i; i--) {
				skb = netdev_alloc_skb(ndev,
						       new_mtu + ETH_HLEN);
				if (!skb) {
					rc = -ENOMEM;
					goto err;
				}

				rc = ntb_transport_rx_enqueue(dev->qp[qpn],
							      skb, skb->data,
							      new_mtu +
							      ETH_HLEN);
				if (rc) {
					dev_kfree_skb(skb);
					goto err;
				}
			}
		}
	}

	ndev->mtu = new_mtu;

	for (qpn = 0; qpn < num_qps; qpn++)
		ntb_transport_link_up(dev->qp[qpn]);

	return 0;

err:
	for (qpn = 0; qpn < num_qps; qpn++)
		ntb_transport_link_down(dev->qp[qpn]);

	for (qpn = 0; qpn < num_qps; qpn++)
		while ((skb = ntb_transport_rx_remove(dev->qp[qpn], &len)))
			dev_kfree_skb(skb);

	netdev_err(ndev, "Error changing MTU, device inoperable\n");
	return rc;
}

static const struct net_device_ops ntb_netdev_ops = {
	.ndo_open = ntb_netdev_open,
	.ndo_stop = ntb_netdev_close,
	.ndo_start_xmit = ntb_netdev_start_xmit,
	.ndo_change_mtu = ntb_netdev_change_mtu,
	.ndo_set_mac_address = eth_mac_addr,
};

static void ntb_get_drvinfo(struct net_device *ndev,
			    struct ethtool_drvinfo *info)
{
	struct ntb_netdev *dev = netdev_priv(ndev);

	strlcpy(info->driver, KBUILD_MODNAME, sizeof(info->driver));
	strlcpy(info->version, NTB_NETDEV_VER, sizeof(info->version));
	strlcpy(info->bus_info, pci_name(dev->pdev), sizeof(info->bus_info));
}

static int ntb_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	cmd->supported = SUPPORTED_Backplane;
	cmd->advertising = ADVERTISED_Backplane;
	cmd->speed = SPEED_UNKNOWN;
	ethtool_cmd_speed_set(cmd, SPEED_UNKNOWN);
	cmd->duplex = DUPLEX_FULL;
	cmd->port = PORT_OTHER;
	cmd->phy_address = 0;
	cmd->transceiver = XCVR_DUMMY1;
	cmd->autoneg = AUTONEG_ENABLE;
	cmd->maxtxpkt = 0;
	cmd->maxrxpkt = 0;

	return 0;
}

static const struct ethtool_ops ntb_ethtool_ops = {
	.get_drvinfo = ntb_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_settings = ntb_get_settings,
};

static const struct ntb_queue_handlers ntb_netdev_handlers = {
	.tx_handler = ntb_netdev_tx_handler,
	.rx_handler = ntb_netdev_rx_handler,
	.event_handler = ntb_netdev_event_handler,
};

static int ntb_netdev_probe(struct pci_dev *pdev)
{
	struct net_device *ndev;
	struct ntb_netdev *dev;
	int rc, i;

	ndev = alloc_etherdev_mq(sizeof(struct ntb_netdev), num_qps);
	if (!ndev)
		return -ENOMEM;

	dev = netdev_priv(ndev);
	dev->ndev = ndev;
	dev->pdev = pdev;
	BUG_ON(!dev->pdev);
	ndev->features = NETIF_F_HIGHDMA;

	ndev->priv_flags |= IFF_LIVE_ADDR_CHANGE;

	ndev->hw_features = ndev->features;
	ndev->watchdog_timeo = msecs_to_jiffies(NTB_TX_TIMEOUT_MS);

	random_ether_addr(ndev->perm_addr);
	memcpy(ndev->dev_addr, ndev->perm_addr, ndev->addr_len);

	ndev->netdev_ops = &ntb_netdev_ops;
	SET_ETHTOOL_OPS(ndev, &ntb_ethtool_ops);

	dev->qp = kcalloc(num_qps, sizeof(struct ntb_transport_qp *),
			  GFP_KERNEL);
	if (!dev->qp) {
		rc = -ENOMEM;
		goto err;
	}

	ndev->mtu = ~0;

	for (i = 0; i < num_qps; i++) {
		dev->qp[i] = ntb_transport_create_queue(ndev, pdev,
							&ntb_netdev_handlers);
		if (!dev->qp[i]) {
			rc = -EIO;
			goto err1;
		}

		ndev->mtu = min(ntb_transport_max_size(dev->qp[i]) - ETH_HLEN,
				ndev->mtu);
	}

	netif_set_real_num_tx_queues(ndev, num_qps);
	netif_set_real_num_rx_queues(ndev, num_qps);

	rc = register_netdev(ndev);
	if (rc)
		goto err1;

	list_add(&dev->list, &dev_list);
	dev_info(&pdev->dev, "%s created\n", ndev->name);
	return 0;

err1:
	for (i--; i >= 0 && dev->qp[i]; i--)
		ntb_transport_free_queue(dev->qp[i]);
	kfree(dev->qp);
err:
	free_netdev(ndev);
	return rc;
}

static void ntb_netdev_remove(struct pci_dev *pdev)
{
	struct net_device *ndev;
	struct ntb_netdev *dev;
	int i;

	list_for_each_entry(dev, &dev_list, list) {
		if (dev->pdev == pdev)
			break;
	}
	if (dev == NULL)
		return;

	list_del(&dev->list);

	ndev = dev->ndev;

	unregister_netdev(ndev);
	for (i = 0; i < num_qps; i++)
		ntb_transport_free_queue(dev->qp[i]);
	kfree(dev->qp);
	free_netdev(ndev);
}

static struct ntb_client ntb_netdev_client = {
	.driver.name = KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.probe = ntb_netdev_probe,
	.remove = ntb_netdev_remove,
};

static int __init ntb_netdev_init_module(void)
{
	int rc;

	rc = ntb_register_client_dev(KBUILD_MODNAME);
	if (rc)
		return rc;
	return ntb_register_client(&ntb_netdev_client);
}
module_init(ntb_netdev_init_module);

static void __exit ntb_netdev_exit_module(void)
{
	ntb_unregister_client(&ntb_netdev_client);
	ntb_unregister_client_dev(KBUILD_MODNAME);
}
module_exit(ntb_netdev_exit_module);
