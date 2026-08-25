// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_CMDLINE_H
#define ES_TO_ES_E2E_CMDLINE_H

#include "e2e_config.h"

int e2e_cmdline_parse(struct e2e_config *cfg, int argc, char **argv);
int e2e_cmdline_set_parameters(struct e2e_config *cfg, int opt, char *val,
			       int mode);

#endif // ES_TO_ES_E2E_CMDLINE_H
