// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "e2e_common.h"
#include "e2e_thread.h"
#include "e2e_agent.h"

typedef struct e2e_thread {
	struct e2e_thread_config cfg;
	enum e2e_thread_state state;
	pthread_t main_thread;
	pthread_t signal_thread;
	int pipe_fds[2];
	void *retval;
	void *ctx;
} e2e_thread;

#define ctor			 __attribute__((constructor))
#define dtor			 __attribute__((destructor))

#define E2E_THREAD_TERM_SIGNAL	 SIGUSR1
#define E2E_THREAD_CUSTOM_SIGNAL SIGUSR2

static pthread_key_t thread_handle;
// struct threads_collected threads = {NULL};

/**
 * Some notes about the thread control flow:
 *
 * e2e_thread_create: Prepare thread
 * e2e_thread_start: Start the main as well as the signal handling thread
 * e2e_thread_stop:
 *     - Send E2E_THREAD_TERM_SIGNAL to the main thread
 *     - Main thread writes into its end of the pipe and wakes the signal
 *       handling thread. We can't handle the complete shutdown in a signal
 *       handler.
 *     - Main thread exit
 *     - Signal thread will care about calling cleanup
 *     - Signal thread end
 *     - Return to caller
 * e2e_thread_destroy: Cleanup
 */

static void ctor e2e_thread_ctor()
{
	int ret;

	ret = pthread_key_create(&thread_handle, NULL);
	if (ret)
		e2e_warn("thread: failed to create thread specific handle");
}

static void dtor e2e_thread_dtor()
{
	int ret;

	ret = pthread_key_delete(thread_handle);
	if (ret)
		e2e_warn("thread: failed to clean thread specific handle");
}

static void e2e_thread_signal_handler(int sig, siginfo_t *si, void *arg)
{
	e2e_thread *t;
	ssize_t r;
	(void)arg;
	(void)si;

	t = pthread_getspecific(thread_handle);
	if (!t)
		return;

	/*
	 * As a signal handler can only call a limited set of save functions
	 * we write the signal into the pipe and handle the remaining parts
	 * in the signal handler thread
	 */
	r = write(t->pipe_fds[1], &sig, sizeof(sig));
	if (r < 0)
		e2e_warn("thread: signal trap write failed: %s",
			 strerror(errno));

	/*
	 * End the main thread here, the cleanup will be executed in the signal
	 * handler thread
	 */
	if (sig == E2E_THREAD_TERM_SIGNAL)
		pthread_exit((void *)(long)-ECANCELED);
}

static void *e2e_thread_signal_entry(void *arg)
{
	e2e_thread *t = (e2e_thread *)arg;
	ssize_t r;
	int sig;

	while (true) {
		/* Block until we received a signal */
		r = read(t->pipe_fds[0], &sig, sizeof(sig));
		switch (r) {
		case -1:
			e2e_warn("thread: read from signaling pipe failed");
		case 0:
			// sender side has been closed -> close reader side
			close(t->pipe_fds[0]);
			return NULL;
		default:;
		}
		switch (sig) {
		case E2E_THREAD_TERM_SIGNAL:
			// Call thread cleanup handler if configured
			if (t->cfg.cleanup)
				t->cfg.cleanup(t, t->cfg.cleanup_args);

			close(t->pipe_fds[0]);
			pthread_exit(NULL);

		default:
			e2e_warn("thread: Unknown signal %d received.", sig);
		}
	}

	return NULL;
}

static int e2e_thread_init_signal_handler(e2e_thread *t)
{
	int ret;

	if (!t)
		return -EINVAL;

	/* Initialize pipe for signal transport */
	ret = pipe(t->pipe_fds);
	if (ret == -1)
		return errno;

	struct sigaction sa = {
		.sa_flags = SA_SIGINFO,
		.sa_sigaction = e2e_thread_signal_handler,
	};
	sigemptyset(&sa.sa_mask);

	ret = sigaction(E2E_THREAD_TERM_SIGNAL, &sa, NULL);
	if (ret == -1)
		return errno;

	ret = pthread_setspecific(thread_handle, (void *)t);
	if (ret)
		return ret;

	return pthread_create(&t->signal_thread, NULL, e2e_thread_signal_entry,
			      t);
}

static int shutdown_signal_handler(e2e_thread *t, void **retval)
{
	close(t->pipe_fds[1]);
	return pthread_join(t->signal_thread, retval);
}

static int e2e_thread_log(e2e_thread *t, const char *fmt, ...)
{
	va_list args;
	int ret;

	va_start(args, fmt);
	ret = t->cfg.logger(t->cfg.logger_args, fmt, args);
	va_end(args);

	return ret;
}

static int e2e_thread_validate_sched_prio(e2e_thread *t, int prio)
{
	if (prio >= 0)
		return 0;

	e2e_thread_log(t, "Invalid sched priority");
	return -EINVAL;
}

static int e2e_thread_get_sched_prio(e2e_thread *t)
{
	int prio = 0;
	int min;
	int max;

	if (t->cfg.set_sched_prio)
		prio = e2e_thread_validate_sched_prio(t, t->cfg.sched_prio);

	if (prio < 0)
		return prio;

	min = sched_get_priority_min(t->cfg.sched_policy);
	max = sched_get_priority_max(t->cfg.sched_policy);

	return min + ((max - min) / 2);
}

