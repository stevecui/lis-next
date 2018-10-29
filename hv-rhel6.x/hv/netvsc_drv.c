/*
 * Copyright (c) 2009, Microsoft Corporation.
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
 * this program; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *   Haiyang Zhang <haiyangz@microsoft.com>
 *   Hank Janssen  <hjanssen@microsoft.com>
 */
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/init.h>
#include <linux/atomic.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/device.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/if_vlan.h>
#include <linux/in.h>
#include <linux/slab.h>
#include <linux/ipv6.h>
#include <linux/mii.h>

#include <net/arp.h>
#include <net/route.h>
#include <net/sock.h>
#include <net/udp.h>
#include <net/pkt_sched.h>
#include <net/checksum.h>
#include <net/ip6_checksum.h>
#include <net/flow_keys.h>
#include <net/bonding.h>

#include <linux/rtnetlink.h>
#include <linux/netpoll.h>

#include "include/linux/hyperv.h"
#include "hyperv_net.h"

#define RING_SIZE_MIN 64
#define LINKCHANGE_INT (2 * HZ)
#ifdef CONFIG_NET_POLL_CONTROLLER
atomic_t netpoll_block_tx = ATOMIC_INIT(0);
#endif

static int ring_size = 128;
module_param(ring_size, int, 0444);
MODULE_PARM_DESC(ring_size, "Ring buffer size (# of pages)");

static const u32 default_msg = NETIF_MSG_DRV | NETIF_MSG_PROBE |
		NETIF_MSG_LINK | NETIF_MSG_IFUP |
		NETIF_MSG_IFDOWN | NETIF_MSG_RX_ERR |
		NETIF_MSG_TX_ERR;

static int debug = -1;
module_param(debug, int, 0444);
MODULE_PARM_DESC(debug, "Debug level (0=none,...,16=all)");

static void netvsc_set_multicast_list(struct net_device *net)
{
	struct net_device_context *net_device_ctx = netdev_priv(net);
	struct netvsc_device *nvdev = rtnl_dereference(net_device_ctx->nvdev);

	rndis_filter_update(nvdev);
}

static int netvsc_open(struct net_device *net)
{
	struct net_device_context *ndev_ctx = netdev_priv(net);
	struct net_device *vf_netdev = rtnl_dereference(ndev_ctx->vf_netdev);
	struct netvsc_device *nvdev = ndev_ctx->nvdev;
	struct rndis_device *rdev;
	int ret = 0;

	netif_carrier_off(net);

	/* Open up the device */
	ret = rndis_filter_open(nvdev);
	if (ret != 0) {
		netdev_err(net, "unable to open device (ret %d).\n", ret);
		return ret;
	}

	netif_tx_wake_all_queues(net);

	rdev = nvdev->extension;
	if (!rdev->link_state)
		netif_carrier_on(net);

	if (vf_netdev) {
		/* Setting synthetic device up transparently sets
		 * slave as up. If open fails, then slave will be
		 * still be offline (and not used).
		 */
		ret = dev_open(vf_netdev);
		if (ret)
			netdev_warn(net,
				    "unable to open slave: %s: %d\n",
				    vf_netdev->name, ret);
	}

	return 0;

}

static int netvsc_close(struct net_device *net)
{
	struct net_device_context *net_device_ctx = netdev_priv(net);
	struct net_device *vf_netdev
			= rtnl_dereference(net_device_ctx->vf_netdev);
	struct netvsc_device *nvdev = net_device_ctx->nvdev;
	int ret = 0;
	u32 aread, i, msec = 10, retry = 0, retry_max = 20;
	struct vmbus_channel *chn;
        WARN_ONCE(1,"hello");
    printk("cl_0:cls:%lx\n",(uintptr_t)net);
	netif_tx_disable(net);
	printk("cl_1\n");

	/* No need to close rndis filter if it is removed already */
	if (!nvdev)
		goto out;

	ret = rndis_filter_close(nvdev);
	
	printk("cl_2\n");
	if (ret != 0) {
		netdev_err(net, "unable to close device (ret %d).\n", ret);
		return ret;
	}
	printk("cl_3\n");

	/* Ensure pending bytes in ring are read */
	while (true) {
		printk("cl_4\n");
		aread = 0;
		for (i = 0; i < nvdev->num_chn; i++) {
			chn = nvdev->chan_table[i].channel;
			if (!chn)
				continue;

			aread = hv_get_bytes_to_read(&chn->inbound);
			if (aread)
				break;

			aread = hv_get_bytes_to_read(&chn->outbound);
			if (aread)
				break;
		}
		printk("cl_5\n");

		retry++;
		if (retry > retry_max || aread == 0)
			break;

		msleep(msec);

		if (msec < 1000)
			msec *= 2;
	}
	printk("cl_6\n");

	if (aread) {
		netdev_err(net, "Ring buffer not empty after closing rndis\n");
		ret = -ETIMEDOUT;
	}

out:
	if (vf_netdev)
		dev_close(vf_netdev);

	return ret;
}

static void *init_ppi_data(struct rndis_message *msg, u32 ppi_size,
				int pkt_type)
{
	struct rndis_packet *rndis_pkt;
	struct rndis_per_packet_info *ppi;

	rndis_pkt = &msg->msg.pkt;
	rndis_pkt->data_offset += ppi_size;

	ppi = (struct rndis_per_packet_info *)((void *)rndis_pkt +
		rndis_pkt->per_pkt_info_offset + rndis_pkt->per_pkt_info_len);

	ppi->size = ppi_size;
	ppi->type = pkt_type;
	ppi->ppi_offset = sizeof(struct rndis_per_packet_info);

	rndis_pkt->per_pkt_info_len += ppi_size;

	return ppi;
}

#ifdef NOTYET
static inline int netvsc_get_tx_queue(struct net_device *ndev,
				      struct sk_buff *skb, int old_idx)
{
	const struct net_device_context *ndc = netdev_priv(ndev);
	struct sock *sk = skb->sk;
	int q_idx;

	q_idx = ndc->tx_table[skb_get_hash(skb) &
			      (VRSS_SEND_TAB_SIZE - 1)];

	/* If queue index changed record the new value */
	if (q_idx != old_idx &&
	    sk && sk_fullsock(sk) && rcu_access_pointer(sk->sk_dst_cache))
		sk_tx_queue_set(sk, q_idx);

	return q_idx;
}
#endif



static u16 netvsc_pick_tx(struct net_device *ndev, struct sk_buff *skb)
{
	struct net_device_context *net_device_ctx = netdev_priv(ndev);
	u32 hash;
	u16 q_idx = 0;

	if (ndev->real_num_tx_queues <= 1)
		return 0;

	if (netvsc_set_hash(&hash, skb)) {
		q_idx = net_device_ctx->tx_table[hash % VRSS_SEND_TAB_SIZE] %
			ndev->real_num_tx_queues;
		skb_set_hash(skb, hash, PKT_HASH_TYPE_L3);
	}

	return q_idx;
}


#ifdef NOTYET
// Divergence from upstream commit:
// 5b54dac856cb5bd6f33f4159012773e4a33704f7
static u16 netvsc_select_queue(struct net_device *ndev, struct sk_buff *skb,
			       void *accel_priv,
			       select_queue_fallback_t fallback)
#endif
static u16 netvsc_select_queue(struct net_device *ndev, struct sk_buff *skb)

{
	struct net_device_context *ndc = netdev_priv(ndev);
	struct net_device *vf_netdev;
	u16 txq;

	rcu_read_lock();
	vf_netdev = rcu_dereference(ndc->vf_netdev);
	if (vf_netdev) {
		txq = skb_rx_queue_recorded(skb) ? skb_get_rx_queue(skb) : 0;
		qdisc_skb_cb(skb)->slave_dev_queue_mapping = skb->queue_mapping;
	} else {
		txq = netvsc_pick_tx(ndev, skb);
	}
	rcu_read_unlock();

	while (unlikely(txq >= ndev->real_num_tx_queues))
		txq -= ndev->real_num_tx_queues;

	return txq;
}


static u32 fill_pg_buf(struct page *page, u32 offset, u32 len,
			struct hv_page_buffer *pb)
{
	int j = 0;

	/* Deal with compund pages by ignoring unused part
	 * of the page.
	 */
	page += (offset >> PAGE_SHIFT);
	offset &= ~PAGE_MASK;

	while (len > 0) {
		unsigned long bytes;

		bytes = PAGE_SIZE - offset;
		if (bytes > len)
			bytes = len;
		pb[j].pfn = page_to_pfn(page);
		pb[j].offset = offset;
		pb[j].len = bytes;

		offset += bytes;
		len -= bytes;

		if (offset == PAGE_SIZE && len) {
			page++;
			offset = 0;
			j++;
		}
	}

	return j + 1;
}

/* Send skb on the slave VF device. */
static int netvsc_vf_xmit(struct net_device *net, struct net_device *vf_netdev,
		  struct sk_buff *skb)
{
	struct net_device_context *ndev_ctx = netdev_priv(net);
	unsigned int len = skb->len;
	int rc;

	skb->dev = vf_netdev;
	skb->queue_mapping = qdisc_skb_cb(skb)->slave_dev_queue_mapping;

	rc = dev_queue_xmit(skb);
	if (likely(rc == NET_XMIT_SUCCESS || rc == NET_XMIT_CN)) {
		struct netvsc_vf_pcpu_stats *pcpu_stats
			= this_cpu_ptr(ndev_ctx->vf_stats);

		u64_stats_update_begin(&pcpu_stats->syncp);
		pcpu_stats->tx_packets++;
		pcpu_stats->tx_bytes += len;
		u64_stats_update_end(&pcpu_stats->syncp);
	} else {
		this_cpu_inc(ndev_ctx->vf_stats->tx_dropped);
	}

	return rc;
}

static u32 init_page_array(void *hdr, u32 len, struct sk_buff *skb,
			   struct hv_netvsc_packet *packet,
			   struct hv_page_buffer **page_buf)
{
	struct hv_page_buffer *pb = *page_buf;
	u32 slots_used = 0;
	char *data = skb->data;
	int frags = skb_shinfo(skb)->nr_frags;
	int i;

	/* The packet is laid out thus:
	 * 1. hdr: RNDIS header and PPI
	 * 2. skb linear data
	 * 3. skb fragment data
	 */
	if (hdr != NULL)
		slots_used += fill_pg_buf(virt_to_page(hdr),
					offset_in_page(hdr),
					len, &pb[slots_used]);

	packet->rmsg_size = len;
	packet->rmsg_pgcnt = slots_used;

	slots_used += fill_pg_buf(virt_to_page(data),
				offset_in_page(data),
				skb_headlen(skb), &pb[slots_used]);

	for (i = 0; i < frags; i++) {
		skb_frag_t *frag = skb_shinfo(skb)->frags + i;

		slots_used += fill_pg_buf(skb_frag_page(frag),
					frag->page_offset,
					skb_frag_size(frag), &pb[slots_used]);
	}
	return slots_used;
}

static int count_skb_frag_slots(struct sk_buff *skb)
{
	int i, frags = skb_shinfo(skb)->nr_frags;
	int pages = 0;

	for (i = 0; i < frags; i++) {
		skb_frag_t *frag = skb_shinfo(skb)->frags + i;
		unsigned long size = skb_frag_size(frag);
		unsigned long offset = frag->page_offset;

		/* Skip unused frames from start of page */
		offset &= ~PAGE_MASK;
		pages += PFN_UP(offset + size);
	}
	return pages;
}

static int netvsc_get_slots(struct sk_buff *skb)
{
	char *data = skb->data;
	unsigned int offset = offset_in_page(data);
	unsigned int len = skb_headlen(skb);
	int slots;
	int frag_slots;

	slots = DIV_ROUND_UP(offset + len, PAGE_SIZE);
	frag_slots = count_skb_frag_slots(skb);
	return slots + frag_slots;
}

static u32 net_checksum_info(struct sk_buff *skb)
{
	skb_reset_mac_header(skb);  //NHM - is this needed?

	if (skb->protocol == htons(ETH_P_IP)) {
		struct iphdr *ip = ip_hdr(skb);

		if (ip->protocol == IPPROTO_TCP)
			return TRANSPORT_INFO_IPV4_TCP;
		else if (ip->protocol == IPPROTO_UDP)
			return TRANSPORT_INFO_IPV4_UDP;
	} else {
		struct ipv6hdr *ip6 = ipv6_hdr(skb);

		if (ip6->nexthdr == IPPROTO_TCP)
			return TRANSPORT_INFO_IPV6_TCP;
		else if (ip6->nexthdr == IPPROTO_UDP)
			return TRANSPORT_INFO_IPV6_UDP;
	}

	return TRANSPORT_INFO_NOT_IP;
}

