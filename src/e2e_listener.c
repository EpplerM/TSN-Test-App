// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <fcntl.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <getopt.h>
#include <pthread.h>
#include <arpa/inet.h>
#include "e2e_agent.h"
#include "e2e_clock.h"
#include "e2e_common.h"
#include "e2e_listener.h"
#include "e2e_sock.h"

#define EXCESSIVE_DELAY_ARRAY_SIZE 100
#define INFLUX_PORT 8086

char ep_self_to_ep[12];
char ep_to_ep_self[12];

struct e2e_listener_stats {
	uint32_t rcvPacketnr;
	uint32_t rcvPacketCount;
	int32_t maxTimeOfFlightFwd;
	int32_t minTimeOfFlightFwd;
	double avgTimeOfFlightFwd;
	int32_t maxTimeOfFlightBack;
	int32_t minTimeOfFlightBack;
	double avgTimeOfFlightBack;
	int32_t maxRoundTripDelay;
	int32_t minRoundTripDelay;
	double avgRoundTripDelay;	
	int32_t excessiveFwdDelayCnt;
	int32_t excessiveBackDelayCnt;
	int32_t excessiveRoundTripCnt;
	int32_t lastRcvPacketNr;
	uint32_t remoteNumberOfPackets; // Never reset!
	uint32_t remoteCycleTime; // Never reset!
	int32_t graf_ToFfwd_min;
	int32_t graf_ToFfwd_max;
	int32_t graf_ToFback_min;
	int32_t graf_ToFback_max;
	int32_t graf_RTD_min;
	int32_t graf_RTD_max;
	int32_t graf_loss_fwd;
	int32_t graf_loss_back;
	int32_t excessiveFwdDelay[EXCESSIVE_DELAY_ARRAY_SIZE];
	int32_t excessiveBackDelay[EXCESSIVE_DELAY_ARRAY_SIZE];
	int32_t excessiveRoundTrip[EXCESSIVE_DELAY_ARRAY_SIZE];
	uint32_t longestGap;
	uint32_t lastMissedPacket;
	uint32_t startOfGap;
};


struct e2e_listener {

	struct e2e_config *cfg;

	e2e_sock *sock;
	FILE *output;
	//FILE *report;
	int grafana_fd;
	char grafana_token[100];
	int grafana_updated;
	pthread_mutex_t grafana_mutex;
	pthread_cond_t grafana_cond;
	int trace_marker_fd;
	char output_filename[120];
	unsigned char *packet;
	ssize_t packet_sz;
	bool last_packet_received;
	struct timespec rx_ts;
	struct timespec origin_ts;
	int32_t timeOfFlightFwd;
	int32_t timeOfFlightBack;
	int32_t roundtripdelay;
	int32_t graf_ToFfwd_max;
	int32_t graf_ToFback_max;
	int32_t graf_RTD_max;
	int32_t graf_ToFfwd_min;
	int32_t graf_ToFback_min;
	int32_t graf_RTD_min;
	int32_t graf_loss_fwd;
	int32_t graf_loss_back;
	struct e2e_listener_stats stats;
	bool reset_packet_received;
	enum e2e_agent_client_state e2e_agent_client_state;
	enum e2e_talker_state talker_state;
	int time_out;
};

struct timespec ts;

static int listener_state = LISTENER_STATE_UNDEFINED;

// refresh l->e2e_agent_client_state; l->talker_state
static void e2e_listener_get_states(struct e2e_listener *l) 
{
	if (l->cfg->agent_mode) {
		e2e_agent_client_get_state(&l->e2e_agent_client_state);}
	else {
		l->e2e_agent_client_state = E2E_CLIENT_STATE_UNDEFINED;}
	if (l->cfg->role == TALK_AND_LISTEN || l->cfg->role == LISTEN_AND_TALK){
		e2e_talker_get_state(&l->talker_state);}
	else {
		l->talker_state = TALKER_STATE_UNDEFINED;}
}

static int e2e_listener_prepare_outfile(struct e2e_listener *l)
{
	struct timespec last_phc;
	struct tm *local_time;
	long sec;
	last_phc = e2e_clock_get(l->cfg);
	sec = last_phc.tv_sec;
	local_time = localtime(&sec);
	struct utsname name;
	uname(&name);

	if (l->cfg->file_output) {
		mkdir("logs", 0777);
		snprintf(l->output_filename, sizeof(l->output_filename),
			"logs/%s_%s_%04d_%02d_%02d__%02d:%02d:%02d.csv",
			name.nodename,
			l->cfg->iface,
			local_time->tm_year + 1900, local_time->tm_mon + 1,
			local_time->tm_mday, local_time->tm_hour, local_time->tm_min,
			local_time->tm_sec);

		e2e_info(ESC_INFO, "Opening logfile %s.\n", l->output_filename);

		l->output = fopen(l->output_filename, "w");
		if (!l->output)
			e2e_agent_log_crit(NULL, true, "unable to open output file\n");

		switch (l->cfg->role) {
			case LISTEN:
				fprintf(l->output, "PacketNr;TimeOfFlight\n");
				break;
			case LISTEN_AND_TALK:
				fprintf(l->output, "PacketNr;TimeOfFlight\n");
				break;
			case TALK_AND_LISTEN:
				if(l->cfg->round_trip_delay) {
					if(l->cfg->verbose_file_output) {
						fprintf(l->output, "PacketNr;SendTime;TimeOfFlight;RoundTripDelay\n");
					} else {
						fprintf(l->output, "PacketNr;TimeOfFlight;RoundTripDelay\n");
					}
				} else {
					if(l->cfg->verbose_file_output) {
						fprintf(l->output, "PacketNr;SendTime;TimeOfFlight\n");
					} else {
						fprintf(l->output, "PacketNr;TimeOfFlight\n");
					}
				}
				break;
			default:
				break;
		}
	}
	if ((l->cfg->file_output && !l->output) || (l->cfg->report && !l->cfg->report))
		return -1;
	else
	return 0;
}

