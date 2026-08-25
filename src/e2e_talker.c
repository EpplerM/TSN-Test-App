// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <linux/net_tstamp.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdbool.h>
#include <byteswap.h>

#include "e2e_agent.h"
#include "e2e_clock.h"
#include "e2e_common.h"
#include "e2e_sock.h"
#include "e2e_talker.h"
#include "e2e_listener.h"

#include <sys/socket.h>
#include <netinet/ip.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <string.h>


#define try bool __HadError=false;
#define catch(x) ExitJmp:if(__HadError)
#define throw(x) {__HadError=true;goto ExitJmp;}

static int talker_state = TALKER_STATE_UNDEFINED;

struct e2e_talker_stats {
	int32_t cycle_time_exceeded;
	int32_t provision_time_exceeded;
	int32_t failed_sends;
};

struct e2e_talker {
	struct e2e_config *cfg;
	e2e_sock *sock;
  	uint32_t linkspeed;
  	uint32_t burst_packet_offset;
	struct e2e_talker_stats stats;
	unsigned char *buf;
	size_t buf_size;
	size_t buf_used;
	uint32_t packet_nr;
	uint32_t noResponsePacketCount;
	uint32_t burstpackets;
	struct timespec send_time;
	struct timespec base_send_time;
	struct {
		FILE *f;
		int32_t next_pattern;
	} training;
	enum e2e_agent_client_state e2e_agent_client_state;
	enum e2e_listener_state listener_state;
};

//helper functions to detect link speed, needed to compute offset between burst packets
struct interface {
    int     index;
    int     flags;      /* IFF_UP etc. */
    long    speed;      /* Mbps; -1 is unknown */
    int     duplex;     /* DUPLEX_FULL, DUPLEX_HALF, or unknown */
    char    name[IF_NAMESIZE + 1];
};

static int get_interface_common(const int fd, struct ifreq *const ifr, struct interface *const info)
{
    struct ethtool_cmd  cmd;
    int                 result;

    /* Interface flags. */
    if (ioctl(fd, SIOCGIFFLAGS, ifr) == -1)
        info->flags = 0;
    else
        info->flags = ifr->ifr_flags;

    ifr->ifr_data = (void *)&cmd;
    cmd.cmd = ETHTOOL_GSET; /* "Get settings" */
    if (ioctl(fd, SIOCETHTOOL, ifr) == -1) {
        /* Unknown */
        info->speed = -1L;
        info->duplex = DUPLEX_UNKNOWN;
    } else {
        info->speed = ethtool_cmd_speed(&cmd);
        info->duplex = cmd.duplex;
    }

    do {
        result = close(fd);
    } while (result == -1 && errno == EINTR);
    if (result == -1)
        return errno;

    return 0;
}

int get_interface_by_name(const char *const name, struct interface *const info)
{
    int             socketfd, result;
    struct ifreq    ifr;

    if (!name || !*name || !info)
        return errno = EINVAL;

    socketfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (socketfd == -1)
        return errno;

    strncpy(ifr.ifr_name, name, IF_NAMESIZE);
    if (ioctl(socketfd, SIOCGIFINDEX, &ifr) == -1) {
        do {
            result = close(socketfd);
        } while (result == -1 && errno == EINTR);
        return errno = ENOENT;
    }

    info->index = ifr.ifr_ifindex;
    strncpy(info->name, name, IF_NAMESIZE);
    info->name[IF_NAMESIZE] = '\0';

    return get_interface_common(socketfd, &ifr, info);
}

static void e2e_talker_get_linkspeed(struct e2e_talker *t)
{
  struct interface iface;
  if (get_interface_by_name(t->cfg->iface, &iface) != 0) {
    perror("failed to detect linkspeed");
  }
  e2e_agent_log_info(NULL, true, ESC_INFO, "detected link speed: %ld Mbps\n", iface.speed);
  e2e_agent_log_report("detected link speed: %ld Mbps\n", iface.speed);
  t->linkspeed=iface.speed;    
}

static void e2e_talker_compute_burst_packet_offset(struct e2e_talker *t)
{
  uint32_t net_packet_size = 0;
  uint32_t interframegap = 0;
  //minimum transmit IFG in ns according to https://en.wikipedia.org/wiki/Interpacket_gap, rounded up to full ns
 	if(t->cfg->interframe_gap == 0)
  {
    switch (t->linkspeed) {
  	case 100:
      interframegap = 960;
      break; 
  	case 1000:
      interframegap = 96;
      break; 	
  	case 2500:
      interframegap = 39;
      break; 	  
  	case 5000:
      interframegap = 20;
      break; 	
  	case 10000:
      interframegap = 10;
      break; 	  
  	case 25000:
      interframegap = 4;
      break;
  	case 40000:
      interframegap = 3;
      break;
  	case 100000:
      interframegap = 1;
      break;
    default:
      perror("unkown interframe gap length for detected linkspeed");
    }
  } else {
    interframegap = t->cfg->interframe_gap;
  }

  if (t->cfg->use_udp_socket)
		{
			net_packet_size = t->cfg->tx_packet_size + LEN_UDP_VLAN_HDR;
		}
		else
		{
			net_packet_size = t->cfg->tx_packet_size + LEN_ETH_VLAN_HDR;
		}

  t->burst_packet_offset = ((net_packet_size * 8000) / t->linkspeed) + interframegap; // *8000: divided by 1000000 and multiplied by 1000000000 and 8 to get nsec from Mbit/s
  e2e_agent_log_info(NULL, true, ESC_INFO, "burst packet offset: %d nsec, computed for %d Bytes Packet size and %d ns IFG\n", t->burst_packet_offset, net_packet_size, interframegap);
  e2e_agent_log_report("burst packet offset: %d nsec, computed for %d Bytes Packet size and %d ns IFG\n", t->burst_packet_offset, net_packet_size, interframegap);
}

