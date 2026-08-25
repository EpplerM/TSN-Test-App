// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_CONFIG_H
#define ES_TO_ES_E2E_CONFIG_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define VERSION_COUNTER 0x07

enum e2e_role {
	ROLE_UNDEFINED,
	TALK_AND_LISTEN,
	LISTEN_AND_TALK,
	TALK,
	LISTEN
};
enum e2e_mode { SW_BASED, HW_BASED, ETF, APP_LAYER };

struct e2e_reported_ts {
	int32_t origin_sec;
	int32_t origin_nsec;
	int32_t recv_sec;
	int32_t recv_nsec;
};

struct e2e_config_lock {
	pthread_cond_t cond;
	pthread_mutex_t mutex;
};

struct e2e_config {
	struct {
		struct e2e_config_lock cfg_lock;
		struct e2e_config_lock test_lock;
		struct e2e_config_lock agent_lock;	
		struct e2e_config_lock dst_mac_lock;
		struct e2e_config_lock listener_lock;
		struct e2e_config_lock talker_lock;
	} locks;
	enum e2e_role role;
	enum e2e_mode send_mode;
	enum e2e_mode receive_mode;
	bool agent_mode;
	bool verbose;
	bool quiet_mode;
	bool quiet_quiet_mode;
	bool reply_mode;
	int reply_retry;
	bool file_output;
	bool use_ptp_directly;
	char *iface;
	bool interface_set;
	char *training_filename;
	bool dest_mac_set;
	uint8_t dest_mac[6];
	clockid_t clkid;
	bool infinite;
	uint32_t cycle_time;
	uint32_t transmission_offset;
	uint32_t send_window;
	uint32_t missed_packet_rate;
	bool endless_number_of_packets;
	uint32_t number_of_packets;
  	uint32_t burst_size;
  	uint32_t interframe_gap;
	uint32_t number_of_packets_setpoint;
	uint32_t provision_time;
	uint16_t vlan_id;
	uint16_t vlan_prio;
	u_int16_t dscp_prio;
	uint32_t socket_prio;
	uint16_t tx_packet_size;
	int16_t pthread_send_prio;
	bool set_recv_prio;
	int16_t pthread_recv_prio;
	bool set_cpu_mask;
	unsigned cpu_mask;
	char grandmaster_identity[20];
	bool gm_present;
	bool ptp_running;
	uint32_t gm_offset;
	struct e2e_reported_ts *reported_ts;
	uint8_t reported_ts_sz;
	bool packet_received;
	bool use_xdp_socket;
	bool xdp_load_bpf;
	bool xdp_use_epoll;
	int xdp_rx_queue;
	int xdp_flags;
	int xdp_bind_flags;
	int libbpf_flags;
	bool use_udp_socket;
	/* bind address for UDP listener (option --bind-listener-ip) */
	uint8_t bind_listener_ip[4];
	uint16_t bind_listener_port;
	bool bind_listener_ip_given;
	bool dest_ip_set;
	uint8_t dest_ip[4];
	uint16_t dest_port;
	int data_offset_send;
	int data_offset_receive;
	uint8_t rcvSrcMac[6];
	uint16_t ethertype;
	int time_out;
	int update_interval;
	bool verbose_file_output;
	bool round_trip_delay;
	bool grafana;
	int grafana_sampling_period;
	char* influx_server;
	char* influx_bucket;
	bool report;
	char* testcase_name;
	char report_filename[130];
	int recovery_period;
};

int e2e_config_init(struct e2e_config *cfg);
void e2e_config_cleanup(struct e2e_config *cfg);
void e2e_config_set_defaults(struct e2e_config *cfg);

int e2e_config_lock_wait(struct e2e_config_lock *l);
int e2e_config_lock_signal(struct e2e_config_lock *l);

int e2e_config_lock_init(struct e2e_config_lock *l);
void e2e_config_lock_destroy(struct e2e_config_lock *l);

int e2e_config_validate(const struct e2e_config *cfg);

#endif // ES_TO_ES_E2E_CONFIG_H
