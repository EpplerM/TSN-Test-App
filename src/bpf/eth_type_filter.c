// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: GPL-2.0-only

#include <linux/bpf.h>

#include <bpf/bpf_helpers.h>

#include "eth_type_filter.h"

struct {
	__uint(type, BPF_MAP_TYPE_XSKMAP);
	__uint(key_size, sizeof(int));
	__uint(value_size, sizeof(int));
	__uint(max_entries, 64); /* Assume netdev has no more than 64 queues */
} E2E_BPF_XSK_MAP SEC(".maps");

SEC("xdp_sock")
int xdp_sock_prog(struct xdp_md *ctx)
{
	unsigned char *data = (unsigned char *)(unsigned long)ctx->data;
	__u32 index = ctx->rx_queue_index;
	__u32 offset = 0;

	if (data + sizeof(__u64) > (unsigned char *)(long)ctx->data_end)
		return XDP_PASS;

	/* No EtherType field or VLAN tag => Kernel stack should care */
	if (data + 14  > (unsigned char *)(long)ctx->data_end)
		return XDP_PASS;

	/* If 802.1q VLAN tag is present the EtherType is 4 bytes behind */
	if (data[12] == 0x81 && data[13] == 0x00)
		offset += 4;

	/* No EtherType field => Kernel stack should care */
	if (data + 14 + offset > (unsigned char *)(long)ctx->data_end)
		return XDP_PASS;

	/* Redirect packets of our own EtherType to the XDP socket */
	if (data[12 + offset] == 0xDA && data[13 + offset] == 0xDA)
		return bpf_redirect_map(&E2E_BPF_XSK_MAP, index, XDP_DROP);

	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
