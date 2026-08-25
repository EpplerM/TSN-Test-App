// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_LISTENER_H
#define ES_TO_ES_E2E_LISTENER_H

#include "e2e_config.h"
#include "e2e_thread.h"

typedef struct e2e_listener e2e_listener;

int e2e_listener_init(e2e_thread **l, struct e2e_config *cfg);

enum e2e_listener_state {
	LISTENER_STATE_UNDEFINED,
	LISTENER_STATE_RUNNING,
	LISTENER_STATE_TIMEOUT,
    LISTENER_STATE_RX_FAILED,
	LISTENER_STATE_STANDBY,
	LISTENER_STATE_DONE,
};

int e2e_listener_get_state(enum e2e_listener_state *state);

#endif // ES_TO_ES_E2E_LISTENER_H
