// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xdp/xsk.h>
#include <netdb.h>

#include <linux/if_link.h>

#include "e2e_agent.h"
#include "e2e_cmdline.h"
#include "e2e_common.h"

#define BUFSIZE 40

struct e2e_cmdline_opt_wrapper {
	struct option option;
	char *help;
	char *metavar;
	bool required;
};

static const struct e2e_cmdline_opt_wrapper long_options[] = {

	// clang-format off
	{{ "help", no_argument, NULL, 'h' }, "Show help", false },

	/*
	 * Mode options, at least one of them must be given, checked during
	 * cmdline validation
	 */
	{{ "listener", no_argument, NULL, 'l'}, "Listener mode", "", true},
	{{ "talker", no_argument, NULL, 't'}, "Talker mode", "", true},
	{{ "tl", no_argument, NULL, 1}, "Talk and listen", "", true},
	{{ "lt", no_argument, NULL, 2}, "Listen and talk", "", true},
	{{ "agent", no_argument, NULL, 'A'}, "Agent mode"},

	{{ "infinite", no_argument, NULL, 'I'},
		"Infinite mode (in addition to -l and -lt only)"},
	{{ "interface", required_argument, NULL, 'i'},
		"Interface (default is enp7s0)", "<iface>"},
	{{ "cycle-time", required_argument, NULL, 'c'},
		"cycle time (default is 1000000ns)", "<cycle-time>"},
	{{ "transmission-offset", required_argument, NULL, 'o'},
		"transmission offset [ns] (default is 100000ns)", "<offset>"},
	{{ "send-window", required_argument, NULL, 'w'},
		"send time window [ns], packets are arbitrarily sent in this window starting from offset (default is 0)", "<window>"},
	{{ "missed-packets", required_argument, NULL, 'm'},
		"permillage of missed packets, i.e. packets not sent (default is 0 permill)", "<percentage>"},
	{{ "reply-mode", required_argument, NULL, 15}, 
		"listening talker only sends a packet, if it received one in the previous cycle, or after the given number of missed packets. Default is 0, meaning never."},
	{{ "training-data", required_argument, NULL, 'T'},
		"generate training data for network AI. Will cause send time jitter and packet loss in a predefined pattern.", "<file>"},
	{{ "packets", required_argument, NULL, 'n'},
		"number of test packets (default is 1000)", "<num>"},
  	{{ "burst-size", required_argument, NULL, 'B'},
		"Burst size, i.e. how many packets are sent in one cycle. Default is 1", "<num>"},
  	{{ "interframe-gap", required_argument, NULL, 'G'},
		"Gap between packets within a burst in ns. Default is the minimun IFG according to ethernet standard.", "<num>"},
	{{ "endless", no_argument, NULL, 'N'}, "endless test"},
	{{ "provision-time", required_argument, NULL, 'p'},
		"provision time [ns] (default is 200000ns)", "<ns>"},
	{{ "vlan-priority", required_argument, NULL, 'P'},
		"vlan priority (default is 5)", "<prio>"},
	{{ "vlan-identifier", required_argument, NULL, 'V'},
		"vlan identifier (default is 42)", "<id>"},
	{{ "DSCP-priority", required_argument, NULL, 20}, "The DSCP priority used for Layer 3 packets. Default is 46 (Expedited Forwarding)."},
	{{ "socket-priority", required_argument, NULL, 'Q'},
		"socket priority (default is 9). Endpoint send Queue is selected based on this value and tc map", "<prio>"},
	{{ "verbose", no_argument, NULL, 'v'}, "verbose"},
	{{ "file-output", no_argument, NULL, 'F'}, "file output"},
	{{ "quiet-mode", no_argument, NULL, 'q'}, "quiet mode"},
	{{ "qq", no_argument, NULL, 14}, "very quiet mode"},
	{{ "brutto-frame-length", required_argument, NULL, 'L'},
		"brutto layer 2 ethernet frame length (default is minimum is 64)", "<size>"},
	{{ "dst-mac", required_argument, NULL, 'd'},
		"destination MAC address in format XX:XX:XX:XX:XX:XX. Default is 03:7A:CE:01:02:03 (locally administered multicast address)", "<MAC>"},
	{{ "dst-ip", required_argument, NULL, 'd'},
		"destination IP address [and port] in format X.X.X.X[:X]. Default is 224.1.2.3:7511", "<IP>[:<Port>]"},
	{{ "dst-name", required_argument, NULL, 'd'},
		"destination DNS name (ports not yet supported)", "<DNS name>"},
	{{ "bind-listener-ip", required_argument, NULL, 19},
		"Bind address for UDP listener - format X.X.X.X[:Port]. Default is 0.0.0.0:7511.", "<IP>[:<Port>]"},
	{{ "send-timestamping", required_argument, NULL, 's'},
		"send time stamp setting: HW or SW (default is SW)", "HW|SW"},
	{{ "recv-timestamping", required_argument, NULL, 'r'},
		"recv time stamp setting: HW or SW (default is HW)", "HW|SW"},
	{{ "send-task-prio", required_argument, NULL, 'S'},
		"sender task priority (default is 60)", "<prio>"},
	{{ "recv-task-prio", required_argument, NULL, 'R'},
		"receiver task priority (default is 80)", "<prio>"},
	{{ "cpu-affinity", required_argument, NULL, 'C'},
		"CPU affinity. Default is ~0, which means 'all CPU cores'", "<bitmask>"},
	{{ "erez", no_argument, NULL, 'E'},
		"use Erez's get_ptp() to retrieve current time from PTP clock"},
	{{ "xdp", no_argument, NULL, 'x'}, "Use AF_XDP sockets instead of AF_PACKET"},
	{{ "xdp-mode", required_argument, NULL, 3}, "Set xdp mode. Possible values are: SKB, DRV, HW"},
	{{ "zero-copy", no_argument, NULL, 'z'}, "Force AF_XDP into zero-copy mode"},
	{{ "xdp-copy", no_argument, NULL, 4}, "Force AF_XDP into copy mode"},
	{{ "xdp-no-bpf", no_argument, NULL, 5}, "Skip loading the bpf program"},
	{{ "xdp-no-epoll", no_argument, NULL, 6}, "Use sendto() for TX queue kick"},
	{{ "udp", no_argument, NULL, 'u'}, "Use AF_INET socket with UDP instead of AF_PACKET"},
	{{ "ethertype", required_argument, NULL, 7}, "Ethertype of test packets. Default is 0xDADA"},
	{{ "timeout", required_argument, NULL, 8}, "The time after listener gives up. Default is 1s"},
	{{ "verbose-file", no_argument, NULL, 9}, "Verbose file output, containing send time for each packet"},
	{{ "round-trip", no_argument, NULL, 10}, "Round trip delay measurement. Only available in role \'talk and listen\' (lt)"},
	{{ "grafana", required_argument, NULL, 11}, "Write output to influxDB to enable Grafana visualization, with given sampling period. Default is 1000. Only available for listeners"},
	{{ "influx-server", required_argument, NULL, 12}, "IP address of the server hosting the influx DB for Grafana output. Default is '127.0.0.1' (localhost)."},
	{{ "influx-bucket", required_argument, NULL, 13}, "Name of the bucket in the influx DB used for Grafana output. Default is 'DelayValues'"},
	{{ "report", no_argument, NULL, 16}, "Enable file reporting."},
	{{ "testcase", required_argument, NULL, 17}, "Enable file reporting and set a testcase name for the report file."},
	{{ "recovery-period", required_argument, NULL, 18}, "The number of received packets, after which the count of missed packets is reset to zero. Default is 1."},
	{{ 0, 0, NULL, 0 }, NULL, false },
	// clang-format on

};

