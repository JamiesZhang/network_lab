#include "stp.h"

#include "base.h"
#include "ether.h"
#include "utils.h"
#include "types.h"
#include "packet.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/types.h>
#include <unistd.h>

#include <pthread.h>
#include <signal.h>

stp_t *stp;

const u8 eth_stp_addr[] = { 0x01, 0x80, 0xC2, 0x00, 0x00, 0x01 };

static bool stp_is_root_switch(stp_t *stp)
{
	return stp->designated_root == stp->switch_id;
}

static bool stp_port_is_designated(stp_port_t *p)
{
	return p->designated_switch == p->stp->switch_id &&
		p->designated_port == p->port_id;
}

static const char *stp_port_state(stp_port_t *p)
{
	if (p->stp->root_port && \
			p->port_id == p->stp->root_port->port_id)
		return "ROOT";
	else if (p->designated_switch == p->stp->switch_id &&
		p->designated_port == p->port_id)
		return "DESIGNATED";
	else
		return "ALTERNATE";
}
static void stp_port_send_config(stp_port_t *p)
{
	// TODO: send config packet from this port
	struct stp_config configSend;
	configSend.header.proto_id = htons(STP_PROTOCOL_ID);
	configSend.header.version = STP_PROTOCOL_VERSION;
	configSend.header.msg_type = STP_TYPE_CONFIG;
	configSend.flags = 0;
	configSend.root_id = htonll(p->stp->designated_root);
	configSend.root_id = htonll(p->stp->designated_root);
	configSend.root_path_cost = htonl(p->stp->root_path_cost);
	configSend.switch_id = htonll(p->stp->switch_id);
	configSend.port_id = htons(p->port_id);
	configSend.msg_age = htons(0);
	configSend.max_age = htons(STP_MAX_AGE);
	configSend.hello_time = htons(STP_HELLO_TIME);
	configSend.fwd_delay = htons(STP_FWD_DELAY);
		
	int pLen = ETHER_HDR_SIZE + LLC_HDR_SIZE + sizeof(configSend);
	char *packet = malloc(pLen);

	struct ether_header *eth = (struct ether_header *)packet;
	memcpy(eth->ether_dhost, eth_stp_addr, 6);
	memcpy(eth->ether_shost, p->iface->mac, 6);
	eth->ether_type = htons(pLen - ETHER_HDR_SIZE);

	struct llc_header *llcH = (struct llc_header *)(packet + ETHER_HDR_SIZE);
	llcH->llc_dsap = LLC_DSAP_SNAP;
	llcH->llc_ssap = LLC_SSAP_SNAP;
	llcH->llc_cntl = LLC_CNTL_SNAP;

	memcpy(packet + ETHER_HDR_SIZE + LLC_HDR_SIZE, &configSend, sizeof(configSend));

	iface_send_packet(p->iface, packet, pLen);

	//fprintf(stdout, "TODO: send config packet.\n");
}

static void stp_send_config(stp_t *stp)
{
	int i;
	for (i = 0; i < stp->nports; i++) {
		stp_port_t *p = &stp->ports[i];
		if (stp_port_is_designated(p)) {
			stp_port_send_config(p);
		}
	}
}

static void stp_handle_hello_timeout(void *arg)
{
	// log(DEBUG, "hello timer expired, now = %llx.", time_tick_now());

	stp_t *stp = arg;
	stp_send_config(stp);
	stp_start_timer(&stp->hello_timer, time_tick_now());
}

static void stp_port_init(stp_port_t *p)
{
	stp_t *stp = p->stp;

	p->designated_root = stp->designated_root;
	p->designated_switch = stp->switch_id;
	p->designated_port = p->port_id;
	p->designated_cost = stp->root_path_cost;
}

void *stp_timer_routine(void *arg)
{
	while (true) {
		long long int now = time_tick_now();

		pthread_mutex_lock(&stp->lock);

		stp_timer_run_once(now);

		pthread_mutex_unlock(&stp->lock);

		usleep(100);
	}

	return NULL;
}

// true means the messages's piro is high
static bool comparePM(stp_port_t* p, struct stp_config* config)
{
	if(p->designated_root != ntohll(config->root_id)){
		return p->designated_root > ntohll(config->root_id);
	}
	if(p->designated_cost != ntohl(config->root_path_cost)){
		return p->designated_cost > ntohl(config->root_path_cost);
	}
	if(p->designated_switch != ntohll(config->switch_id)){
		return p->designated_switch > ntohll(config->switch_id);
	}
	if(p->designated_port != ntohs(config->port_id)){
		return p->designated_port > ntohs(config->port_id);
	}
	// shouldn't be here
	return false;
}

// true means p2's piro is high
static bool comparePP(stp_port_t* p1, stp_port_t* p2){
	if(p1->designated_root != p2->designated_root){
		return p1->designated_root > p2->designated_root;
	}
	if(p1->designated_cost != p2->designated_cost){
		return p1->designated_cost > p2->designated_cost;
	}
	if(p1->designated_switch != p2->designated_switch){
		return p1->designated_switch > p2->designated_switch;
	}
	if(p1->designated_port != p2->designated_port){
		return p1->designated_port > p2->designated_port;
	}
	// shouldn't be here
	return false;
}

