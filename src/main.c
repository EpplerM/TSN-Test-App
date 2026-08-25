// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <locale.h>
#include <signal.h>

#include <net/if.h>

#include "e2e_agent.h"
#include "e2e_bpf.h"
#include "e2e_build_cfg.h"
#include "e2e_clock.h"
#include "e2e_cmdline.h"
#include "e2e_common.h"
#include "e2e_listener.h"
#include "e2e_talker.h"
#include "e2e_thread.h"

struct e2e_app {
	struct e2e_config *cfg;
	struct e2e_bpf_cfg bpf_cfg;
	e2e_thread *thread_listener;
	e2e_thread *thread_talker;
	e2e_thread *thread_worker;
	struct e2e_agent *agent;
};

static struct e2e_app app = { 0 };

static int e2e_init_threads()
{
	int ret;

	ret = e2e_listener_init(&app.thread_listener, app.cfg);
	if (ret)
		return ret;

	return e2e_talker_init(&app.thread_talker, app.cfg);
}

static int e2e_run_tests()
{
	bool listener_started = false;
	bool talker_started = false;
	void *retval_l = NULL;
	void *retval_t = NULL;
	int ret_l = 0;
	int ret_t = 0;
	int ret;

	/* Wait for a valid config if agent is running */
	if (app.agent) {
		ret = e2e_config_lock_wait(&app.cfg->locks.cfg_lock);
		if (ret)
			return ret;
	}
	
	if (app.agent && !e2e_agent_running(app.agent)) {
		/* Agent already exited, no more tests to run */
		return -ECANCELED;
	}

	/*
	 * Start the threads, they are already configured, just start them
	 *
	 * Listen, ListenAndTalk, TalkAndListen => Start listener
	 * Talk, ListenAndTalk, TalkAndListen => Start talker
	 */
	switch (app.cfg->role) {
	case LISTEN:
	case LISTEN_AND_TALK:
	case TALK_AND_LISTEN:
		ret = e2e_thread_start(app.thread_listener);
		if (ret) {
			e2e_warn("main: Failed to start listener %d", ret);
			return ret;
		}
		listener_started = true;
	default:
		break;
	}

	switch (app.cfg->role) {
	case TALK:
	case LISTEN_AND_TALK:
	case TALK_AND_LISTEN:
		ret = e2e_thread_start(app.thread_talker);
		if (ret) {
			e2e_warn("main: Failed to start talker %d", ret);
			return ret;
		}
		talker_started = true;
	default:
		break;
	}

	/*
	 * Wait for threads to complete
	 * => might be aborted by the agent
	 * => test finished (with or without error)
	 */
	if (listener_started) {

		ret_l = e2e_thread_join(app.thread_listener, &retval_l);

		if (ret_l)
			e2e_warn("main: Failed to join listener %d", ret_l);
		if ((long)retval_l && (long)retval_l != -ECANCELED)
			e2e_warn("main: Listener failed: %d", -(long)retval_l);
		
	}

	if (talker_started) {

		ret_t = e2e_thread_join(app.thread_talker, &retval_t);
	
		if (ret_t)
			e2e_warn("main: Failed to join talker %d", ret_t);
		if ((long)retval_t && (long)retval_t != -ECANCELED)
			e2e_warn("main: Talker failed: %d", -(long)retval_t);
		
	}

	/* Signal that threads have ended, only relevant if agent enabled */
	ret = e2e_config_lock_signal(&app.cfg->locks.test_lock);
	if (ret)
		e2e_warn("main: Failed to signal the test lock %d", ret);

	if (ret_l || ret_t)
		return ret_l ?: ret_t;

	if (retval_l || retval_t)
		return retval_l ? (int)(long)retval_l : (int)(long)retval_t;

	return 0;
}

static int e2e_xdp_init()
{
	if (!app.cfg->use_xdp_socket || !app.cfg->xdp_load_bpf)
		return 0;

	/* Create the bpf loader configuration */
	app.bpf_cfg.if_idx = (int)if_nametoindex(app.cfg->iface);
	app.bpf_cfg.xdp_flags = app.cfg->xdp_flags;
	strncpy(app.bpf_cfg.progname, E2E_BPF_PROGRAM_NAME,
		sizeof(app.bpf_cfg.progname));
	strncpy(app.bpf_cfg.filename, E2E_BPF_PROGRAM_PATH,
		sizeof(app.bpf_cfg.filename));

	return e2e_bpf_load(&app.bpf_cfg);
}