static void e2e_cmdline_print_options(bool required)
{
	int i, pos;
	char buf[BUFSIZE];

	for (i = 0; long_options[i].option.name != 0; i++) {
		if (long_options[i].required != required)
			continue;

		if (long_options[i].option.val > 64) /* ord('A') = 65 */
			printf(" -%c,", long_options[i].option.val);
		else
			printf("    ");

		pos = snprintf(buf, BUFSIZE, " --%s",
			       long_options[i].option.name);
		if (long_options[i].metavar)
			snprintf(&buf[pos], BUFSIZE - pos, " %s",
				 long_options[i].metavar);
		printf("%-35s", buf);
		printf("  %s", long_options[i].help);
		printf("\n");
	}
}

static void e2e_cmdline_usage(const char *prog_name)
{
	printf("Usage: %s [options]\n", prog_name);

	printf("Required options (one of them is sufficient):\n");
	e2e_cmdline_print_options(true);
	printf("\n");
	printf("Other options:\n");
	e2e_cmdline_print_options(false);
	printf("\n");
}

static int
option_wrappers_to_options(const struct e2e_cmdline_opt_wrapper *wrapper,
			   struct option **options)
{
	int i, num;
	struct option *new_options;

	for (i = 0; wrapper[i].option.name != 0; i++)
		continue;