static void stp_handle_config_packet(stp_t *stp, stp_port_t *p,
		struct stp_config *config)
{
	if(!comparePM(p,config)){
		stp_port_send_config(p);
	}	
	else{
		//replace
		p->designated_root = ntohll(config->root_id);
		p->designated_switch = ntohll(config->switch_id);
		p->designated_port = ntohs(config->port_id);
		p->designated_cost = ntohl(config->root_path_cost);
		
		//update node state
		int i;
		stp_port_t* newRP = NULL;
		for(i=0;i<stp->nports;i++){
			if(!stp_port_is_designated(&stp->ports[i])){
				bool isR = true;
				int j;
				for(j=0;j<stp->nports;j++){
					if(stp_port_is_designated(&stp->ports[j])||i==j){
						continue;
					}
					else{
						if(comparePP(&stp->ports[i],&stp->ports[j])){
							isR = false;
							break;
						}
					}
				}
				if(isR){
					newRP = &stp->ports[i];
				}
			}
		}
		if(newRP == NULL){
			stp->root_port = NULL;
			stp->designated_root = stp->switch_id;
			stp->root_path_cost = 0;
		}
		else{
			stp->root_port = newRP;
			stp->designated_root = newRP->designated_root;
			stp->root_path_cost = newRP->designated_cost + newRP->path_cost;
		}

		//update other port's config
		int j;
		for(j=0;j<stp->nports;j++){
			if(!stp_port_is_designated(&stp->ports[j])){
				//if(comparePP(&stp->ports[j],))
				//stp->ports[j].designated_switch = stp->switch_id;
				//stp->ports[j].designated_port = stp->ports[j].port_id;
			}
			else{
				stp->ports[j].designated_root = stp->designated_root;
				stp->ports[j].designated_cost = stp->root_path_cost;
			}
		}

		stp_stop_timer(&stp->hello_timer);

		//send from designated port
		for(j=0;j<stp->nports;j++){
			if(stp_port_is_designated(&stp->ports[j])){
				stp_port_send_config(&stp->ports[j]);
			}
		}	
	}
	//fprintf(stdout, "TODO: handle config packet here.\n");
}

static void *stp_dump_state(void *arg)
{
#define get_switch_id(switch_id) (int)(switch_id & 0xFFFF)
#define get_port_id(port_id) (int)(port_id & 0xFF)

	pthread_mutex_lock(&stp->lock);

	bool is_root = stp_is_root_switch(stp);
	if (is_root) {
		log(INFO, "this switch is root."); 
	}
	else {
		log(INFO, "non-root switch, designated root: %04x, root path cost: %d.", \
				get_switch_id(stp->designated_root), stp->root_path_cost);
	}
	int i;
	for (i = 0; i < stp->nports; i++) {
		stp_port_t *p = &stp->ports[i];
		log(INFO, "port id: %02d, role: %s.", get_port_id(p->port_id), \
				stp_port_state(p));
		log(INFO, "\tdesignated ->root: %04x, ->switch: %04x, " \
				"->port: %02d, ->cost: %d.", \
				get_switch_id(p->designated_root), \
				get_switch_id(p->designated_switch), \
				get_port_id(p->designated_port), \
				p->designated_cost);
	}

	pthread_mutex_unlock(&stp->lock);

	exit(0);
}

static void stp_handle_signal(int signal)
{
	if (signal == SIGTERM) {
		log(DEBUG, "received SIGTERM, terminate this program.");
		
		pthread_t pid;
		pthread_create(&pid, NULL, stp_dump_state, NULL);
	}
}

void stp_init(struct list_head *iface_list)
{
	stp = malloc(sizeof(*stp));

	// set switch ID
	u64 mac_addr = 0;
	iface_info_t *iface = list_entry(iface_list->next, iface_info_t, list);
	int i;
	for (i = 0; i < sizeof(iface->mac); i++) {
		mac_addr <<= 8;
		mac_addr += iface->mac[i];
	}
	stp->switch_id = mac_addr | ((u64) STP_BRIDGE_PRIORITY << 48);

	stp->designated_root = stp->switch_id;
	stp->root_path_cost = 0;
	stp->root_port = NULL;

	stp_init_timer(&stp->hello_timer, STP_HELLO_TIME, \
			stp_handle_hello_timeout, (void *)stp);

	stp_start_timer(&stp->hello_timer, time_tick_now());

	stp->nports = 0;
	list_for_each_entry(iface, iface_list, list) {
		stp_port_t *p = &stp->ports[stp->nports];

		p->stp = stp;
		p->port_id = (STP_PORT_PRIORITY << 8) | (stp->nports + 1);
		p->port_name = strdup(iface->name);
		p->iface = iface;
		p->path_cost = 1;

		stp_port_init(p);

		// store stp port in iface for efficient access
		iface->port = p;

		stp->nports += 1;
	}

	pthread_mutex_init(&stp->lock, NULL);
	pthread_create(&stp->timer_thread, NULL, stp_timer_routine, NULL);

	signal(SIGTERM, stp_handle_signal);
}

void stp_destroy()
{
	pthread_kill(stp->timer_thread, SIGKILL);
	int i;
	for (i = 0; i < stp->nports; i++) {
		stp_port_t *port = &stp->ports[i];
		port->iface->port = NULL;
		free(port->port_name);
	}

	free(stp);
}

void stp_port_handle_packet(stp_port_t *p, char *packet, int pkt_len)
{
	stp_t *stp = p->stp;

	pthread_mutex_lock(&stp->lock);
	
	// protocol insanity check is omitted
	struct stp_header *header = (struct stp_header *)(packet + ETHER_HDR_SIZE + LLC_HDR_SIZE);

	if (header->msg_type == STP_TYPE_CONFIG) {
		stp_handle_config_packet(stp, p, (struct stp_config *)header);
	}
	else if (header->msg_type == STP_TYPE_TCN) {
		log(ERROR, "TCN packet is not supported in this lab.");
	}
	else {
		log(ERROR, "received invalid STP packet.");
	}

	pthread_mutex_unlock(&stp->lock);
}
