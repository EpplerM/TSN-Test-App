// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_BPF_H
#define ES_TO_ES_E2E_BPF_H

#include <stdbool.h>
#include <bpf/libbpf.h>

struct e2e_bpf_cfg {
	int if_idx;
	__u32 xdp_flags;
	char filename[512];
	char progname[32];
	bool reuse_maps;
	bool verbose;

	// internal usage only
	char pin_dir[512];
};

int e2e_bpf_load(struct e2e_bpf_cfg *cfg);
int e2e_bpf_unload(const struct e2e_bpf_cfg *cfg);
int e2e_bpf_update_xsk_map(int idx, int sock_fd);

#endif // ES_TO_ES_E2E_BPF_H
