// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_COMMON_H
#define ES_TO_ES_E2E_COMMON_H

#include <stdarg.h>
#include <stdio.h>
#include <pthread.h>
#include <errno.h>

#define NSEC_PER_SEC		1000000000
#define LEN_ETH_VLAN_HDR 	22 			// length of Ethernet header (14) + VLAN tag (4) + FCS (4)
#define LEN_UDP_VLAN_HDR	50			// length of Ethernet header (14) + VLAN tag (4) + IP header (20) + UDP header (8) + FCS (4)

#define unlikely(x)	__builtin_expect(!!(x), 0)

static inline int e2e_log(const char *esc, const char *prefix,
			  const char *suffix, const char *fmt, va_list args)
{
	const char *fmt_wrap = "\033[0;%sm%s%s\033[0m%s";
	char fmt_buf[256];
	int ret;


	ret = snprintf(fmt_buf, sizeof(fmt_buf), fmt_wrap, esc, prefix, fmt,
		       suffix);
	if (ret >= sizeof(fmt_buf))
		return -1; // format string truncated

	return vprintf(fmt_buf, args);
}

static inline int e2e_warn(const char *fmt, ...)
{
	const char *fmt_wrap = "\033[0;33mWARNING: %s\033[0m\n";
	char fmt_buf[1024];
	va_list args;
	int ret;

	ret = snprintf(fmt_buf, sizeof(fmt_buf), fmt_wrap, fmt);
	if (ret >= sizeof(fmt_buf))
		return -1; // format string truncated

	va_start(args, fmt);
	ret = e2e_log("33", "WARNING: ", "\n", fmt, args);
	va_end(args);

	return ret;
}

static inline int e2e_info(const char *esc, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = e2e_log(esc, "", "", fmt, args);
	// ret = e2e_log("32", "", "\n", fmt, args);
	va_end(args);

	return ret;
}

static inline int e2e_crit(const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = e2e_log("31", "", "\n", fmt, args);
	va_end(args);

	return ret;
}

#endif // ES_TO_ES_E2E_COMMON_H