static int netvsc_start_xmit(struct sk_buff *skb, struct net_device *net)
{
	struct net_device_context *net_device_ctx = netdev_priv(net);
	struct hv_netvsc_packet *packet = NULL;
	int ret;
	unsigned int num_data_pgs;
	struct rndis_message *rndis_msg;
	struct rndis_packet *rndis_pkt;
	struct net_device *vf_netdev;
	u32 rndis_msg_size;
	struct rndis_per_packet_info *ppi;
	u32 hash;
	struct hv_page_buffer page_buf[MAX_PAGE_BUFFER_COUNT];
	struct hv_page_buffer *pb = page_buf;

	/* if VF is present and up then redirect packets
	 * already called with rcu_read_lock_bh
	 */
	vf_netdev = rcu_dereference_bh(net_device_ctx->vf_netdev);
	if (vf_netdev && netif_running(vf_netdev) &&
	    !netpoll_tx_running(net))
	return netvsc_vf_xmit(net, vf_netdev, skb);


	/* We will atmost need two pages to describe the rndis
	 * header. We can only transmit MAX_PAGE_BUFFER_COUNT number
	 * of pages in a single packet. If skb is scattered around
	 * more pages we try linearizing it.
	 */

	num_data_pgs = netvsc_get_slots(skb) + 2;

	if (unlikely(num_data_pgs > MAX_PAGE_BUFFER_COUNT)) {
		++net_device_ctx->eth_stats.tx_scattered;

		if (skb_linearize(skb))
			goto no_memory;

		num_data_pgs = netvsc_get_slots(skb) + 2;
		if (num_data_pgs > MAX_PAGE_BUFFER_COUNT) {
			++net_device_ctx->eth_stats.tx_too_big;
			goto drop;
		}
	}

	/*
	 * Place the rndis header in the skb head room and
	 * the skb->cb will be used for hv_netvsc_packet
	 * structure.
	 */
	ret = skb_cow_head(skb, RNDIS_AND_PPI_SIZE);
	if (ret)
		goto no_memory;

	/* Use the skb control buffer for building up the packet */
	BUILD_BUG_ON(sizeof(struct hv_netvsc_packet) >
			FIELD_SIZEOF(struct sk_buff, cb));
	packet = (struct hv_netvsc_packet *)skb->cb;

	/* TODO: This will likely evaluate to false, since RH7 and
	 * below kernels will set next pointer to NULL before calling
	 * into here. Should find another way to set this flag.
	 */
	packet->xmit_more = (skb->next != NULL);

	packet->q_idx = skb_get_queue_mapping(skb);

	packet->total_data_buflen = skb->len;
	packet->total_bytes = skb->len;
	packet->total_packets = 1;

	rndis_msg = (struct rndis_message *)skb->head;

	memset(rndis_msg, 0, RNDIS_AND_PPI_SIZE);

	packet->send_completion_ctx = packet;

	/* Add the rndis header */
	rndis_msg->ndis_msg_type = RNDIS_MSG_PACKET;
	rndis_msg->msg_len = packet->total_data_buflen;
	rndis_pkt = &rndis_msg->msg.pkt;
	rndis_pkt->data_offset = sizeof(struct rndis_packet);
	rndis_pkt->data_len = packet->total_data_buflen;
	rndis_pkt->per_pkt_info_offset = sizeof(struct rndis_packet);

	rndis_msg_size = RNDIS_MESSAGE_SIZE(struct rndis_packet);

#ifdef NOTYET
	// Divergence from upstream commit:
	// 307f099520b66504cf6c5638f3f404c48b9fb45b
	hash = skb_get_hash_raw(skb);
#endif
	hash = skb_get_hash(skb);
	if (hash != 0 && net->real_num_tx_queues > 1) {
		rndis_msg_size += NDIS_HASH_PPI_SIZE;
		ppi = init_ppi_data(rndis_msg, NDIS_HASH_PPI_SIZE,
				    NBL_HASH_VALUE);
		*(u32 *)((void *)ppi + ppi->ppi_offset) = hash;
	}

	if (skb_vlan_tag_present(skb)) {
		struct ndis_pkt_8021q_info *vlan;

		rndis_msg_size += NDIS_VLAN_PPI_SIZE;
		ppi = init_ppi_data(rndis_msg, NDIS_VLAN_PPI_SIZE,
					IEEE_8021Q_INFO);
		vlan = (struct ndis_pkt_8021q_info *)((void *)ppi +
						ppi->ppi_offset);
		vlan->vlanid = skb->vlan_tci & VLAN_VID_MASK;
		vlan->pri = (skb->vlan_tci & VLAN_PRIO_MASK) >>
				VLAN_PRIO_SHIFT;
	}

	if (skb_is_gso(skb)) {
		struct ndis_tcp_lso_info *lso_info;

		rndis_msg_size += NDIS_LSO_PPI_SIZE;
		ppi = init_ppi_data(rndis_msg, NDIS_LSO_PPI_SIZE,
				    TCP_LARGESEND_PKTINFO);

		lso_info = (struct ndis_tcp_lso_info *)((void *)ppi +
							ppi->ppi_offset);

		lso_info->lso_v2_transmit.type = NDIS_TCP_LARGE_SEND_OFFLOAD_V2_TYPE;
		if (skb->protocol == htons(ETH_P_IP)) {
			lso_info->lso_v2_transmit.ip_version =
				NDIS_TCP_LARGE_SEND_OFFLOAD_IPV4;
			ip_hdr(skb)->tot_len = 0;
			ip_hdr(skb)->check = 0;
			tcp_hdr(skb)->check =
				~csum_tcpudp_magic(ip_hdr(skb)->saddr,
						   ip_hdr(skb)->daddr, 0, IPPROTO_TCP, 0);
		} else {
			lso_info->lso_v2_transmit.ip_version =
				NDIS_TCP_LARGE_SEND_OFFLOAD_IPV6;
			ipv6_hdr(skb)->payload_len = 0;
			tcp_hdr(skb)->check =
				~csum_ipv6_magic(&ipv6_hdr(skb)->saddr,
						 &ipv6_hdr(skb)->daddr, 0, IPPROTO_TCP, 0);
		}
		lso_info->lso_v2_transmit.tcp_header_offset = skb_transport_offset(skb);
		lso_info->lso_v2_transmit.mss = skb_shinfo(skb)->gso_size;
	} else if (skb->ip_summed == CHECKSUM_PARTIAL) {
		if (net_checksum_info(skb) & net_device_ctx->tx_checksum_mask) {
			struct ndis_tcp_ip_checksum_info *csum_info;

			rndis_msg_size += NDIS_CSUM_PPI_SIZE;
			ppi = init_ppi_data(rndis_msg, NDIS_CSUM_PPI_SIZE,
					    TCPIP_CHKSUM_PKTINFO);

			csum_info = (struct ndis_tcp_ip_checksum_info *)((void *)ppi +
									 ppi->ppi_offset);

			csum_info->transmit.tcp_header_offset = skb_transport_offset(skb);

			if (skb->protocol == htons(ETH_P_IP)) {
				csum_info->transmit.is_ipv4 = 1;

				if (ip_hdr(skb)->protocol == IPPROTO_TCP)
					csum_info->transmit.tcp_checksum = 1;
				else
					csum_info->transmit.udp_checksum = 1;
			} else {
				csum_info->transmit.is_ipv6 = 1;

				if (ipv6_hdr(skb)->nexthdr == IPPROTO_TCP)
					csum_info->transmit.tcp_checksum = 1;
				else
					csum_info->transmit.udp_checksum = 1;
			}
		} else {
			/* Can't do offload of this type of checksum */
			if (skb_checksum_help(skb))
				goto drop;
		}
	}

	/* Start filling in the page buffers with the rndis hdr */
	rndis_msg->msg_len += rndis_msg_size;
	packet->total_data_buflen = rndis_msg->msg_len;
	packet->page_buf_cnt = init_page_array(rndis_msg, rndis_msg_size,
					       skb, packet, &pb);

#if defined(RHEL_RELEASE_VERSION) && (RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(6,3))
	/* timestamp packet in software */
	skb_tx_timestamp(skb);
#endif

	ret = netvsc_send(net_device_ctx, packet, rndis_msg, &pb, skb);
	if (likely(ret == 0))
		return NETDEV_TX_OK;

	if (ret == -EAGAIN) {
		++net_device_ctx->eth_stats.tx_busy;
		return NETDEV_TX_BUSY;
	}
drop:
	dev_kfree_skb_any(skb);
	net->stats.tx_dropped++;

	return NETDEV_TX_OK;

no_memory:
	++net_device_ctx->eth_stats.tx_no_memory;
	goto drop;
}

/*
 * netvsc_linkstatus_callback - Link up/down notification
 */
void netvsc_linkstatus_callback(struct hv_device *device_obj,
				struct rndis_message *resp)
{
	struct rndis_indicate_status *indicate = &resp->msg.indicate_status;
	struct net_device *net;
	struct net_device_context *ndev_ctx;
	struct netvsc_reconfig *event;
	unsigned long flags;

	net = hv_get_drvdata(device_obj);

	if (!net)
		return;

	ndev_ctx = netdev_priv(net);

	/* Update the physical link speed when changing to another vSwitch */
	if (indicate->status == RNDIS_STATUS_LINK_SPEED_CHANGE) {
		u32 speed;

		speed = *(u32 *)((void *)indicate + indicate->
				 status_buf_offset) / 10000;
		ndev_ctx->speed = speed;
		return;
	}

	/* Handle these link change statuses below */
	if (indicate->status != RNDIS_STATUS_NETWORK_CHANGE &&
	    indicate->status != RNDIS_STATUS_MEDIA_CONNECT &&
	    indicate->status != RNDIS_STATUS_MEDIA_DISCONNECT)
		return;

	if (net->reg_state != NETREG_REGISTERED)
		return;

	event = kzalloc(sizeof(*event), GFP_ATOMIC);
	if (!event)
		return;
	event->event = indicate->status;

	spin_lock_irqsave(&ndev_ctx->lock, flags);
	list_add_tail(&event->list, &ndev_ctx->reconfig_events);
	spin_unlock_irqrestore(&ndev_ctx->lock, flags);

	schedule_delayed_work(&ndev_ctx->dwork, 0);
}

static struct sk_buff *netvsc_alloc_recv_skb(struct net_device *net,
					     const struct ndis_tcp_ip_checksum_info *csum_info,
					     const struct ndis_pkt_8021q_info *vlan,
					     void *data, u32 buflen)
{
	struct sk_buff *skb;

	skb = netdev_alloc_skb_ip_align(net, buflen);
	if (!skb)
		return skb;

	/*
	 * Copy to skb. This copy is needed here since the memory pointed by
	 * hv_netvsc_packet cannot be deallocated
	 */
	memcpy(skb_put(skb, buflen), data, buflen);

	skb->protocol = eth_type_trans(skb, net);

	/* skb is already created with CHECKSUM_NONE */
	skb_checksum_none_assert(skb);

	/*
	 * In Linux, the IP checksum is always checked.
	 * Do L4 checksum offload if enabled and present.
	 */
	if (csum_info && (net->features & NETIF_F_RXCSUM)) {
		if (csum_info->receive.tcp_checksum_succeeded ||
		    csum_info->receive.udp_checksum_succeeded)
			skb->ip_summed = CHECKSUM_UNNECESSARY;
	}

	if (vlan) {
		u16 vlan_tci = vlan->vlanid | (vlan->pri << VLAN_PRIO_SHIFT);

		__vlan_hwaccel_put_tag(skb, vlan_tci);
	}

	return skb;
}

/*
 * netvsc_recv_callback -  Callback when we receive a packet from the
 * "wire" on the specified device.
 */
int netvsc_recv_callback(struct net_device *net,
			 struct vmbus_channel *channel,
			 void  *data, u32 len,
			 const struct ndis_tcp_ip_checksum_info *csum_info,
			 const struct ndis_pkt_8021q_info *vlan)
{
	struct net_device_context *net_device_ctx = netdev_priv(net);
	struct netvsc_device *net_device = net_device_ctx->nvdev;
	u16 q_idx = channel->offermsg.offer.sub_channel_index;
	struct netvsc_channel *nvchan = &net_device->chan_table[q_idx];
	struct sk_buff *skb;
	struct sk_buff *vf_skb;
	struct netvsc_stats *rx_stats;
	int ret = 0;
    //printk("rx_pas\n");
	if (!net || net->reg_state != NETREG_REGISTERED)
		return NVSP_STAT_FAIL;

	if (READ_ONCE(net_device_ctx->vf_inject)) {
		atomic_inc(&net_device_ctx->vf_use_cnt);
		if (!READ_ONCE(net_device_ctx->vf_inject)) {
			/*
			 * We raced; just move on.
			 */
			atomic_dec(&net_device_ctx->vf_use_cnt);
			goto vf_injection_done;
		}

		/*
		 * Inject this packet into the VF inerface.
		 * On Hyper-V, multicast and brodcast packets
		 * are only delivered on the synthetic interface
		 * (after subjecting these to policy filters on
		 * the host). Deliver these via the VF interface
		 * in the guest.
		 */
		vf_skb = netvsc_alloc_recv_skb(net_device_ctx->vf_netdev,
					       csum_info, vlan, data, len);
		if (vf_skb != NULL) {
			++net_device_ctx->vf_netdev->stats.rx_packets;
			net_device_ctx->vf_netdev->stats.rx_bytes +=
				len;
			netif_receive_skb(vf_skb);
		} else {
			++net->stats.rx_dropped;
			ret = NVSP_STAT_FAIL;
		}
		atomic_dec(&net_device_ctx->vf_use_cnt);
		return ret;
	}

vf_injection_done:
	rx_stats = &nvchan->rx_stats;

	/* Allocate a skb - TODO direct I/O to pages? */
	skb = netvsc_alloc_recv_skb(net, csum_info, vlan, data, len);
	//printk("net:%lx,net_dev:%lx\n",(uintptr_t)net,(uintptr_t)net_device);
	if (unlikely(!skb)) {
		++net->stats.rx_dropped;
		return NVSP_STAT_FAIL;
	}
	skb_record_rx_queue(skb, q_idx);

	u64_stats_update_begin(&rx_stats->syncp);
	rx_stats->packets++;
	rx_stats->bytes += len;

	if (skb->pkt_type == PACKET_BROADCAST)
		++rx_stats->broadcast;
	else if (skb->pkt_type == PACKET_MULTICAST)
		++rx_stats->multicast;
	u64_stats_update_end(&rx_stats->syncp);

	net->stats.rx_packets++;
	net->stats.rx_bytes += len;

	napi_gro_receive(&nvchan->napi, skb);
	return NVSP_STAT_SUCCESS;
}

