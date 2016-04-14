#include <net/netmap_user.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <poll.h>
#include <sys/socket.h>
#include <ifaddrs.h>
#include <stddef.h>
#include <string.h>
#include <linux/if_packet.h> //No BSD
#include <stdio.h>
#include <stdlib.h>
#include "netmap.h"

//#define DEBUG

void hexdump(uint8_t* p, unsigned int bytes){
	printf("Dump of address: %p, %u bytes", p, bytes);
	uint8_t* end = p+bytes;
	int counter = 0;
	while(p < end){
		if((counter & 0xf) == 0 ){ printf("\n  %04x:  ", counter);}
		printf(" %02x%02x", *p, *(p+1));
		p += 2;
		counter += 2;
	}
	printf("\n");
}

struct netmap_if* NETMAP_IF_wrapper(void* base, uint32_t ofs){
	return NETMAP_IF(base, ofs);
}

struct netmap_ring* NETMAP_TXRING_wrapper(struct netmap_if* nifp, uint32_t index){
	return NETMAP_TXRING(nifp, index);
}

struct netmap_ring* NETMAP_RXRING_wrapper(struct netmap_if* nifp, uint32_t index){
	return NETMAP_RXRING(nifp, index);
}

char* NETMAP_BUF_wrapper(struct netmap_ring* ring, uint32_t index){
	return NETMAP_BUF(ring, index);
}

char* NETMAP_BUF_smart_wrapper(struct netmap_ring* ring, uint32_t index){
	return NETMAP_BUF(ring, ring->slot[index].buf_idx);
}

uint64_t NETMAP_BUF_IDX_wrapper(struct netmap_ring* ring, char* buf){
	return NETMAP_BUF_IDX(ring, buf);
}

int ioctl_NIOCGINFO(int fd, struct nmreq* nmr){
	return ioctl(fd, NIOCGINFO, nmr);
}

int ioctl_NIOCREGIF(int fd, struct nmreq* nmr){
	return ioctl(fd, NIOCREGIF, nmr);
}

int ioctl_NIOCTXSYNC(int fd){
	return ioctl(fd, NIOCTXSYNC);
}

int ioctl_NIOCRXSYNC(int fd){
	return ioctl(fd, NIOCRXSYNC);
}