// refresh t->e2e_agent_client_state; t->listener_state
static void e2e_talker_get_states(struct e2e_talker *t) 
{
	if (t->cfg->agent_mode) {
		e2e_agent_client_get_state(&t->e2e_agent_client_state);
	}
	else {
		t->e2e_agent_client_state = E2E_CLIENT_STATE_UNDEFINED;
	}
	if (t->cfg->role == TALK_AND_LISTEN || t->cfg->role == LISTEN_AND_TALK){
		e2e_listener_get_state(&t->listener_state);
	}
	else {
		t->listener_state = LISTENER_STATE_UNDEFINED;
	}
}

static int e2e_talker_seed_training_pattern(FILE **f, const char *path)
{
	if (!f || !path)
		return -EINVAL;

	FILE *fp = fopen(path, "r");
	if (!fp) {
		e2e_agent_log_warn(NULL, true, 
			"talker: Failed to open training pattern file (%s)",
			path);
		return -EINVAL;
	}

	*f = fp;
	return fseek(fp, 0, SEEK_SET);
}

static int e2e_talker_init_sendbuf(struct e2e_talker *t)
{
	struct ether_header *eh = (struct ether_header *)t->buf;
	struct e2e_config *cfg = t->cfg;
	unsigned char *buf = t->buf;
	struct ifreq if_mac = { 0 };
	int tx_len = 0;
	int ret;

	if (cfg->use_udp_socket)
	{
		/* vlan id and vlan prio must be set via interface */

		/* adjust tx_len to cope with missing ethernet header in data */
		tx_len = cfg->data_offset_send; 
	}
	else
	{
		strncpy(if_mac.ifr_name, cfg->iface, IFNAMSIZ - 1);

		/* Get the MAC address of the interface to send on */
		ret = e2e_sock_ioctl(t->sock, SIOCGIFHWADDR, &if_mac);
		if (ret == -1) {
			perror("SIOCGIFHWADDR");
			return -errno;
		}

		/* destination MAC */
		memcpy(eh->ether_dhost, t->cfg->dest_mac, sizeof(t->cfg->dest_mac));

		/* source MAC */
		memcpy(eh->ether_shost, &if_mac.ifr_hwaddr.sa_data,
			sizeof(eh->ether_shost));

		/* Ethertype field: VLAN */
		eh->ether_type = htons(ETHERTYPE_VLAN);
		tx_len += sizeof(struct ether_header);

		/* VLAN Tag */
		buf[tx_len++] = (cfg->vlan_prio << 5) + ((cfg->vlan_id & 0x0F00) >> 8);
		buf[tx_len++] = cfg->vlan_id & 0xFF;

		/* Ethertype field, >=1500 means protocol identifier */
		uint32_t *pkg;
		pkg = (uint32_t *)(&t->buf[tx_len++]);
		*pkg = cfg->ethertype;
		tx_len++;
		//buf[tx_len++] = 0xDA;
		//buf[tx_len++] = 0xDA;
	}

	/* DATA */
	buf[tx_len++] = 0xca;
	buf[tx_len++] = 0xfe;
	buf[tx_len++] = 0xc0;
	buf[tx_len++] = 0xde;
	for (int i = 0; i < cfg->tx_packet_size - 4; i++)
		buf[tx_len++] = 0xFF;
	
	buf[cfg->data_offset_send + 18] = VERSION_COUNTER;

	// add grandmaster identity
	for (int i = 0; i < strlen(cfg->grandmaster_identity); i++)
		buf[cfg->data_offset_send + 20 + i] = cfg->grandmaster_identity[i];

	/* return at least packet size including grandmaster identity */
	if (tx_len > (cfg->data_offset_send + 20 + strlen(cfg->grandmaster_identity)))
		return tx_len;
	else
		return cfg->data_offset_send + 20 + strlen(cfg->grandmaster_identity);
}

static void e2e_talker_print_system_time(const struct e2e_talker *t)
{
	struct timespec curr_time;
	struct timespec rt_time;
	struct tm *day_time;

	// AZ: print system time
	clock_gettime(CLOCK_REALTIME, &rt_time);
	clock_gettime(t->cfg->clkid, &curr_time);
	day_time = localtime(&rt_time.tv_sec);
	e2e_info(ESC_TALKER, "CLK_REALTIME: %02d:%02d:%02d, %'11ld", day_time->tm_hour,
		 day_time->tm_min, day_time->tm_sec, rt_time.tv_nsec);
	day_time = localtime(&curr_time.tv_sec);
	e2e_info(ESC_TALKER, "system time: %02d:%02d:%02d, %'11ld", day_time->tm_hour,
		 day_time->tm_min, day_time->tm_sec, curr_time.tv_nsec);
}

