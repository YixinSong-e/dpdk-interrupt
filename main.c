#define _GNU_SOURCE
#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <pthread.h>
#include<string.h>
#include "dpdk.h"
#include <sys/syscall.h>
#include <signal.h>
#define RX_RING_SIZE 128
#define TX_RING_SIZE 512
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 8
#define ARRAY_LENGTH 10000000
#define SEND_PPS 30
#define GENTIME (1000000000 / SEND_PPS)
uint64_t rx_time[ARRAY_LENGTH];
FILE *fp;
uint64_t t1,t2;
static uint8_t rss_intel_key[40] = { 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
	                             0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
				     0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
				     0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A,
				     0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A, 0x6D, 0x5A
	                           };
//这里用skleten 默认配置
static const struct rte_eth_conf port_conf_default = {
	.rxmode = { 
		.max_lro_pkt_size = RTE_ETHER_MAX_LEN,
		.mq_mode = RTE_ETH_MQ_RX_RSS,
		.split_hdr_size = 0,
		.offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM,
       	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_key = rss_intel_key,
			.rss_key_len = 40,
			.rss_hf = RTE_ETH_RSS_UDP,
		},
	}
};
void pin_to_cpu(int core){
	int ret;
	cpu_set_t cpuset;
	pthread_t thread;

	thread = pthread_self();
	CPU_ZERO(&cpuset);
	CPU_SET(core, &cpuset);
	ret = pthread_setaffinity_np(thread, sizeof(cpu_set_t), &cpuset);
	if (ret != 0)
	    printf("Cannot pin thread\n");
}


#define TX_NUM 16
#define RX_NUM 4
/*
 *这个是简单的端口初始化 
 *我在这里简单的端口0 初始化了一个 接收队列和一个发送队列
 *并且打印了一条被初始化的端口的MAC地址信息
 */
