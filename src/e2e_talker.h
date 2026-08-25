// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_TALKER_H
#define ES_TO_ES_E2E_TALKER_H

#include "e2e_config.h"
#include "e2e_thread.h"

typedef struct e2e_talker e2e_talker;

int e2e_talker_init(e2e_thread **t, struct e2e_config *cfg);

enum e2e_talker_state {
    TALKER_STATE_UNDEFINED,
	TALKER_STATE_RUNNING,
    TALKER_STATE_STOPPED,
    TALKER_STATE_DONE,
};

int e2e_talker_get_state(enum e2e_talker_state *state);

#endif // ES_TO_ES_E2E_TALKER_H