static void netvsc_get_drvinfo(struct net_device *net,
			       struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, KBUILD_MODNAME, sizeof(info->driver));
	strlcpy(info->fw_version, "N/A", sizeof(info->fw_version));
}

#if (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(7,0))
static void netvsc_get_channels(struct net_device *net,
				struct ethtool_channels *channel)
{
	struct net_device_context *net_device_ctx = netdev_priv(net);
	struct netvsc_device *nvdev = net_device_ctx->nvdev;

	if (nvdev) {
		channel->max_combined	= nvdev->max_chn;
		channel->combined_count = nvdev->num_chn;
	}
}

static int netvsc_set_channels(struct net_device *net,
			       struct ethtool_channels *channels)
{
	struct net_device_context *net_device_ctx = netdev_priv(net);
	struct hv_device *dev = net_device_ctx->device_ctx;
	struct netvsc_device *nvdev = net_device_ctx->nvdev;
	unsigned int orig, count = channels->combined_count;
	struct netvsc_device_info device_info;
	bool was_opened;
	int ret = 0;

	/* We do not support separate count for rx, tx, or other */
	if (count == 0 ||
	    channels->rx_count || channels->tx_count || channels->other_count)
		return -EINVAL;

	if (count > net->num_tx_queues || count > VRSS_CHANNEL_MAX)
		return -EINVAL;

	if (!nvdev || nvdev->destroy)
		return -ENODEV;

	if (nvdev->nvsp_version < NVSP_PROTOCOL_VERSION_5)
		return -EINVAL;

	if (count > nvdev->max_chn)
		return -EINVAL;
	orig = nvdev->num_chn;

	was_opened = rndis_filter_opened(nvdev);
	if (was_opened)
		rndis_filter_close(nvdev);

	rndis_filter_device_remove(dev, nvdev);

	memset(&device_info, 0, sizeof(device_info));
	device_info.num_chn = count;
	device_info.ring_size = ring_size;

	nvdev = rndis_filter_device_add(dev, &device_info);
	if (IS_ERR(nvdev)) {
		device_info.num_chn = orig;
		rndis_filter_device_add(dev, &device_info);
	}

	if (was_opened)
		rndis_filter_open(nvdev);

	/* We may have missed link change notifications */
	net_device_ctx->last_reconfig = 0;
	schedule_delayed_work(&net_device_ctx->dwork, 0);

	return ret;
}
#endif

static bool netvsc_validate_ethtool_ss_cmd(const struct ethtool_cmd *cmd)
{
	struct ethtool_cmd diff1 = *cmd;
	struct ethtool_cmd diff2 = {};

	ethtool_cmd_speed_set(&diff1, 0);
	diff1.duplex = 0;
	/* advertising and cmd are usually set */
	diff1.advertising = 0;
	diff1.cmd = 0;
	/* We set port to PORT_OTHER */
	diff2.port = PORT_OTHER;

	return !memcmp(&diff1, &diff2, sizeof(diff1));
}

static void netvsc_init_settings(struct net_device *dev)
{
	struct net_device_context *ndc = netdev_priv(dev);

	ndc->speed = SPEED_UNKNOWN;
	ndc->duplex = DUPLEX_FULL;
}

static int netvsc_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct net_device_context *ndc = netdev_priv(dev);

	ethtool_cmd_speed_set(cmd, ndc->speed);
	cmd->duplex = ndc->duplex;
	cmd->port = PORT_OTHER;

	return 0;
}

static int netvsc_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct net_device_context *ndc = netdev_priv(dev);
	u32 speed;

	speed = ethtool_cmd_speed(cmd);
	if (!ethtool_validate_speed(speed) ||
	    !ethtool_validate_duplex(cmd->duplex) ||
	    !netvsc_validate_ethtool_ss_cmd(cmd))
		return -EINVAL;

	ndc->speed = speed;
	ndc->duplex = cmd->duplex;

	return 0;
}

static int netvsc_change_mtu(struct net_device *ndev, int mtu)
{
	struct net_device_context *ndevctx = netdev_priv(ndev);
	struct net_device *vf_netdev = rtnl_dereference(ndevctx->vf_netdev);
	struct netvsc_device *nvdev = ndevctx->nvdev;
	struct hv_device *hdev = ndevctx->device_ctx;
	int orig_mtu = ndev->mtu;
	struct netvsc_device_info device_info;
	int limit = ETH_DATA_LEN;
	bool was_opened;
	int ret = 0;

	if (!nvdev || nvdev->destroy)
		return -ENODEV;

	if (nvdev->nvsp_version >= NVSP_PROTOCOL_VERSION_2)
		limit = NETVSC_MTU - ETH_HLEN;

	if (mtu < NETVSC_MTU_MIN || mtu > limit)
		return -EINVAL;

	/* Change MTU of underlying VF netdev first. */
	if (vf_netdev) {
		ret = dev_set_mtu(vf_netdev, mtu);
		if (ret)
			return ret;
	}

	netif_device_detach(ndev);
	was_opened = rndis_filter_opened(nvdev);
	if (was_opened)
		rndis_filter_close(nvdev);

	memset(&device_info, 0, sizeof(device_info));
	device_info.ring_size = ring_size;
	device_info.num_chn = nvdev->num_chn;

	rndis_filter_device_remove(hdev, nvdev);

	ndev->mtu = mtu;

	nvdev = rndis_filter_device_add(hdev, &device_info);
	if (IS_ERR(nvdev)) {
		ret = PTR_ERR(nvdev);

		/* Attempt rollback to original MTU */
		ndev->mtu = orig_mtu;
		rndis_filter_device_add(hdev, &device_info);

		if (vf_netdev)
			dev_set_mtu(vf_netdev, orig_mtu);
	}

	if (was_opened)
		rndis_filter_open(nvdev);

	netif_device_attach(ndev);

	/* We may have missed link change notifications */
	schedule_delayed_work(&ndevctx->dwork, 0);

	return ret;
}

static void netvsc_get_vf_stats(struct net_device *net,
				struct netvsc_vf_pcpu_stats *tot)
{
	struct net_device_context *ndev_ctx = netdev_priv(net);
	int i;

	memset(tot, 0, sizeof(*tot));

	for_each_possible_cpu(i) {
		const struct netvsc_vf_pcpu_stats *stats
			= per_cpu_ptr(ndev_ctx->vf_stats, i);
		u64 rx_packets, rx_bytes, tx_packets, tx_bytes;
		unsigned int start;

		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			rx_packets = stats->rx_packets;
			tx_packets = stats->tx_packets;
			rx_bytes = stats->rx_bytes;
			tx_bytes = stats->tx_bytes;
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));

		tot->rx_packets += rx_packets;
		tot->tx_packets += tx_packets;
		tot->rx_bytes   += rx_bytes;
		tot->tx_bytes   += tx_bytes;
		tot->tx_dropped += stats->tx_dropped;
	}
}


#if (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(7,0))
static void *netvsc_get_stats64(struct net_device *net,
				struct rtnl_link_stats64 *t)
{
	struct net_device_context *ndev_ctx = netdev_priv(net);
	struct netvsc_device *nvdev = rcu_dereference_rtnl(ndev_ctx->nvdev);
	struct netvsc_vf_pcpu_stats vf_tot;
	int i;

	if (!nvdev)
		return;
	netdev_stats_to_stats64(t, &net->stats);

	netvsc_get_vf_stats(net, &vf_tot);
	t->rx_packets += vf_tot.rx_packets;
	t->tx_packets += vf_tot.tx_packets;
	t->rx_bytes   += vf_tot.rx_bytes;
	t->tx_bytes   += vf_tot.tx_bytes;
	t->tx_dropped += vf_tot.tx_dropped;


	for (i = 0; i < nvdev->num_chn; i++) {
		const struct netvsc_channel *nvchan = &nvdev->chan_table[i];
		const struct netvsc_stats *stats;
		u64 packets, bytes, multicast;
		unsigned int start;

		stats = &nvchan->tx_stats;
		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			packets = stats->packets;
			bytes = stats->bytes;
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));

		t->tx_bytes	+= bytes;
		t->tx_packets	+= packets;

		stats = &nvchan->rx_stats;
		do {
			start = u64_stats_fetch_begin_irq(&stats->syncp);
			packets = stats->packets;
			bytes = stats->bytes;
			multicast = stats->multicast + stats->broadcast;
		} while (u64_stats_fetch_retry_irq(&stats->syncp, start));

		t->rx_bytes	+= bytes;
		t->rx_packets	+= packets;
		t->multicast	+= multicast;
	}

}
#endif

static int netvsc_set_mac_addr(struct net_device *ndev, void *p)
{
	struct sockaddr *addr = p;
	char save_adr[ETH_ALEN];
	unsigned char save_aatype;
	int err;

	memcpy(save_adr, ndev->dev_addr, ETH_ALEN);
	save_aatype = ndev->addr_assign_type;

	err = eth_mac_addr(ndev, p);
	if (err != 0)
		return err;

	err = rndis_filter_set_device_mac(ndev, addr->sa_data);
	if (err != 0) {
		/* roll back to saved MAC */
		memcpy(ndev->dev_addr, save_adr, ETH_ALEN);
		ndev->addr_assign_type = save_aatype;
	}

	return err;
}

static const struct {
	char name[ETH_GSTRING_LEN];
	u16 offset;
} netvsc_stats[] = {
	{ "tx_scattered", offsetof(struct netvsc_ethtool_stats, tx_scattered) },
	{ "tx_no_memory",  offsetof(struct netvsc_ethtool_stats, tx_no_memory) },
	{ "tx_no_space",  offsetof(struct netvsc_ethtool_stats, tx_no_space) },
	{ "tx_too_big",	  offsetof(struct netvsc_ethtool_stats, tx_too_big) },
	{ "tx_busy",	  offsetof(struct netvsc_ethtool_stats, tx_busy) },
	{ "stop_queue", offsetof(struct netvsc_ethtool_stats, stop_queue) },
	{ "wake_queue", offsetof(struct netvsc_ethtool_stats, wake_queue) },
}, vf_stats[] = {
	{ "vf_rx_packets", offsetof(struct netvsc_vf_pcpu_stats, rx_packets) },
	{ "vf_rx_bytes",   offsetof(struct netvsc_vf_pcpu_stats, rx_bytes) },
	{ "vf_tx_packets", offsetof(struct netvsc_vf_pcpu_stats, tx_packets) },
	{ "vf_tx_bytes",   offsetof(struct netvsc_vf_pcpu_stats, tx_bytes) },
	{ "vf_tx_dropped", offsetof(struct netvsc_vf_pcpu_stats, tx_dropped) },
};

#define NETVSC_GLOBAL_STATS_LEN	ARRAY_SIZE(netvsc_stats)
#define NETVSC_VF_STATS_LEN	ARRAY_SIZE(vf_stats)

/* 4 statistics per queue (rx/tx packets/bytes) */
#define NETVSC_QUEUE_STATS_LEN(dev) ((dev)->num_chn * 4)

static int netvsc_get_sset_count(struct net_device *dev, int string_set)
{
	struct net_device_context *ndc = netdev_priv(dev);
	struct netvsc_device *nvdev = rtnl_dereference(ndc->nvdev);

	if (!nvdev)
		return -ENODEV;

	switch (string_set) {
	case ETH_SS_STATS:
		return NETVSC_GLOBAL_STATS_LEN
			+  NETVSC_VF_STATS_LEN
			+ NETVSC_QUEUE_STATS_LEN(nvdev);
	default:
		return -EINVAL;
	}
}