static int e2e_talker_set_txtime(const struct e2e_talker *t)
{
	int receive_errors = SOF_TXTIME_REPORT_ERRORS;
	struct sock_txtime sk_txtime;
	int use_deadline_mode = 0;
	int ret;

	if (t->cfg->send_mode != ETF)
		return 0;

	/* Set SO_TXTIME socket option */
	sk_txtime.clockid = t->cfg->clkid;
	sk_txtime.flags = (use_deadline_mode | receive_errors);

	ret = e2e_sock_setopt(t->sock, SOL_SOCKET, SO_TXTIME, &sk_txtime,
			      sizeof(sk_txtime));
	if (ret)
		e2e_agent_log_warn(NULL, true, "setsockopt SO_TXTIME failed: %m");

	return ret;
}

static int e2e_talker_check_provision_time(struct e2e_talker *t)
{
#ifdef ENABLE_PROVISIONTIME
	struct timespec phc;
	struct tm *local_time;
	long ahead;
	long sec;

	phc = e2e_clock_get(t->cfg);

	ahead = (t->send_time.tv_sec - phc.tv_sec) * NSEC_PER_SEC;
	ahead += (t->send_time.tv_nsec - phc.tv_nsec);

	if (t->cfg->verbose) {
		sec = phc.tv_sec;
		local_time = localtime(&sec);
		e2e_info(ESC_TALKER, "             provision time: %02d:%02d:%02d, %'11ld;",
			 local_time->tm_hour, local_time->tm_min,
			 local_time->tm_sec, phc.tv_nsec);
		if (ahead < 0) {
			e2e_info(ESC_TALKER, " ahead %'11ld", ahead);
			t->stats.provision_time_exceeded++;
		} else {
			e2e_info(ESC_TALKER, " ahead %'11ld", ahead);
		}
	}

// removed warning to avoid stdout overload
//	if (ahead > t->cfg->cycle_time && ahead > 0) {
//		e2e_agent_log_warn(
//			NULL,
//			true,
//			"provision time earlier than start of previous cycle, please check configuration of cycle time and provision time");
		//return -1; Only warn and don't stop program.
//	}
#endif
	return 0;
}

static void e2e_talker_announce_start(struct e2e_talker *t)
{
	struct timespec ts;
	struct tm *lt;
	long sec;

	ts = e2e_clock_get(t->cfg);

	if (t->cfg->verbose) {
		// AZ: print wakeup time
		sec = ts.tv_sec;
		lt = localtime(&sec);
		e2e_info(ESC_TALKER, "wakeup time: %02d:%02d:%02d, %'11ld", lt->tm_hour,
			 lt->tm_min, lt->tm_sec, ts.tv_nsec);
	}

	if (t->cfg->clkid == CLOCK_TAI)
		e2e_info(ESC_INFO, "using CLOCK_TAI\n");
	else if (t->cfg->clkid == -37)
		e2e_info(ESC_INFO, 
		"using PTP CLOCK directly from NIC. Erez's kernel patch required to use this clock!\n");	
	else
		e2e_crit(
			"using clock with id: %d. (0 = CLOCK_REALTIME) WARNING, THIS CLOCK IS NOT RECOMMENDED for use with ETF or TAPRIO!",
			t->cfg->clkid);
}

static int e2e_talker_sleep_until_provisioning_time(struct e2e_talker *t)
{
#ifndef ENABLE_CLASSIC_SLEEP
	int ret;
	struct timespec prov_time;

	// sleep until point in time (more precise, but not supported in
	// all platforms)
	prov_time.tv_sec = t->send_time.tv_sec;
	prov_time.tv_nsec = t->send_time.tv_nsec - t->cfg->provision_time;

	if (prov_time.tv_nsec < 0) {
		// if tv_nsec accumulate to less than zero, add a second
		// to tv_nsec and substract the second from tv_sec
		prov_time.tv_sec = prov_time.tv_sec - 1;
		prov_time.tv_nsec = prov_time.tv_nsec + NSEC_PER_SEC;
	}

#ifdef ENABLE_DEBUG_SEND
	struct timespec now = e2e_clock_get(t->cfg);
	e2e_info(ESC_INFO, "sleeping until %d.%d, now is %d.%d\n", prov_time.tv_sec,
	        prov_time.tv_nsec, now.tv_sec, now.tv_nsec);
#endif
	ret = clock_nanosleep(t->cfg->clkid, TIMER_ABSTIME, &prov_time, NULL);
	if (!ret)
		return 0;

	if (ret == EINVAL) {
		t->stats.cycle_time_exceeded++;
		e2e_agent_log_warn(
			NULL,
			"too late to sleep for packet %d, wakeup time was %d.%d\n",
			t->packet_nr, prov_time.tv_sec, prov_time.tv_nsec);
	} else {
		e2e_agent_log_warn(
			NULL,
			"nanosleep error while sleeping for packet nr %d!\n",
			t->packet_nr);
	}

	return ret;
#else
	struct timespec last_phc;
	struct timespec sleep_time;
	// sleep for time period (less precise, but supported in all
	// platforms)
	last_phc = e2e_clock_get(t->cfg);
	// sleep_time.tv_nsec = send_time - provisionOffset - last_phc;
	sleep_time.tv_sec = t->send_time.tv_sec - last_phc.tv_sec;
	sleep_time.tv_nsec = t->send_time.tv_nsec - t->cfg->provision_time -
			     last_phc.tv_nsec;

	if (sleep_time.tv_nsec < 0) {
		// if tv_nsec accumulate to less than zero, add a second
		// to tv_nsec and substract the second from tv_sec
		sleep_time.tv_sec = sleep_time.tv_sec - 1;
		sleep_time.tv_nsec = sleep_time.tv_nsec + NSEC_PER_SEC;
	}

	if (sleep_time.tv_nsec > 0) {
#ifdef ENABLE_DEBUG_SEND
		e2e_info(ESC_INFO, "sleeping for %'ld\n", sleep_time.tv_nsec);
#endif
		nanosleep(&sleep_time, NULL);
	} else {
		t->stats.cycle_time_exceeded++;
		e2e_agent_log_warn(
			NULL,
			true, 
			"too late to sleep for packet %d. rest sleep time %ld\n",
			t->packet_nr, sleep_time.tv_nsec);
	}

	return 0;
#endif
}