static inline int
port_init(struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	const uint16_t rx_rings = RX_NUM, tx_rings = 0;
	int retval;
	uint16_t q;
	uint8_t socketid;
	struct rte_eth_dev_info dev_info;
	int ret = rte_eth_dev_info_get(0, &dev_info);
	port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
	if(port_conf.rx_adv_conf.rss_conf.rss_hf == 0)
		port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
	port_conf.rxmode.offloads &= dev_info.rx_offload_capa;
	port_conf.intr_conf.rxq = 1;
	/*配置端口0,给他分配一个接收队列和一个发送队列*/
	retval = rte_eth_dev_configure(0, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	/* Allocate and set up RX_NUM RX queues per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(0, q, RX_RING_SIZE,
				rte_eth_dev_socket_id(0), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(0, q, TX_RING_SIZE,
				rte_eth_dev_socket_id(0), NULL);
		if (retval < 0)
			return retval;
	}

	/* Start the Ethernet port. */
	retval = rte_eth_dev_start(0);
	if (retval < 0)
		return retval;

	return 0;
}
struct Request {
    uint64_t runNs;
    uint64_t genNs;
};
struct Response {
    uint64_t runNs;
    uint64_t genNs;
};
struct th_tx_arg{
	int pin_to_cpu;
	int pin_tx_id;
	struct rte_mempool *mp;
}tx_arg[TX_NUM];

 #define ntoh16(x)	(rte_be_to_cpu_16(x))
#define ntoh32(x)	(rte_be_to_cpu_32(x))
#define ntoh64(x)	(rte_be_to_cpu_64(x))

#define hton16(x)	(rte_cpu_to_be_16(x))
#define hton32(x)	(rte_cpu_to_be_32(x))
#define hton64(x)	(rte_cpu_to_be_64(x))

uint64_t tx_num[TX_NUM] = { 0 };
struct rte_mempool *mbuf_pool[TX_NUM];
struct rte_mempool *rx_mbuf_pool;
uint64_t res[10000000];
uint64_t getintcount = 0;
void sigint_handler(int sig) {
    for(int i = 0; i < getintcount; i++)
	    fprintf(fp, "%d\n", res[i]);
    syscall(SYS_exit_group, 0);

}
void *pt_send(void *tx_arg)
{
	struct th_tx_arg *arg = (struct th_tx_arg *)tx_arg;
	printf("%d %d\n",arg->pin_to_cpu, arg->pin_tx_id);
	pin_to_cpu(arg->pin_to_cpu);
	struct rte_ether_addr s_addr = {{0xe4,0x43,0x4b,0xe6,0xbc,0x00}};
	struct rte_ether_addr d_addr = {{0xe4,0x43,0x4b,0x76,0x27,0x96}};
	uint16_t ether_type =hton16( 0x0800); 	
	uint16_t           eth_type,udp_port;
	uint32_t           ip_addr;
	struct rte_udp_hdr    *udp_h;
	struct rte_ipv4_hdr   *ip_h;
	struct rte_ether_hdr *eth_hdr;


	//对每个buf ， 给他们添加包
	
	struct rte_mbuf * pkt[2];
	int i = 0;
	
	while(true)
	{
//		pkt[i] = rte_pktmbuf_alloc(arg->mp);
//		unsigned char *payload;
//		payload = (unsigned char *)((uint64_t)pkt[i] + 298);
//		struct Request *req = (struct Request *)payload;
//		req->genNs = getCurNs() + GENTIME;
//		req->runNs = 1000;
//		eth_hdr = rte_pktmbuf_mtod(pkt[i],struct rte_ether_hdr*);
//		eth_hdr->dst_addr = d_addr;
//		eth_hdr->src_addr = s_addr;
//		eth_hdr->ether_type = ether_type;
//		ip_h = (struct rte_ipv4_hdr*) (rte_pktmbuf_mtod(pkt[i],char*) + sizeof(struct rte_ether_hdr));
//		ip_h->type_of_service = 0;
//		ip_h->total_length = hton16(sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct Response));
//		ip_h->packet_id = 0;
//		ip_h->time_to_live = 64;
//		ip_h->next_proto_id = IPPROTO_UDP;
//		ip_h->hdr_checksum = 0;
//		ip_h->src_addr = hton32(0xc0aa0002);
//		ip_h->dst_addr = hton32(0xc0aa0001);
//		ip_h->version_ihl = 4;
//		ip_h->version_ihl = ip_h->version_ihl<<4 | sizeof(struct rte_ipv4_hdr) / 4; 
//		ip_h->hdr_checksum = chksum_internet((void *)ip_h, sizeof(struct rte_ipv4_hdr));
//		int pkt_size = sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_ether_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct Request);
//		pkt[i]->data_len = pkt_size;
//		pkt[i]->pkt_len = pkt_size;
//		udp_h = (struct rte_udp_hdr *) ((char *) ip_h + sizeof(struct rte_ipv4_hdr));
//		udp_h->src_port = hton16(5678);
//		udp_h->dst_port = hton16(1234);
//		udp_h->dgram_len = hton16(sizeof(struct Response) + sizeof(struct rte_udp_hdr));
//		udp_h->dgram_cksum = 0;
//
//		while(getCurNs() < req->genNs);
//		uint16_t nb_tx = rte_eth_tx_burst(0,arg->pin_tx_id,pkt,1);
//		tx_num[arg->pin_tx_id] += nb_tx;
//		rte_pktmbuf_free(pkt[i]);

	//	printf("tx_num %d\n", tx_num);
	}

//	uint16_t nb_tx = rte_eth_tx_burst(0,0,pkt,BURST_SIZE);
//	printf("发送成功%d个包\n",nb_tx);
	//发送完成，答应发送了多少个
	
	for(i=0;i<BURST_SIZE;i++)
		rte_pktmbuf_free(pkt[i]);
}

void *pt_recv(void *c)
{
	pin_to_cpu(29);
	struct rte_ether_hdr * eth_hdr;
	struct rte_udp_hdr    *udp_h;
	struct rte_ipv4_hdr   *ip_h;
	struct Response *resp;
	uint64_t num_rx = 0;
	struct rte_mbuf *pkt[BURST_SIZE];
	for(;;)
	{
//		int i;
//
//		//从接受队列中取出包
//		uint16_t nb_rx = rte_eth_rx_burst(0, 0,pkt,BURST_SIZE);
//		
//		if(nb_rx == 0)
//		{
//			//如果没有接受到就跳过
//			continue;
//		}
//		//打印信息
//		for(i=0;i<nb_rx;i++)
//		{
//			eth_hdr = rte_pktmbuf_mtod(pkt[i],struct rte_ether_hdr*);
//			num_rx++;
//			#ifdef DEBUG
//			printf("收到包 来自MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
//				   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 " : %d\n",
//				eth_hdr->src_addr.addr_bytes[0],eth_hdr->src_addr.addr_bytes[1],
//				eth_hdr->src_addr.addr_bytes[2],eth_hdr->src_addr.addr_bytes[3],
//				eth_hdr->src_addr.addr_bytes[4],eth_hdr->src_addr.addr_bytes[5], num_rx);
//			#endif
//			resp = (struct Response *)((char *)eth_hdr + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct rte_ether_hdr));
//			uint64_t time = getCurNs() - resp->genNs - 1000;
//			printf("%llu %llu %llu\n", resp->genNs, getCurNs, time);
//			rx_time[ rx_num ] = time;
////			printf("%llu now %lld gen %lld\n",time, getCurNs(), resp->genNs);
////			fwrite((void *)&time, sizeof(uint64_t), 1, fp);
//			rte_pktmbuf_free(pkt[i]);
//		}
		
	}
}

static void turn_on_off_intr(uint16_t portid, uint8_t queueid, int on)
{
	if(on)
		rte_eth_dev_rx_intr_enable(portid, queueid);
	else
		rte_eth_dev_rx_intr_disable(portid, queueid);
}
/**
 * force polling thread sleep until one-shot rx interrupt triggers
 * @param port_id
 *  Port id.
 * @param queue_id
 *  Rx queue id.
 * @return
 *  0 on success
 */
int in_sleep[16];
static int sleep_until_rx_interrupt(int num, int lcore)
{
	struct rte_epoll_event event[num];
	uint16_t port_id;
	uint8_t queue_id;
	int n, i;
	void *data;
	if(in_sleep[lcore] == 0){
		//printf("lcore %d sleeps until interrupt triggers\n", lcore);
		in_sleep[lcore] = 1;
	}
	n = rte_epoll_wait(RTE_EPOLL_PER_THREAD, event, num, -1);
	for(i = 0; i < n; i++)
	{
		data = event[i].epdata.data;
		port_id = ((uintptr_t)data) >> CHAR_BIT;
		queue_id = ((uintptr_t)data) &
			RTE_LEN2MASK(CHAR_BIT, uint8_t);
//		printf("lcore %d wake!\n", lcore);
		in_sleep[lcore] = 0;
	}
	return 0;
}

static int event_register(uint16_t portid, uint8_t queueid)
{
	uint32_t data;
	int ret;
	data = portid << CHAR_BIT | queueid;
	ret = rte_eth_dev_rx_intr_ctl_q(portid, queueid, RTE_EPOLL_PER_THREAD, RTE_INTR_EVENT_ADD, (void *)((uintptr_t)data));
	return ret;
}
#define DEBUG
static int main_intr_loop(__rte_unused void *dummy)
{
	unsigned int lcore_id;
	struct rte_ether_hdr * eth_hdr;
	struct rte_udp_hdr    *udp_h;
	struct rte_ipv4_hdr   *ip_h;
	struct Response *resp;
	uint64_t num_rx = 0;
	int intr_en = 0;
	struct rte_mbuf *pkt[BURST_SIZE];
	lcore_id = rte_lcore_id();
	printf("test rx on core %d\n", lcore_id);
	if(event_register(0, lcore_id) == 0)
		intr_en = 1;
	else printf("this NIC won't enanle interrupt\n");
	uint64_t cpuhz = rte_get_tsc_hz();
	printf("cpu tsc hz is %llu\n", cpuhz);
	int j = 0;
	for(;;)
	{
		turn_on_off_intr(0, lcore_id, 1);
		uint64_t t1 = rte_rdtsc();
		sleep_until_rx_interrupt(1, lcore_id);
		uint64_t t2 = rte_rdtsc();
		res[j++] = t2 - t1;
		getintcount++;
	//	printf("need overhead %llu\n", t2 - t1);
		turn_on_off_intr(0, lcore_id, 0);	
		int i;
		//从接受队列中取出包
		uint16_t nb_rx = rte_eth_rx_burst(0, lcore_id, pkt, BURST_SIZE);
		if(nb_rx == 0)
		{
			//如果没有接受到就跳过
			continue;
		}
		else
		{
			for(i = 0; i < nb_rx; i++)
				rte_pktmbuf_free(pkt[i]);
			continue;
		}
		//打印信息
		for(i=0;i<nb_rx;i++)
		{
			eth_hdr = rte_pktmbuf_mtod(pkt[i],struct rte_ether_hdr*);
			#ifdef DEBUG
			printf("收到包 来自MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
				   " %02" PRIx8 " %02" PRIx8 " %02"  PRIx8 " : %d \n",
				eth_hdr->src_addr.addr_bytes[0],eth_hdr->src_addr.addr_bytes[1],
				eth_hdr->src_addr.addr_bytes[2],eth_hdr->src_addr.addr_bytes[3],
				eth_hdr->src_addr.addr_bytes[4],eth_hdr->src_addr.addr_bytes[5], lcore_id);
			#endif
			resp = (struct Response *)((char *)eth_hdr + sizeof(struct rte_ipv4_hdr) + sizeof(struct rte_udp_hdr) + sizeof(struct rte_ether_hdr));
			rte_pktmbuf_free(pkt[i]);
		}
	}
	return 0;
}
int main(int argc, char *argv[])
{

	struct rte_eth_dev_info dev_info;
	uint32_t dev_rxq_num, dev_txq_num, nb_lcores;
	fp = fopen("data", "w+");
	if(fp == 0)
	{
		printf("open failed!\n");
		return -1;
	}
	pthread_t receiver, sender[TX_NUM];
	/*进行总的初始话*/
	int ret = rte_eal_init(argc, argv);
	if (ret < 0)
		rte_exit(EXIT_FAILURE, "initlize fail!");

	//I don't clearly know this two lines
	argc -= ret;
	argv += ret;

	/* Creates a new mempool in memory to hold the mbufs. */
	//分配内存池
	
	rx_mbuf_pool = rte_pktmbuf_pool_create("RX_POOL", NUM_MBUFS,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

	//init lcores we use.
	nb_lcores = rte_lcore_count();
	printf("set lcores num %d\n", nb_lcores);

	//get device info.
	ret = rte_eth_dev_info_get(0, &dev_info);
	if(ret != 0)
	{
		rte_exit(EXIT_FAILURE, "Error during getting device :%d info: %s\n", 0, strerror(-ret));
	}
	dev_rxq_num = dev_info.max_rx_queues;
	dev_txq_num = dev_info.max_tx_queues;
	printf("device port:%d, max_rx_queues %u, max_tx_queues %u\n", 0, dev_rxq_num, dev_txq_num);
	/* Initialize all ports. */
	//初始话端口设备 顺便给他们分配  队列
	if (port_init(rx_mbuf_pool) != 0)
		rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu8 "\n",
					0);
	signal(SIGINT, sigint_handler);	

	rte_eal_mp_remote_launch(main_intr_loop, NULL, CALL_MAIN);

	return 0;
}