static void netvsc_get_ethtool_stats(struct net_device *dev,
				     struct ethtool_stats *stats, u64 *data)
{
	struct net_device_context *ndc = netdev_priv(dev);
	struct netvsc_device *nvdev = ndc->nvdev;
	const void *nds = &ndc->eth_stats;
	const struct netvsc_stats *qstats;
	struct netvsc_vf_pcpu_stats sum;
	unsigned int start;
	u64 packets, bytes;
	int i, j;

	for (i = 0; i < NETVSC_GLOBAL_STATS_LEN; i++)
		data[i] = *(unsigned long *)(nds + netvsc_stats[i].offset);

	netvsc_get_vf_stats(dev, &sum);
	for (j = 0; j < NETVSC_VF_STATS_LEN; j++)
		data[i++] = *(u64 *)((void *)&sum + vf_stats[j].offset);


	for (j = 0; j < nvdev->num_chn; j++) {
		qstats = &nvdev->chan_table[j].tx_stats;

		do {
			start = u64_stats_fetch_begin_irq(&qstats->syncp);
			packets = qstats->packets;
			bytes = qstats->bytes;
		} while (u64_stats_fetch_retry_irq(&qstats->syncp, start));
		data[i++] = packets;
		data[i++] = bytes;

		qstats = &nvdev->chan_table[j].rx_stats;
		do {
			start = u64_stats_fetch_begin_irq(&qstats->syncp);
			packets = qstats->packets;
			bytes = qstats->bytes;
		} while (u64_stats_fetch_retry_irq(&qstats->syncp, start));
		data[i++] = packets;
		data[i++] = bytes;
	}
}

static void netvsc_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	struct net_device_context *ndc = netdev_priv(dev);
	struct netvsc_device *nvdev = ndc->nvdev;
	u8 *p = data;
	int i;

	switch (stringset) {
	case ETH_SS_STATS:
		for (i = 0; i < ARRAY_SIZE(netvsc_stats); i++) {
			memcpy(p, netvsc_stats[i].name, ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}

		for (i = 0; i < ARRAY_SIZE(vf_stats); i++) {
			memcpy(p, vf_stats[i].name, ETH_GSTRING_LEN);
			p += ETH_GSTRING_LEN;
		}

		for (i = 0; i < nvdev->num_chn; i++) {
			sprintf(p, "tx_queue_%u_packets", i);
			p += ETH_GSTRING_LEN;
			sprintf(p, "tx_queue_%u_bytes", i);
			p += ETH_GSTRING_LEN;
			sprintf(p, "rx_queue_%u_packets", i);
			p += ETH_GSTRING_LEN;
			sprintf(p, "rx_queue_%u_bytes", i);
			p += ETH_GSTRING_LEN;
		}

		break;
	}
}

static int
netvsc_get_rss_hash_opts(struct netvsc_device *nvdev,
			 struct ethtool_rxnfc *info)
{
	info->data = RXH_IP_SRC | RXH_IP_DST;

	switch (info->flow_type) {
	case TCP_V4_FLOW:
	case TCP_V6_FLOW:
		info->data |= RXH_L4_B_0_1 | RXH_L4_B_2_3;
		/* fallthrough */
	case UDP_V4_FLOW:
	case UDP_V6_FLOW:
#ifdef NOTYET
	case IPV4_FLOW:
	case IPV6_FLOW:
#endif
		break;
	default:
		info->data = 0;
		break;
	}

	return 0;
}

#ifdef NOTYET
static int
netvsc_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *info,
		 u32 *rules)
#endif
static int
netvsc_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *info,
		 void *rules)

{
	struct net_device_context *ndc = netdev_priv(dev);
	struct netvsc_device *nvdev = ndc->nvdev;

	switch (info->cmd) {
	case ETHTOOL_GRXRINGS:
		info->data = nvdev->num_chn;
		return 0;

	case ETHTOOL_GRXFH:
		return netvsc_get_rss_hash_opts(nvdev, info);
	}
	return -EOPNOTSUPP;
}

#ifdef CONFIG_NET_POLL_CONTROLLER
static void netvsc_poll_controller(struct net_device *dev)
{
	struct net_device_context *ndc = netdev_priv(dev);
	struct netvsc_device *ndev;
	int i;

	rcu_read_lock();
	ndev = rcu_dereference(ndc->nvdev);
	if (ndev) {
		for (i = 0; i < ndev->num_chn; i++) {
			struct netvsc_channel *nvchan = &ndev->chan_table[i];

			napi_schedule(&nvchan->napi);
		}
	}
	rcu_read_unlock();
}
#endif

#if (RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(6,10))
static u32 netvsc_get_rxfh_key_size(struct net_device *dev)
{
	return NETVSC_HASH_KEYLEN;
}

static u32 netvsc_rss_indir_size(struct net_device *dev)
{
	return ITAB_NUM;
}

static int netvsc_get_rxfh(struct net_device *dev, u32 *indir, u8 *key,
			   u8 *hfunc)
{
	struct net_device_context *ndc = netdev_priv(dev);
	struct netvsc_device *ndev = ndc->nvdev;
	struct rndis_device *rndis_dev;
	int i;

	if (hfunc)
		*hfunc = ETH_RSS_HASH_TOP;	/* Toeplitz */

	rndis_dev = ndev->extension;
	if (indir) {
		for (i = 0; i < ITAB_NUM; i++)
			indir[i] = rndis_dev->ind_table[i];
	}

	if (key)
		memcpy(key, rndis_dev->rss_key, NETVSC_HASH_KEYLEN);

	return 0;
}

static int netvsc_set_rxfh(struct net_device *dev, const u32 *indir,
			   const u8 *key, const u8 hfunc)
{
	struct net_device_context *ndc = netdev_priv(dev);
	struct netvsc_device *ndev = ndc->nvdev;
	struct rndis_device *rndis_dev;
	int i;

	if (hfunc != ETH_RSS_HASH_NO_CHANGE && hfunc != ETH_RSS_HASH_TOP)
		return -EOPNOTSUPP;

	rndis_dev = ndev->extension;
	if (indir) {
		for (i = 0; i < ITAB_NUM; i++)
			if (indir[i] >= VRSS_CHANNEL_MAX)
				return -EINVAL;

		for (i = 0; i < ITAB_NUM; i++)
			rndis_dev->ind_table[i] = indir[i];
	}

	if (!key) {
		if (!indir)
			return 0;

		key = rndis_dev->rss_key;
	}

	return rndis_filter_set_rss_param(rndis_dev, key, ndev->num_chn);
}
#endif

static const struct ethtool_ops ethtool_ops = {
	.get_drvinfo	= netvsc_get_drvinfo,
	.get_link	= ethtool_op_get_link,
	.get_ethtool_stats = netvsc_get_ethtool_stats,
	.get_sset_count = netvsc_get_sset_count,
	.get_strings	= netvsc_get_strings,
#if (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(7,0))
	.get_channels   = netvsc_get_channels,
	.set_channels   = netvsc_set_channels,
	.get_ts_info	= ethtool_op_get_ts_info,
#endif
	.get_settings	= netvsc_get_settings,
	.set_settings	= netvsc_set_settings,
	.get_rxnfc	= netvsc_get_rxnfc,
#if (RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(6,10))
	.get_rxfh_key_size = netvsc_get_rxfh_key_size,
	.get_rxfh_indir_size = netvsc_rss_indir_size,
	.get_rxfh	= netvsc_get_rxfh,
	.set_rxfh	= netvsc_set_rxfh,
#endif
};

static const struct net_device_ops device_ops = {
	.ndo_open =			netvsc_open,
	.ndo_stop =			netvsc_close,
	.ndo_start_xmit =		netvsc_start_xmit,
	.ndo_set_rx_mode =		netvsc_set_multicast_list,
	.ndo_change_mtu =		netvsc_change_mtu,
	.ndo_validate_addr =		eth_validate_addr,
	.ndo_set_mac_address =		netvsc_set_mac_addr,
	.ndo_select_queue =		netvsc_select_queue,
#if (RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(7,0))
	.ndo_get_stats64 =		netvsc_get_stats64,
#endif
#ifdef CONFIG_NET_POLL_CONTROLLER
	.ndo_poll_controller =		netvsc_poll_controller,
#endif
};

/*
 * Handle link status changes. For RNDIS_STATUS_NETWORK_CHANGE emulate link
 * down/up sequences. In case of RNDIS_STATUS_MEDIA_CONNECT when carrier is
 * present send GARP packet to network peers with netif_notify_peers().
 */
static void netvsc_link_change(struct work_struct *w)
{
	struct net_device_context *ndev_ctx =
		container_of(w, struct net_device_context, dwork.work);
	struct hv_device *device_obj = ndev_ctx->device_ctx;
	struct net_device *net = hv_get_drvdata(device_obj);
	struct netvsc_device *net_device;
	struct rndis_device *rdev;
	struct netvsc_reconfig *event = NULL;
	bool notify = false, reschedule = false;
	unsigned long flags, next_reconfig, delay;

	if (!rtnl_trylock()) {
		schedule_delayed_work(&ndev_ctx->dwork, LINKCHANGE_INT);
		return;
	}

	net_device = rtnl_dereference(ndev_ctx->nvdev);
	if (!net_device)
		goto out_unlock;

	rdev = net_device->extension;

	next_reconfig = ndev_ctx->last_reconfig + LINKCHANGE_INT;
	if (time_is_after_jiffies(next_reconfig)) {
		/* link_watch only sends one notification with current state
		 * per second, avoid doing reconfig more frequently. Handle
		 * wrap around.
		 */
		delay = next_reconfig - jiffies;
		delay = delay < LINKCHANGE_INT ? delay : LINKCHANGE_INT;
		schedule_delayed_work(&ndev_ctx->dwork, delay);
		goto out_unlock;
	}
	ndev_ctx->last_reconfig = jiffies;

	spin_lock_irqsave(&ndev_ctx->lock, flags);
	if (!list_empty(&ndev_ctx->reconfig_events)) {
		event = list_first_entry(&ndev_ctx->reconfig_events,
					 struct netvsc_reconfig, list);
		list_del(&event->list);
		reschedule = !list_empty(&ndev_ctx->reconfig_events);
	}
	spin_unlock_irqrestore(&ndev_ctx->lock, flags);

	if (!event)
		goto out_unlock;

	switch (event->event) {
		/* Only the following events are possible due to the check in
		 * netvsc_linkstatus_callback()
		 */
	case RNDIS_STATUS_MEDIA_CONNECT:
		if (rdev->link_state) {
			rdev->link_state = false;
			netif_carrier_on(net);
			netif_tx_wake_all_queues(net);
		} else {
			notify = true;
		}
		kfree(event);
		break;
	case RNDIS_STATUS_MEDIA_DISCONNECT:
		if (!rdev->link_state) {
			rdev->link_state = true;
			netif_carrier_off(net);
			netif_tx_stop_all_queues(net);
		}
		kfree(event);
		break;
	case RNDIS_STATUS_NETWORK_CHANGE:
		/* Only makes sense if carrier is present */
		if (!rdev->link_state) {
			rdev->link_state = true;
			netif_carrier_off(net);
			netif_tx_stop_all_queues(net);
			event->event = RNDIS_STATUS_MEDIA_CONNECT;
			spin_lock_irqsave(&ndev_ctx->lock, flags);
			list_add(&event->list, &ndev_ctx->reconfig_events);
			spin_unlock_irqrestore(&ndev_ctx->lock, flags);
			reschedule = true;
		}
		break;
	}

	rtnl_unlock();

	if (notify)
		netif_notify_peers(net);

	/* link_watch only sends one notification with current state per
	 * second, handle next reconfig event in 2 seconds.
	 */
	if (reschedule)
		schedule_delayed_work(&ndev_ctx->dwork, LINKCHANGE_INT);

	return;

out_unlock:
	rtnl_unlock();
}

#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(6,2))
static void netvsc_notify_peers(struct work_struct *wrk)
{
	struct garp_wrk *gwrk;

	gwrk = container_of(wrk, struct garp_wrk, dwrk);

	netif_notify_peers(gwrk->netdev);

	atomic_dec(&gwrk->net_device_ctx->vf_use_cnt);
}
#endif

static struct net_device *get_netvsc_bymac(const u8 *mac)
{
	struct net_device *dev;

	ASSERT_RTNL();

	for_each_netdev(&init_net, dev) {
		if (dev->netdev_ops != &device_ops)
			continue;	/* not a netvsc device */

		/* deviation from upstream - we are using dev_addr, not perm_addr */
		if (ether_addr_equal(mac, dev->dev_addr))
			return dev;
	}

	return NULL;
}

static struct net_device *get_netvsc_byref(const struct net_device *vf_netdev)
{
	struct net_device *dev;

	ASSERT_RTNL();

	for_each_netdev(&init_net, dev) {
		struct net_device_context *net_device_ctx;

		if (dev->netdev_ops != &device_ops)
			continue;	/* not a netvsc device */

		net_device_ctx = netdev_priv(dev);
		if (!rtnl_dereference(net_device_ctx->nvdev))
			continue;	/* device is removed */

		if (net_device_ctx->vf_netdev == vf_netdev)
			return dev;	/* a match */
	}