static void e2e_talker_wait_for_first_tx_time(struct e2e_talker *t)
{
	struct timespec last_phc;
	struct timespec sleep_time;

	// compute first send time
	last_phc = e2e_clock_get(t->cfg);
	// use start time as random seed
	srandom(last_phc.tv_nsec);

	switch (t->cfg->role) {
	case TALK_AND_LISTEN:
	case TALK:
		// initial talker waits for the next second to start
		t->base_send_time.tv_nsec = t->cfg->transmission_offset; 
		t->base_send_time.tv_sec = last_phc.tv_sec + 1;		
		break;
	case LISTEN_AND_TALK:
	default:
		t->base_send_time.tv_sec = last_phc.tv_sec;
		t->base_send_time.tv_nsec =
			last_phc.tv_nsec -
			(last_phc.tv_nsec % t->cfg->cycle_time) +
			t->cfg->transmission_offset + t->cfg->cycle_time;
		if (t->base_send_time.tv_nsec > NSEC_PER_SEC) {
			/* if tv_nsec accumulate to more than a second,
			reduce tv_nsec by a second and add the second
			to tv_sec */
			t->base_send_time.tv_sec = t->base_send_time.tv_sec + 1;
			t->base_send_time.tv_nsec =
				t->base_send_time.tv_nsec - NSEC_PER_SEC;
		}
		if (t->base_send_time.tv_nsec < 0) {
			/* if tv_nsec accumulate to less than zero, add
			a second to tv_nsec and substract the second
			from tv_sec */
			t->base_send_time.tv_sec = t->base_send_time.tv_sec - 1;
			t->base_send_time.tv_nsec =
				t->base_send_time.tv_nsec + NSEC_PER_SEC;
		}
	}

	/* sleep_time is the time to sleep till the wakeup time before
	the first test packet is sent at this place */
	sleep_time.tv_sec = t->base_send_time.tv_sec - last_phc.tv_sec;
	sleep_time.tv_nsec = t->base_send_time.tv_nsec - last_phc.tv_nsec -
			     (t->cfg->cycle_time);

	if (sleep_time.tv_nsec < 0) {
		/* if tv_nsec accumulate to less than zero, add a second
		to tv_nsec and substract the second from tv_sec */
		sleep_time.tv_sec = sleep_time.tv_sec - 1;
		sleep_time.tv_nsec = sleep_time.tv_nsec + NSEC_PER_SEC;
	}

	if (sleep_time.tv_nsec > 0) {
#ifdef DEBUG_SEND
		e2e_info(ESC_INFO, "sleeping %'ld ns\n\033[0m", sleep_time.tv_nsec);
#endif
		nanosleep(&sleep_time, NULL);
	}
}

static inline bool e2e_talker_continue(struct e2e_talker *t)
{
	bool ret = true;

	if (t->cfg->endless_number_of_packets)
		return ret;

	// get e2e_client state and listener state once per update interval
	if (t->packet_nr % t->cfg->update_interval == 0)
	{
		e2e_talker_get_states(t);
		// check conditions for premature termination of talker
		if (t->cfg->role == LISTEN_AND_TALK || t->cfg->role == TALK_AND_LISTEN)
		{	
			switch (t->listener_state)
			{
			case LISTENER_STATE_TIMEOUT:
			case LISTENER_STATE_RX_FAILED:
			case LISTENER_STATE_STANDBY:
				return(0);
			default:
				break;
			}
		}

	}
	ret = (t->e2e_agent_client_state != E2E_CLIENT_STATE_STOPPED 
		&& t->packet_nr <= t->cfg->number_of_packets);
	return (ret);
}

static void e2e_talker_generate_trainging_data(struct e2e_talker *t)
{
	int ret;

	/* used to alternate send window and missedPacketRate in a predefined
	pattern in order to generate training data for network AI pattern is
	specified in a test file */
	if (t->packet_nr != t->training.next_pattern)
		return;

	ret = fscanf(t->training.f, "%d %d %d", &t->training.next_pattern,
		     &t->cfg->send_window, &t->cfg->missed_packet_rate);
	if (ret != 3)
		e2e_warn("talker: Unable to read training data");

	if (t->training.next_pattern > t->packet_nr) {
		e2e_info(
			ESC_TALKER,
			"new training pattern starting from packet %d, valid through packet %d: send window %d, missed packet rate %d",
			t->packet_nr, t->training.next_pattern - 1,
			t->cfg->send_window, t->cfg->missed_packet_rate);
	} else {
		e2e_info(
			ESC_TALKER,
			"no new training pattern available. Send window and missed packet rate set to zero.");
		t->cfg->send_window = 0;
		t->cfg->missed_packet_rate = 0;
	}
}