static int e2e_xdp_cleanup()
{
	if (!app.cfg->use_xdp_socket || !app.cfg->xdp_load_bpf)
		return 0;

	return e2e_bpf_unload(&app.bpf_cfg);
}

static void *e2e_worker_cleanup()
{
	e2e_thread_destroy(app.thread_talker);
	e2e_thread_destroy(app.thread_listener);
	e2e_agent_destroy(app.agent);
	e2e_xdp_cleanup();

	return NULL;
}

static void *e2e_worker_entry()
{
	int ret;

	ret = e2e_xdp_init();
	if (ret)
		return (void *)(long)ret;

	/* Initialize listener and talker treads, do not start yet */
	ret = e2e_init_threads();
	if (ret)
		return (void *)(long)ret;

	ret = e2e_agent_create(&app.agent, app.cfg);
	if (ret)
		return (void *)(long)ret;

	ret = e2e_agent_set_threads(app.agent, app.thread_listener, app.thread_talker);
	if (ret)
		return (void *)(long)ret;

	do {
		ret = e2e_run_tests();

		if (ret && ret != -ECANCELED){
			if (e2e_agent_running(app.agent)){
				e2e_warn("main: Test execution failed: %d", ret);
			}
		}
		if (ret == -ECANCELED)
			break;

	} while (e2e_agent_running(app.agent) || app.cfg->infinite);

	e2e_worker_cleanup();

	return (void *)(long)ret;
}

static int e2e_worker_start()
{
	int ret;
	struct e2e_thread_config worker_cfg = {
		.name = "worker",
		.entry = e2e_worker_entry,
		.cleanup = e2e_worker_cleanup,
	};

	ret = e2e_thread_create(&app.thread_worker, &worker_cfg);
	if (ret)
		return ret;

	ret = e2e_thread_start(app.thread_worker);
	if (ret)
		return ret;

	ret = e2e_thread_join(app.thread_worker, NULL);
	if (ret)
		return ret;

	e2e_info(ESC_INFO, "\nClosing tsn_test_app ...\n\n");

	return e2e_thread_destroy(app.thread_worker);
}

static void e2e_signal_handler()
{
	int err_t, err_l, err_a;

	e2e_info(ESC_CRIT, "\nReceived break using Ctrl+C\n");

	err_t = e2e_thread_stop(app.thread_talker);
	err_l = e2e_thread_stop(app.thread_listener);
	err_a = e2e_agent_stop(app.agent);

	/* If no worker owned threads were alive stop the worker himself */
	if (err_t == err_l && err_l == err_a && err_a == -EINVAL)
		e2e_thread_stop(app.thread_worker);
}

int main(int argc, char *argv[])
{
	struct e2e_config cfg = { 0 };
	int ret;

	e2e_info(ESC_INFO, "\nStarting tsn_test_app ...\n\n");

	// check syntax of given short and long options
	for (int i = 0; i<argc; i++) {
		// short option
		ret = (!strncmp(argv[i], "--", 2) && strlen(argv[i]) == 3);
		if (ret) {
			e2e_crit("Invalid syntax for short option: %s.\nTry -%s instead.\n", argv[i], argv[i]+2);
			goto err_cfg;
		};
		// long option
		ret = (strncmp(argv[i], "--", 2) && !strncmp(argv[i], "-", 1) && strlen(argv[i]) > 2);
		if (ret) {
			e2e_crit("Invalid syntax for long option: %s.\nTry --%s instead.\n", argv[i], argv[i]+1);
			goto err_cfg;
		};
    }

	setlocale(LC_NUMERIC, "");

	/* Catch Ctrl^C */
	signal(SIGINT, e2e_signal_handler);

	e2e_config_set_defaults(&cfg);
	app.cfg = &cfg;

	ret = e2e_config_init(&cfg);
	if (ret)
		goto err_cfg;

	e2e_agent_log_report("Issues detected in testapp configuration:\n\n");

	ret = e2e_cmdline_parse(&cfg, argc, argv);
	if (ret)
		goto err_cfg;

	ret = e2e_config_validate(&cfg);
	if (ret)
		goto err_cfg;

	e2e_clock_set_gm_id(&cfg);
	e2e_worker_start();

err_cfg:
	e2e_config_cleanup(&cfg);

	return ret;
}
