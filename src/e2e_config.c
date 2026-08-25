#include <xdp/xsk.h>
// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <net/if.h>

#include "e2e_agent.h"

int e2e_config_lock_init(struct e2e_config_lock *l)
{
	int ret;

	ret = pthread_cond_init(&l->cond, NULL);
	if (ret)
		return ret;

	return pthread_mutex_init(&l->mutex, NULL);
}

void e2e_config_lock_destroy(struct e2e_config_lock *l)
{
	pthread_mutex_destroy(&l->mutex);
	pthread_cond_destroy(&l->cond);
}

void e2e_config_set_defaults(struct e2e_config *cfg)
{
	struct e2e_config defaults = {
		.verbose = false,
		.send_mode = SW_BASED,
		.receive_mode = HW_BASED,
		.clkid = CLOCK_TAI,
		.iface = strdup("enp7s0"),
		.interface_set = false,
		.dest_mac = { 0x03, 0x7A, 0xCE, 0x01, 0x02, 0x03 },		// multi-cast mac-addr
		.cycle_time = 1000000, // nanoseconds
		.transmission_offset = 10000, // nanoseconds
		.number_of_packets = 100000,
		.burst_size = 1,
		.interframe_gap = 0,
		.reply_mode = false,
		.reply_retry = 0,
		.quiet_mode = false,
		.quiet_quiet_mode = false,
		.number_of_packets_setpoint = 100000,
		.provision_time = 200000, // send earlier as computed to meet
					    // schedule [ns]
		.vlan_id = 42,
		.vlan_prio = 5,
		.socket_prio = 9,
		.dscp_prio = 46,
		.tx_packet_size = 42, // Payload size without ethernet header, VLAN tag and FCS
				      // Min 42 bytes
		.pthread_recv_prio = 80,
		.pthread_send_prio = 60,
		.xdp_load_bpf = true,
		.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD,
		.xdp_use_epoll = true,
		.dest_ip = {224, 1, 2, 3},
		.dest_port = 7511,
		/* default bind address for UDP listener */
		.bind_listener_ip = {0, 0, 0, 0},
		.bind_listener_port = 7511,
		.bind_listener_ip_given = false,
		.data_offset_send = 18, // data offset for raw/xdp sockets: 2x mac address, VLAN tag, type field 
		.data_offset_receive = 14, // data offset for raw/xdp sockets: 2x mac address, type field 
		.ethertype = 0xDADA,
		.time_out = 1,
		.send_window = 0,
		.verbose_file_output = false,
		.update_interval = 1000,
		.round_trip_delay = false,
		.grafana = false,
		.grafana_sampling_period = 1000,
		.influx_server = strdup("127.0.0.1"),
		.influx_bucket = strdup("DelayValues"),
		.report = false,
		.testcase_name = "",
		.recovery_period = 1,
		.gm_offset = INT32_MAX,
		.gm_present = false,
		.ptp_running = false
	};

	*cfg = defaults;
}

int e2e_config_init(struct e2e_config *cfg)
{
	int ret;

	cfg->reported_ts_sz = 30;
	cfg->reported_ts =
		calloc(1, cfg->reported_ts_sz * sizeof(*cfg->reported_ts));
	if (!cfg->reported_ts)
		return -ENOMEM;

	ret = e2e_config_lock_init(&cfg->locks.cfg_lock);
	if (ret)
		return ret;

	ret = e2e_config_lock_init(&cfg->locks.test_lock);
	if (ret) {
		e2e_config_lock_destroy(&cfg->locks.cfg_lock);
		return ret;
	}

	ret = e2e_config_lock_init(&cfg->locks.agent_lock);
	if (ret) {
		e2e_config_lock_destroy(&cfg->locks.cfg_lock);		
		e2e_config_lock_destroy(&cfg->locks.test_lock);
		return ret;
	}

	ret = e2e_config_lock_init(&cfg->locks.dst_mac_lock);
	if (ret) {
		e2e_config_lock_destroy(&cfg->locks.cfg_lock);		
		e2e_config_lock_destroy(&cfg->locks.test_lock);		
		e2e_config_lock_destroy(&cfg->locks.agent_lock);
		return ret;
	}

	return ret;
}

void e2e_config_cleanup(struct e2e_config *cfg)
{
	free(cfg->iface);
	free(cfg->training_filename);
	free(cfg->reported_ts);
	e2e_config_lock_destroy(&cfg->locks.test_lock);
	e2e_config_lock_destroy(&cfg->locks.cfg_lock);
	e2e_config_lock_destroy(&cfg->locks.agent_lock);
	e2e_config_lock_destroy(&cfg->locks.dst_mac_lock);
}

int e2e_config_lock_wait(struct e2e_config_lock *l)
{
	int ret;

	ret = pthread_mutex_lock(&l->mutex);
	if (ret)
		return ret;

	ret = pthread_cond_wait(&l->cond, &l->mutex);
	if (ret)
		return ret;

	return pthread_mutex_unlock(&l->mutex);
}

int e2e_config_lock_signal(struct e2e_config_lock *l)
{
	int ret;

	ret = pthread_mutex_lock(&l->mutex);
	if (ret)
		return ret;

	pthread_cond_signal(&l->cond);

	return pthread_mutex_unlock(&l->mutex);
}

static int e2e_config_check_role(const struct e2e_config *cfg)
{
	const char *err_role =
		"No mode given. One of -A, -l, -t, --lt or --tl required";

	if (cfg->role == ROLE_UNDEFINED && !cfg->agent_mode) {
		e2e_agent_log_misc(NULL, err_role);
		return -EXIT_FAILURE;
	}

	return 0;
}