static void e2e_talker_compute_base_send_time(struct e2e_talker *t)
{
	// compute base send time
  if (t->cfg->burst_size == t->burstpackets) {
    // one cycle time between bursts or single packets
  	t->base_send_time.tv_nsec += t->cfg->cycle_time;
  	if (t->base_send_time.tv_nsec >= NSEC_PER_SEC) {
  		/* if tv_nsec accumulate to more than a second, reduce
  		 tv_nsec by a second and add the second to tv_sec */
  		t->base_send_time.tv_sec = t->base_send_time.tv_sec + 1;
  		t->base_send_time.tv_nsec =
  			t->base_send_time.tv_nsec - NSEC_PER_SEC;
  	}
  
  	t->send_time.tv_sec = t->base_send_time.tv_sec;
  	t->send_time.tv_nsec = t->base_send_time.tv_nsec;
  } else {
    // within a burst shift the send time by one packet length including IFG.
    t->send_time.tv_nsec += t->burst_packet_offset;
   	if (t->send_time.tv_nsec >= NSEC_PER_SEC) {
  		/* if tv_nsec accumulate to more than a second, reduce
  		 tv_nsec by a second and add the second to tv_sec */
  		t->send_time.tv_sec = t->send_time.tv_sec + 1;
  		t->send_time.tv_nsec = t->send_time.tv_nsec - NSEC_PER_SEC;
    }
  }

	if (t->cfg->send_window == 0)
		return;

	// send window is configured: add jitter to base send time
	t->send_time.tv_nsec += random() * t->cfg->send_window / RAND_MAX;

	if (t->send_time.tv_nsec >= NSEC_PER_SEC) {
		/* if tv_nsec accumulate to more than a second,
		reduce tv_nsec by a second and add the second
		to tv_sec */
		t->send_time.tv_sec = t->send_time.tv_sec + 1;
		t->send_time.tv_nsec = t->send_time.tv_nsec - NSEC_PER_SEC;
	}
}

// signal agent client to send buffer content to client and empty buffer
static void e2e_talker_signal_agent_client(const struct e2e_talker *t)
{	

	if (t->cfg->agent_mode)
	{
		int ret;
		
		bool std_out = true;
		if (t->cfg->quiet_quiet_mode){
			std_out =false;
		};
		ret = e2e_config_lock_signal(&t->cfg->locks.agent_lock);
		if (ret)
			e2e_agent_log_warn(NULL, std_out, "agent: Failed to signal agent client");
		
		// wait for sending completed
		ret = e2e_config_lock_wait(&t->cfg->locks.agent_lock);
		if (ret)
			e2e_agent_log_warn(NULL, std_out, "talker: Failed to wait for agent client");
	}
}

static void e2e_talker_status_update(struct e2e_talker *t)
{
	time_t secs;
	struct tm *lt;

	secs = t->send_time.tv_sec;
	lt = localtime(&secs);

	// print send time
	if (t->cfg->verbose) {
		pthread_mutex_lock(&t->cfg->locks.agent_lock.mutex);
		e2e_info(
			ESC_TALKER, 
			"Packet %'10d send time: %02d:%02d:%02d, %'2ld nsec",
			 t->packet_nr, lt->tm_hour, lt->tm_min, lt->tm_sec,
			 t->send_time.tv_nsec);
		pthread_mutex_unlock(&t->cfg->locks.agent_lock.mutex);
	} 
	else 
	{
		if ((t->packet_nr % t->cfg->update_interval) == 0) 
		{	
			if (t->cfg->role == TALK_AND_LISTEN) {
				printf("\n");
			}
			pthread_mutex_lock(&t->cfg->locks.agent_lock.mutex);
			e2e_agent_log_info(
				NULL,
				true, 	// to std_out
				ESC_TALKER,
				"Packet %'10d send time: %02d:%02d:%02d, %'2ld nsec",
				t->packet_nr, lt->tm_hour, lt->tm_min,
				lt->tm_sec, t->send_time.tv_nsec);
			pthread_mutex_unlock(&t->cfg->locks.agent_lock.mutex);
			if (t->cfg->agent_mode) {
				// signal agent client to send data to client
				e2e_talker_signal_agent_client(t);
			}
		}
	}
}

static void e2e_talker_write_packet_nr(struct e2e_talker *t)
{
	// alternating write number of packets to send and cycle time
	// into packets
	const struct e2e_config *c = t->cfg;
	uint32_t *packet_cnt = (uint32_t *)(&t->buf[t->cfg->data_offset_send]);
	uint32_t *packet_num = (uint32_t *)(&t->buf[t->cfg->data_offset_send + 4]);

	*packet_cnt = (t->packet_nr % 2) ? c->cycle_time : c->number_of_packets;
	*packet_num = t->packet_nr;
}

static void e2e_talker_write_ts_to_packet(struct e2e_talker *t,
					  const struct timespec *ts)
{
	int32_t *addr;

	addr = (int32_t *)(&t->buf[t->cfg->data_offset_send + 8]);
	*addr = ts->tv_sec;
	addr = (int32_t *)(&t->buf[t->cfg->data_offset_send + 12]);
	*addr = ts->tv_nsec;
}