	return NULL;
}



//////////////////////////////////////////////////////////////////////////////////////////////////////////
/**
 * bond_set_dev_addr - clone slave's address to bond
 * @bond_dev: bond net device
 * @slave_dev: slave net device
 *
 * Should be called with RTNL held.
 */

static void bond_add_vlans_on_slave(struct bonding *bond, struct net_device *slave_dev)
{
	struct vlan_entry *vlan;
	const struct net_device_ops *slave_ops = slave_dev->netdev_ops;

	if (!bond->vlgrp)
		return;

	if ((slave_dev->features & NETIF_F_HW_VLAN_RX) &&
	    slave_ops->ndo_vlan_rx_register)
		slave_ops->ndo_vlan_rx_register(slave_dev, bond->vlgrp);

	if (!(slave_dev->features & NETIF_F_HW_VLAN_FILTER) ||
	    !(slave_ops->ndo_vlan_rx_add_vid))
		return;

	list_for_each_entry(vlan, &bond->vlan_list, vlan_list)
		slave_ops->ndo_vlan_rx_add_vid(slave_dev, vlan->vlan_id);
}

/* Get link speed and duplex from the slave's base driver
 * using ethtool. If for some reason the call fails or the
 * values are invalid, set speed and duplex to -1,
 * and return.
 */
static void bond_update_speed_duplex(struct slave *slave)
{
	struct net_device *slave_dev = slave->dev;
	struct ethtool_cmd etool = { .cmd = ETHTOOL_GSET };
	u32 slave_speed;
	int res;

	slave->speed = SPEED_UNKNOWN;
	slave->duplex = DUPLEX_UNKNOWN;

	if (!slave_dev->ethtool_ops || !slave_dev->ethtool_ops->get_settings)
		return;

	res = slave_dev->ethtool_ops->get_settings(slave_dev, &etool);
	if (res < 0)
		return;

	slave_speed = ethtool_cmd_speed(&etool);
	if (slave_speed == 0 || slave_speed == ((__u32) -1))
		return;

	switch (etool.duplex) {
	case DUPLEX_FULL:
	case DUPLEX_HALF:
		break;
	default:
		return;
	}

	slave->speed = slave_speed;
	slave->duplex = etool.duplex;

	return;
}


/* if <dev> supports MII link status reporting, check its link status.
 *
 * We either do MII/ETHTOOL ioctls, or check netif_carrier_ok(),
 * depending upon the setting of the use_carrier parameter.
 *
 * Return either BMSR_LSTATUS, meaning that the link is up (or we
 * can't tell and just pretend it is), or 0, meaning that the link is
 * down.
 *
 * If reporting is non-zero, instead of faking link up, return -1 if
 * both ETHTOOL and MII ioctls fail (meaning the device does not
 * support them).  If use_carrier is set, return whatever it says.
 * It'd be nice if there was a good way to tell if a driver supports
 * netif_carrier, but there really isn't.
 */
static int bond_check_dev_link(struct bonding *bond,
			       struct net_device *slave_dev, int reporting)
{
	const struct net_device_ops *slave_ops = slave_dev->netdev_ops;
	int (*ioctl)(struct net_device *, struct ifreq *, int);
	struct ifreq ifr;
	struct mii_ioctl_data *mii;

	if (!reporting && !netif_running(slave_dev))
		return 0;

	if (bond->params.use_carrier)
		return netif_carrier_ok(slave_dev) ? BMSR_LSTATUS : 0;

	/* Try to get link status using Ethtool first. */
	if (slave_dev->ethtool_ops->get_link)
		return slave_dev->ethtool_ops->get_link(slave_dev) ?
			BMSR_LSTATUS : 0;

	/* Ethtool can't be used, fallback to MII ioctls. */
	ioctl = slave_ops->ndo_do_ioctl;
	if (ioctl) {
		/* TODO: set pointer to correct ioctl on a per team member
		 *       bases to make this more efficient. that is, once
		 *       we determine the correct ioctl, we will always
		 *       call it and not the others for that team
		 *       member.
		 */

		/* We cannot assume that SIOCGMIIPHY will also read a
		 * register; not all network drivers (e.g., e100)
		 * support that.
		 */

		/* Yes, the mii is overlaid on the ifreq.ifr_ifru */
		strncpy(ifr.ifr_name, slave_dev->name, IFNAMSIZ);
		mii = if_mii(&ifr);
		if (IOCTL(slave_dev, &ifr, SIOCGMIIPHY) == 0) {
			mii->reg_num = MII_BMSR;
			if (IOCTL(slave_dev, &ifr, SIOCGMIIREG) == 0)
				return mii->val_out & BMSR_LSTATUS;
		}
	}

	/* If reporting, report that either there's no dev->do_ioctl,
	 * or both SIOCGMIIREG and get_link failed (meaning that we
	 * cannot report link status).  If not reporting, pretend
	 * we're ok.
	 */
	return reporting ? -1 : BMSR_LSTATUS;
}


#ifdef CONFIG_NET_POLL_CONTROLLER
static inline int slave_enable_netpoll(struct slave *slave)
{
	struct netpoll *np;
	int err = 0;

	np = kzalloc(sizeof(*np), GFP_ATOMIC);
	err = -ENOMEM;
	if (!np)
		goto out;

	err = __netpoll_setup(np, slave->dev, GFP_ATOMIC);
	if (err) {
		kfree(np);
		goto out;
	}
	slave->np = np;
out:
	return err;
}
#if 0
static inline void slave_disable_netpoll(struct slave *slave)
{
	struct netpoll *np = slave->np;

	if (!np)
		return;

	slave->np = NULL;
	synchronize_rcu_bh();
	__netpoll_cleanup(np);
	kfree(np);
}
#endif

#else
#endif


static int bond_master_upper_dev_link(struct net_device *bond_dev,
				      struct net_device *slave_dev,
				      struct slave *slave)
{
	int err;

	err = netdev_set_master(slave_dev, bond_dev);

	return err;
}


#define BOND_VLAN_FEATURES	(NETIF_F_ALL_CSUM | NETIF_F_SG | \
				 NETIF_F_FRAGLIST | NETIF_F_ALL_TSO | \
				 NETIF_F_HIGHDMA | NETIF_F_LRO)

static struct slave *bond_alloc_slave(struct bonding *bond)
{
	struct slave *slave = NULL;

	slave = kzalloc(sizeof(struct slave), GFP_KERNEL);
	if (!slave)
		return NULL;
	INIT_LIST_HEAD(&slave->list);

	return slave;
}

int bond_create_slave_symlinks(struct net_device *master,
			       struct net_device *slave)
{
	char linkname[IFNAMSIZ+7];
	int ret = 0;

	/* first, create a link from the slave back to the master */
	ret = sysfs_create_link(&(slave->dev.kobj), &(master->dev.kobj),
				"master");
	if (ret)
		return ret;
	/* next, create a link from the master to the slave */
	sprintf(linkname, "slave_%s", slave->name);
	ret = sysfs_create_link(&(master->dev.kobj), &(slave->dev.kobj),
				linkname);

	/* free the master link created earlier in case of error */
	if (ret)
		sysfs_remove_link(&(slave->dev.kobj), "master");

	return ret;

}


static inline void tlb_init_slave(struct slave *slave)
{
	SLAVE_TLB_INFO(slave).load = 0;
	SLAVE_TLB_INFO(slave).head = TLB_NULL_INDEX;
}

static int netdev_master_upper_dev_link(struct net_device *vf_netdev,
                                      struct net_device *ndev)
{
        int err;
        printk("netdev_set_master:nd:%lx,nd->flags:%x\n",(uintptr_t)ndev,ndev->flags);
        printk("netdev_set_master:vf:%lx,vf->flags:%x\n",(uintptr_t)vf_netdev->flags);
        //err = netdev_set_master(ndev, vf_netdev);
        //vf_netdev->master = NULL;
        err = netdev_set_master(vf_netdev,ndev);

        return err;
}

#if 0
/**
 * netdev_upper_dev_unlink - Removes a link to upper device
 * @dev: device
 * @upper_dev: new upper device
 *
 * Removes a link to device which is upper to this one. The caller must hold
 * the RTNL lock.
 */
void netdev_upper_dev_unlink_75(struct net_device *dev,
			     struct net_device *upper_dev)
{
	struct netdev_notifier_changeupper_info changeupper_info;
	struct netdev_adjacent *i, *j;
	ASSERT_RTNL();

	changeupper_info.upper_dev = upper_dev;
	changeupper_info.master = netdev_master_upper_dev_get(dev) == upper_dev;
	changeupper_info.linking = false;

	call_netdevice_notifiers_info(NETDEV_PRECHANGEUPPER, dev,
				      &changeupper_info.info);

	__netdev_adjacent_dev_unlink_neighbour(dev, upper_dev);

	/* Here is the tricky part. We must remove all dev's lower
	 * devices from all upper_dev's upper devices and vice
	 * versa, to maintain the graph relationship.
	 */
	list_for_each_entry(i, &dev->lower_dev_list, list)
		list_for_each_entry(j, &upper_dev->upper_dev_list, list)
			__netdev_adjacent_dev_unlink(i->dev, j->dev, i->ref_nr);

	/* remove also the devices itself from lower/upper device
	 * list
	 */
	list_for_each_entry(i, &dev->lower_dev_list, list)
		__netdev_adjacent_dev_unlink(i->dev, upper_dev, i->ref_nr);

	list_for_each_entry(i, &upper_dev->upper_dev_list, list)
		__netdev_adjacent_dev_unlink(dev, i->dev, i->ref_nr);

	call_netdevice_notifiers_info(NETDEV_CHANGEUPPER, dev,
				      &changeupper_info.info);
}
#endif
//call_netdevice_notifiers

static void netdev_upper_dev_unlink(struct net_device *vf_netdev,
                                  struct net_device *ndev)
{
        //netdev_set_master(NULL, ndev);
//        netdev_set_master(ndev, NULL);
        netdev_set_master(vf_netdev, NULL);

        //atomic_set(&dev->refcnt,0);
//		atomic_sub
#if 0
		struct netdev_notifier_changeupper_info changeupper_info;
	struct netdev_adjacent *i, *j;
	ASSERT_RTNL();

	changeupper_info.upper_dev = upper_dev;
	changeupper_info.master = netdev_master_upper_dev_get(dev) == upper_dev;
	changeupper_info.linking = false;

	call_netdevice_notifiers_info(NETDEV_PRECHANGEUPPER, dev,
				      &changeupper_info.info);

	__netdev_adjacent_dev_unlink_neighbour(dev, upper_dev);

	call_netdevice_notifiers_info(NETDEV_CHANGEUPPER, dev,
				      &changeupper_info.info);
#endif	
}

/* Called when VF is injecting data into network stack.
 * Change the associated network device from VF to netvsc.
 * note: already called with rcu_read_lock
 */
static rx_handler_result_t netvsc_vf_handle_frame(struct sk_buff **pskb)
{
	struct sk_buff *skb = *pskb;
	struct net_device_extended * steve_temp = netdev_extended(skb->dev);
	struct net_device *ndev = rcu_dereference(steve_temp->dev);
	struct net_device_context *ndev_ctx = netdev_priv(ndev);
	struct netvsc_vf_pcpu_stats *pcpu_stats
		 = this_cpu_ptr(ndev_ctx->vf_stats);

	skb->dev = ndev;
    printk("nd_vf_hd:%lx\n",(uintptr_t)ndev);
	u64_stats_update_begin(&pcpu_stats->syncp);
	pcpu_stats->rx_packets++;
	pcpu_stats->rx_bytes += skb->len;
	u64_stats_update_end(&pcpu_stats->syncp);

	return RX_HANDLER_ANOTHER;
}
int my_netdev_rx_handler_register(struct net_device *dev,
			       rx_handler_func_t *rx_handler,
			       void *rx_handler_data)
{
	ASSERT_RTNL();

	if (netdev_extended(dev)->rx_handler)
		return -EBUSY;

	/* Note: rx_handler_data must be set before rx_handler */
	rcu_assign_pointer(netdev_extended(dev)->rx_handler_data, rx_handler_data);
	rcu_assign_pointer(netdev_extended(dev)->rx_handler, rx_handler);

	return 0;
}

