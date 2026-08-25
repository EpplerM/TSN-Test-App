// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_AGENT_H
#define ES_TO_ES_E2E_AGENT_H

#include <stdarg.h>

#include "e2e_config.h"
#include "e2e_thread.h"
#include "e2e_talker.h"
#include "e2e_listener.h"


#define ESC_TALKER "32"		    // esc green 
#define ESC_LISTENER "36"	    // esc blue
#define ESC_INFO "0"            // reset to standard 
#define ESC_CRIT "31"           // esc red

typedef struct e2e_agent e2e_agent;

int e2e_agent_create(e2e_agent **a, struct e2e_config *cfg);
int e2e_agent_destroy(e2e_agent *a);
int e2e_agent_stop(e2e_agent *a);

int e2e_agent_set_threads(e2e_agent *a, e2e_thread *lt, e2e_thread *tt);

enum e2e_agent_client_state {
	E2E_CLIENT_STATE_UNDEFINED,
	E2E_CLIENT_STATE_RUNNING,
	E2E_CLIENT_STATE_STOPPED,
};

int e2e_agent_log_warn(void *ctx, bool to_stdout, const char *fmt, ...);
int e2e_agent_log_warnv(void *ctx, const char *fmt, va_list args);
int e2e_agent_log_misc(void *ctx, const char *fmt, ...);
int e2e_agent_log_crit(void *ctx, bool to_stdout, const char *fmt, ...);
int e2e_agent_log_info(void *ctx, bool to_stdout, const char *esc, const char *fmt, ...);
int e2e_agent_log_only(void *ctx, const char *fmt, ...);
int e2e_agent_log_report(const char *fmt, ...);
int e2e_agent_client_get_state(enum e2e_agent_client_state *state);
bool e2e_agent_running(e2e_agent *a);


#endif // ES_TO_ES_E2E_AGENT_H