static void e2e_talker_write_origin_ts(struct e2e_talker *t)
{
	uint32_t *pkg;
	unsigned idx;

	// responder: return origin time stamp and receiver time stamp
	if (t->cfg->role != LISTEN_AND_TALK)
		return;

	idx = t->packet_nr % t->cfg->reported_ts_sz;

	pkg = (uint32_t *)(&t->buf[t->cfg->data_offset_send + 16]);
	*pkg = t->cfg->reported_ts[idx].origin_sec;
	pkg = (uint32_t *)(&t->buf[t->cfg->data_offset_send + 20]);
	*pkg = t->cfg->reported_ts[idx].origin_nsec;
	pkg = (uint32_t *)(&t->buf[t->cfg->data_offset_send + 24]);
	*pkg = t->cfg->reported_ts[idx].recv_sec;
	pkg = (uint32_t *)(&t->buf[t->cfg->data_offset_send + 28]);
	*pkg = t->cfg->reported_ts[idx].recv_nsec;
	// set timestamps to zero after they have been used once, so that they aren't used again if a packet was lost
	t->cfg->reported_ts[idx].origin_sec = 0;
	t->cfg->reported_ts[idx].origin_nsec = 0;
	t->cfg->reported_ts[idx].recv_sec = 0;
	t->cfg->reported_ts[idx].recv_nsec = 0;
}

static int e2e_talker_prepare_payload(void *ctx, const struct timespec *ts)
{
	struct e2e_talker *t = (struct e2e_talker *)ctx;

	if (!ctx || !ts)
		return -EINVAL;

	e2e_talker_write_packet_nr(t);
	e2e_talker_write_ts_to_packet(t, ts);
	e2e_talker_write_origin_ts(t);

	return 0;
}

static int e2e_talker_send_packet(struct e2e_talker *t)
{
	int ret;

	if (t->cfg->missed_packet_rate > 0)
		if (random() * t->cfg->update_interval / RAND_MAX < t->cfg->missed_packet_rate) {
			e2e_info(ESC_TALKER,
				"Packet %'d not sent due to planned missed packet rate\n",
				t->packet_nr);
			e2e_agent_log_report("Packet %'d not sent due to planned missed packet rate\n",	t->packet_nr);
			return 0;
		}

	// reply mode: do not send a packet, if no packet was received.
	if(t->cfg->reply_mode && t->cfg->packet_received == false && t->packet_nr < (t->cfg->number_of_packets-3)) {
		// to avoid virtual packet loss due to not sending of last packet in reply mode, we always send the last 3 packets
		t->noResponsePacketCount+=1;
		if(t->cfg->reply_retry == 0 || (t->cfg->reply_retry > 0 && t->noResponsePacketCount % t->cfg->reply_retry > 0))
			return 0; //return without sending the packet
		else
			e2e_agent_log_report("sending packet number %d due to configured reply-retry rate %d.\n", t->packet_nr, t->cfg->reply_retry);	
	} else if (t->noResponsePacketCount > 0) {
		e2e_agent_log_warn(NULL, true, "%d response packet(s) before packet number %d not sent.\n", t->noResponsePacketCount, t->packet_nr);
		t->noResponsePacketCount = 0;
	}
	t->cfg->packet_received=false;

	ret = e2e_sock_send(t->sock, t->buf, t->cfg->use_udp_socket ? t->cfg->tx_packet_size : (t->cfg->tx_packet_size + LEN_ETH_VLAN_HDR), &t->send_time,
			    t->packet_nr);

	if (!ret)
		return ret;

	e2e_agent_log_warn(NULL, true, "failed to send packet %d, reason: %s",
			   t->packet_nr, strerror(-ret));
	t->stats.failed_sends++;

	return ret;
}