void* grafanaThread(void* arg)
{
	struct e2e_listener* l = (struct e2e_listener*)arg;
	char value[300];
	char values[900];
	char request[1500];
	struct sockaddr_in serv_addr;
	char influxDB_url[100];
	bool send_success = false;

	sprintf(influxDB_url, "http://%s:8086/api/v2/write?org=TestApp&bucket=%s&precision=ms", l->cfg->influx_server, l->cfg->influx_bucket);
	
	while (true)
	{
		pthread_mutex_lock(&l->grafana_mutex);
		while (!l->grafana_updated) {
			pthread_cond_wait(&l->grafana_cond, &l->grafana_mutex);
		}
		pthread_mutex_unlock(&l->grafana_mutex);
		l->grafana_updated = 0;
		//prepare data
		values[0] = '\0';
		if(l->graf_ToFfwd_min < INT32_MAX && l->graf_ToFfwd_max > INT32_MIN) {
			snprintf(value, sizeof(value), "min-forward-delay value=%d\nmax-forward-delay value=%d\n",
			l->graf_ToFfwd_min, l->graf_ToFfwd_max);
			strcat(values, value);
		}
		if(l->graf_ToFback_min < INT32_MAX && l->graf_ToFback_max > INT32_MIN) {
			snprintf(value, sizeof(value), "min-backward-delay value=%d\nmax-backward-delay value=%d\n", 
			l->graf_ToFback_min, l->graf_ToFback_max);
			strcat(values, value);
		}
		if(l->graf_RTD_min < INT32_MAX && l->graf_RTD_max > INT32_MIN) {
			snprintf(value, sizeof(value), "min-round-trip-delay value=%d\nmax-round-trip-delay value=%d\n", 
			l->graf_RTD_min, l->graf_RTD_max);
			strcat(values, value);
		}
		if(l->cfg->role == TALK_AND_LISTEN) {
			if(((double)l->graf_loss_back*1000.0/(double)l->cfg->grafana_sampling_period) <= 1000.0 && ((double)l->graf_loss_back*1000.0/(double)l->cfg->grafana_sampling_period) >= 0) {
				snprintf(value, sizeof(value), "packet-loss-forward value=%f\n", 
				(double)l->graf_loss_back*1000.0/(double)l->cfg->grafana_sampling_period);
				strcat(values, value);
			}
			if(((double)l->graf_loss_fwd*1000.0/(double)l->cfg->grafana_sampling_period) <= 1000.0 && ((double)l->graf_loss_fwd*1000.0/(double)l->cfg->grafana_sampling_period) >= 0) {
				snprintf(value, sizeof(value), "packet-loss-backward value=%f\n", 
				(double)l->graf_loss_fwd*1000.0/(double)l->cfg->grafana_sampling_period);
				strcat(values, value);
			}
		} else {
			if(((double)l->graf_loss_fwd*1000.0/(double)l->cfg->grafana_sampling_period) <= 1000.0 && ((double)l->graf_loss_fwd*1000.0/(double)l->cfg->grafana_sampling_period) >= 0) {
				snprintf(value, sizeof(value), "packet-loss-forward value=%f\n", 
				(double)l->graf_loss_fwd*1000.0/(double)l->cfg->grafana_sampling_period);
				strcat(values, value);
			}
		}

		snprintf(request, sizeof(request),
			"POST %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Authorization: Token %s\r\n"
			"Content-Type: text/plain\r\n"
			"Content-Length: %zu\r\n"
			"\r\n"
			"%s",
			influxDB_url, l->cfg->influx_server, l->grafana_token, strlen(values), values);

		// Create socket
		if ((l->grafana_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
			e2e_agent_log_crit(NULL, true, "unable to create socket for grafana output\n");
		}

		serv_addr.sin_family = AF_INET;
		serv_addr.sin_port = htons(INFLUX_PORT);

		// Convert IPv4 and IPv6 addresses from text to binary form
		if (inet_pton(AF_INET, l->cfg->influx_server, &serv_addr.sin_addr) <= 0) {
			e2e_agent_log_crit(NULL, true, "unable to create socket for grafana output: Invalid address/ Address not supported\n");
			close(l->grafana_fd);
		}

		// Connect to the server
		if (connect(l->grafana_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
			e2e_agent_log_crit(NULL, true, "Connection to socket for grafana output failed\n");
			close(l->grafana_fd);
		}

		// Send the HTTP request
		ssize_t bytes_sent = send(l->grafana_fd, request, strlen(request), 0);
		if (bytes_sent == -1) {
			e2e_agent_log_crit(NULL, true, "Failed to send HTTP request to influx server\n");
		} 
		// check the HTTP response - only once if it works to save performace
		if(!send_success) {
			char buffer[1024];
			ssize_t bytes_received = recv(l->grafana_fd, buffer, sizeof(buffer) - 1, 0);
			if (bytes_received == -1) {
				e2e_agent_log_crit(NULL, true, "Failed to receive HTTP response from influx server\n");
			} else {
				buffer[bytes_received] = '\0'; // Null-terminate the received data
				int response_code = 0;
				char *status_line = strtok(buffer, "\r\n");
				if (status_line) {
					sscanf(status_line, "HTTP/%*s %d", &response_code);
					if(response_code != 204) {
						e2e_agent_log_crit(NULL, true, "Unexpected HTTP response code from influx server\n");
					} else {
						send_success = true;
					}
				} else {
					e2e_agent_log_crit(NULL, true, "Failed to parse the response from the influx server\n");
				}
				e2e_agent_log_info(NULL, true, "Response from influx server: %s\n", buffer);
			}
		}
		if (l->grafana_fd)
			close(l->grafana_fd);
	}
	return 0;
};

static int e2e_listener_prepare_grafana(struct e2e_listener *l)
{

	size_t len = 0;
	char * line = NULL;
	pthread_t grafanaThread_id;
	l->grafana_updated = 0;
    pthread_mutex_init(&l->grafana_mutex, NULL);
    pthread_cond_init(&l->grafana_cond, NULL);

	if (!l->cfg->grafana)
		return 0;

	//get token from file
	FILE *fp = fopen("./influxDBtoken.txt", "r");
	if (!fp) {
		e2e_agent_log_warn(NULL, true, 
			"listener: Failed to open influxDB token file (./influxDBtoken.txt)\n");
		return -EINVAL;
	}

	getline(&line, &len, fp);
	if(line) {
		strtok(line, "\n");
		e2e_agent_log_info(NULL, true, "got influxDB token %s\n", line);
		snprintf(l->grafana_token, sizeof(l->grafana_token), "%s", line);
	} else {
		e2e_agent_log_crit(NULL, true, "influx DB acces token not found!\n");
	}

	// start thread
	if(pthread_create(&grafanaThread_id, NULL, grafanaThread, (void*)l) != 0) {
		e2e_agent_log_crit(NULL, true, "Failed to start grafana output thread!\n");
	}

	return (0);
}

static int e2e_listener_sock_promiscuous_mode(const struct e2e_listener *l)
{
	struct ifreq if_request = { 0 };
	int ret;

	strncpy(if_request.ifr_name, l->cfg->iface, IFNAMSIZ);

	ret = e2e_sock_ioctl(l->sock, SIOCGIFFLAGS, &if_request);
	if (ret == -1)
		return -errno;

	if_request.ifr_flags |= IFF_PROMISC;
	ret = e2e_sock_ioctl(l->sock, SIOCSIFFLAGS, &if_request);
	if (ret == -1) {
		e2e_agent_log_crit(NULL, true, "Failed to activate promiscuous mode\n");
		ret = -errno;
	}

	return ret;
}

static int e2e_listener_sock_init(const struct e2e_listener *l)
{
	struct packet_mreq multicast_req = { 0 };
	struct ip_mreqn mreq = { 0 };
	int enabled = 0;
	int ret;

	ret = e2e_sock_init_sockaddr_rx(l->sock);
	if (ret) {
		e2e_agent_log_crit(NULL, true, "Failed to init rx socket address\n");
		return ret;
	}

	if (l->cfg->receive_mode == HW_BASED)
		enabled = SOF_TIMESTAMPING_RX_HARDWARE |
			  SOF_TIMESTAMPING_RAW_HARDWARE;

	if (l->cfg->receive_mode == SW_BASED)
		enabled = SOF_TIMESTAMPING_SOFTWARE |
			  SOF_TIMESTAMPING_RAW_HARDWARE;

	if (enabled) {
		ret = e2e_sock_setopt(l->sock, SOL_SOCKET, SO_TIMESTAMPING,
				      &enabled, sizeof(enabled));
		if (ret)
			e2e_agent_log_crit(
				NULL, true, "Failed to activate %s timestamping",
				l->cfg->receive_mode == HW_BASED ? "HW" : "SW");
	}

	ret = e2e_sock_bind(l->sock);
	if (ret) {
		e2e_agent_log_crit(NULL, true, "Failed to bind receive socket\n");
		return ret;
	}

	if (l->cfg->use_udp_socket)
	{
		/* add multicast membership for multicast addresses */
		if ((224 <= l->cfg->dest_ip[0]) && (l->cfg->dest_ip[0] <= 239))
		{
			/* bind to interface in case of multicast -> really necessary? */
			ret = e2e_sock_setopt(l->sock, SOL_SOCKET, SO_BINDTODEVICE,
				l->cfg->iface, strlen(l->cfg->iface));
			if (ret) {
				e2e_agent_log_crit(NULL, true,
				"Failed to set receive socket options.\n");
				return ret;
			}

			memcpy ((void*)&mreq.imr_multiaddr, (void*)l->cfg->dest_ip, sizeof(l->cfg->dest_ip));
			mreq.imr_address.s_addr = htonl(INADDR_ANY);
			mreq.imr_ifindex = (int)if_nametoindex(l->cfg->iface);

			ret = e2e_sock_setopt(l->sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
						&mreq, sizeof(mreq));
			if (ret) {
				e2e_agent_log_crit(NULL, true, "Failed to set membership socket option\n");
				return ret;
			}
		}
	}
	else
	{
		ret = e2e_sock_setopt(l->sock, SOL_SOCKET, SO_BINDTODEVICE,
		      l->cfg->iface, strlen(l->cfg->iface));
		if (ret) {
			e2e_agent_log_crit(NULL, true,
				   "Failed to set receive socket options.\n");
			return ret;
		}

		multicast_req.mr_ifindex = (int)if_nametoindex(l->cfg->iface);
		multicast_req.mr_type = PACKET_MR_MULTICAST;
		multicast_req.mr_alen = 6;
		memcpy(multicast_req.mr_address, l->cfg->dest_mac, 6);

		ret = e2e_sock_setopt(l->sock, SOL_PACKET, PACKET_ADD_MEMBERSHIP,
					&multicast_req, sizeof(multicast_req));
		if (ret) {
			e2e_agent_log_crit(NULL, true, "Failed to set membership socket option\n");
			return ret;
		}
	}

	return ret;
}

static void e2e_listener_print_info(const struct e2e_listener *l)
{
	struct ifreq if_mac = { 0 };
	struct timespec last_phc;
	struct tm *lt;
	long secs;
	int ret;

	strncpy(if_mac.ifr_name, l->cfg->iface, IFNAMSIZ - 1);

	ret = e2e_sock_ioctl(l->sock, SIOCGIFHWADDR, &if_mac);
	if (ret == -1)
		e2e_warn("Could not fetch MAC from listener iface.\n");

	e2e_info(ESC_INFO,
			"Listener receiving from interface with mac-addr: %02X:%02X:%02X:%02X:%02X:%02X.\n",
			(unsigned char)(if_mac.ifr_hwaddr.sa_data)[0],
			(unsigned char)(if_mac.ifr_hwaddr.sa_data)[1],
			(unsigned char)(if_mac.ifr_hwaddr.sa_data)[2],
			(unsigned char)(if_mac.ifr_hwaddr.sa_data)[3],
			(unsigned char)(if_mac.ifr_hwaddr.sa_data)[4],
			(unsigned char)(if_mac.ifr_hwaddr.sa_data)[5]);

	// print listener ready time
	last_phc = e2e_clock_get(l->cfg);
	secs = last_phc.tv_sec;
	lt = localtime(&secs);

	e2e_agent_log_info(
		NULL, 
		true, 	// to std_out
		ESC_LISTENER,
		"Listener started, waiting for packets since %02d:%02d:%02d ...\n",
		lt->tm_hour, lt->tm_min, lt->tm_sec);
}

static void e2e_listener_set_nice()
{
	int ret;

	ret = nice(-20);
	if (ret != -1)
		return;

	e2e_agent_log_crit(NULL, true, "nice() failed: (%m)\n");
	e2e_agent_log_crit(NULL, true, "ARE YOU ROOT?\n");
}

static int e2e_listener_rx(struct e2e_listener *l,  void *arg)
{
	int ret;
	static ssize_t packet_sz;
	struct e2e_config *cfg = (struct e2e_config *)arg;
	ret = e2e_sock_rx(l->sock, (void **)&l->packet, &l->packet_sz,
			&l->rx_ts, cfg->time_out, cfg);

	// reset to 0 for first packet
	if (l->stats.rcvPacketCount == 0) {
		packet_sz = 0;
	}
	else if (!packet_sz && l->packet_sz) {
			packet_sz = l->packet_sz;
	}
	else if (!l->packet_sz && packet_sz) {
		// preserve packet size if 0 bytes received 
		l->packet_sz = packet_sz;
		packet_sz = 0;
	}

	if (ret)
		return ret;

	if (l->rx_ts.tv_nsec > NSEC_PER_SEC)
		e2e_agent_log_crit(
			NULL,
			true,
			"IMPLAUSIBLE VALUE FOR tv_nsec at extract AL timestamp\n");

	return ret;
}

static void e2e_listener_reset_stats(struct e2e_listener *l)
{
	struct e2e_listener_stats *s = &l->stats;

	s->rcvPacketnr = 0;
	s->rcvPacketCount = 0;
	s->maxTimeOfFlightFwd = INT32_MIN;
	s->minTimeOfFlightFwd = INT32_MAX;
	s->avgTimeOfFlightFwd = 0;
	s->maxTimeOfFlightBack = INT32_MIN;
	s->minTimeOfFlightBack = INT32_MAX;
	s->avgTimeOfFlightBack = 0;
	s->avgRoundTripDelay = 0;
	s->maxRoundTripDelay = INT32_MIN;
	s->minRoundTripDelay = INT32_MAX;
	s->excessiveFwdDelayCnt = 0;
	s->excessiveBackDelayCnt = 0;
	s->excessiveRoundTripCnt = 0;
	s->lastRcvPacketNr = 0;
	s->graf_ToFfwd_max = INT32_MIN;
	s->graf_ToFback_max = INT32_MIN;
	s->graf_RTD_max = INT32_MIN;
	s->graf_ToFfwd_min = INT32_MAX;
	s->graf_ToFback_min = INT32_MAX;
	s->graf_RTD_min = INT32_MAX;
	s->graf_loss_fwd = 0;
	s->graf_loss_back = 0;
	s->longestGap = 0;
	s->lastMissedPacket = 0;
	s->startOfGap = 0;
}

static void e2e_listener_set_dst_mac(struct e2e_listener *l)
{
	if (!l->cfg->use_udp_socket)
	{
		// store source MAC from which packets have been received from for statistics
		memcpy(l->cfg->rcvSrcMac, &l->packet[6],
				sizeof(l->cfg->rcvSrcMac));
		/* Use the received source MAC as dest MAC for replies if no dst mac configured */
		if (!l->cfg->dest_mac_set)
			memcpy(l->cfg->dest_mac, &l->packet[6],
				sizeof(l->cfg->dest_mac));

		e2e_agent_log_info(NULL, 		
				true, 	// to std_out
				ESC_INFO,
				"Destination mac-addr set to: %02X:%02X:%02X:%02X:%02X:%02X.",
				l->cfg->dest_mac[0], l->cfg->dest_mac[1],
				l->cfg->dest_mac[2], l->cfg->dest_mac[3],
				l->cfg->dest_mac[4], l->cfg->dest_mac[5]);
	}
}

static int e2e_listener_compute_forward_delay(struct e2e_listener *l)
{
	bool std_out = true;

	std_out = !l->cfg->quiet_mode;

	// compute forward delay
	l->timeOfFlightFwd =
		((l->rx_ts.tv_sec - l->origin_ts.tv_sec) * 1000000000) +
		(l->rx_ts.tv_nsec - l->origin_ts.tv_nsec);
	// exit on non-working time sync
	if ((l->timeOfFlightFwd < INT32_MIN) ||
	    (l->timeOfFlightFwd > INT32_MAX)) {
		e2e_agent_log_crit(
			NULL,
			std_out,
			"Excessive end-to-end delay of %d ns. Please check synchronization.\n");
		e2e_agent_log_warn(
			NULL,
			std_out,
			"Debug info: \n\treceive timestamp seconds %'11ld\n\torigin  timestamp seconds %'11ld\n\treceive timestamp nanosec %'11ld\n\torigin  timestamp nanosec %'11ld\n",
			l->rx_ts.tv_sec, l->origin_ts.tv_sec, l->rx_ts.tv_nsec,
			l->origin_ts.tv_nsec);

		if (l->cfg->agent_mode)
			return -EAGAIN;

		return -ECANCELED;
	}

	return 0;
}

static void e2e_listener_compute_backward_delay(struct e2e_listener *l)
{
	struct e2e_reported_ts *rep_ts;
	uint32_t packet_nr;
	int32_t *tempAddr;
	unsigned idx;
	bool std_out = true;

	if (l->cfg->quiet_mode){
		std_out =false;
	};

	if (l->cfg->role != TALK_AND_LISTEN)
		return;

	packet_nr = l->stats.rcvPacketnr;
	rep_ts = l->cfg->reported_ts;
	idx = packet_nr % l->cfg->reported_ts_sz;

	// AZ: get time stamps from packet payload
	tempAddr = (int32_t *)(&l->packet[l->cfg->data_offset_receive + 16]);
	rep_ts[idx].origin_sec = (int32_t)*tempAddr;
	tempAddr = (int32_t *)(&l->packet[l->cfg->data_offset_receive + 20]);
	rep_ts[idx].origin_nsec = (int32_t)*tempAddr;
	tempAddr = (int32_t *)(&l->packet[l->cfg->data_offset_receive + 24]);
	rep_ts[idx].recv_sec = (int32_t)*tempAddr;
	tempAddr = (int32_t *)(&l->packet[l->cfg->data_offset_receive + 28]);
	rep_ts[idx].recv_nsec = (int32_t)*tempAddr;

	// do statistics only if origin timestamps where provided - might not be the case if a forward packet was lost
	if(rep_ts[idx].origin_sec > 0) {
		l->timeOfFlightBack =
			((rep_ts[idx].recv_sec - rep_ts[idx].origin_sec) * 1000000000) +
			(rep_ts[idx].recv_nsec - rep_ts[idx].origin_nsec);

		if (l->timeOfFlightBack < l->stats.minTimeOfFlightBack)
			l->stats.minTimeOfFlightBack = l->timeOfFlightBack;

		if (l->timeOfFlightBack > l->stats.maxTimeOfFlightBack)
			l->stats.maxTimeOfFlightBack = l->timeOfFlightBack;

		if (l->stats.avgTimeOfFlightBack != 0 &&
			abs(l->timeOfFlightBack) / 10 > abs(l->stats.avgTimeOfFlightBack)) {
			l->stats.excessiveBackDelayCnt++;
			if (l->stats.excessiveBackDelayCnt < EXCESSIVE_DELAY_ARRAY_SIZE)
				l->stats.excessiveBackDelay
					[l->stats.excessiveBackDelayCnt] =
					l->stats.rcvPacketCount - 1;

			if (l->cfg->verbose)
				e2e_agent_log_crit(
					NULL,
					std_out,
					"Excessive backward delay for packet %'10d: %'11d, avg %'11d.",
					l->stats.rcvPacketnr, abs(l->timeOfFlightBack),
					abs(l->stats.avgTimeOfFlightBack /
						(l->stats.rcvPacketCount - 1)));
		} 
	} else {
		l->timeOfFlightBack = 0;
		l->stats.graf_loss_back += 1;
		if (l->cfg->verbose)
			e2e_agent_log_crit(
				NULL,
				std_out,
				"No origin timestamps available for packet %'10d.",
				l->stats.rcvPacketnr);
	}
}

static void e2e_listener_compute_roundtrip_delay(struct e2e_listener *l)
{
	bool std_out = true;

	if (l->cfg->quiet_mode){
		std_out =false;
	};

	if (l->cfg->role != TALK_AND_LISTEN)
		return;

	if(l->timeOfFlightBack != 0) {

		l->roundtripdelay = l->timeOfFlightFwd + l->timeOfFlightBack;

		if (l->roundtripdelay < l->stats.minRoundTripDelay)
			l->stats.minRoundTripDelay = l->roundtripdelay;

		if (l->roundtripdelay > l->stats.maxRoundTripDelay)
			l->stats.maxRoundTripDelay = l->roundtripdelay;

		if (l->stats.avgRoundTripDelay != 0 &&
			abs(l->roundtripdelay) / 10 > abs(l->stats.avgRoundTripDelay)) {
			l->stats.excessiveRoundTripCnt++;
			if (l->stats.excessiveRoundTripCnt < EXCESSIVE_DELAY_ARRAY_SIZE)
				l->stats.excessiveRoundTrip
					[l->stats.excessiveRoundTripCnt] =
					l->stats.rcvPacketCount - 1;

			if (l->cfg->verbose)
				e2e_agent_log_crit(
					NULL,
					std_out,
					"Excessive round trip delay for packet %'10d: %'11d, avg %'11d.",
					l->stats.rcvPacketnr, abs(l->roundtripdelay),
					abs(l->stats.avgRoundTripDelay /
						(l->stats.rcvPacketCount - 1)));
		}
		// rolling computaion of average
		l->stats.avgRoundTripDelay =
			(l->stats.rcvPacketCount - 1) * l->stats.avgRoundTripDelay /
				(double)l->stats.rcvPacketCount +
			(double)l->roundtripdelay / (double)l->stats.rcvPacketCount;
	} else {
		l->roundtripdelay = 0;
	}
}

static void e2e_listener_print_rx_time(struct e2e_listener *l)
{
	struct tm *t;

	if (!l->cfg->verbose)
		return;

	// AZ: print receive time
	t = localtime(&l->rx_ts.tv_sec);

	e2e_agent_log_info(NULL,
				false, 	// to std_out
				ESC_LISTENER,
			   "Packet  %10d rcv time: %02d:%02d:%02d, %'11ld\n",
			   l->stats.rcvPacketnr, t->tm_hour, t->tm_min,
			   t->tm_sec, l->rx_ts.tv_nsec);
}

static void e2e_listener_print_remote_tx_time(struct e2e_listener *l)
{
	struct tm *t;

	if (!l->cfg->verbose)
		return;

	t = localtime(&l->origin_ts.tv_sec);
	e2e_agent_log_info(NULL,
			false, 	// to std_out
			ESC_LISTENER,
			"         origin at %02d:%02d:%02d, %'11ld\n",
			t->tm_hour, t->tm_min, t->tm_sec,
			l->origin_ts.tv_nsec);
}

static void e2e_listener_check_for_drops_and_duplicates(struct e2e_listener *l)
{
	bool std_out = true;
	int gap;
	if (l->cfg->quiet_quiet_mode){
		std_out =false;
	};

	// check for missing or duplicate packets
	if (l->stats.rcvPacketnr == l->stats.lastRcvPacketNr + 1)
		return;

	if ((l->stats.rcvPacketnr - (l->stats.lastRcvPacketNr + 1)) > 0) {
		gap = l->stats.rcvPacketnr - (l->stats.lastRcvPacketNr + 1);
		e2e_agent_log_crit(
			NULL, 
			std_out, 
			"Missed %d packets before packet %d.\n",
			gap,
			l->stats.rcvPacketnr);
		l->stats.graf_loss_fwd += l->stats.rcvPacketnr - (l->stats.lastRcvPacketNr + 1);
		if (l->stats.lastRcvPacketNr - l->cfg->recovery_period < l->stats.lastMissedPacket) {
			//not enough packets received to recover between two gaps
			gap = l->stats.rcvPacketnr - (l->stats.startOfGap + 1);
		} else {
			//recovered from last gap
			l->stats.startOfGap = l->stats.lastRcvPacketNr;
		}
		l->stats.lastMissedPacket = l->stats.rcvPacketnr -1;
		if (gap > l->stats.longestGap) {
			l->stats.longestGap = gap;
		}
	}

	if ((l->stats.rcvPacketnr - (l->stats.lastRcvPacketNr + 1)) < 0) {
		e2e_agent_log_crit(
			NULL,
			std_out,
			"Received %d duplicates of packet %d.\n",
		    (l->stats.rcvPacketnr - (l->stats.lastRcvPacketNr + 1)) * -1,
		    l->stats.rcvPacketnr);
			l->stats.graf_loss_fwd += l->stats.rcvPacketnr - (l->stats.lastRcvPacketNr + 1);
	}
}

static void e2e_listener_set_reported_ts(struct e2e_listener *l)
{
	uint32_t packet_nr;
	unsigned idx;

	if (l->cfg->role != LISTEN_AND_TALK)
		return;

	// if the role is responder we need to send back origin-TS and
	// receive-TS.
	if (l->origin_ts.tv_nsec > NSEC_PER_SEC) {
		e2e_agent_log_crit(
			false,
			NULL,
			"UNPLAUSIBLE VALUE FOR orig->tv_nsec at reply origin TS\n");
	}

	packet_nr = l->stats.rcvPacketnr;
	idx = packet_nr % l->cfg->reported_ts_sz;

	l->cfg->reported_ts[idx].origin_sec = l->origin_ts.tv_sec;
	l->cfg->reported_ts[idx].origin_nsec = l->origin_ts.tv_nsec;
	l->cfg->reported_ts[idx].recv_sec = l->rx_ts.tv_sec;
	l->cfg->reported_ts[idx].recv_nsec = l->rx_ts.tv_nsec;

	if (l->rx_ts.tv_nsec > NSEC_PER_SEC) {
		e2e_agent_log_crit(
			NULL,
			true,
			"UNPLAUSIBLE VALUE FOR rx_ts.tv_nsec at reply receive TS\n");
	}
}

static void e2e_listener_trace_init(struct e2e_listener *l)
{
	const char *trace_marker = "/sys/kernel/debug/tracing/trace_marker";

	l->trace_marker_fd = open(trace_marker, O_WRONLY);

	if (l->trace_marker_fd == -1)
		e2e_agent_log_warn(NULL, true, "Tracing not available!\n");
}

static void e2e_listener_trace_mark(struct e2e_listener *l, const char *str)
{
	int ret;

	if (l->trace_marker_fd == -1)
		return;

	ret = write(l->trace_marker_fd, str, strlen(str));
	if (ret == -1)
		e2e_warn("Failed to set trace marker\n");
}

static void e2e_listener_do_statistics(struct e2e_listener *l)
{
	struct e2e_listener_stats *s = &l->stats;
	bool std_out = true;

	if (l->cfg->quiet_mode){
		std_out =false;
	};
	
    // update min / max latency
	if (l->timeOfFlightFwd < s->minTimeOfFlightFwd)
		s->minTimeOfFlightFwd = l->timeOfFlightFwd;
	if (l->timeOfFlightFwd > s->maxTimeOfFlightFwd)
		s->maxTimeOfFlightFwd = l->timeOfFlightFwd;
    
    // check increased latency
    if (s->avgTimeOfFlightFwd != 0 &&
	    abs(l->timeOfFlightFwd) / 2 > abs(s->avgTimeOfFlightFwd)) {

        // check excessive latency
        if (s->avgTimeOfFlightFwd != 0 &&
            abs(l->timeOfFlightFwd) / 10 > abs(s->avgTimeOfFlightFwd)) {
            s->excessiveFwdDelayCnt++;
            if (s->excessiveFwdDelayCnt < EXCESSIVE_DELAY_ARRAY_SIZE)
                s->excessiveFwdDelay[s->excessiveFwdDelayCnt] =
                    s->rcvPacketCount - 1;
				// "Latency%s : %'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec",
            e2e_agent_log_crit(
                NULL,
                std_out,
                "Excessive delay for packet %d: %'2d nsec (avg: %'2d nsec).\n", 
                s->rcvPacketnr, l->timeOfFlightFwd,
                abs(s->avgTimeOfFlightFwd));
            e2e_listener_trace_mark(l, "excessive delay");
        }
        else {
            e2e_agent_log_warn(
			NULL,
			std_out,
			"Increased delay for packet %d: %'2d nsec (avg: %'2d nsec).\n",
			s->rcvPacketnr, l->timeOfFlightFwd,
			abs(s->avgTimeOfFlightFwd));
        }
    }

	// rolling computation of average
	s->avgTimeOfFlightFwd =
		(s->rcvPacketCount - 1) * s->avgTimeOfFlightFwd /
			(double)s->rcvPacketCount +
		(double)l->timeOfFlightFwd / (double)s->rcvPacketCount;

	if (l->cfg->verbose) {
		e2e_info(ESC_LISTENER, "Latency%s:\t%'2d nsec "
			"(avg %'2.0f nsec, min %'2d nsec, max %'2d nsec, jitter %'2d nsec)\n",
				ep_self_to_ep,l->timeOfFlightFwd, 
				s->avgTimeOfFlightFwd, s->minTimeOfFlightFwd,
				s->maxTimeOfFlightFwd, (s->maxTimeOfFlightFwd - s->minTimeOfFlightFwd));

	} else {
		if ((s->rcvPacketCount) % l->cfg->update_interval == 0){
            // printf("Latency: %'2d nsec", l->timeOfFlightFwd);
			if (l->cfg->role == LISTEN_AND_TALK) {
				printf("\n");
			}
			e2e_info(ESC_LISTENER, "Packet %'10d received in cycle %6d\nLatency%s: %'2d nsec "
				"(avg %'2.0f nsec, min %'2d nsec, max %'2d nsec, jitter %'2d nsec)\n",
			    	s->rcvPacketnr, s->rcvPacketCount, ep_self_to_ep, l->timeOfFlightFwd, 
					s->avgTimeOfFlightFwd, s->minTimeOfFlightFwd,
			       	s->maxTimeOfFlightFwd, (s->maxTimeOfFlightFwd - s->minTimeOfFlightFwd));
			if (l->cfg->round_trip_delay) { //nur wenn RTD gemessen werden soll
				e2e_info(ESC_LISTENER, "  round trip delay: %'2d nsec "
				"(avg %'2.0f nsec, min %'2d nsec, max %'2d nsec, jitter %'2d nsec)\n",
			    	l->roundtripdelay, 
					s->avgRoundTripDelay, s->minRoundTripDelay,
			       	s->maxRoundTripDelay, (s->maxRoundTripDelay - s->minRoundTripDelay));
			}
		}
	}
}

// signal agent client to send buffer content to client and empty buffer
static void e2e_listener_signal_agent_client(struct e2e_listener *l)
{	
	// e2e_listener_get_states(l);
	if (l->cfg->agent_mode)
	{
		int ret;
		
		bool std_out = true;
		if (l->cfg->quiet_quiet_mode){
			std_out =false;
		};
		ret = e2e_config_lock_signal(&l->cfg->locks.agent_lock);
		if (ret)
			e2e_agent_log_warn(NULL, std_out, "agent: Failed to signal agent client\n");
		
		// wait for sending completed
		ret = e2e_config_lock_wait(&l->cfg->locks.agent_lock);
		if (ret)
			e2e_agent_log_warn(NULL, std_out, "listener: Failed to wait for agent client\n");
	}
}

static void e2e_listener_write_output(struct e2e_listener *l)
{
	if (l->cfg->role == TALK)
		return;

	if (l->output && l->stats.rcvPacketCount>0) {
		if(l->cfg->verbose_file_output) {//extended file output
			time_t secs;
			struct tm *lt;
			secs = l->origin_ts.tv_sec;
			lt = localtime(&secs);
			if(l->cfg->round_trip_delay){ //round trip measurement
				fprintf(l->output, "%d;%02d:%02d:%02d,%'11ld;%'06d;%'d\n", l->stats.rcvPacketnr,
					lt->tm_hour, lt->tm_min, lt->tm_sec,
					l->origin_ts.tv_nsec,
					l->timeOfFlightFwd, l->roundtripdelay);
			} else { //no round trip measurement
				fprintf(l->output, "%d;%02d:%02d:%02d,%'11ld;%'06d\n", l->stats.rcvPacketnr,
					lt->tm_hour, lt->tm_min, lt->tm_sec,
					l->origin_ts.tv_nsec,
					l->timeOfFlightFwd);
			}
		} else { //standard file output
			if(l->cfg->round_trip_delay){ //round trip measurement
				fprintf(l->output, "%d;%'d;%'d\n", l->stats.rcvPacketnr,
				l->timeOfFlightFwd, l->roundtripdelay);
			} else { //no round trip measurement
				fprintf(l->output, "%d;%'d\n", l->stats.rcvPacketnr,
				l->timeOfFlightFwd);
			}
		}
	}
	
	if (l->cfg->grafana == true && l->stats.rcvPacketCount>0) {
		if (l->timeOfFlightFwd < l->stats.graf_ToFfwd_min)
			l->stats.graf_ToFfwd_min = l->timeOfFlightFwd;
		if (l->timeOfFlightFwd > l->stats.graf_ToFfwd_max)
			l->stats.graf_ToFfwd_max = l->timeOfFlightFwd;
		if (l->timeOfFlightBack != 0) { //no statistics, if no value for ToF back is available
			if (l->timeOfFlightBack < l->stats.graf_ToFback_min)
				l->stats.graf_ToFback_min = l->timeOfFlightBack;
			if (l->timeOfFlightBack > l->stats.graf_ToFback_max)
				l->stats.graf_ToFback_max = l->timeOfFlightBack;
			if (l->roundtripdelay < l->stats.graf_RTD_min)
				l->stats.graf_RTD_min = l->roundtripdelay;
			if (l->roundtripdelay > l->stats.graf_RTD_max)
				l->stats.graf_RTD_max = l->roundtripdelay;
		}
		
		if (l->stats.rcvPacketCount % l->cfg->grafana_sampling_period == 0) {
			// Update the data and signal the worker thread every update interval
			l->graf_ToFfwd_min = l->stats.graf_ToFfwd_min;
			l->graf_ToFfwd_max = l->stats.graf_ToFfwd_max;
			l->graf_ToFback_min = l->stats.graf_ToFback_min;
			l->graf_ToFback_max = l->stats.graf_ToFback_max;
			l->graf_RTD_min = l->stats.graf_RTD_min;
			l->graf_RTD_max = l->stats.graf_RTD_max;
			l->graf_loss_back = l->stats.graf_loss_back;
			l->graf_loss_fwd = l->stats.graf_loss_fwd;
			l->stats.graf_ToFfwd_min = INT32_MAX;
			l->stats.graf_ToFfwd_max = INT32_MIN;
			l->stats.graf_ToFback_min = INT32_MAX;
			l->stats.graf_ToFback_max = INT32_MIN;
			l->stats.graf_RTD_min = INT32_MAX;
			l->stats.graf_RTD_max = INT32_MIN;
			l->stats.graf_loss_back = 0;
			l->stats.graf_loss_fwd = 0;
			pthread_mutex_lock(&l->grafana_mutex);
			l->grafana_updated = 1;
			pthread_cond_signal(&l->grafana_cond);
			pthread_mutex_unlock(&l->grafana_mutex);
		}
	}

	if (l->cfg->agent_mode)
	{
		// signal agent to send data (listener ready) from info buffer to client 
		if (l->stats.rcvPacketCount == 0)
		{
			e2e_listener_signal_agent_client(l);
		}
		else 
		{
			// write test packet data to info buffer
			pthread_mutex_lock(&l->cfg->locks.agent_lock.mutex);
			e2e_agent_log_info(
				NULL,
				false, 	// to std_out
				ESC_LISTENER,
				"%d;%d\n", l->stats.rcvPacketnr, l->timeOfFlightFwd);
			pthread_mutex_unlock(&l->cfg->locks.agent_lock.mutex);

			// e2e_agent: send buffer content to client, clear buffer
			if ((l->stats.rcvPacketCount) % l->cfg->update_interval == 0 && (l->stats.rcvPacketCount) !=0) {
				e2e_listener_signal_agent_client(l);
			}
		}
	}
}

static int e2e_listener_handle_reset_packet(struct e2e_listener *l)
{
	bool gm_diff = false;

	if (l->packet[l->cfg->data_offset_receive + 18] != VERSION_COUNTER) {
		e2e_agent_log_crit(
			NULL,
			true,
			"RECEIVED RESET PACKET FROM INCOMPATIBLE VERSION!\n");
		e2e_agent_log_crit(
			NULL,
			true,
			"Remote version counter is 0x%02x, local is 0x%02x.",
			l->packet[l->cfg->data_offset_receive + 18], VERSION_COUNTER);
		return -EBADMSG;
	}

	// check grandmaster identity
	for (int i = 0; i < strlen(l->cfg->grandmaster_identity); i++) {
		if (l->packet[l->cfg->data_offset_receive + 20 + i] !=
		    (unsigned char)l->cfg->grandmaster_identity[i]) {
			gm_diff = true;
			break;
		}
	}
	if (gm_diff) {
		e2e_warn("Different grandmasters in sender and receiver! Check synchronization!\n");
		e2e_agent_log_report("Different grandmasters in sender and receiver! Check synchronization!\n");
	}

	// reset statistic counters
	if (l->cfg->role != TALK || l->cfg->role != TALK_AND_LISTEN) {
		e2e_agent_log_info(
			NULL,
			true, 	// to std_out
			ESC_INFO,
			"Received reset packet. Resetting all counters.\n");
		e2e_listener_reset_stats(l);
		e2e_listener_set_dst_mac(l);
		l->reset_packet_received = true;
		return 0;
	}

	e2e_agent_log_warn(
		NULL,
		true, 
		"Received reset packet. Ignoring, as neither in listener nor responder role\n");
	return 0;
}

static void e2e_listener_handle_last_packet(struct e2e_listener *l)
{
	struct e2e_config *cfg = l->cfg;
	struct e2e_listener_stats *s = &l->stats;
	enum e2e_role role = l->cfg->role;
	bool to_std_out = true;
	struct timespec phc = e2e_clock_get(cfg);

	// file output
	if (l->output) 
	{
		fprintf(l->output,
			"\nReceived %d of %d packets, cycle time %d ns, frame size %ld Bytes\n\n",
			s->rcvPacketCount, s->remoteNumberOfPackets,
			s->remoteCycleTime, 
			l->cfg->use_udp_socket ? l->packet_sz : (l->packet_sz+4));
			//+4 in case of raw socket because VLAN tag is already stripped when frame length is counted.
		if(l->stats.longestGap > 0) {
			fprintf(l->output, "Longest consecutive number of missed packets: %d\n", l->stats.longestGap);
		}
	}
	// report output
	if (l->cfg->report) 
	{
		e2e_agent_log_report("\n\nStatistics at end of test:\n\n");
	}
	//log data
	if (s->rcvPacketCount != s->remoteNumberOfPackets && l->e2e_agent_client_state != E2E_CLIENT_STATE_STOPPED) 
	{
		e2e_agent_log_crit(NULL, true,
			"\nReceived %d of %d packets, cycle time %d ns, frame size %ld Bytes\n",
			s->rcvPacketCount, s->remoteNumberOfPackets, s->remoteCycleTime,
			l->cfg->use_udp_socket ? l->packet_sz : (l->packet_sz+4));
			//+4 in case of raw socket because VLAN tag is already stripped when frame length is counted.
	}
	else {
		e2e_agent_log_info(
			NULL,
			true,
			ESC_LISTENER,
			"\nReceived %d of %d packets, cycle time %d ns, frame size %ld Bytes\n",
			s->rcvPacketCount, s->remoteNumberOfPackets, s->remoteCycleTime,
			l->cfg->use_udp_socket ? l->packet_sz : (l->packet_sz+4));
			//+4 in case of raw socket because VLAN tag is already stripped when frame length is counted.
			if (l->cfg->report) {
				e2e_agent_log_report(
					"\nReceived %d of %d packets, cycle time %d ns, frame size %ld Bytes\n",
					s->rcvPacketCount, s->remoteNumberOfPackets, s->remoteCycleTime,
					l->cfg->use_udp_socket ? l->packet_sz : (l->packet_sz+4));
					//+4 in case of raw socket because VLAN tag is already stripped when frame length is counted.
			} 
	}
	if(l->stats.longestGap > 0) {
		e2e_agent_log_crit(NULL, true, "Longest consecutive number of missed packets: %d\n", l->stats.longestGap);
	}


	if (l->e2e_agent_client_state == E2E_CLIENT_STATE_STOPPED)
	{
		e2e_agent_log_info(NULL, true, ESC_LISTENER, "Listener stopped by request ...\n\n");
	}
	else if (listener_state == LISTENER_STATE_RUNNING || listener_state == LISTENER_STATE_STANDBY)
	{
		e2e_agent_log_info(
			NULL,
			to_std_out,
			ESC_LISTENER,
			"Listener: Job done.\n");
	}
	else {
		e2e_agent_log_info(NULL, true, ESC_LISTENER, "Listener stopped ...\n\n");
	}

	// warnings
	if (s->remoteCycleTime != cfg->cycle_time &&
	    (role == TALK_AND_LISTEN || role == LISTEN_AND_TALK)) {
		e2e_warn(
			"Different cycle times in sender and receiver (remote %d ns, local %d ns)\n",
			s->remoteCycleTime, cfg->cycle_time);
		e2e_agent_log_report(
			"Warning: Different cycle times in sender and receiver (remote %d ns, local %d ns)\n",
			s->remoteCycleTime, cfg->cycle_time);
	}

	if (s->remoteNumberOfPackets != cfg->number_of_packets &&
	    (role == TALK_AND_LISTEN || role == LISTEN_AND_TALK) &&
	    !cfg->agent_mode) {
		e2e_warn(
			"Different number of test packets in sender and receiver (remote %d, local %d)\n",
			s->remoteNumberOfPackets, cfg->number_of_packets);
		e2e_agent_log_report(
			"Warning: Different number of test packets in sender and receiver (remote %d, local %d)\n",
			s->remoteNumberOfPackets, cfg->number_of_packets);
	}

	// statistics at end of test
	// file output
	if (l->output) {
		fprintf(l->output,
		"Latency%s:\t%'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
		ep_self_to_ep, s->avgTimeOfFlightFwd,
		s->minTimeOfFlightFwd, 
		s->maxTimeOfFlightFwd,
		(s->maxTimeOfFlightFwd - s->minTimeOfFlightFwd));
		if(l->cfg->round_trip_delay) {
			fprintf(l->output,
				"  round trip delay: %'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
				s->avgRoundTripDelay,
				s->minRoundTripDelay, 
				s->maxRoundTripDelay,
				(s->maxRoundTripDelay - s->minRoundTripDelay));
		}
	}
	// log data
	if (s->minTimeOfFlightFwd < 0 || s->maxTimeOfFlightFwd < 0 ||
		s->maxTimeOfFlightFwd > s->remoteCycleTime ||
		(s->maxTimeOfFlightFwd - s->minTimeOfFlightFwd) >
		s->remoteCycleTime / 10) 
	{
		e2e_agent_log_crit(
			NULL,
			to_std_out,
			"Latency%s: %'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
			ep_self_to_ep, s->avgTimeOfFlightFwd,
			s->minTimeOfFlightFwd, 
			s->maxTimeOfFlightFwd,
			(s->maxTimeOfFlightFwd - s->minTimeOfFlightFwd));
		if(l->cfg->round_trip_delay) {
			e2e_agent_log_crit(
				NULL,
				to_std_out,
				"  round trip delay: %'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
				s->avgRoundTripDelay,
				s->minRoundTripDelay, 
				s->maxRoundTripDelay,
				(s->maxRoundTripDelay - s->minRoundTripDelay));
		}
	}
	else 
	{
		e2e_agent_log_info(
			NULL,
			to_std_out,
			ESC_INFO,
			"Latency%s: %'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
			ep_self_to_ep, s->avgTimeOfFlightFwd,
			s->minTimeOfFlightFwd, 
			s->maxTimeOfFlightFwd,
			(s->maxTimeOfFlightFwd - s->minTimeOfFlightFwd));
		if(l->cfg->round_trip_delay) {
			e2e_agent_log_info(
				NULL,
				to_std_out,
				ESC_INFO,
				"  round trip delay: %'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
				s->avgRoundTripDelay,
				s->minRoundTripDelay, 
				s->maxRoundTripDelay,
				(s->maxRoundTripDelay - s->minRoundTripDelay));
		}
		if (l->cfg->report) {
			e2e_agent_log_report("\tLatency%s:\t%'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
			ep_self_to_ep, s->avgTimeOfFlightFwd,
			s->minTimeOfFlightFwd, 
			s->maxTimeOfFlightFwd,
			(s->maxTimeOfFlightFwd - s->minTimeOfFlightFwd));
			if(l->cfg->round_trip_delay) {
				e2e_agent_log_report("\tround trip delay:\t%'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
					s->avgRoundTripDelay,
					s->minRoundTripDelay, 
					s->maxRoundTripDelay,
					(s->maxRoundTripDelay - s->minRoundTripDelay));
			}
		}
	}

	// backward data
	if (role == TALK_AND_LISTEN) 
	{	
		// file output
		if (l->output) {
			fprintf(l->output,
				"Latency%s:\t%'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
				ep_to_ep_self, s->avgTimeOfFlightBack,
				s->minTimeOfFlightBack, 
				s->maxTimeOfFlightBack,
				(s->maxTimeOfFlightBack - s->minTimeOfFlightBack));	
		}
		// log data
		if (s->minTimeOfFlightBack < 0 || s->maxTimeOfFlightBack < 0 ||
			s->maxTimeOfFlightBack > s->remoteCycleTime ||
			(s->maxTimeOfFlightBack - s->minTimeOfFlightBack) >
			s->remoteCycleTime / 10) 
		{
			e2e_agent_log_crit(
				NULL,
				to_std_out, 
				"Latency%s : %'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
				ep_to_ep_self, s->avgTimeOfFlightBack,
				s->minTimeOfFlightBack, 
				s->maxTimeOfFlightBack,
				(s->maxTimeOfFlightBack - s->minTimeOfFlightBack));
		}
		else 
		{
			e2e_agent_log_info(
				NULL,
				to_std_out,
				ESC_INFO,
				"Latency%s : %'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
				ep_to_ep_self, s->avgTimeOfFlightBack,
				s->minTimeOfFlightBack, 
				s->maxTimeOfFlightBack,
				(s->maxTimeOfFlightBack - s->minTimeOfFlightBack));
				if (l->cfg->report) {
					e2e_agent_log_report("\tLatency%s:\t%'2.0f nsec (min %'2d nsec, max %'2d nsec),  Jitter: %'2d nsec\n",
					ep_to_ep_self, s->avgTimeOfFlightBack,
					s->minTimeOfFlightBack, 
					s->maxTimeOfFlightBack,
					(s->maxTimeOfFlightBack - s->minTimeOfFlightBack));	
			}
		}
	}

	// exception analysis
	// not to stdout for quiet mode / stopped tests 	
	if (l->cfg->quiet_quiet_mode || l->e2e_agent_client_state == E2E_CLIENT_STATE_STOPPED) 
	{
		to_std_out = false;
	}

	if (s->excessiveFwdDelayCnt > 0) {
		e2e_agent_log_crit(
			NULL,
			to_std_out,
			"Excessive %s delay occurred %d times, affected packets: ",
			ep_self_to_ep,
			s->excessiveFwdDelayCnt);
		int arrayCount = EXCESSIVE_DELAY_ARRAY_SIZE - 1;
		if (s->excessiveFwdDelayCnt < arrayCount)
			arrayCount = s->excessiveFwdDelayCnt;
		for (int i = 1; i <= arrayCount; i++) {
			e2e_agent_log_crit(NULL, to_std_out, "%d, ",
						s->excessiveFwdDelay[i]);
		}

		if (s->excessiveFwdDelayCnt >= EXCESSIVE_DELAY_ARRAY_SIZE)
		{
			e2e_agent_log_crit(NULL, to_std_out, " and %d more.\n",
						s->excessiveFwdDelayCnt -
							100);
		} else {
			e2e_agent_log_crit(NULL, to_std_out, "\n");
		}
	}
	if (role == TALK_AND_LISTEN) {
		if (s->excessiveBackDelayCnt > 0) 
		{
			e2e_agent_log_crit(
				NULL,
				to_std_out,
				"Excessive %s delay occurred %d times, affected packets: ",
				ep_to_ep_self,
				s->excessiveBackDelayCnt);

			int arrayCount = EXCESSIVE_DELAY_ARRAY_SIZE - 1;
			if (s->excessiveBackDelayCnt < arrayCount)
				arrayCount = s->excessiveBackDelayCnt;

			for (int i = 1; i <= arrayCount; i++)
			{
				e2e_agent_log_crit(NULL, to_std_out,"%d, ",
							s->excessiveBackDelay[i]);
			}

			if (s->excessiveBackDelayCnt >=
				EXCESSIVE_DELAY_ARRAY_SIZE) {
				e2e_agent_log_crit(NULL, to_std_out, " and %d more.\n",
							s->excessiveBackDelayCnt - 100);
			} else {
				e2e_agent_log_crit(NULL, to_std_out, "\n");
			}
		}
	}
	
	if (role == LISTEN_AND_TALK || role == TALK_AND_LISTEN || role == TALK)
	{
		if (l->cfg->use_udp_socket) {
			e2e_info(ESC_INFO, "\nTalker has sent packets to destination IP address: %hhu.%hhu.%hhu.%hhu:%hu.\n",
		       cfg->dest_ip[0], cfg->dest_ip[1], cfg->dest_ip[2],
		       cfg->dest_ip[3], cfg->dest_port );
			if (l->cfg->report) {
				e2e_agent_log_report("\nTalker has sent packets to destination IP address: %hhu.%hhu.%hhu.%hhu:%hu.\n",
		       cfg->dest_ip[0], cfg->dest_ip[1], cfg->dest_ip[2],
		       cfg->dest_ip[3], cfg->dest_port );
			}
		} else {
			e2e_info(ESC_INFO, "\nTalker has sent packets to destination MAC address: %02X:%02X:%02X:%02X:%02X:%02X.\n",
		       cfg->dest_mac[0], cfg->dest_mac[1], cfg->dest_mac[2],
		       cfg->dest_mac[3], cfg->dest_mac[4], cfg->dest_mac[5]);
			if (l->cfg->report) {
				e2e_agent_log_report("\nTalker has sent packets to destination MAC address: %02X:%02X:%02X:%02X:%02X:%02X.\n",
		       cfg->dest_mac[0], cfg->dest_mac[1], cfg->dest_mac[2],
		       cfg->dest_mac[3], cfg->dest_mac[4], cfg->dest_mac[5]);
			}
		}
	}

	//ToDo: add info about received VLAN ID and Prio
	if (l->cfg->use_udp_socket) {
		//ToDo
	} else {
		e2e_agent_log_report("Packets have been received from source MAC address: %02X:%02X:%02X:%02X:%02X:%02X.\n",
			l->cfg->rcvSrcMac[0], l->cfg->rcvSrcMac[1],
			l->cfg->rcvSrcMac[2], l->cfg->rcvSrcMac[3],
			l->cfg->rcvSrcMac[4], l->cfg->rcvSrcMac[5]);
	}

	switch (cfg->receive_mode) {
	case HW_BASED:
		e2e_info(ESC_INFO, "Receive mode: HW-based.\n");
		break;
	case SW_BASED:
		e2e_info(ESC_INFO, "Receive mode: SW-based.\n");
		break;
	case APP_LAYER:
		e2e_info(ESC_INFO, "Receive mode: app layer.\n");
		break;
	default:
		break;
	}

	if (role == LISTEN_AND_TALK || role == TALK_AND_LISTEN) {
		if (cfg->send_mode == HW_BASED)
			e2e_info(ESC_INFO, "Send mode: HW-based.\n");
		else if (l->cfg->send_mode == ETF)
			e2e_info(ESC_INFO, "Send mode: ETF offloading.\n");
		else
			e2e_info(ESC_INFO, "Send mode: SW-based.\n");
	}
	
	e2e_agent_log_info(
		NULL,
		true, 	// to std_out
		ESC_INFO,
		"Transmission offset: %d ns, provision time offset: %d ns, VLAN ID %d, prio %d, socket-prio %d",
		cfg->transmission_offset, cfg->provision_time,
		cfg->vlan_id, cfg->vlan_prio, cfg->socket_prio);

	if (cfg->send_window > 0) 
	{
		e2e_agent_log_info(
			NULL, 
			true, 	// to std_out
			ESC_LISTENER,
			"send window: %d ns\n",
			cfg->send_window);
	}

	// print test time
	long testTimeSeconds = phc.tv_sec;
	struct tm *testTimeStruct = localtime(&testTimeSeconds);

	e2e_agent_log_info(
		NULL,
		true, 	// to std_out
		ESC_INFO,
		"Test ended at %02d:%02d:%02d on %d/%d/%d\n",
		testTimeStruct->tm_hour, testTimeStruct->tm_min,
		testTimeStruct->tm_sec,
		testTimeStruct->tm_year + 1900,
		testTimeStruct->tm_mon + 1, testTimeStruct->tm_mday);
	
	if (l->cfg->report)
		e2e_agent_log_report("Test ended at %02d:%02d:%02d on %d/%d/%d\n",
			testTimeStruct->tm_hour, testTimeStruct->tm_min,
			testTimeStruct->tm_sec,
			testTimeStruct->tm_year + 1900,
			testTimeStruct->tm_mon + 1, testTimeStruct->tm_mday);
	e2e_agent_log_info(
		NULL,
		false, 	// to std_out	
		ESC_INFO,
		"Resetting statistic counters ...\n");

	// initialize statistic values
	e2e_listener_reset_stats(l);
	cfg->number_of_packets = cfg->number_of_packets_setpoint;

	if (l->output) {
		e2e_info(ESC_INFO, "Test results have been written to logfile %s\n",
				l->output_filename);
		e2e_agent_log_report("Test results have been written to logfile %s\n",
				l->output_filename);
		}
}

static int e2e_listener_handle_test_packet(struct e2e_listener *l)
{
	int32_t *tempAddr;
	int *intTempAddr;
	int ret;

	/* Tell the waiting talker once that dst mac is now ready */
	if (l->cfg->role == LISTEN_AND_TALK && l->stats.rcvPacketCount == 0) {
		e2e_config_lock_signal(&l->cfg->locks.dst_mac_lock);
	}
	
	intTempAddr = (int32_t *)(&l->packet[l->cfg->data_offset_receive + 4]);
	l->stats.rcvPacketnr = (int32_t)*intTempAddr;

	l->stats.rcvPacketCount++;
	intTempAddr = (int *)(&l->packet[l->cfg->data_offset_receive + 0]);
	if (l->stats.rcvPacketnr % 2 == 0)
		l->stats.remoteNumberOfPackets = (int)*intTempAddr;
	else
		l->stats.remoteCycleTime = (int)*intTempAddr;

	if (l->stats.rcvPacketnr == 42 && !l->cfg->use_udp_socket)
	{
		// store source MAC from which packets have been received from for statistics, just once and not directly in the beginning
		memcpy(l->cfg->rcvSrcMac, &l->packet[6],
				sizeof(l->cfg->rcvSrcMac));
	}

	// get origin time from packet payload
	tempAddr = (int32_t *)(&l->packet[l->cfg->data_offset_receive + 8]);
	l->origin_ts.tv_sec = (long)(*tempAddr);
	tempAddr = (int32_t *)(&l->packet[l->cfg->data_offset_receive + 12]);
	l->origin_ts.tv_nsec = (long)(*tempAddr);
	// for reply mode: set packet received indication
	l->cfg->packet_received = true;

	ret = e2e_listener_compute_forward_delay(l);
	if (ret)
		return ret;

	e2e_listener_print_rx_time(l);
	e2e_listener_check_for_drops_and_duplicates(l);
	e2e_listener_set_reported_ts(l);

	l->stats.lastRcvPacketNr = l->stats.rcvPacketnr;

	e2e_listener_print_remote_tx_time(l);
	e2e_listener_do_statistics(l);
	e2e_listener_compute_backward_delay(l);
	e2e_listener_compute_roundtrip_delay(l);
	e2e_listener_write_output(l);

	// rolling computaion of average
	l->stats.avgTimeOfFlightBack =
		(l->stats.rcvPacketCount - 1) * l->stats.avgTimeOfFlightBack /
			(double)l->stats.rcvPacketCount +
		(double)l->timeOfFlightBack / (double)l->stats.rcvPacketCount;

	/* The last packet needs additional handling */

	if (l->stats.rcvPacketnr > 1) 
	{
		if (l->cfg->agent_mode)
		{
			if (unlikely(l->stats.rcvPacketnr == l->stats.remoteNumberOfPackets) || (l->e2e_agent_client_state == E2E_CLIENT_STATE_STOPPED))
			{ 
				e2e_listener_handle_last_packet(l);
				l->last_packet_received = true;
			}
		}
		else 
		{
			if (unlikely(l->stats.rcvPacketnr == l->stats.remoteNumberOfPackets) && !l->cfg->endless_number_of_packets)
			{ 
				e2e_listener_handle_last_packet(l);
				l->last_packet_received = true;
			}
		}
	}
	return 0;
}

static int e2e_listener_handle_packet(struct e2e_listener *l)
{	
	/* Handle reset packet */
	if (unlikely(l->packet[l->cfg->data_offset_receive] == 0xCA &&
		     l->packet[l->cfg->data_offset_receive + 1] == 0xFE &&
		     l->packet[l->cfg->data_offset_receive + 2] == 0xC0 &&
		     l->packet[l->cfg->data_offset_receive + 3] == 0xDE &&
		     l->packet[l->cfg->data_offset_receive + 4] == 0xFF &&
		     l->packet[l->cfg->data_offset_receive + 5] == 0xFF))
		return e2e_listener_handle_reset_packet(l);

	return e2e_listener_handle_test_packet(l);
}

static inline bool e2e_listener_continue(struct e2e_listener *l)
{
	return !l->last_packet_received;
}

static int e2e_listener_loop(struct e2e_listener *l)
{
	int ret;
	switch (l->cfg->role)
	{
	// uni-directed test
	case LISTEN:

		strcpy(ep_self_to_ep, "");
		break;
	// bi-directed test
	case LISTEN_AND_TALK:
		strcpy(ep_self_to_ep, " (forward)");
		break;
	case TALK_AND_LISTEN:
		strcpy(ep_self_to_ep, " (backward)");
		strcpy(ep_to_ep_self, " (forward)");
		break;
	default:
		break;
	}

	//  agent mode
	if (l->cfg->agent_mode) {

		do {	
			ret = e2e_listener_rx(l, l->cfg); 
			if (ret == -EAGAIN)
				continue;
			if (ret == -EBADMSG)
				break;
			if (ret < 0) 
			{
				// refresh agent client state
				e2e_listener_get_states(l);
				if (ret == -ETIME) 
				{	
					if (l->e2e_agent_client_state != E2E_CLIENT_STATE_STOPPED) {
						e2e_agent_log_crit(NULL, true, "Listener timed out...\n");
					}
					listener_state = LISTENER_STATE_TIMEOUT;
				}
				else {
					e2e_agent_log_crit(NULL, true, "e2e_listener_rx failed\n");
					listener_state = LISTENER_STATE_RX_FAILED;
				}
				// handle last packet if any packet was received
				if ((l->stats.rcvPacketCount) != 0) {
					e2e_listener_handle_last_packet(l);}
				else {
					e2e_agent_log_info(NULL, true, ESC_LISTENER, "A Listener stopped ...\n\n");
				}
				break;
			}

			if((l->stats.rcvPacketCount) % l->cfg->update_interval == 0 && (l->stats.rcvPacketCount) !=0)
			{
				// refresh agent client state once per update interval
				e2e_listener_get_states(l);
			}

			ret = e2e_listener_handle_packet(l);
			if (ret == -EAGAIN)
				continue;

		} while (e2e_listener_continue(l));

		// listener job done -> signal e2e_agent_client
		ret = e2e_config_lock_signal(&l->cfg->locks.listener_lock);
		if (ret)
			e2e_agent_log_warn(NULL, true, "agent: Failed to signal e2e_agent_client\n");

		// return ret;
	}

	// others than agent mode
	else {
		do {
			ret = e2e_listener_rx(l, l->cfg);
			if (ret == -EAGAIN)
				continue;
			if (ret == -EBADMSG)
				break;
			if (ret < 0) {
				if (ret == -ETIME) 
				{
					// Listen-Talk: repeat e2e_listener_rx until first packet received
					if ((l->cfg->role == LISTEN_AND_TALK || l->cfg->role == LISTEN) 
						&& l->stats.rcvPacketCount == 0
						&& !l->reset_packet_received)
						{
							continue;
						}
	
					else if (l->stats.lastRcvPacketNr == l->stats.remoteNumberOfPackets){
						if (l->stats.lastRcvPacketNr == 0) 
						{
							e2e_agent_log_crit(NULL, true, "Nothing received, remote talker inactive  ...\n");
							listener_state = LISTENER_STATE_TIMEOUT;
						}
						else 
						{
							e2e_agent_log_info(NULL, true, ESC_LISTENER, "Last sent packet received ...\n");
							listener_state = LISTENER_STATE_STANDBY;
							// reset ETIME
							ret = 0;
						}
					}
					else {
						e2e_agent_log_crit(NULL, true, "Listener timed out. Last received packet # %d.\n", l->stats.rcvPacketnr);
						listener_state = LISTENER_STATE_TIMEOUT;
					}
				}
				else {
					e2e_agent_log_crit(NULL, true, "e2e_listener_rx failed\n");
					listener_state = LISTENER_STATE_RX_FAILED;
				}
				// handle last packet if any packet was received
				if ((l->stats.rcvPacketCount) !=0) {
					e2e_listener_handle_last_packet(l);}
				break;
			}
			ret = e2e_listener_handle_packet(l);
			if (ret == -EAGAIN)
				continue;
			if (ret == -1)
				break;

		} while (e2e_listener_continue(l));
	}

		if (l->cfg->role == LISTEN_AND_TALK && l->stats.rcvPacketCount == 0) {
			/*Signal still waiting talker as no packets were received */
			e2e_config_lock_signal(&l->cfg->locks.dst_mac_lock);
		}

	// listener regularly done if no error state was set in listener loop
	if (listener_state == LISTENER_STATE_RUNNING) {
		listener_state = LISTENER_STATE_DONE;
	}
	return ret;
}

static void *e2e_listener_cleanup(e2e_thread *thread, void *arg)
{
	struct e2e_listener *l;
	int ret;
	bool std_out = true;

	e2e_thread_get_ctx(thread, (void **)&l);

	if (l->cfg->quiet_quiet_mode){
		std_out =false;
	};

	// final signalling 
	ret = e2e_config_lock_signal(&l->cfg->locks.agent_lock);
	if (ret)
		e2e_agent_log_warn(NULL, std_out, "agent: Failed to signal agent client\n");

	if (l->output)
		fclose(l->output);

	e2e_sock_destroy(l->sock);

	if (l->trace_marker_fd != -1)
		close(l->trace_marker_fd);

	free(l);
	return NULL;
}

static void *e2e_listener_entry(e2e_thread *thread, void *arg)
{	
	struct e2e_config *cfg = (struct e2e_config *)arg;
	struct e2e_sock_cfg sock_cfg = {
		.type = cfg->use_udp_socket ? E2E_SOCK_UDP : cfg->use_xdp_socket ? E2E_SOCK_XDP : E2E_SOCK_RAW,
		.raw.protocol = htons(cfg->ethertype),
		.xdp.rx_queue_id = 1,
	};
	struct e2e_listener *l;
	int ret;
	
	l = calloc(1, sizeof(struct e2e_listener));
	if (!l)
		return (void *)(long)-ENOMEM;
	
	e2e_thread_set_ctx(thread, l);
	l->cfg = cfg;
	e2e_listener_trace_init(l);

	ret = e2e_config_validate(cfg);
	if (ret) {
		e2e_agent_log_crit(NULL, true, "Failed to validate config.\n");
		goto err;
	}

	ret = e2e_sock_create(&l->sock, &sock_cfg, cfg);
	if (ret) {
		e2e_agent_log_crit(NULL, true, "Failed to create socket.\n");
		goto err;
	}

	ret = e2e_listener_sock_init(l);
	if (ret) {
		e2e_agent_log_crit(NULL, true, "Failed to init socket.\n");
		goto err;
	}

	/* promiscuous mode not needed for XDP and UDP sockets */
	if (!l->cfg->use_xdp_socket && !l->cfg->use_udp_socket) {
		ret = e2e_listener_sock_promiscuous_mode(l);
		if (ret) {
			e2e_agent_log_crit(NULL, true,
					   "Failed to set promiscuous mode.\n");
			goto err;
		}
	}

	ret = e2e_listener_prepare_outfile(l);
	if (ret) {
		e2e_agent_log_crit(NULL, true,
				   "Failed to prepare output file.\n");
		goto err;
	}

	ret = e2e_listener_prepare_grafana(l);
	if (ret) {
		e2e_agent_log_crit(NULL, true,
				   "Failed to prepare socket for grafana output.\n");
		goto err;
	}

	// listener running
	listener_state = LISTENER_STATE_RUNNING;

	e2e_listener_set_nice();
	e2e_listener_print_info(l);
	e2e_listener_reset_stats(l);
	e2e_listener_write_output(l);

	// refresh agent client state
	e2e_listener_get_states(l);

	// start listener loop
	ret = e2e_listener_loop(l);

err:
	e2e_listener_cleanup(thread, NULL);
	return (void *)(long)ret;
}

int e2e_listener_init(e2e_thread **listener, struct e2e_config *cfg)
{
	struct e2e_thread_config tc = { 0 };

	tc.name = "listener";
	tc.entry = e2e_listener_entry;
	tc.entry_args = cfg;

	tc.cleanup = e2e_listener_cleanup;

	tc.set_sched_prio = cfg->set_recv_prio;
	tc.sched_prio = cfg->pthread_recv_prio;
	tc.set_sched_policy = true;
	tc.sched_policy = SCHED_FIFO;
	tc.set_sched_cpu_mask = cfg->set_cpu_mask;
	tc.sched_cpu_mask = cfg->cpu_mask;

	tc.logger = e2e_agent_log_warnv;
	tc.logger_args = NULL;

	tc.verbose = cfg->verbose;
	
	return e2e_thread_create(listener, &tc);
}

int e2e_listener_get_state(enum e2e_listener_state *state)
{
	*state = listener_state;
	return 0;
}