/* enslave device <slave> to bond device <master> */
int my_bond_enslave(struct net_device *bond_dev, struct net_device *slave_dev)
{
	struct bonding *bond = netdev_priv(bond_dev) + ALIGN(sizeof(struct net_device_context), NETDEV_ALIGN);
	const struct net_device_ops *slave_ops = slave_dev->netdev_ops;
	struct slave *new_slave = NULL, *prev_slave;
	struct dev_mc_list *dmi;
	struct sockaddr addr;
	int link_reporting;
	int res = 0, i;
    printk("bd_0\n");
	if (!bond->params.use_carrier &&
	    slave_dev->ethtool_ops->get_link == NULL &&
	    slave_ops->ndo_do_ioctl == NULL) {
		netdev_warn(bond_dev, "no link monitoring support for %s\n",
			    slave_dev->name);
	}
	printk("bd_1\n");

	/* already enslaved */
	if (slave_dev->flags & IFF_SLAVE) {
		netdev_dbg(bond_dev, "Error: Device was already enslaved\n");
		return -EBUSY;
	}
	printk("bd_2\n");

	if (bond_dev == slave_dev) {
		netdev_err(bond_dev, "cannot enslave bond to itself.\n");
		return -EPERM;
	}
	printk("bd_3\n");

	/* vlan challenged mutual exclusion */
	/* no need to lock since we're protected by rtnl_lock */
	if (slave_dev->features & NETIF_F_VLAN_CHALLENGED) {
		netdev_dbg(bond_dev, "%s is NETIF_F_VLAN_CHALLENGED\n",
			   slave_dev->name);
		if (bond->vlgrp) {
			netdev_err(bond_dev, "Error: cannot enslave VLAN challenged slave %s on VLAN enabled bond %s\n",
				   slave_dev->name, bond_dev->name);
			return -EPERM;
		} else {
			netdev_warn(bond_dev, "enslaved VLAN challenged slave %s. Adding VLANs will be blocked as long as %s is part of bond %s\n",
				    slave_dev->name, slave_dev->name,
				    bond_dev->name);
		}
	} else {
		netdev_dbg(bond_dev, "%s is !NETIF_F_VLAN_CHALLENGED\n",
			   slave_dev->name);
	}
	printk("bd_4\n");

	/* Old ifenslave binaries are no longer supported.  These can
	 * be identified with moderate accuracy by the state of the slave:
	 * the current ifenslave will set the interface down prior to
	 * enslaving it; the old ifenslave will not.
	 */
	if ((slave_dev->flags & IFF_UP)) {
		netdev_err(bond_dev, "%s is up - this may be due to an out of date ifenslave\n",
			   slave_dev->name);
		res = -EPERM;
		goto err_undo_flags;
	}
	printk("bd_5\n");

	/* set bonding device ether type by slave - bonding netdevices are
	 * created with ether_setup, so when the slave type is not ARPHRD_ETHER
	 * there is a need to override some of the type dependent attribs/funcs.
	 *
	 * bond ether type mutual exclusion - don't allow slaves of dissimilar
	 * ether type (eg ARPHRD_ETHER and ARPHRD_INFINIBAND) share the same bond
	 */
	if (!bond_has_slaves(bond)) {
		printk("bd_5_0\n");

	} else if (bond_dev->type != slave_dev->type) {
		netdev_err(bond_dev, "%s ether type (%d) is different from other slaves (%d), can not enslave it\n",
			   slave_dev->name, slave_dev->type, bond_dev->type);
		res = -EINVAL;
		goto err_undo_flags;
	}
	printk("bd_6\n");

	if (slave_ops->ndo_set_mac_address == NULL) {
		printk("bd_6_0\n");

		netdev_warn(bond_dev, "The slave device specified does not support setting the MAC address\n");
		if (BOND_MODE(bond) == BOND_MODE_ACTIVEBACKUP &&
		    bond->params.fail_over_mac != BOND_FOM_ACTIVE) {
			if (!bond_has_slaves(bond)) {
				bond->params.fail_over_mac = BOND_FOM_ACTIVE;
				netdev_warn(bond_dev, "Setting fail_over_mac to active for active-backup mode\n");
			} else {
				netdev_err(bond_dev, "The slave device specified does not support setting the MAC address, but fail_over_mac is not set to active\n");
				res = -EOPNOTSUPP;
				goto err_undo_flags;
			}
		}
	}
	printk("bd_7\n");

	call_netdevice_notifiers(NETDEV_JOIN, slave_dev);
	printk("bd_8\n");

 	/* If this is the first slave, then we need to set the master's hardware
 	 * address to be the same as the slave's.
 	 */  //bond_has_slaves returning 0 means empty
	if (!bond_has_slaves(bond) &&
	    bond->dev->addr_assign_type == NET_ADDR_RANDOM)
	{

		printk("bd_8_0\n");
	}

	new_slave = bond_alloc_slave(bond);
	if (!new_slave) {
		res = -ENOMEM;
		goto err_undo_flags;
	}
	printk("bd_9\n");

	new_slave->bond = bond;
	new_slave->dev = slave_dev;
	/* Set the new_slave's queue_id to be zero.  Queue ID mapping
	 * is set via sysfs or module option if desired.
	 */
	new_slave->queue_id = 0;

	/* Save slave's original mtu and then set it to match the bond */
	new_slave->original_mtu = slave_dev->mtu;
	res = dev_set_mtu(slave_dev, bond->dev->mtu);
	if (res) {
		netdev_dbg(bond_dev, "Error %d calling dev_set_mtu\n", res);
		goto err_free;
	}
	printk("bd_a\n");

	/* Save slave's original ("permanent") mac address for modes
	 * that need it, and for restoring it upon release, and then
	 * set it to the master's address
	 */
	memcpy(new_slave->perm_hwaddr, slave_dev->dev_addr, ETH_ALEN);
	printk("bd_b\n");

	if (!bond->params.fail_over_mac ||
	    BOND_MODE(bond) != BOND_MODE_ACTIVEBACKUP) {
		/* Set slave to master's mac address.  The application already
		 * set the master's mac address to that of the first slave
		 */
		 
		printk("bd_b_0\n");
		memcpy(addr.sa_data, bond_dev->dev_addr, bond_dev->addr_len);
		addr.sa_family = slave_dev->type;
		res = dev_set_mac_address(slave_dev, &addr);
		if (res) {
			netdev_dbg(bond_dev, "Error %d calling set_mac_address\n", res);
			goto err_restore_mtu;
		}
	}

	/* set slave flag before open to prevent IPv6 addrconf */
	slave_dev->flags |= IFF_SLAVE;
	printk("bd_c\n");

	/* open the slave since the application closed it */
	res = dev_open(slave_dev);
	if (res) {
		netdev_dbg(bond_dev, "Opening slave %s failed\n", slave_dev->name);
		goto err_restore_mac;
	}

	slave_dev->priv_flags |= IFF_BONDING;
	/* initialize slave stats */
	dev_get_stats64(new_slave->dev, &new_slave->slave_stats);
	printk("bd_d\n");

	printk("bd_e\n");

	/* If the mode uses primary, then the new slave gets the
	 * master's promisc (and mc) settings only if it becomes the
	 * curr_active_slave, and that is taken care of later when calling
	 * bond_change_active()
	 */
	if (!bond_uses_primary(bond)) {
		printk("bd_e_0\n");
		/* set promiscuity level to new slave */
		if (bond_dev->flags & IFF_PROMISC) {
			printk("bd_e_1\n");
			res = dev_set_promiscuity(slave_dev, 1);
			if (res)
				goto err_close;
		}

		/* set allmulti level to new slave */
		if (bond_dev->flags & IFF_ALLMULTI) {
			printk("bd_e_2\n");
			res = dev_set_allmulti(slave_dev, 1);
			if (res)
				goto err_close;
		}
		printk("bd_e_3\n");

		netif_addr_lock_bh(bond_dev);
		/* upload master's mc_list to new slave */
		for (dmi = bond_dev->mc_list; dmi; dmi = dmi->next)
		{	
		    printk("bd_e_4\n");
		    dev_mc_add(slave_dev, dmi->dmi_addr,
				   dmi->dmi_addrlen, 0);
		}
		printk("bd_e_5\n");
		netif_addr_unlock_bh(bond_dev);
	}
	printk("bd_f\n");

	bond_add_vlans_on_slave(bond, slave_dev);
	printk("bd_10\n");

	prev_slave = bond_last_slave(bond);

	new_slave->delay = 0;
	new_slave->link_failure_count = 0;

	bond_update_speed_duplex(new_slave);

	new_slave->last_rx = jiffies -
		(msecs_to_jiffies(bond->params.arp_interval) + 1);
	
	for (i = 0; i < BOND_MAX_ARP_TARGETS; i++)
	{
	    printk("bd_10_0\n");
	    new_slave->target_last_arp_rx[i] = new_slave->last_rx;
	}

	if (bond->params.miimon && !bond->params.use_carrier) {
        printk("bd_10_1\n");
		link_reporting = bond_check_dev_link(bond, slave_dev, 1);
		

		if ((link_reporting == -1) && !bond->params.arp_interval) {
			/* miimon is set but a bonded network driver
			 * does not support ETHTOOL/MII and
			 * arp_interval is not set.  Note: if
			 * use_carrier is enabled, we will never go
			 * here (because netif_carrier is always
			 * supported); thus, we don't need to change
			 * the messages for netif_carrier.
			 */
			netdev_warn(bond_dev, "MII and ETHTOOL support not available for interface %s, and arp_interval/arp_ip_target module parameters not specified, thus bonding will not detect link failures! see bonding.txt for details\n",
				    slave_dev->name);
		} else if (link_reporting == -1) {
			/* unable get link status using mii/ethtool */
			netdev_warn(bond_dev, "can't get link status from interface %s; the network driver associated with this interface does not support MII or ETHTOOL link status reporting, thus miimon has no effect on this interface\n",
				    slave_dev->name);
		}
	}
	printk("bd_11\n");

	/* check for initial state */
	if (bond->params.miimon) {
		printk("bd_11_0\n");

	} else if (bond->params.arp_interval) {
	    printk("bd_11_0_0\n");
		new_slave->link = (netif_carrier_ok(slave_dev) ?
			BOND_LINK_UP : BOND_LINK_DOWN);
	} else {
  	    printk("bd_11_0_0\n");
		new_slave->link = BOND_LINK_UP;
	}
	printk("bd_12\n");

	if (new_slave->link != BOND_LINK_DOWN)
		new_slave->last_link_up = jiffies;
	netdev_dbg(bond_dev, "Initial state of slave_dev is BOND_LINK_%s\n",
		   new_slave->link == BOND_LINK_DOWN ? "DOWN" :
		   (new_slave->link == BOND_LINK_UP ? "UP" : "BACK"));

	if (bond_uses_primary(bond) && bond->params.primary[0]) {
		printk("bd_12_0\n");

	}
	printk("bd_13\n");
	(bond)->params.mode = BOND_MODE_ACTIVEBACKUP;

	switch (BOND_MODE(bond)) {
	case BOND_MODE_ACTIVEBACKUP:
		bond_set_slave_inactive_flags(new_slave,
					      BOND_SLAVE_NOTIFY_NOW);
		printk("bd_13_0\n");
		break;
	default:
		printk("bd_13_3\n");

		break;
	} /* switch(bond_mode) */

#ifdef CONFIG_NET_POLL_CONTROLLER
	slave_dev->npinfo = bond->dev->npinfo;
	if (slave_dev->npinfo) {
		if (slave_enable_netpoll(new_slave)) {
			netdev_info(bond_dev, "master_dev is using netpoll, but new slave device does not support netpoll\n");
			res = -EBUSY;
			goto err_detach;
		}
	}
	printk("bd_13_4\n");
#endif
	printk("bd_14\n");

	res = bond_create_slave_symlinks(bond_dev, slave_dev);
	if (res)
		goto err_detach;

	if (!(bond_dev->features & NETIF_F_LRO))
		dev_disable_lro(slave_dev);
	printk("bd_15:bd_dev:%lx,bd_dev->flags:%x\n",(uintptr_t)bond_dev,bond_dev->flags);

	//res = netdev_rx_handler_register(slave_dev, bond_handle_frame,
	//				 new_slave);

	//res = netdev_rx_handler_register(slave_dev,netvsc_recv_callback,new_slave);//using this tx pa increasing,but rx is always 0
	res = netdev_rx_handler_register(slave_dev,netvsc_vf_handle_frame,new_slave);
	//	res= netdev_rx_handler_register(bond_dev,
		//			 netvsc_vf_handle_frame, new_slave);
	printk("bd_15_0:slave_dev:%lx,slave_dev->rx_hanler:%lx\n",(uintptr_t)slave_dev,(uintptr_t)(netdev_extended(slave_dev)->rx_handler));
	if (res) {
		netdev_dbg(bond_dev, "Error %d calling netdev_rx_handler_register\n", res);
		goto err_dest_symlinks;
	}
	printk("bd_15_1:&new_slave->list:%lx, &bond->slave_list:%lx,(&bond->slave_list)->prev:%lx\n",(uintptr_t)(&new_slave->list), (uintptr_t)(&bond->slave_list),(uintptr_t)((&bond->slave_list)->prev));

	res = bond_master_upper_dev_link(bond_dev, slave_dev, new_slave);
	printk("bd_15_2:new_slave:%lx\n",(uintptr_t)new_slave);

	if (res) {
		netdev_dbg(bond_dev, "Error %d calling bond_master_upper_dev_link\n", res);
		goto err_unregister;
	}
	printk("bd_15_3\n");
	goto my_define;

/* Undo stages on error */
err_upper_unlink:
	
err_unregister:
	
err_dest_symlinks:
	
err_detach:
	
err_close:
	
err_restore_mac:
	
err_restore_mtu:
	
err_free:
	
err_undo_flags:
	

my_define:
	printk("bd_22:res:%d\n",res);
	return res;
}
#if 0
////////////////////////////////////////////////////////////////////////////////////////////////////////
/* Try to release the slave device <slave> from the bond device <master>
 * It is legal to access curr_active_slave without a lock because all the function
 * is RTNL-locked. If "all" is true it means that the function is being called
 * while destroying a bond interface and all slaves are being released.
 *
 * The rules for slave state should be:
 *   for Active/Backup:
 *     Active stays on all backups go down
 *   for Bonded connections:
 *     The first up interface should be left on and all others downed.
 */
