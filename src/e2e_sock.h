// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_SOCK_H
#define ES_TO_ES_E2E_SOCK_H

#include "e2e_config.h"

typedef struct e2e_sock e2e_sock;

struct e2e_sock_stats {
	uint32_t invalid_param;
	uint32_t tx_time_missed;
	uint32_t no_buffer_space;
};

enum e2e_sock_type {
	E2E_SOCK_RAW,
	E2E_SOCK_XDP,
	E2E_SOCK_UDP,
};

struct e2e_sock_cfg {
	enum e2e_sock_type type;
	struct {
		int protocol;
	} raw;
	struct {
		int rx_queue_id;
	} xdp;
};

int e2e_sock_create(e2e_sock **s, const struct e2e_sock_cfg *sock_cfg,
		    const struct e2e_config *cfg);
int e2e_sock_destroy(e2e_sock *s);

int e2e_sock_init_sockaddr_tx(e2e_sock *s);
int e2e_sock_init_sockaddr_rx(e2e_sock *s);

int e2e_sock_setopt(e2e_sock *s, int level, int optname, const void *optval,
		    socklen_t len);
int e2e_sock_ioctl(e2e_sock *s, unsigned long int req, void *resp);
int e2e_sock_bind(e2e_sock *s);

ssize_t e2e_sock_sendto(e2e_sock *s, const void *buf, size_t buf_sz, int flags);

int e2e_sock_send(e2e_sock *s, unsigned char *buf, size_t buf_sz,
		  const struct timespec *send_time, uint32_t packet_nr);

int e2e_sock_rx(e2e_sock *s, void **p, ssize_t *p_sz, struct timespec *ts, int time_out, void *arg);

const struct e2e_sock_stats *e2e_sock_get_stats(e2e_sock *s);
int e2e_sock_get_tx_buf(e2e_sock *s, void **buf, size_t *buf_sz);

int e2e_sock_set_tx_hook(e2e_sock *s, void *ctx,
			 int (*hook)(void *ctx, const struct timespec *ts));

#endif // ES_TO_ES_E2E_SOCK_H