static int e2e_talker_send_loop(struct e2e_talker *t)
{
	int ret = 0;
	// bool std_out = true;
	
	e2e_talker_announce_start(t);

	// wait for listener received test packets of first update intervall//
	if (t->cfg->role == LISTEN_AND_TALK) 

	{
		e2e_config_lock_wait(&t->cfg->locks.dst_mac_lock);
		/* copy received destination MAC to ethernet header of test packet */
		memcpy(&t->buf[0], t->cfg->dest_mac, sizeof(t->cfg->dest_mac));
	}

	e2e_info(ESC_TALKER, "Talker started, sending packets ...\n\n");

	e2e_talker_wait_for_first_tx_time(t);

	// send loop
	for (t->packet_nr = 1; e2e_talker_continue(t); ) {
		if (t->cfg->training_filename)
			e2e_talker_generate_trainging_data(t);

    t->burstpackets = t->cfg->burst_size;
    while(t->burstpackets > 0) {
      
  		e2e_talker_compute_base_send_time(t);
  		e2e_talker_status_update(t);
  
  		if (t->cfg->burst_size == t->burstpackets) {
        // waiting for the send time is only required before the first packet of a burst
        	ret = e2e_talker_sleep_until_provisioning_time(t);
    		if (ret)
    			break;
    
    		ret = e2e_talker_check_provision_time(t);
    		if (ret)
    			break;
      }
      ret = e2e_talker_send_packet(t);
  
  		if (unlikely(((t->stats.failed_sends % 10) == 0) &&
  			     (t->packet_nr > 0) &&
  			     (t->packet_nr < t->stats.failed_sends))) {
  			e2e_agent_log_misc(
  				NULL,
  				"Ten of ten attempts to send a packet have failed. Please check configuration.");
  			break;
  		}
  
  		switch (ret) {
    		case -ENETDOWN:
    		case -EOPNOTSUPP:
    			/* Unlikely that we recover from those errors => stop */
    			goto out;
    		default:
    			break;
  		}
      t->burstpackets--;
      t->packet_nr++;
    }
	}

	// handle events triggering premature exit of talker loop  
	if (t->e2e_agent_client_state == E2E_CLIENT_STATE_STOPPED) {
		e2e_agent_log_info(
			NULL,
			true, // to std_out,
			ESC_TALKER,
			"\nSending packets stopped by request...");
	}
	else if (t->listener_state == LISTENER_STATE_STANDBY) {
			e2e_agent_log_info(
			NULL,
			true, // to std_out
			ESC_TALKER,
			"\nSending packets stopped after last sent packet was received...");
	}
	else if (t->listener_state == LISTENER_STATE_STANDBY) {
			e2e_agent_log_info(
			NULL,
			true, // to std_out
			ESC_TALKER,
			"\nSending packets stopped after last sent packet was received...");
	}
	else if (t->listener_state == LISTENER_STATE_TIMEOUT) {
			e2e_agent_log_crit(
			NULL,
			true, // to std_out
			"\nSending packets stopped after listener timed out...");
	}

	else if (t->listener_state == LISTENER_STATE_RX_FAILED) {
			e2e_agent_log_crit(
			NULL,
			true, // to std_out
			"\nSending packets stopped after listener failed...");
	}

out:
	return ret;
}

static void e2e_talker_write_agent_log(const struct e2e_talker *t)
{
	const struct e2e_sock_stats *ss = e2e_sock_get_stats(t->sock);
	const struct e2e_talker_stats *s = &t->stats;
	int ret;
	
	// set talker_state after finishing talker loop
	if (t->e2e_agent_client_state == E2E_CLIENT_STATE_STOPPED) {
		
		e2e_agent_log_info(NULL,
					true, // to std_out
					ESC_TALKER,
					"Talker stopped by request...\n");
		
		talker_state = TALKER_STATE_STOPPED;
	} 
	else if (t->listener_state == LISTENER_STATE_TIMEOUT || t->listener_state == LISTENER_STATE_RX_FAILED)
	{	
		e2e_agent_log_crit(NULL,
					true, // to std_out
					"Talker stopped ...\n");
		
		talker_state = TALKER_STATE_STOPPED;		
	}
	else 
	{	
		e2e_agent_log_info(NULL,
					true, // to std_out
					ESC_TALKER, "Talker: Job done.");

		talker_state = TALKER_STATE_DONE;
	}

	// talker job finished -> signal e2e_agent_client
	ret = e2e_config_lock_signal(&t->cfg->locks.talker_lock);
	if (ret)
		e2e_agent_log_warn(NULL, true, "agent: Failed to signal e2e_agent_client");

	if (s->cycle_time_exceeded > 0 || s->provision_time_exceeded > 0) 
	{
		e2e_agent_log_warn(
			NULL,
			true, 
			"cycle time exceeded %d times, provision time exceeded %d times",
			s->cycle_time_exceeded, s->provision_time_exceeded);
	}

	if (s->failed_sends > 0) 
	{
		e2e_agent_log_warn(
			NULL,
			true, 
			"e2e_sock_send failed %d times, %d times due to no buffer space available",
			s->failed_sends, ss->no_buffer_space);
	}

	if (!ss)
		return;

	if (ss->invalid_param > 0 || ss->tx_time_missed > 0) 
	{
		e2e_agent_log_warn(
			NULL,
			true, 
			"%d packets dropped, %d due to invalid parameters, %d due to missed TX time",
			(ss->invalid_param + ss->tx_time_missed),
			ss->invalid_param, ss->tx_time_missed);
	}
}

static void *e2e_talker_cleanup(e2e_thread *thread, void *arg)
{
	(void)arg;
	struct e2e_talker *t;

	int ret;

	bool std_out = true;

	e2e_thread_get_ctx(thread, (void **)&t);

	if (t->cfg->quiet_quiet_mode){
		std_out =false;
	};

	// final signalling 
	ret = e2e_config_lock_signal(&t->cfg->locks.agent_lock);
	if (ret)
		e2e_agent_log_warn(NULL, std_out, "agent: Failed to signal agent client");

	e2e_thread_get_ctx(thread, (void **)&t);
	e2e_sock_destroy(t->sock);

	if (t->training.f)
		fclose(t->training.f);

	free(t);

	return NULL;
}