static int __bond_release_one(struct net_device *bond_dev,
			      struct net_device *slave_dev,
			      bool all)
{
	struct bonding *bond = netdev_priv(bond_dev);
	struct slave *slave, *oldcurrent;
	struct sockaddr addr;
	u32 old_features = bond_dev->features;
	int old_flags = bond_dev->flags;

	/* slave is not a slave or master is not master of this slave */
	if (!(slave_dev->flags & IFF_SLAVE) ||
	    (slave_dev->master != bond_dev)) {
		netdev_err(bond_dev, "cannot release %s\n",
			   slave_dev->name);
		return -EINVAL;
	}

	block_netpoll_tx();

	slave = bond_get_slave_by_dev(bond, slave_dev);
	if (!slave) {
		/* not a slave of this bond */
		netdev_info(bond_dev, "%s not enslaved\n",
			    slave_dev->name);
		unblock_netpoll_tx();
		return -EINVAL;
	}

	bond_sysfs_slave_del(slave);

	/* recompute stats just before removing the slave */
	bond_get_stats(bond->dev, &bond->bond_stats);

	/* release the slave from its bond */
	bond_detach_slave(bond, slave);

	bond_upper_dev_unlink(bond_dev, slave_dev);
	/* unregister rx_handler early so bond_handle_frame wouldn't be called
	 * for this slave anymore.
	 */
	netdev_rx_handler_unregister(slave_dev);

	if (BOND_MODE(bond) == BOND_MODE_8023AD)
		bond_3ad_unbind_slave(slave);

	if (bond_mode_uses_xmit_hash(bond))
		bond_update_slave_arr(bond, slave);

	netdev_info(bond_dev, "Releasing %s interface %s\n",
		    bond_is_active_slave(slave) ? "active" : "backup",
		    slave_dev->name);

	oldcurrent = rcu_access_pointer(bond->curr_active_slave);

	RCU_INIT_POINTER(bond->current_arp_slave, NULL);

	if (!all && (!bond->params.fail_over_mac ||
		     BOND_MODE(bond) != BOND_MODE_ACTIVEBACKUP)) {
		if (ether_addr_equal_64bits(bond_dev->dev_addr, slave->perm_hwaddr) &&
		    bond_has_slaves(bond))
			netdev_warn(bond_dev, "the permanent HWaddr of %s - %pM - is still in use by %s - set the HWaddr of %s to a different address to avoid conflicts\n",
				    slave_dev->name, slave->perm_hwaddr,
				    bond_dev->name, slave_dev->name);
	}

	if (rtnl_dereference(bond->primary_slave) == slave)
		RCU_INIT_POINTER(bond->primary_slave, NULL);

	if (oldcurrent == slave) {
		bond_unicast_sync(bond, oldcurrent, NULL);
		bond_change_active_slave(bond, NULL);
	}

	if (bond_is_lb(bond)) {
		/* Must be called only after the slave has been
		 * detached from the list and the curr_active_slave
		 * has been cleared (if our_slave == old_current),
		 * but before a new active slave is selected.
		 */
		bond_alb_deinit_slave(bond, slave);
	}

	if (all) {
		RCU_INIT_POINTER(bond->curr_active_slave, NULL);
	} else if (oldcurrent == slave) {
		/* Note that we hold RTNL over this sequence, so there
		 * is no concern that another slave add/remove event
		 * will interfere.
		 */
		bond_select_active_slave(bond);
	}

	if (!bond_has_slaves(bond)) {
		bond_set_carrier(bond);
		eth_hw_addr_random(bond_dev);
	}

	unblock_netpoll_tx();
	synchronize_rcu();
	bond->slave_cnt--;

	if (!bond_has_slaves(bond)) {
		call_netdevice_notifiers(NETDEV_CHANGEADDR, bond->dev);
		call_netdevice_notifiers(NETDEV_RELEASE, bond->dev);
	}

	bond_compute_features(bond);
	if (!(bond_dev->features & NETIF_F_VLAN_CHALLENGED) &&
	    (old_features & NETIF_F_VLAN_CHALLENGED))
		netdev_info(bond_dev, "last VLAN challenged slave %s left bond %s - VLAN blocking is removed\n",
			    slave_dev->name, bond_dev->name);

	/* must do this from outside any spinlocks */
	bond_destroy_slave_symlinks(bond_dev, slave_dev);

	bond_del_vlans_from_slave(bond, slave_dev);

	/* If the mode uses primary, then we should only remove its
	 * promisc and mc settings if it was the curr_active_slave, but that was
	 * already taken care of above when we detached the slave
	 */
	if (!bond_uses_primary(bond)) {
		/* unset promiscuity level from slave
		 * NOTE: The NETDEV_CHANGEADDR call above may change the value
		 * of the IFF_PROMISC flag in the bond_dev, but we need the
		 * value of that flag before that change, as that was the value
		 * when this slave was attached, so we cache at the start of the
		 * function and use it here. Same goes for ALLMULTI below
		 */
		if (old_flags & IFF_PROMISC)
			dev_set_promiscuity(slave_dev, -1);

		/* unset allmulti level from slave */
		if (old_flags & IFF_ALLMULTI)
			dev_set_allmulti(slave_dev, -1);

		/* flush master's mc_list from slave */
		netif_addr_lock_bh(bond_dev);
		bond_mc_list_flush(bond_dev, slave_dev);
		netif_addr_unlock_bh(bond_dev);
	}

	slave_disable_netpoll(slave);

	/* close slave before restoring its mac address */
	dev_close(slave_dev);

	if (bond->params.fail_over_mac != BOND_FOM_ACTIVE ||
	    BOND_MODE(bond) != BOND_MODE_ACTIVEBACKUP) {
		/* restore original ("permanent") mac address */
		memcpy(addr.sa_data, slave->perm_hwaddr, ETH_ALEN);
		addr.sa_family = slave_dev->type;
		dev_set_mac_address(slave_dev, &addr);
	}

	dev_set_mtu(slave_dev, slave->original_mtu);

	slave_dev->priv_flags &= ~IFF_BONDING;

	bond_free_slave(slave);

	return 0;
}

/* A wrapper used because of ndo_del_link */
int bond_release(struct net_device *bond_dev, struct net_device *slave_dev)
{
	return __bond_release_one(bond_dev, slave_dev, false);
}

/* First release a slave and then destroy the bond if no more slaves are left.
 * Must be under rtnl_lock when this function is called.
 */
static int  bond_release_and_destroy(struct net_device *bond_dev,
				     struct net_device *slave_dev)
{
	struct bonding *bond = netdev_priv(bond_dev);
	int ret;

	ret = bond_release(bond_dev, slave_dev);
	if (ret == 0 && !bond_has_slaves(bond)) {
		bond_dev->priv_flags |= IFF_DISABLE_NETPOLL;
		netdev_info(bond_dev, "Destroying bond %s\n",
			    bond_dev->name);
		unregister_netdevice(bond_dev);
	}
	return ret;
}
//////////////////////////////////////////////////////////////////////////////////////////////////////
#endif
//struct slave  struct bond
static int netvsc_vf_join(struct net_device *vf_netdev,
			  struct net_device *ndev)
{
	struct net_device_context *ndev_ctx = netdev_priv(ndev);
	struct bonding *bond_dev = netdev_priv(ndev) + ALIGN(sizeof(struct net_device_context), NETDEV_ALIGN);

	int ret;
	printk("vf_netdev:%lx,vf->flags:%x,ndev:%lx,ndev->flags:%x\n",(uintptr_t)(vf_netdev),vf_netdev->flags,(uintptr_t)(ndev),ndev->flags);

	ret = my_bond_enslave(ndev, vf_netdev);
	//rcu_assign_pointer(netdev_extended(vf_netdev)->dev, ndev);
	rcu_assign_pointer(netdev_extended(vf_netdev)->dev, vf_netdev);
	printk("vf->rx_handler:%lx\n",(uintptr_t)(netdev_extended(vf_netdev)->rx_handler));
	if(ret != 0){
		netdev_err(vf_netdev,
			   "can not bond_enslave (err = %d)\n",
			   ret);
		goto rx_handler_failed;
	}	
	
	//rcu_assign_pointer(netdev_extended(ndev)->rx_handler_data, vf_netdev);
	//rcu_assign_pointer(netdev_extended(vf_netdev)->rx_handler, NULL);

#if 1	
	//ret = netdev_rx_handler_register(vf_netdev,
	//				 netvsc_vf_handle_frame, ndev);

	if (ret != 0) {
		netdev_err(vf_netdev,
			   "can not register netvsc VF receive handler (err = %d)\n",
			   ret);
		goto rx_handler_failed;
	}


	//ret = netdev_upper_dev_link(vf_netdev, ndev,
	//				   NULL, NULL, NULL);
	ret = netdev_master_upper_dev_link(vf_netdev, ndev);

	if (ret != 0) {
		netdev_err(vf_netdev,
			   "can not set master device %s (err = %d)\n",
			   ndev->name, ret);
		goto upper_link_failed;
	}

#endif		

	/* set slave flag before open to prevent IPv6 addrconf */
	vf_netdev->flags |= IFF_SLAVE;

	schedule_work(&ndev_ctx->vf_takeover);

	netdev_info(vf_netdev, "joined to %s\n", ndev->name);
	return 0;

upper_link_failed:
	//netdev_rx_handler_unregister(vf_netdev);
rx_handler_failed:
	return ret;
}

static void __netvsc_vf_setup(struct net_device *ndev,
			      struct net_device *vf_netdev)
{
	int ret;

	call_netdevice_notifiers(NETDEV_JOIN, vf_netdev);

	/* Align MTU of VF with master */
	ret = dev_set_mtu(vf_netdev, ndev->mtu);
	if (ret)
		netdev_warn(vf_netdev,
			    "unable to change mtu to %u\n", ndev->mtu);

	if (netif_running(ndev)) {
		ret = dev_open(vf_netdev);
		if (ret)
			netdev_warn(vf_netdev,
				    "unable to open: %d\n", ret);
	}
}

/* Setup VF as slave of the synthetic device.
 * Runs in workqueue to avoid recursion in netlink callbacks.
 */
static void netvsc_vf_setup(struct work_struct *w)
{
	struct net_device_context *ndev_ctx
		= container_of(w, struct net_device_context, vf_takeover);
	struct net_device *ndev = hv_get_drvdata(ndev_ctx->device_ctx);
	struct net_device *vf_netdev;

        if(!rtnl_trylock()) {
		schedule_work(w);
		return;
	}
	vf_netdev = rtnl_dereference(ndev_ctx->vf_netdev);
	if (vf_netdev)
		__netvsc_vf_setup(ndev, vf_netdev);

	rtnl_unlock();
}

//bond_slave_get_rcu
//	struct sock
//	struct sk_buff

static int netvsc_register_vf(struct net_device *vf_netdev)
{
	struct net_device *ndev;
	struct net_device_context *net_device_ctx;
	struct netvsc_device *netvsc_dev;
	struct bonding * bond_dev;

	if (vf_netdev->addr_len != ETH_ALEN)
		return NOTIFY_DONE;

	/*
	 * We will use the MAC address to locate the synthetic interface to
	 * associate with the VF interface. If we don't find a matching
	 * synthetic interface, move on.
	 */
	/* deviation from upstream - we are using dev_addr rather than perm_addr */
	ndev = get_netvsc_bymac(vf_netdev->dev_addr);
	if (!ndev)
		return NOTIFY_DONE;

	net_device_ctx = netdev_priv(ndev);
	netvsc_dev = net_device_ctx->nvdev;
    bond_dev = netdev_priv(ndev) + ALIGN(sizeof(struct net_device_context), NETDEV_ALIGN);
	if (!netvsc_dev || net_device_ctx->vf_netdev)
		return NOTIFY_DONE;
	net_device_ctx->vf_netdev = vf_netdev;

	if (netvsc_vf_join(vf_netdev, ndev) != 0)
		return NOTIFY_DONE;

	netdev_info(ndev, "VF registering: %s\n", vf_netdev->name);

	/* Prevent this module from being unloaded while VF is registered */
	try_module_get(THIS_MODULE);

	dev_hold(vf_netdev);
	net_device_ctx->vf_netdev = vf_netdev;
	printk("end:vf_rg:vf->rx_handler:%lx\n",(uintptr_t)(netdev_extended(vf_netdev)->rx_handler));
		printk("end:vf_rg:vf_extend->dev:%lx\n",(uintptr_t)(netdev_extended(vf_netdev)->dev));
	return NOTIFY_OK;
}