	num = i;

	new_options = malloc(sizeof(struct option) * num);
	if (!new_options)
		return -1;

	for (i = 0; i < num; i++)
		memcpy(&new_options[i], &wrapper[i], sizeof(struct option));

	*options = new_options;

	return 0;
}

static inline void err(struct e2e_config *cfg)
{
	e2e_config_cleanup(cfg);
	exit(EXIT_FAILURE);
}

int e2e_cmdline_set_parameters(struct e2e_config *cfg, int opt, char *val,
			       int mode)
{
	int ret = 0;
	// mode=0 is for terminal config
	// mode=1 is for socket config

	switch (opt) {
	case 's':
		if (cfg->role == LISTEN) {
			e2e_warn("send mode options are ignored for listener.");
			break;
		}

		if (strcmp(val, "HW") == 0) {
			cfg->send_mode = HW_BASED;
			e2e_info(ESC_INFO, "Send mode set to HW-based.\n");			
		} else if (strcmp(val, "AL") == 0) {
			cfg->send_mode = SW_BASED;
			e2e_info(ESC_INFO, "Send mode set to application-layer-based.\n");
		} else if (strcmp(val, "ETF") == 0) {
			cfg->send_mode = ETF;
			e2e_info(ESC_INFO, "Send mode set to ETF offloading.\n");
		} else {
			e2e_warn("Wrong send mode. Must be AL, HW or ETF");
			if (mode == 0)
				err(cfg);
		}
		break;
	case 'r':
		if (cfg->role == TALK) {
			e2e_warn("receive mode options are ignored for talker");
			break;
		}

		if (strcmp(val, "HW") == 0) {
			cfg->receive_mode = HW_BASED;
			e2e_info(ESC_INFO, "Receive mode set to HW-based.\n");
		} else if (strcmp(val, "SW") == 0) {
			cfg->receive_mode = SW_BASED;
			e2e_info(ESC_INFO, "Receive mode set to SW-based.\n");
		} else if (strcmp(val, "AL") == 0) {
			cfg->receive_mode = APP_LAYER;
			e2e_info(ESC_INFO, "Receive mode set to application-layer-based.\n");
		} else {
			e2e_warn("Wrong receive mode. Must be AL, SW or HW.");
			if (mode == 0)
				err(cfg);
		}
		break;
	case 'c':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -c given. Exiting.\n");
			err(cfg);
		}
		cfg->cycle_time = atoi(val);
		//todo: not 0
		cfg->update_interval=1000000000/cfg->cycle_time*cfg->burst_size;
		e2e_info(ESC_INFO, "Cycle time:\t%d\n", cfg->cycle_time);
		break;
	case 'o':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -o given. Exiting.\n");
			err(cfg);
		}
		cfg->transmission_offset = atoi(val);
		if (cfg->role == LISTEN)
			e2e_warn("Transmission offset is ignored for listener.");
		else
			e2e_info(ESC_INFO, "Transmission offset:\t%d\n",
			       cfg->transmission_offset);
		break;
	case 'w':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -w given. Exiting.\n");
			err(cfg);
		}
		cfg->send_window = atoi(val);

		if (cfg->role == LISTEN)
			e2e_warn("Send window is ignored for listener.");
		else
			e2e_info(ESC_INFO, "Send window:\t%d\n", cfg->send_window);
		break;
	case 'm':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -m given. Exiting.\n");
			err(cfg);
		}
		cfg->missed_packet_rate = atoi(val);

		if (cfg->role == LISTEN)
			e2e_warn("-m is ignored for listener");
		else
			e2e_info(ESC_INFO, "Missed packet rate:\t%d percent\n",
			       cfg->missed_packet_rate);
		break;
	case 'n':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -n given. Exiting.\n");
			err(cfg);
		}
		cfg->number_of_packets = atoi(val);
		cfg->number_of_packets_setpoint = cfg->number_of_packets;

		if (cfg->role == LISTEN)
			e2e_warn(
				"number of test packets is ignored for listener");
		else
			e2e_info(ESC_INFO, "number of test packets:\t%d\n",
			       cfg->number_of_packets);
		break;
	case 'B':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -B given. Exiting.\n");
			err(cfg);
		}
		cfg->burst_size = atoi(val);
		cfg->update_interval=1000000000/cfg->cycle_time*cfg->burst_size;
		if (cfg->role == LISTEN)
			e2e_warn(
				"burst size is ignored for listener");
		else
			e2e_info(ESC_INFO, "burst size:\t%d\n",
			       cfg->burst_size);
		break;
	case 'G':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -G given. Exiting.\n");
			err(cfg);
		}
		cfg->interframe_gap = atoi(val);

		if (cfg->role == LISTEN)
			e2e_warn(
				"interframe gap is ignored for listener");
		else
			e2e_info(ESC_INFO, "burst size:\t%d\n",
			       cfg->interframe_gap);
		break;
	case 'p':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -p given. Exiting.\n");
			err(cfg);
		}
		cfg->provision_time = atoi(val);

		e2e_info(ESC_INFO, "provision time:\t%d\n",
			       cfg->provision_time);

		break;
	case 'P':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -P given. Exiting.\n");
			err(cfg);
		}
		cfg->vlan_prio = atoi(val);

		if (cfg->role == LISTEN)
			e2e_warn("VLAN priority is ignored for listener");
		else
			e2e_info(ESC_INFO, "vlan priority:\t%d\n", cfg->vlan_prio);
		break;
	case 'Q':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -Q given. Exiting.\n");
			err(cfg);
		}
		cfg->socket_prio = atoi(optarg);

		if (cfg->role == LISTEN)
			e2e_warn("Socket priority is ignored for listener");
		else
			e2e_info(ESC_INFO, "Socket priority:\t%d\n", cfg->socket_prio);
		break;
	case 'V':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -V given. Exiting.\n");
			err(cfg);
		}
		cfg->vlan_id = atoi(val);

		if (cfg->role == LISTEN)
			e2e_warn(
				"VLAN ID is ignored for listener. Use vlan interface, e.g. enp7s0.3, to filter on specific VLAN!");
		else
			e2e_info(ESC_INFO, "vlan identifier:\t%d\n", cfg->vlan_id);
		break;
	case 'L':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -L given. Exiting.\n");
			err(cfg);
		}
		if (cfg->use_udp_socket)
		{
			cfg->tx_packet_size = atoi(val); 	//in UDP mode, we can directly use the frame length desired by the user. 
												//the INET socket cares about all headers and computes teh remaining payload length.
		}
		else
		{
			cfg->tx_packet_size = atoi(val) - LEN_ETH_VLAN_HDR; //in layer 2 mode, we substract the layer 2 header length from the frame length.
												//with this we get the payload length, to which we will add the header lateron.
												//the raw socket does not care about any header.
		}


		if (cfg->role == LISTEN)
			e2e_warn("packet size is ignored for listener");
		else
			e2e_info(ESC_INFO, "packet length (brutto):\t%d\n",
			       cfg->tx_packet_size);
		break;
	case 'd':
		if (val == NULL || val[0] == '-') {
			e2e_crit("No valid argument for Option -d given. Exiting.\n");
			err(cfg);
		}
		if (cfg->role == LISTEN && !cfg->use_udp_socket) {
			e2e_warn(
				"destination MAC address is ignored for listener");
			break;
		}

		uint8_t mac[6] = { 0 };
		uint8_t ip[4] = { 0 };
		uint16_t port = 0;
		char dnsname[255] = "";
		struct addrinfo hints;
    	struct addrinfo *result;

		ret = sscanf(val, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx", &mac[0],
			     &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
		if (ret == 6) {
			memcpy(cfg->dest_mac, &mac, sizeof(mac));
			cfg->dest_mac_set = true;
			ret = 0;
			e2e_info(ESC_INFO, "Destination address used: %hhx:%hhx:%hhx:%hhx:%hhx:%hhx\n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
			break;
		}
		
		/* check for destination ip/ip+port */
		if (cfg->use_udp_socket)
		{
			if ((ret = sscanf(val, "%hhu.%hhu.%hhu.%hhu:%hu", &ip[0],
					&ip[1], &ip[2], &ip[3], &port) == 5) ||
					((ret = sscanf(optarg, "%hhu.%hhu.%hhu.%hhu", &ip[0],
					&ip[1], &ip[2], &ip[3]) == 4)))
			{
				memcpy(cfg->dest_ip, ip, sizeof(ip));
				if (port != 0)
				{
					cfg->dest_port = port;
					e2e_info(ESC_INFO, "Destination address used: %hhu.%hhu.%hhu.%hhu:%hu\n", ip[0],ip[1],ip[2],ip[3],port);
				}
				else
					e2e_info(ESC_INFO, "Destination address used: %hhu.%hhu.%hhu.%hhu\n", ip[0],ip[1],ip[2],ip[3]);
				cfg->dest_ip_set = true;
				ret = 0;
				break;
			}
			else
			{
				/* check if it's a DNS address */
				if ((ret = sscanf(val, "%s:%hu", dnsname, &port) == 2) ||
					(ret = sscanf(val, "%s", dnsname) == 1))

				{

					memset(&hints, 0, sizeof(struct addrinfo));
					hints.ai_family = AF_INET;    
					hints.ai_socktype = 0; 
					hints.ai_flags = 0;    
					hints.ai_protocol = 0;          
					hints.ai_canonname = NULL;
					hints.ai_addr = NULL;
					hints.ai_next = NULL;

					ret = getaddrinfo(dnsname, NULL, &hints, &result);
					if (ret) {
						e2e_agent_log_crit(NULL, true, "Address resolution failed with error %d (%s). Info: ports not supported yet\n", ret, val);
					}

					/* endianess swap needed? */
					memcpy(cfg->dest_ip, &(((struct sockaddr_in*)(result->ai_addr))->sin_addr.s_addr), sizeof(cfg->dest_ip)); 

					freeaddrinfo(result);


					if (port != 0)
					{
						cfg->dest_port = port;
						e2e_info(ESC_INFO, "Destination address after resolution used\n: %hhu.%hhu.%hhu.%hhu:%hu\n", cfg->dest_ip[0],cfg->dest_ip[1],cfg->dest_ip[2],cfg->dest_ip[3],port);
					}
					else 
					{
						e2e_info(ESC_INFO, "Destination address after resolution used\n: %hhu.%hhu.%hhu.%hhu\n", cfg->dest_ip[0],cfg->dest_ip[1],cfg->dest_ip[2],cfg->dest_ip[3]);
					}

					cfg->dest_ip_set = true;
					ret = 0;
					break;
				}
			}
		}
		e2e_warn("Invalid address given (%s). -d will be ignored.\n", val);
		break;
	default:
		if (mode == 0)
			ret = 1;
	}

	return ret;
}

