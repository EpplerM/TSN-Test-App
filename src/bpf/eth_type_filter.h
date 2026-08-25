// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: GPL-2.0-only

#ifndef ES_TO_ES_ETH_TYPE_FILTER_H
#define ES_TO_ES_ETH_TYPE_FILTER_H

#define stringify(str)	     stringify_(str)
#define stringify_(str)	     #str

#define E2E_BPF_XSK_MAP	     xsks_map

#define E2E_BPF_XSK_MAP_NAME stringify(E2E_BPF_XSK_MAP)

#endif // ES_TO_ES_ETH_TYPE_FILTER_H
