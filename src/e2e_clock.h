// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_CLOCK_H
#define ES_TO_ES_E2E_CLOCK_H

#include "e2e_config.h"

void e2e_clock_set_gm_id(struct e2e_config *cfg);
struct timespec e2e_clock_get(const struct e2e_config *cfg);

#endif // ES_TO_ES_E2E_CLOCK_H