static void netvsc_inject_disable(struct net_device_context *net_device_ctx)
{
	net_device_ctx->vf_inject = false;

	/* Wait for currently active users to drain out. */
	while (atomic_read(&net_device_ctx->vf_use_cnt) != 0)
		udelay(50);
}

/* Change datapath */
static int netvsc_vf_up(struct net_device *vf_netdev)
{
	struct net_device_context *net_device_ctx;
	struct netvsc_device *netvsc_dev;
	struct net_device *ndev;
    printk("up_0\n");
	ndev = get_netvsc_byref(vf_netdev);
	printk("up_1\n");

	if (!ndev)
	{
    	printk("up_2\n");
		return NOTIFY_DONE;
	}

	net_device_ctx = netdev_priv(ndev);
	printk("up_3\n");
	netvsc_dev = rtnl_dereference(net_device_ctx->nvdev);
	printk("up_4\n");
	if (!netvsc_dev)
	{
	    printk("up_5\n");
		return NOTIFY_DONE;
	}
	printk("up_6\n");

	/* Bump refcount when datapath is acvive - Why? */
	rndis_filter_open(netvsc_dev);
	printk("up_7\n");

	/* notify the host to switch the data path. */
	netvsc_switch_datapath(ndev, true);
	printk("up_8\n");
	netdev_info(ndev, "Data path switched to VF: %s\n", vf_netdev->name);
	printk("up_9:vf->rx_handler:%lx\n",(uintptr_t)(netdev_extended(vf_netdev)->rx_handler));

	return NOTIFY_OK;
}
 
static int netvsc_vf_down(struct net_device *vf_netdev)
{
	struct net_device_context *net_device_ctx;
	struct netvsc_device *netvsc_dev;
	struct net_device *ndev;

	ndev = get_netvsc_byref(vf_netdev);
	if (!ndev)
		return NOTIFY_DONE;

	net_device_ctx = netdev_priv(ndev);
	netvsc_dev = rtnl_dereference(net_device_ctx->nvdev);
	if (!netvsc_dev)
		return NOTIFY_DONE;

	netvsc_switch_datapath(ndev, false);
	netdev_info(ndev, "Data path switched from VF: %s\n", vf_netdev->name);
	rndis_filter_close(netvsc_dev);

	return NOTIFY_OK;
}

static int netvsc_unregister_vf_75(struct net_device *vf_netdev)
{
	struct net_device *ndev;
	struct net_device_context *net_device_ctx;

	ndev = get_netvsc_byref(vf_netdev);
	if (!ndev)
		return NOTIFY_DONE;

	net_device_ctx = netdev_priv(ndev);
	cancel_delayed_work_sync(&net_device_ctx->vf_takeover);

	netdev_info(ndev, "VF unregistering: %s\n", vf_netdev->name);

	netdev_rx_handler_unregister(vf_netdev);
	netdev_upper_dev_unlink(vf_netdev, ndev);
	RCU_INIT_POINTER(net_device_ctx->vf_netdev, NULL);
	//dev_put(vf_netdev);

	return NOTIFY_OK;
}
//IFF_SLAVE

static int netvsc_unregister_vf(struct net_device *vf_netdev)
{
	struct net_device *ndev;
	struct net_device_context *net_device_ctx;

	ndev = get_netvsc_byref(vf_netdev);
	if (!ndev)
		return NOTIFY_DONE;

	net_device_ctx = netdev_priv(ndev);
	cancel_work_sync(&net_device_ctx->vf_takeover);
 
	netvsc_vf_down(vf_netdev);

	netdev_info(ndev, "VF unregistering: %s\n", vf_netdev->name);

	netdev_upper_dev_unlink(vf_netdev, ndev);
	//rtmsg_ifinfo(RTM_NEWLINK, ndev, IFF_MASTER);
	netvsc_inject_disable(net_device_ctx);
	net_device_ctx->vf_netdev = NULL;
	dev_put(vf_netdev);
	module_put(THIS_MODULE);
	return NOTIFY_OK;
}

static int netvsc_probe(struct hv_device *dev,
			const struct hv_vmbus_device_id *dev_id)
{
	struct net_device *net = NULL;
	struct net_device_context *net_device_ctx;
	struct netvsc_device_info device_info;
	struct netvsc_device *nvdev;
	struct bonding *bond_dev;
	unsigned int size_all;
	int ret = -ENOMEM;

    size_all = ALIGN(sizeof(struct net_device_context), NETDEV_ALIGN)+ ALIGN(sizeof(struct bonding), NETDEV_ALIGN);
	net = alloc_etherdev_mq(size_all,
				VRSS_CHANNEL_MAX);
	if (!net)
		goto no_net;

	netif_carrier_off(net);

	netvsc_init_settings(net);

	net_device_ctx = netdev_priv(net);
	net_device_ctx->device_ctx = dev;
	net_device_ctx->msg_enable = netif_msg_init(debug, default_msg);
	bond_dev = netdev_priv(net) + ALIGN(sizeof(struct net_device_context), NETDEV_ALIGN);
	bond_dev->dev = net;//no problem sc
	if (netif_msg_probe(net_device_ctx))
		netdev_dbg(net, "netvsc msg_enable: %d\n",
			net_device_ctx->msg_enable);

	hv_set_drvdata(dev, net);

	net_device_ctx->synthetic_data_path = true;

	INIT_DELAYED_WORK(&net_device_ctx->dwork, netvsc_link_change);
#if (RHEL_RELEASE_CODE < RHEL_RELEASE_VERSION(6,2))
	INIT_WORK(&net_device_ctx->gwrk.dwrk, netvsc_notify_peers);
#endif
	spin_lock_init(&net_device_ctx->lock);
	INIT_LIST_HEAD(&net_device_ctx->reconfig_events);
	INIT_WORK(&net_device_ctx->vf_takeover, netvsc_vf_setup);

	//net_device_ctx->vf_stats
	//	= netdev_alloc_pcpu_stats(struct netvsc_vf_pcpu_stats);
	net_device_ctx->vf_stats
		= alloc_percpu(struct netvsc_vf_pcpu_stats);
	if (!net_device_ctx->vf_stats)
		goto no_stats;

	atomic_set(&net_device_ctx->vf_use_cnt, 0);
	net_device_ctx->vf_netdev = NULL;
	net_device_ctx->vf_inject = false;

	net->netdev_ops = &device_ops;
	net->ethtool_ops = &ethtool_ops;
	SET_NETDEV_DEV(net, &dev->device);

	netif_set_real_num_tx_queues(net, 1);
#ifdef NOTYET
	netif_set_real_num_rx_queues(net, 1);
#endif

	/* Notify the netvsc driver of the new device */
	memset(&device_info, 0, sizeof(device_info));
	device_info.ring_size = ring_size;
	device_info.num_chn = VRSS_CHANNEL_DEFAULT;

	nvdev = rndis_filter_device_add(dev, &device_info);
	if (IS_ERR(nvdev)) {
		ret = PTR_ERR(nvdev);
		netdev_err(net, "unable to add netvsc device (ret %d)\n", ret);
		goto rndis_failed;

	}
	memcpy(net->dev_addr, device_info.mac_adr, ETH_ALEN);

#ifdef NOTYET
	/* hw_features computed in rndis_filter_device_add */
	net->features = net->hw_features |
		NETIF_F_HIGHDMA | NETIF_F_SG |
		NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_CTAG_RX;
#endif
	// Need to explicitly set NETIF_F_HW_VLAN_TX support on RH6.X kernels.
	net->features |= NETIF_F_HIGHDMA | NETIF_F_SG |
	NETIF_F_HW_VLAN_CTAG_TX | NETIF_F_HW_VLAN_TX;

	net->vlan_features = net->features;

	netdev_lockdep_set_classes(net);

	dev_info(&dev->device, "real num tx,rx queues:%u, %u\n",
		 net->real_num_tx_queues, nvdev->num_chn);

	ret = register_netdev(net);
	if (ret != 0) {
		pr_err("Unable to register netdev.\n");
		goto register_failed;
	}

	return ret;

register_failed:
	rndis_filter_device_remove(dev, nvdev);
rndis_failed:
	free_percpu(net_device_ctx->vf_stats);
no_stats:
	hv_set_drvdata(dev, NULL);
	free_netdev(net);
no_net:
	return ret;
}

static int netvsc_remove(struct hv_device *dev)
{
	struct net_device *net;
	struct net_device_context *ndev_ctx;

	net = hv_get_drvdata(dev);

	if (net == NULL) {
		dev_err(&dev->device, "No net device to remove\n");
		return 0;
	}

	ndev_ctx = netdev_priv(net);

	netif_device_detach(net);

	cancel_delayed_work_sync(&ndev_ctx->dwork);

	unregister_netdev(net);

	/*
	 * Call to the vsc driver to let it know that the device is being
	 * removed. Also blocks mtu and channel changes.
	 */
	rtnl_lock();
	rndis_filter_device_remove(dev,
				   rtnl_dereference(ndev_ctx->nvdev));
	rtnl_unlock();

	hv_set_drvdata(dev, NULL);

	free_percpu(ndev_ctx->vf_stats);
	free_netdev(net);	
	return 0;
}

static const struct hv_vmbus_device_id id_table[] = {
	/* Network guid */
	{ HV_NIC_GUID, },
	{ },
};

MODULE_DEVICE_TABLE(vmbus, id_table);

/* The one and only one */
static struct  hv_driver netvsc_drv = {
	.name = KBUILD_MODNAME,
	.id_table = id_table,
	.probe = netvsc_probe,
	.remove = netvsc_remove,
};

/*
 * On Hyper-V, every VF interface is matched with a corresponding
 * synthetic interface. The synthetic interface is presented first
 * to the guest. When the corresponding VF instance is registered,
 * we will take care of switching the data path.
 */
static int netvsc_netdev_event(struct notifier_block *this,
			       unsigned long event, void *ptr)
{
#if (RHEL_RELEASE_CODE > RHEL_RELEASE_VERSION(6,10))
	/* Not in RHEL 6.8 kernel - check again when 6.9 releases - NHM */
	struct net_device *event_dev = netdev_notifier_info_to_dev(ptr);
#else
	struct net_device *event_dev = ptr;
#endif
    printk("aabb:event_dev:%lx,event:%d\n",(uintptr_t)event_dev,event);
	/* Skip our own events */
	if (event_dev->netdev_ops == &device_ops)
	{
	    printk("aabb_0\n");
	    return NOTIFY_DONE;
	}

	/* Avoid non-Ethernet type devices */
	if (event_dev->type != ARPHRD_ETHER)
	{	printk("aabb_1\n");
	    return NOTIFY_DONE;
	}

	/* Avoid Vlan dev with same MAC registering as VF */
	if (is_vlan_dev(event_dev))
	{	printk("aabb_2\n");
	    return NOTIFY_DONE;
	}

	/* Avoid Bonding master dev with same MAC registering as VF */
	if ((event_dev->priv_flags & IFF_BONDING) &&
	    (event_dev->flags & IFF_MASTER))
	{	printk("aabb_3\n");
	    return NOTIFY_DONE;
	}
	switch (event) {
	case NETDEV_REGISTER:
		printk("aabb_4\n");
		return netvsc_register_vf(event_dev);
	case NETDEV_UNREGISTER:
		printk("aabb_5\n");
		return netvsc_unregister_vf(event_dev);
	case NETDEV_UP:
		printk("aabb_6\n");
		return netvsc_vf_up(event_dev);
	case NETDEV_DOWN:
		printk("aabb_7\n");
		return netvsc_vf_down(event_dev);
	default:
		return NOTIFY_DONE;
	}
}

static struct notifier_block netvsc_netdev_notifier = {
	.notifier_call = netvsc_netdev_event,
};


static void __exit netvsc_drv_exit(void)
{
	unregister_netdevice_notifier(&netvsc_netdev_notifier);
	vmbus_driver_unregister(&netvsc_drv);
}

static int __init netvsc_drv_init(void)
{
	int ret;

	if (ring_size < RING_SIZE_MIN) {
		ring_size = RING_SIZE_MIN;
		pr_info("Increased ring_size to %d (min allowed)\n",
			ring_size);
	}
	ret = vmbus_driver_register(&netvsc_drv);
    printk("drv_init\n");
	if (ret)
		return ret;

	register_netdevice_notifier(&netvsc_netdev_notifier);
	return 0;
}

MODULE_LICENSE("GPL");
MODULE_VERSION(HV_DRV_VERSION);
MODULE_DESCRIPTION("Microsoft Hyper-V network driver");
MODULE_ALIAS("vmbus:635161f83edfc546913ff2d2f965ed0e");

module_init(netvsc_drv_init);
module_exit(netvsc_drv_exit);
