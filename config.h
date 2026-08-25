// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <linux/if_packet.h>
#include <linux/filter.h>
#include <linux/net_tstamp.h>


#define ETH_STREAM_MAC ((uint8_t *)"\x1\x1\x1\x1\x1\x1")
#define ETH_STREAM_TYPE (ETH_P_TSN) // Stream Ethernet Type

// Rx interface Linux ifIndex
#define RX_ETH_INDEX      (2)
/* HINT:
 * interface ifIndex is the first number of the 'ip addr' command in the shell
 * For examle:
 * $ ip addr
 * 1: lo: ...
 *     link ...
 *     ...
 * 2: enp7s0: ...
 *     link ...
 *     ...
 *
 *  In the example the loopback (lo) uses ifIndex = 1 and enp7s0 is using ifIndex = 2
 */

#define TX_PRIORIT (60) // real time priority for talker
#define TX_ETH_TSN_INDEX  (0) // The first TSN capable interface
#define TX_STREAM_NAME "Test-Talker"
#define TX_STREAM_VID     (2)  // Vlan ID
#define TX_STREAM_PCP     (2)  // Vlan priprority
#define TX_STREAM_PKTS4CYC (1)
#define TX_STREAM_PKT_COUNT (TX_STREAM_PKTS4CYC * 4) // Keep memory for 4 cycles

#define PKT_PAYLOAD {0xde, 0xad, 0x0, 0xde, 0xad}

/* For statistics vector */
#define RX_COUNT_STAT_PRINT (5000) // Numbers of packets to receive per statistics print
#define RX_VEC_STEP  (100)  // 100 nanoseconds
#define RX_VEC_COUNT (40)    // Vector count
#define RX_VEC_START (CYCLE_TIME - (RX_VEC_STEP * RX_VEC_COUNT / 2))