static void *e2e_talker_entry(e2e_thread *thread, void *arg)
{
	struct e2e_config *cfg = (struct e2e_config *)arg;
	struct e2e_sock_cfg sock_cfg = {
		.type = cfg->use_udp_socket ? E2E_SOCK_UDP : cfg->use_xdp_socket ? E2E_SOCK_XDP : E2E_SOCK_RAW,
		.raw.protocol = IPPROTO_RAW,
		.xdp.rx_queue_id = 0,
	};
	struct e2e_talker *t;
	ssize_t bytes_send;
	int sock_prio = 0;
	int ret;
	
	t = calloc(1, sizeof(struct e2e_talker));
	if (!t)
		return (void *)(long)-ENOMEM;

	e2e_thread_set_ctx(thread, t);
	t->cfg = cfg;
	t->noResponsePacketCount = 0;
	ret = e2e_sock_create(&t->sock, &sock_cfg, cfg);
	if (ret) {
		e2e_agent_log_crit(NULL, true, "Failed to create sockets.");
		goto err;
	}

	ret = e2e_sock_set_tx_hook(t->sock, t, e2e_talker_prepare_payload);
	if (ret)
		goto err;

	ret = e2e_sock_get_tx_buf(t->sock, (void *)&t->buf, &t->buf_size);
	if (ret) {
		e2e_agent_log_crit(NULL, true,
				   "Failed to get transmit buffer.");
		goto err;
	}

	ret = e2e_config_validate(cfg);
	if (ret) {
		e2e_agent_log_crit(NULL, true, "Failed to validate config.");
		goto err;
	}

	if (cfg->training_filename) {
		ret = e2e_talker_seed_training_pattern(&t->training.f,
						       cfg->training_filename);
		if (ret)
			goto err;
	}
 
	// check link speed if burst traffic should be send
	// linkspeed is required to compute offset between packets in burst
	if (t->cfg->burst_size > 1 && t->cfg->role != LISTEN) {
		e2e_talker_get_linkspeed(t);
		e2e_talker_compute_burst_packet_offset(t);
	}

	/*
	 * reset packet is sent with low prio.
	 * Priority is changed to linux-priority later
	 */
	ret = e2e_sock_setopt(t->sock, SOL_SOCKET, SO_PRIORITY, &sock_prio,
			      sizeof(sock_prio));
	if (ret) {
		e2e_agent_log_warn(NULL, true, "Couldn't set socket priority");
		e2e_agent_log_warn(NULL, true, "ARE YOU ROOT?");
	}

	/* Prepare the send buffer */
	t->buf_used = e2e_talker_init_sendbuf(t);
	if (t->buf_used < 0)
		goto err;

	ret = e2e_sock_init_sockaddr_tx(t->sock);
	if (ret) {
		e2e_agent_log_crit(NULL, true,
				   "Failed to init socket address.");
		goto err;
	}

	/* binding to interface only if given */
	if (t->cfg->interface_set) {
		ret = e2e_sock_setopt(t->sock, SOL_SOCKET, SO_BINDTODEVICE,
				      t->cfg->iface, strlen(t->cfg->iface));
		if (ret) {
			e2e_agent_log_crit(
				NULL, true,
				"Failed to set send socket options.");
			goto err;
		}
	}

	if (cfg->verbose)
		e2e_talker_print_system_time(t);

	/* Send the reset packet */
	bytes_send = e2e_sock_sendto(t->sock, t->buf, t->buf_used, 0);
	if (bytes_send <= 0)
		e2e_agent_log_warn(NULL, true, "Sending reset packet failed");

	ret = e2e_talker_set_txtime(t);
	if (ret)
	{
		e2e_agent_log_crit(NULL, true,
			   "Failed to set transmit time.");
		goto err;
	}

	/*
	 * change socket priority after reset packet was sent
	 */
	ret = e2e_sock_setopt(t->sock, SOL_SOCKET, SO_PRIORITY,
			      &cfg->socket_prio, sizeof(cfg->socket_prio));
	if (ret) {
		e2e_agent_log_warn(NULL, true, "Couldn't set socket priority: %m");
		ret = -errno;
		goto err;
	}

	/* Set payload to 0x42 */
	int startbuf;
	if (t->cfg->use_udp_socket) // for the raw socket, the buffer count starts with the first byte of the dest addr, for the inet socket with the payload
		startbuf = 0;
	else
		startbuf = 23;
	for (int i = startbuf; i < t->buf_size-startbuf; i++)
		t->buf[i] = 0x42;

	// refresh t->e2e_agent_client_state, t->listener_state
	e2e_talker_get_states(t);
	talker_state = TALKER_STATE_RUNNING;
	
	// start talker send loop
	ret = e2e_talker_send_loop(t);


	e2e_talker_write_agent_log(t);

	/* Reset the number of packets to the configured one */
	t->cfg->number_of_packets = t->cfg->number_of_packets_setpoint;
err:
	e2e_talker_cleanup(thread, NULL);
	return (void *)(long)ret;
}

int e2e_talker_init(e2e_thread **t, struct e2e_config *cfg)
{
	struct e2e_thread_config tc = { 0 };

	tc.name = "talker";
	tc.entry = e2e_talker_entry;
	tc.entry_args = cfg;

	tc.cleanup = e2e_talker_cleanup;

	tc.set_sched_prio = true;
	tc.sched_prio = cfg->pthread_send_prio;
	tc.set_sched_policy = true;
	tc.sched_policy = SCHED_FIFO;
	tc.set_sched_cpu_mask = cfg->set_cpu_mask;
	tc.sched_cpu_mask = cfg->cpu_mask;

	tc.logger = e2e_agent_log_warnv;
	tc.logger_args = NULL;

	tc.verbose = cfg->verbose;

	return e2e_thread_create(t, &tc);
}

int e2e_talker_get_state(enum e2e_talker_state *state)
{
	*state = talker_state;
	return 0;
}