int e2e_cmdline_parse(struct e2e_config *cfg, int argc, char **argv)
{
	struct option *options;
	int ret = 0;
	int longidx;
	int flags;
	int opt;
	
	if (option_wrappers_to_options(long_options, &options)) {
		fprintf(stderr, "Unable to malloc()\n");
		exit(EXIT_FAILURE);
	}

	for (;;) {
		opt = getopt_long(
			argc, argv,
			"lthAIi:c:o:w:m:T:n:B:G:Np:P:V:Q:vFqL:d:s:r:S:R:C:Exzu",
			options, &longidx);
		if (opt < 0)
			break;

		switch (opt) {
		case 1: // --tl
			cfg->role = TALK_AND_LISTEN;
			break;
		case 2: // --lt
			cfg->role = LISTEN_AND_TALK;
			break;
		case 't':
			cfg->role = TALK;
			break;
		case 'l':
			cfg->role = LISTEN;
			break;
		case 'A':
			cfg->agent_mode = true;
			break;
		case 'i':
			free(cfg->iface);
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option -i given. Exiting.\n");
				err(cfg);
			}
			cfg->iface = strdup(optarg);
			cfg->interface_set = true;
			e2e_info(ESC_INFO, "Interface set to: %s.\n", cfg->iface);

			break;
		case 'F':
			e2e_info(ESC_INFO, "File output enabled.\n");
			cfg->file_output = true;
			break;
		case 'I':
			if (cfg->role == TALK || cfg->role == TALK_AND_LISTEN)
				e2e_warn(
					"Infinite mode is ignored for talker.");
			else {
				e2e_info(ESC_INFO, "Infinite mode. Use ctrl-C to exit.\n");
				cfg->infinite = true;
			}
			break;

		case 'T':
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option -T given. Exiting.\n");
				err(cfg);
			}
			e2e_warn(
				"generation of training data for network AI enabled");
			cfg->training_filename = strdup(optarg);
			e2e_info(ESC_INFO, "Pattern definition file set to: %s.\n",
			       cfg->training_filename);
			break;
		case 'N':
			if (cfg->role == LISTEN)
				e2e_warn(
					"number of test packets is ignored for listener");
			else {
				e2e_info(ESC_INFO, "Number of test packets set to infinite.\n");
				cfg->endless_number_of_packets = true;
			}
			break;
		case 'E':
			cfg->use_ptp_directly = true;
			cfg->clkid = -37;
#ifndef ENABLE_CLASSIC_SLEEP
			if (cfg->role == TALK || cfg->role == TALK_AND_LISTEN) {
				e2e_info(ESC_INFO, 
				"Using PTP clock with ID -37 directly. \033[0;31mMust be used with CLASSIC_SLEEP only! Exiting.\033[0m\n");
				err(cfg);
			} else {
				e2e_info(ESC_INFO, "Using PTP clock with ID -37 directly.\n");
			}
#else
			e2e_info(ESC_INFO, "Using PTP clock with ID -37 directly.\033[0m\n");
#endif
			break;
		case 'v':
			cfg->verbose = true;
			e2e_info(ESC_INFO, "Verbose output enabled.\n");
			break;
		case 'q':
			cfg->quiet_mode = true;
			e2e_info(ESC_INFO, "Quiet mode enabled.\n");
			break;
		case 'L':
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option -L given. Exiting.\n");
				err(cfg);
			}			
			if (cfg->use_udp_socket)
			{
				cfg->tx_packet_size = atoi(optarg); //header length must not be substracted in UDP mode, becase it is added by UDP stack
			}
			else
			{
				cfg->tx_packet_size = atoi(optarg) - LEN_ETH_VLAN_HDR;
			}

			if (cfg->role == LISTEN)
				e2e_warn("frame length is ignored for listener");
			else
			{
				e2e_info(ESC_INFO, "frame length set to %d Bytes.\n",
					   atoi(optarg));
				if (atoi(optarg) < 64)
					e2e_warn("frame size %d Bytes is shorter than minimal layer 2 ethernet frame (64 Bytes). Might result in unexpected behavior!",
						atoi(optarg));
			}
			break;
		case 'S':
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option -S given. Exiting.\n");
				err(cfg);
			}		
			cfg->pthread_send_prio = atoi(optarg);
			e2e_info(ESC_INFO, "Setting send task priority to %d.\n",
			       cfg->pthread_send_prio);
			break;
		case 'R':
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option -R given. Exiting.\n");
				err(cfg);
			}		
			cfg->pthread_recv_prio = atoi(optarg);
			cfg->set_recv_prio = true;
			e2e_info(ESC_INFO, "Setting receive task priority to %d.\n",
			       cfg->pthread_recv_prio);
			break;
		case 'C':
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option -C given. Exiting.\n");
				err(cfg);
			}		
			cfg->cpu_mask = atoi(optarg);
			cfg->set_cpu_mask = true;
			e2e_info(ESC_INFO, "Setting CPU affinity to CPU %d.\n",
			       cfg->cpu_mask);
			break;
		case 'x':
			cfg->use_xdp_socket = true;
			e2e_info(ESC_INFO, "Using AF_XDP sockets.\n");				
			break;
		case 3: // --xdp-mode
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --xdp-mode given. Exiting.\n");
				err(cfg);
			}			
			flags = 0;
			if (!strcmp(optarg, "SKB")) {
				flags |= XDP_FLAGS_SKB_MODE;
				e2e_info(ESC_INFO, "Using xdp skb mode\n");
			} else if (!strcmp(optarg, "DRV")) {
				flags |= XDP_FLAGS_DRV_MODE;
				e2e_info(ESC_INFO, "Using xdp driver mode\n");
			} else if (!strcmp(optarg, "HW")) {
				flags |= XDP_FLAGS_HW_MODE;
				e2e_info(ESC_INFO, "Using xdp hardware mode\n");
			} else
				e2e_warn("Invalid --xdp-mode. Ignoring.");

			cfg->xdp_flags = flags;
			break;
		case 'z':
			cfg->xdp_bind_flags &= ~XDP_COPY; /* clear copy flag */
			cfg->xdp_bind_flags |= XDP_ZEROCOPY;
			e2e_info(ESC_INFO, "xdp: Using zero copy mode.\n");
			break;
		case 4: // --xdp-copy
			cfg->xdp_bind_flags &= ~XDP_ZEROCOPY;
			cfg->xdp_bind_flags |= XDP_COPY;
			e2e_info(ESC_INFO, "xdp: Using copy mode.\n");
			break;
		case 5: // --xdp-no-bpf
			cfg->xdp_load_bpf = false;
			e2e_info(ESC_INFO, "xdp: Skipping bpf program load.\n");
			break;
		case 6: // --xdp-no-epoll
			cfg->xdp_use_epoll = false;
			e2e_info(ESC_INFO, "xdp: No epoll.\n");
			break;
		case 'u':
			cfg->use_udp_socket = true;
			e2e_info(ESC_INFO, "Using UDP sockets.\n");

			/* change some default values for UDP */
			cfg->data_offset_send = 0; 
			cfg->data_offset_receive = 0;
			if (cfg->tx_packet_size == 42)
				cfg->tx_packet_size = 64; /* Minimum Ethernet frame size */
			break;
		case 7: // ethertype
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --ethertype. Exiting.\n");
				err(cfg);
			}	
			cfg->ethertype = __bswap_16(strtol(optarg, NULL, 16));
			e2e_info(ESC_INFO, "ethertype set to:\t%x\n",
			       __bswap_16(cfg->ethertype));
			break;
		case 8: // time_out
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --timeout given. Exiting.\n");
				err(cfg);
			}			
			cfg->time_out = atoi(optarg);
			e2e_info(ESC_INFO, "Setting timeout to %d seconds.\n",
			       cfg->time_out);
			if (cfg->role == TALK)
				e2e_warn("timeout is ignored for talker");
			break;
		case 9: // verbose file output
			e2e_info(ESC_INFO, "File output set to verbose.\n");
			cfg->file_output = true;
			cfg->verbose_file_output = true;
			break;
		case 10: // round trp delay measurement
			if(cfg->role==TALK_AND_LISTEN) {
				e2e_info(ESC_INFO, "Round trip delay measurement activated.\n");
				cfg->round_trip_delay = true;
			} else {
				e2e_warn("Round trip delay measurement is only possible in mode \'talk and listen\' (tl)!");
			}
			break;
		case 11: // Grafana / influxDB output
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid sampling period for Option --grafana given. Exiting.\n");
				err(cfg);
			}			
			if(cfg->role==TALK_AND_LISTEN || cfg->role==LISTEN || cfg->agent_mode) {
				cfg->grafana = true;
				cfg->grafana_sampling_period = atoi(optarg);
				if(cfg->grafana_sampling_period == 0)
					cfg->grafana_sampling_period = 1000;
				e2e_info(ESC_INFO, "Grafana output activated. Sampling period set to %d\n", cfg->grafana_sampling_period);
			} else {
				e2e_warn("Grafana output is only possible in mode \'talk and listen\' (tl) or \'listen\' (l)!");
			}
			break;
		case 12: // Grafana / influxDB server
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --influx-server given. Exiting.\n");
				err(cfg);
			}			
			if(cfg->role==TALK_AND_LISTEN || cfg->role==LISTEN || cfg->agent_mode) {
				free(cfg->influx_server);
				cfg->influx_server = strdup(optarg);
				e2e_info(ESC_INFO, "Influx DB server address set to %s\n", cfg->influx_server);
			}
			break;
		case 13: // Grafana / influxDB bucket
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --influx-bucket given. Exiting.\n");
				err(cfg);
			}			
			if(cfg->role==TALK_AND_LISTEN || cfg->role==LISTEN || cfg->agent_mode) {
				free(cfg->influx_bucket);
				cfg->influx_bucket = strdup(optarg);
				e2e_info(ESC_INFO, "Influx DB bucket name set to %s\n", cfg->influx_bucket);
			}
			break;
		case 14:
			cfg->quiet_mode = true;
			cfg->quiet_quiet_mode = true;
			e2e_info(ESC_INFO, "Very quiet mode enabled.\n");
			break;
		case 15:
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --reply-mode given. Exiting.\n");
				err(cfg);
			}				
			cfg->reply_mode = true;
			cfg->reply_retry = atoi(optarg);
			if(cfg->reply_retry > 0) {
				e2e_info(ESC_INFO, "Reply mode enabled. Sending reply packet after %d not received packets.\n", cfg->reply_retry);
			} else {
				e2e_info(ESC_INFO, "Reply mode enabled. Never sending a reply packet while not receiving packets.\n");
			}
			break;
		case 16:
			cfg->report = true;
			e2e_info(ESC_INFO, "Report file output enabled.\n");
			break;
		case 17:
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --testcase given. Exiting.\n");
				err(cfg);
			}			
			cfg->report = true;
			cfg->testcase_name = strdup(optarg);
			e2e_info(ESC_INFO, "Report file output enabled. Testcase name set to %s\n", cfg->testcase_name);
			break;
		case 18:
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --recovery-period given. Exiting.\n");
				err(cfg);
			}			
			cfg->recovery_period = atoi(optarg);
			e2e_info(ESC_INFO, "Recovery period set to %d packets.\n", cfg->recovery_period);
			break;
		case 19: /* --bind-listener-ip */
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --bind-listener-ip given. Exiting.\n");
				err(cfg);
			}
			{
				uint8_t ip[4] = {0};
				uint16_t port = 0;
				int r;
				/* try IP:PORT first */
				r = sscanf(optarg, "%hhu.%hhu.%hhu.%hhu:%hu", &ip[0], &ip[1], &ip[2], &ip[3], &port);
				if (r == 5) {
					memcpy(cfg->bind_listener_ip, ip, sizeof(ip));
					cfg->bind_listener_port = port;
					cfg->bind_listener_ip_given = true;
					e2e_info(ESC_INFO, "Bind listener address set to: %hhu.%hhu.%hhu.%hhu:%hu\n", ip[0], ip[1], ip[2], ip[3], port);
					break;
				}
				r = sscanf(optarg, "%hhu.%hhu.%hhu.%hhu", &ip[0], &ip[1], &ip[2], &ip[3]);
				if (r == 4) {
					memcpy(cfg->bind_listener_ip, ip, sizeof(ip));
					cfg->bind_listener_port = 7511;
					cfg->bind_listener_ip_given = true;
					e2e_info(ESC_INFO, "Bind listener address set to: %hhu.%hhu.%hhu.%hhu:%hu\n", ip[0], ip[1], ip[2], ip[3], cfg->bind_listener_port);
					break;
				}
				e2e_warn("Invalid bind-listener-ip given (%s). Option ignored.\n", optarg);
				break;
			}
		case 20:
			if (optarg == NULL || optarg[0] == '-') {
				e2e_crit("No valid argument for Option --DSCP-priority given. Exiting.\n");
				err(cfg);
			}			
			cfg->dscp_prio = atoi(optarg);
			if (cfg->dscp_prio < 0 || cfg->dscp_prio > 63) {
				e2e_warn("DSCP priority must be a value between 0 and 63, not %d. Ignoring.", cfg->dscp_prio);
				cfg->dscp_prio = 46;
			}
			e2e_info(ESC_INFO, "DSCP priority set to %d.\n", cfg->dscp_prio);
			break;
		default:
			ret = e2e_cmdline_set_parameters(cfg, opt, optarg, 0);
			if (ret)
				e2e_cmdline_usage(argv[0]);
			break;
		}
	}

	free(options);

	/* If user supplied --bind-listener-ip but did not enable UDP sockets, warn.
	 * Check here after parsing so option order doesn't affect the warning.
	 */
	if (cfg->bind_listener_ip_given && !cfg->use_udp_socket) {
		e2e_warn("--bind-listener-ip provided but not using UDP sockets; option will be ignored.");
	}

	return ret;
}