static int e2e_thread_set_sched_policy_and_prio(e2e_thread *t)
{
	struct sched_param sp;
	pthread_t self;
	int policy;
	int prio;
	int ret;

	self = pthread_self();
	ret = pthread_getschedparam(self, &policy, &sp);
	if (ret) {
		e2e_thread_log(t, "pthread_getschedparam: %s\n", strerror(ret));
		return ret;
	}

	if (t->cfg.set_sched_policy)
		policy = t->cfg.sched_policy;

	prio = e2e_thread_get_sched_prio(t);
	if (prio < 0)
		return prio;

	sp.sched_priority = prio;

	ret = pthread_setschedparam(self, policy, &sp);
	if (ret) {
		e2e_thread_log(t, "pthread_setschedparam: %s %s", strerror(ret),
			       t->cfg.name);
		return ret;
	}

	if (t->cfg.verbose)
		e2e_thread_log(t, "task priority set to %d\n", prio);

	return 0;
}

static void e2e_thread_fill_cpuset(cpu_set_t *set, unsigned mask)
{
	long bit = 0;

	while (mask) {
		if (mask & 1)
			CPU_SET(bit, set);

		bit++;
		mask >>= 1; /* Right shift */
	}
}

static int e2e_thread_set_cpuaffinity(e2e_thread *t)
{
	cpu_set_t cpuset;
	pthread_t self;
	int ret = 0;

	if (!t->cfg.set_sched_cpu_mask)
		return ret;

	self = pthread_self();

	CPU_ZERO(&cpuset);
	e2e_thread_fill_cpuset(&cpuset, t->cfg.sched_cpu_mask);

	ret = pthread_setaffinity_np(self, sizeof(cpuset), &cpuset);
	if (ret) {
		e2e_thread_log(t, "pthread_setaffinity_np: %d", ret);
		return ret;
	}

	if (t->cfg.verbose)
		e2e_thread_log(t, "CPU affinity set to CPU %d\n",
			       t->cfg.sched_cpu_mask);

	return ret;
}

static int e2e_thread_set_sched_params(e2e_thread *t)
{
	int ret;

	ret = e2e_thread_set_sched_policy_and_prio(t);
	if (ret)
		return ret;

	return e2e_thread_set_cpuaffinity(t);
}

static void *e2e_thread_main_entry(void *arg)
{
	e2e_thread *t = (e2e_thread *)arg;
	int ret;

	ret = e2e_thread_init_signal_handler(t);
	if (ret)
		return NULL;

	ret = e2e_thread_set_sched_params(t);
	if (ret)
		return NULL;

	return t->cfg.entry(t, t->cfg.entry_args);
}

static inline int
e2e_thread_validate_config(const struct e2e_thread_config *cfg)
{
	if (!cfg || !cfg->entry)
		return -EINVAL;

	return 0;
}

static int e2e_thread_default_logger(void *ctx, const char *fmt, va_list args)
{
	(void)ctx;
	int ret;

	ret = vprintf(fmt, args);

	return ret;
}

int e2e_thread_create(e2e_thread **thread, const struct e2e_thread_config *cfg)
{
	e2e_thread *t;

	/* Require NULL, otherwise we might leak/overwrite something */
	if (*thread)
		return -EINVAL;

	if (e2e_thread_validate_config(cfg))
		return -EINVAL;

	t = calloc(1, sizeof(struct e2e_thread));
	if (!t)
		return -ENOMEM;

	/* Copy the thread config to avoid lifetime issues */
	t->cfg = *cfg;

	/* Set default logger if none given */
	if (!t->cfg.logger)
		t->cfg.logger = e2e_thread_default_logger;

	*thread = t;

	return 0;
}

int e2e_thread_destroy(e2e_thread *t)
{
	if (!t)
		return -EINVAL;

	free(t);
	return 0;
}

int e2e_thread_start(e2e_thread *t)
{
	int ret;

	if (!t)
		return -EINVAL;

	if (t->state == E2E_T_STATE_RUNNING)
		return -EINVAL;

	ret = pthread_create(&t->main_thread, NULL, e2e_thread_main_entry, t);
	if (ret)
		return ret;

	t->state = E2E_T_STATE_RUNNING;
	// threads.talker_thread = t;

	return ret;
}

int e2e_thread_stop(e2e_thread *t)
{
	if (!t)
		return -EINVAL;

	if (t->state != E2E_T_STATE_RUNNING)
		return -EINVAL;

	/* Tell the main thread to shut down */
	return pthread_kill(t->main_thread, E2E_THREAD_TERM_SIGNAL);
}

int e2e_thread_join(e2e_thread *t, void **retval)
{
	int ret;

	if (!t)
		return -EINVAL;

	if (t->state != E2E_T_STATE_RUNNING)
		return -EINVAL;

	/* Wait for the main thread to finish */
	ret = pthread_join(t->main_thread, retval);
	if (ret)
		return ret;

	if (retval)
		t->retval = *retval;

	t->state = E2E_T_STATE_STOPPED;

	/* Shut down the signal handler thread */
	return shutdown_signal_handler(t, NULL);
}

int e2e_thread_get_retval(e2e_thread *t, void **retval)
{
	if (!t)
		return -EINVAL;

	if (t->state != E2E_T_STATE_STOPPED)
		return -EINVAL;

	if (retval)
		*retval = t->retval;

	return 0;
}

int e2e_thread_set_ctx(e2e_thread *t, void *ctx)
{
	if (!t)
		return -EINVAL;

	t->ctx = ctx;

	return 0;
}

int e2e_thread_get_ctx(e2e_thread *t, void **ctx)
{
	if (!t)
		return -EINVAL;

	*ctx = t->ctx;

	return 0;
}