static int e2e_config_check_cycle_time(const struct e2e_config *cfg)
{
	uint32_t sum = cfg->provision_time + cfg->send_window;
	const char *err_cycle =
		"Sum of provision time (not offset) and send window should be less than cycle time!\n";

	if (sum > cfg->cycle_time) {
		//e2e_agent_log_misc(NULL, err_cycle);
    	e2e_agent_log_warn(NULL, true, err_cycle);
		//if (!cfg->agent_mode)
		//	return -EXIT_FAILURE;
	}
	
	return 0;
}

static int e2e_config_check_vlan_prio(const struct e2e_config *cfg)
{
	if (cfg->vlan_prio >= 0 && cfg->vlan_prio <= 7)
		return 0;

	e2e_agent_log_misc(NULL,
			   "config: VLAN prio must be in the range [0, 7]");

	return -EINVAL;
}

static int e2e_config_check_vlan_id(const struct e2e_config *cfg)
{
	if (cfg->vlan_id >= 0 && cfg->vlan_id <= 4095)
		return 0;

	e2e_agent_log_misc(NULL,
			   "config: VLAN ID must be in the range [0, 4095]");

	return -EINVAL;
}

static int e2e_config_check_socket_prio(const struct e2e_config *cfg)
{
	if (cfg->socket_prio >= 0 && cfg->socket_prio <= 15)
		return 0;

	e2e_agent_log_misc(NULL,
			   "config: Linux prio must be in the range [0, 15]");

	return -EINVAL;
}

static int e2e_config_check_dscp_prio(const struct e2e_config *cfg)
{
	if (cfg->dscp_prio != 46) {// DSCP prio set by user
		if(cfg->role == LISTEN)
			e2e_agent_log_warn(NULL, true, "config: DSCP prio is ignored for listener");
		if(cfg->use_udp_socket == false)
			e2e_agent_log_warn(NULL, true, "config: DSCP prio is ignored for layer 2 tests");
	}
	return 0;
}

static int e2e_config_check_xdp(const struct e2e_config *cfg)
{
	if (cfg->send_mode == ETF && cfg->use_xdp_socket == true) {
		e2e_agent_log_misc(
			NULL,
			"config: ETF is not available in combination with XDP");
		return -EINVAL;
	}

	/*
	 * --xdp-mode defined but xdp sockets not in use
	 * => Report a failure as the result might be completely unexpected
	 */
	if (cfg->xdp_flags && !cfg->use_xdp_socket) {
		e2e_agent_log_misc(
			NULL,
			"config: --xdp-mode set but not using xdp sockets");
		return -EINVAL;
	}

	return 0;
}

static int e2e_config_check_iface(const struct e2e_config *cfg)
{
	// clang-format off
	char *e1 = "VLAN ID (%d) in interface %s would trigger double tagging\n";
	char *e2 = "Use option -V to adjust VLAN ID to send.\n";
	char *e3 = "Ambiguous VLAN configuration detected!\n";
	char *e4 = "VLAN ID (%d) from interface %s differs from VLAN %d specified in option -V\n";
	char *e5 = "You have specified VLAN ID %d in option -V, but you do not filter VLAN IDs on interface %s!\n";
	char *e6 = "VLAN interface should be used!\n";
	// clang-format on

	if (strchr(cfg->iface, '.')) {
		// interface specification contains VLAN ID
		char iface[IFNAMSIZ];
		char *search = ".";
		char *vlan_str;
		int vlan_id;
		strcpy(iface, cfg->iface);
		vlan_str = strtok(iface, search);
		vlan_str = strtok(NULL, search);
		vlan_id = atoi(vlan_str);

		switch (cfg->role) {
		case TALK:
		case TALK_AND_LISTEN:
		case LISTEN_AND_TALK:
			/* double taggin is only an issue if not using UDP sockets */
			if (!cfg->use_udp_socket)
			{
				e2e_agent_log_misc(NULL, e1, vlan_id, cfg->iface);
				e2e_agent_log_misc(NULL, e2);
				return -EINVAL;
			}
		default:
			break;
		}

		switch (cfg->role) {
		case TALK_AND_LISTEN:
		case LISTEN_AND_TALK:
			if (vlan_id == cfg->vlan_id)
				break;
			e2e_agent_log_misc(NULL, e3);
			e2e_agent_log_misc(NULL, e4, vlan_id, cfg->iface,
					   cfg->vlan_id);
		default:
			break;
		}
	} else {
		// interface specification does not contain VLAN ID
		switch (cfg->role) {
		case LISTEN:
		case LISTEN_AND_TALK:
		case TALK_AND_LISTEN:
			if  (cfg->use_udp_socket)
			{
				e2e_agent_log_warn(NULL, true, e6);
			}
			else
			{
				if (cfg->vlan_id == 42)
					break;
				e2e_agent_log_warn(NULL, true, e5, cfg->vlan_id, cfg->iface);
			}
		default:
			break;
		}
	}

	return 0;
}

int e2e_config_validate(const struct e2e_config *cfg)
{
	int ret;

	if (!cfg)
		return -EINVAL;

	ret = e2e_config_check_role(cfg);
	if (ret)
		return ret;

	ret = e2e_config_check_cycle_time(cfg);
	if (ret)
		return ret;

	ret = e2e_config_check_vlan_prio(cfg);
	if (ret)
		return ret;
		
	ret = e2e_config_check_vlan_id(cfg);
	if (ret)
		return ret;

	ret = e2e_config_check_socket_prio(cfg);
	if (ret)
		return ret;

	ret = e2e_config_check_xdp(cfg);
	if (ret)
		return ret;

	ret = e2e_config_check_dscp_prio(cfg);
	if (ret)
		return ret;

	return e2e_config_check_iface(cfg);
}