int get_mac(char* ifname, char* mac){ // No BSD
	uint8_t raw[6] = {0};
	struct ifaddrs* ifap;
	int ret = getifaddrs(&ifap);
	struct ifaddrs* head = ifap;
	if(ret != 0){
		return -1;
	}
	while(ifap != NULL){
		if(strncmp(ifname, ifap->ifa_name, 16) == 0){
			struct sockaddr_ll* ll = (struct sockaddr_ll*) ifap->ifa_addr;
			memcpy(raw, ll->sll_addr, 6);
			sprintf(mac, "%02x:%02x:%02x:%02x:%02x:%02x",
					raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
			return 0;
		}
		ifap = ifap->ifa_next;
	}
	freeifaddrs(head);
	return -1;
}

struct rte_mbuf** nm_alloc_mbuf_array(uint32_t num){
	size_t mbuf_len = sizeof(struct rte_mbuf);
	struct rte_mbuf** mbufs = (struct rte_mbuf**) malloc(mbuf_len * num);
	for(uint32_t i=0; i<num; i++){
		mbufs[i] = (struct rte_mbuf*) malloc(mbuf_len);
	}
	return mbufs;
}

void mbufs_len_update(struct nm_device* dev, uint16_t ringid, uint32_t start, uint32_t count, uint16_t len){
	struct nm_ring* ring = dev->nm_ring[ringid];
	struct netmap_ring* nm_ring = NETMAP_TXRING(dev->nm_ring[ringid]->nifp, ringid);
	for(uint32_t i=0; i < count; i++){
		// prefetch the next mbuf
		__builtin_prefetch(&ring->mbufs_tx[start+1]->pkt.data_len, 1, 1);
		__builtin_prefetch(&nm_ring->slot[start+1]);
		ring->mbufs_tx[start]->pkt.pkt_len = len;
		ring->mbufs_tx[start]->pkt.data_len = len;
		nm_ring->slot[start].flags = 0;
		start = nm_ring_next(nm_ring, start);
	}
}

void mbufs_slots_update(struct nm_device* dev, uint16_t ringid, uint32_t start, uint32_t count){
	struct nm_ring* ring = dev->nm_ring[ringid];
	struct netmap_ring* nm_ring = NETMAP_RXRING(dev->nm_ring[ringid]->nifp, ringid);
	__atomic_add_fetch (&dev->rx_pkts, count, __ATOMIC_RELAXED);
	for(uint32_t i=0; i<count; i++){
		// prefetch the next mbuf and slot
		__builtin_prefetch(&ring->mbufs_rx[start+1]->pkt.data_len, 1, 1);
		__builtin_prefetch(&nm_ring->slot[start+1]);
		uint16_t len = nm_ring->slot[start].len;
		ring->mbufs_rx[start]->pkt.pkt_len = len;
		ring->mbufs_rx[start]->pkt.data_len = len;
		nm_ring->slot[start].flags = 0;
		__atomic_add_fetch (&dev->rx_octetts, (uint64_t) len, __ATOMIC_RELAXED);
		start = nm_ring_next(nm_ring, start);
	}
}

void slot_mbuf_update(struct nm_device* dev, uint16_t ringid, uint32_t start, uint32_t count){
	struct nm_ring* ring = dev->nm_ring[ringid];
	struct netmap_ring* nm_ring = NETMAP_TXRING(dev->nm_ring[ringid]->nifp, ringid);
	__atomic_add_fetch (&dev->tx_pkts, count, __ATOMIC_RELAXED);
	for(uint32_t i=0; i<count; i++){
		// prefetch the next mbuf and slot
		__builtin_prefetch(&ring->mbufs_rx[start+1]->pkt.data_len, 1, 1);
		__builtin_prefetch(&nm_ring->slot[start+1]);
		uint16_t len = ring->mbufs_tx[start]->pkt.data_len;
		nm_ring->slot[start].len = len;
		__atomic_add_fetch (&dev->tx_octetts, (uint64_t) len, __ATOMIC_RELAXED);
		start = nm_ring_next(nm_ring, start);
	}
	nm_ring->head = start;
	nm_ring->cur = start;
	nm_ring->slot[start].flags = NS_REPORT;
}

void swap_bufs(uint32_t count, struct nm_device* txDev, uint16_t txId, struct nm_device* rxDev, uint16_t rxId){
	//debug output
	//printf("count=%d, txDev=%p, txId=%d, rxDev=%p, rxid=%d\n", count, txDev, txId, rxDev, rxId);

	struct netmap_ring* txRing = NETMAP_TXRING(txDev->nm_ring[txId]->nifp, txId);
	struct netmap_ring* rxRing = NETMAP_TXRING(rxDev->nm_ring[rxId]->nifp, rxId);
	uint32_t txStart = txRing->head;
	uint32_t rxStart = rxRing->head;
	struct nm_ring* nm_ringTxId = txDev->nm_ring[txId];
	struct nm_ring* nm_ringRxId = rxDev->nm_ring[rxId];

	// sanity check
	for(uint32_t i=0; i<txRing->num_slots; i++){
		if(nm_ringTxId->mbufs_tx[i]->data == NULL
				|| nm_ringTxId->mbufs_tx[i]->pkt.data == NULL){
			printf("Error: data pointer was NULL\n");
			int j = *((int*) NULL+1); //provoce segfault
			printf("%d\n", j);
		}
	}
	for(uint32_t i=0; i<rxRing->num_slots; i++){
		if(nm_ringRxId->mbufs_rx[i]->data == NULL
				|| nm_ringRxId->mbufs_rx[i]->pkt.data == NULL){
			printf("Error: data pointer was NULL\n");
			int j = *((int*) NULL+1); //provoce segfault
			printf("%d\n", j);
		}
	}

	__atomic_add_fetch (&txDev->tx_pkts, count, __ATOMIC_RELAXED);
	for(uint32_t i=0; i<count; i++){
		// swap buffers in slots
		uint32_t swapBuf = txRing->slot[txStart].buf_idx;
		txRing->slot[txStart].buf_idx = rxRing->slot[rxStart].buf_idx;
		rxRing->slot[rxStart].buf_idx = swapBuf;

		// update addresses in mbufs
		nm_ringTxId->mbufs_tx[txStart]->data = NETMAP_BUF(txRing, txRing->slot[txStart].buf_idx);
		nm_ringTxId->mbufs_tx[txStart]->pkt.data = NETMAP_BUF(txRing, txRing->slot[txStart].buf_idx);
		nm_ringRxId->mbufs_rx[rxStart]->data = NETMAP_BUF(txRing, txRing->slot[txStart].buf_idx);
		nm_ringRxId->mbufs_rx[rxStart]->pkt.data = NETMAP_BUF(txRing, txRing->slot[txStart].buf_idx);
		//printf("txData=%p\nrxData=%p\n", nm_ringTxId->mbufs_tx[txStart]->data, nm_ringRxId->mbufs_rx[rxStart]->data);

		// update length fields
		uint16_t rxLen = rxDev->nm_ring[rxId]->mbufs_rx[rxStart]->pkt.data_len;
		txRing->slot[txStart].len = rxLen;
		__atomic_add_fetch (&txDev->tx_octetts, (uint64_t) rxLen, __ATOMIC_RELAXED);
		// update flag
		txRing->slot[txStart].flags = NS_BUF_CHANGED;
		rxRing->slot[rxStart].flags = NS_BUF_CHANGED;

		// nextSlot for txStart and rxStart
		if(likely(i+1 < count)){
			txStart = nm_ring_next(txRing, txStart);
			rxStart = nm_ring_next(rxRing, rxStart);
		}
	}

	txRing->slot[txStart].flags = NS_REPORT;
	rxRing->slot[rxStart].flags = NS_REPORT;

	txStart = nm_ring_next(txRing, txStart);
	rxStart = nm_ring_next(rxRing, rxStart);

	txRing->head = txStart;
	txRing->cur = txStart;

	rxRing->head = rxStart;
	rxRing->cur = rxStart;

	// sanity check
	for(uint32_t i=0; i<txRing->num_slots; i++){
		if(nm_ringTxId->mbufs_tx[i]->data == NULL
				|| nm_ringTxId->mbufs_tx[i]->pkt.data == NULL){
			printf("Error: data pointer was NULL\n");
			int j = *((int*) NULL+1); //provoce segfault
			printf("%d\n", j);
		}
	}
	for(uint32_t i=0; i<rxRing->num_slots; i++){
		if(nm_ringRxId->mbufs_rx[i]->data == NULL
				|| nm_ringRxId->mbufs_rx[i]->pkt.data == NULL){
			printf("Error: data pointer was NULL\n");
			int j = *((int*) NULL+1); //provoce segfault
			printf("%d\n", j);
		}
	}
}

uint32_t fetch_tx_pkts(struct nm_device* dev){
	return __atomic_fetch_and(&dev->tx_pkts, 0x0, __ATOMIC_RELAXED);
}
uint32_t fetch_rx_pkts(struct nm_device* dev){
	return __atomic_fetch_and(&dev->rx_pkts, 0x0, __ATOMIC_RELAXED);
}
uint64_t fetch_tx_octetts(struct nm_device* dev){
	return __atomic_fetch_and(&dev->tx_octetts, 0x0, __ATOMIC_RELAXED);
}
uint64_t fetch_rx_octetts(struct nm_device* dev){
	return __atomic_fetch_and(&dev->rx_octetts, 0x0, __ATOMIC_RELAXED);
}

struct nm_device* nm_get(const char port[]){
	for(int i=0; i<nm_devs.num_devs; i++){
		if(strncmp(port, nm_devs.dev[i]->nmr.nr_name, 16) == 0){
			return nm_devs.dev[i];
		}
	}
	return NULL;
}

static int nm_reopen(uint16_t ringid, struct nm_device* dev){
	struct nmreq nmr;
	struct nmreq* nmr_orig = &dev->nmr;
	memcpy(&nmr, nmr_orig, sizeof(struct nmreq));
	nmr.nr_ringid = ringid | NETMAP_NO_TX_POLL;

	int fd = open("/dev/netmap", O_RDWR);
#ifdef DEBUG
	printf("ringid: %d, fd: %d\n", ringid, fd);
#endif
	if(fd == -1){
		printf("nm_config(): could not open device /dev/netmap\n");
		return -1;
	}

	int ret = ioctl(fd, NIOCREGIF, &nmr);
	if(ret == -1){
		printf("nm_config(): error issuing NIOCREFIF\n");
		return -1;
	}

	if(nmr.nr_tx_rings < dev->nmr.nr_tx_rings || nmr.nr_rx_rings != dev->nmr.nr_rx_rings){
		printf("Could not configure the ring count. Please do so using ethtool\n");
		printf("interface : %s\n", dev->nmr.nr_name);
		printf("requested : tx=%03d rx=%03d\n", dev->nmr.nr_tx_rings, dev->nmr.nr_tx_rings);
		printf("configured: tx=%03d rx=%03d\n", nmr.nr_tx_rings, nmr.nr_rx_rings);
		return -1;
	}

	if(netmap_mmap == NULL){
		netmap_mmap = mmap(NULL , nmr.nr_memsize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	}

	dev->nm_ring[ringid]->fd = fd;
	dev->nm_ring[ringid]->nifp = NETMAP_IF(netmap_mmap, nmr.nr_offset);

	return 0;
}

struct nm_device* nm_config(struct nm_config_struct* config){
	if(config == NULL){
		printf("nm_config(): config is NULL\n");
		return NULL;
	}
	if(nm_get(config->port) != NULL){
		printf("nm_config(): device \"%s\" is already configured\n", config->port);
		return nm_get(config->port);
	}

	struct nm_device* dev = (struct nm_device*) malloc(sizeof(struct nm_device));
	memset(dev, 0, sizeof(struct nm_device));
	struct nmreq* nmr = &dev->nmr;

	strncpy(nmr->nr_name, config->port, 16);
	nmr->nr_version = NETMAP_API;
	nmr->nr_flags = NR_REG_ONE_NIC;
	nmr->nr_tx_rings = config->txQueues;
	nmr->nr_rx_rings = config->rxQueues;

	int queues = 0;
	if(nmr->nr_tx_rings > nmr->nr_rx_rings){
		queues = nmr->nr_tx_rings;
	} else{
		queues = nmr->nr_rx_rings;
	}

	for(int i=0; i < queues; i++){
		dev->nm_ring[i] = (struct nm_ring*) malloc(sizeof(struct nm_ring));
	}

	for(int i=0; i < queues; i++){
		if(nm_reopen(i, dev) == -1){
			printf("nm_config(): error opening interface \"%s\", ring %d\n", config->port, i);
			return NULL;
		}
	}

	if(dev->nmr.nr_tx_rings < config->txQueues || dev->nmr.nr_rx_rings != config->rxQueues){
		printf("Could not configure the ring count. Please do so using ethtool\n");
		printf("interface : %s\n", config->port);
		printf("requested : tx=%03d rx=%03d\n", config->txQueues, config->rxQueues);
		printf("configured: tx=%03d rx=%03d\n", dev->nmr.nr_tx_rings, dev->nmr.nr_rx_rings);
		return NULL;
	}

	nm_devs.dev[nm_devs.num_devs++] = dev;

	return dev;
}
