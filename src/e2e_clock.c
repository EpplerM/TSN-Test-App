// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "e2e_agent.h"
#include "e2e_clock.h"
#include "e2e_common.h"

#define CLOCKFD		  3
#define FD_TO_CLOCKID(fd) ((clockid_t)((((unsigned int)~(fd)) << 3) | CLOCKFD))

void e2e_clock_set_gm_id(struct e2e_config *cfg)
{
	int32_t master_offset = INT32_MAX;
	int gotresponse = 0;
	char response[65];
	bool foundptp = 0;
	char *offset;
	FILE *pipe;

	/*
	 * TODO:
	 * Is there no better way to do that? Maybe look into pmc and see if we
	 * could do it here the same way?
	 */
	pipe = popen("sudo pmc -u -b 0 'GET TIME_STATUS_NP'", "r");

parse:
	while (fgets(response, sizeof(response), pipe) != NULL) {
		gotresponse += 1;
		e2e_info(ESC_INFO, "response: %s", response);
		// printf("response: %s", response);
		//  check for sync offset
		if (strstr(response, "master_offset")) {
			foundptp = 1;
			offset = strtok(response, " ");
			offset = strtok(NULL, " ");
			master_offset = atoi(offset);
			cfg->gm_offset = master_offset;
			cfg->ptp_running = true;

			if (master_offset > 100 || master_offset < -100)
				e2e_warn("Master offset is: %d", master_offset);
		}

		// check for sync master
		if (strstr(response, "gmPresent")) {
			if (strstr(response, "false")) {
				// clang-format off
				e2e_warn("No sync master found!");
				e2e_warn("Either this node is sync master, or it is synchronized!");
				// clang-format on
			} else {
				cfg->gm_present = true;
			}
		}

		// get grandmaster identity
		if (strstr(response, "gmIdentity")) {
			offset = strtok(response, " ");
			offset = strtok(NULL, " ");
			snprintf(cfg->grandmaster_identity,
				 sizeof(cfg->grandmaster_identity), "%s",
				 offset);
			e2e_info(ESC_INFO, "GM identity: %s\n", cfg->grandmaster_identity);
		}
	}
	pclose(pipe);

	if (gotresponse == 0) {
		// try to get pmc info again, without sudo this time
		pipe = popen("pmc -u -b 0 'GET TIME_STATUS_NP'", "r");

		// avoid that we try again and again
		gotresponse = 3;

		// restart loop if pmc command needs to be executed without sudo
		goto parse;
	}

	if (!foundptp) {
		e2e_warn("No running PTP instance found!");
	}
}

// reads PTP clock directly from NIC. Needs Erez' Kernel patch
static int e2e_clock_get_ptp(const char *dev, struct timespec *ts)
{
	struct ethtool_ts_info interface_info = { 0 };
	struct ifreq ifr = { 0 };
	clockid_t clkid_ptp;
	char ptp_dev[20];
	int ret;
	int fd;

	fd = socket(PF_UNIX, SOCK_RAW, 0);
	if (fd == -1) {
		perror("failed to open socket");
		return -errno;
	}

	strncpy(ifr.ifr_name, dev, sizeof(ifr.ifr_name) - 1);
	interface_info.cmd = ETHTOOL_GET_TS_INFO;
	ifr.ifr_data = (char *)&interface_info;
	ret = ioctl(fd, SIOCETHTOOL, &ifr);
	if (ret == -1) {
		perror("failed to open ethtool");
		close(fd);
		return -errno;
	}
	close(fd);

	if (interface_info.phc_index < 0) {
		e2e_agent_log_warn(NULL, true, "interface '%s' does not support PTP",
				   dev);
		return -1;
	}

	snprintf(ptp_dev, sizeof(ptp_dev), "/dev/ptp%d",
		 interface_info.phc_index);

	fd = open(ptp_dev, O_RDONLY);
	if (fd == -1) {
		perror("failed to open the PTP device file");
		return -errno;
	}

	clkid_ptp = FD_TO_CLOCKID(fd);
	/* check if clkid is valid */
	ret = clock_gettime(clkid_ptp, ts);
	if (ret == -1) {
		perror("failed to read the PTP clock");
		close(fd);
		return -errno;
	}
	close(fd);

	// printf("Use dynamic clock ID %d of the PTP device \'%s\'\n",
	// clkid_ptp, ptp_dev);
	/**
	 * file descriptor will be closed when application exit
	 * we use a dynamic clock ID assosiate to a file description
	 * net-link will convet the dynamic clock ID
	 * back to the file description and will use it to feth
	 * the posix clock reference.
	 * Application can safly close the file after sending
	 * the setting message.
	 */
	return 0;
}

struct timespec e2e_clock_get(const struct e2e_config *cfg)
{
	struct timespec ts = { 0 };
	int ret;

	if (cfg->use_ptp_directly)
		ret = e2e_clock_get_ptp(cfg->iface, &ts);
	else
		ret = clock_gettime(cfg->clkid, &ts);

	if (ret && cfg->use_ptp_directly)
		perror("e2e_clock_get_ptp");
	else if (ret && !cfg->use_ptp_directly)
		perror("clock_gettime");
	else if (ret && ret == ENODEV)
		e2e_agent_log_warn(NULL, true, "e2e_clock_get: No such device");

	return ts;
}
