// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#ifndef ES_TO_ES_E2E_THREAD_H
#define ES_TO_ES_E2E_THREAD_H

#include <stdbool.h>

typedef struct e2e_thread e2e_thread;

struct e2e_thread_config {
	char *name;
	/**
	 * The main thread entry function, required
	 */
	void *(*entry)(e2e_thread *t, void *);

	/**
	 * The second argument passed to the main thread entry function, optional
	 */
	void *entry_args;


	void *(*cleanup)(e2e_thread *t, void *arg); // Optional: The thread cleanup function
	void *cleanup_args; // Optional Second argument for cleanup

	int (*logger)(void *ctx, const char *fmt, va_list args);
	void *logger_args; // Optional: First argument for logger

	bool set_sched_prio;
	int sched_prio;

	bool set_sched_policy;
	int sched_policy;

	bool set_sched_cpu_mask;
	unsigned sched_cpu_mask;

	bool verbose;
};

enum e2e_thread_state {
	E2E_T_STATE_UNDEFINED,
	E2E_T_STATE_RUNNING,
	E2E_T_STATE_STOPPED,
};

int e2e_thread_create(e2e_thread **t, const struct e2e_thread_config *cfg);
int e2e_thread_destroy(e2e_thread *t);
int e2e_thread_start(e2e_thread *t);
int e2e_thread_stop(e2e_thread *t);
int e2e_thread_join(e2e_thread *t, void **retval);
int e2e_thread_get_retval(e2e_thread *t, void **retval);
int e2e_thread_set_ctx(e2e_thread *t, void *ctx);
int e2e_thread_get_ctx(e2e_thread *t, void **ctx);

#endif // ES_TO_ES_E2E_THREAD_H
