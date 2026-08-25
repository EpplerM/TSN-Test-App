#include <arpa/inet.h>
// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "e2e_agent.h"
#include "e2e_cmdline.h"
#include "e2e_common.h"
#include "e2e_listener.h"
#include "e2e_clock.h"
#include <sys/utsname.h>
#include <stdio.h>
#include <sys/stat.h>


#define LOG_BUFFER_LENGTH  (256*1000)
#define CLIENT_BUFFER_SIZE 1024
#define PORT		   65000
#define BACKLOG_SIZE	   10

//report file
FILE *report_file;  // Global file pointer

// threads receiving and sending data to client
pthread_t agent_client_send;
pthread_t agent_client_read;
pthread_t get_listener_state;
pthread_t get_talker_state;

// corresponding thread functions
void *e2e_agent_sock_send_data(void *arg);
void *e2e_agent_sock_read_data(void* arg);
void *e2e_talker_active(void* arg);
void *e2e_listener_active(void* arg);

static int e2e_client_state = E2E_CLIENT_STATE_UNDEFINED;

struct e2e_agent_log_desc {
	bool reset_idx;
	size_t idx;
	char buf[LOG_BUFFER_LENGTH];
};

struct e2e_agent_log_info {
	struct e2e_agent_log_desc info;
	struct e2e_agent_log_desc err;
};

static struct e2e_agent_log_info logs;

typedef struct e2e_agent {
	e2e_thread *agent_thread;
	e2e_thread *listener_thread;
	e2e_thread *talker_thread;
	struct e2e_config *cfg;
	void *last_client_heap;
	int main_sock;
	bool running;
} e2e_agent;

enum e2e_agent_action {
	E2E_AGENT_ACTION_CONTINUE = 0,
	E2E_AGENT_ACTION_CLIENT_CLOSE,
};


struct e2e_agent_client {
	e2e_agent *agent;
	int sock;
	char buffer[CLIENT_BUFFER_SIZE];
	struct sockaddr_in addr;
	struct e2e_config new_cfg;
	bool start_trigger;
	bool signoff;
	enum e2e_listener_state listener_state;
	enum e2e_talker_state talker_state;
	bool host_done;
};

// refresh c->listener_state; c->talker_state
static void e2e_agent_get_states(struct e2e_agent_client *c) 
{
	if (c->agent->cfg->role == TALK_AND_LISTEN || c->agent->cfg->role == LISTEN_AND_TALK)
	{
		pthread_mutex_lock(&c->agent->cfg->locks.agent_lock.mutex);
		e2e_listener_get_state(&c->listener_state);
		e2e_talker_get_state(&c->talker_state);
		pthread_mutex_unlock(&c->agent->cfg->locks.agent_lock.mutex);
	}
	else if (c->agent->cfg->role == LISTEN)
	{
		pthread_mutex_lock(&c->agent->cfg->locks.agent_lock.mutex);
		e2e_listener_get_state(&c->listener_state);
		c->talker_state = TALKER_STATE_UNDEFINED;
		pthread_mutex_unlock(&c->agent->cfg->locks.agent_lock.mutex);
	}
	else if (c->agent->cfg->role == TALK)
	{
		pthread_mutex_lock(&c->agent->cfg->locks.agent_lock.mutex);
		e2e_talker_get_state(&c->talker_state);
		c->listener_state = LISTENER_STATE_UNDEFINED;
		pthread_mutex_unlock(&c->agent->cfg->locks.agent_lock.mutex);
	}
}

static void e2e_agent_failure()
{
	e2e_warn("Agent failure: %s", strerror(errno));
	exit(EXIT_FAILURE);
}

static void e2e_agent_copy_cfg(struct e2e_config *dst, struct e2e_config *src)
{
	/* Copy everything but the lock */
	char *d = (char *)dst + sizeof(dst->locks);
	char *s = (char *)src + sizeof(src->locks);

	memcpy(d, s, sizeof(*src) - sizeof(src->locks));
}

static int e2e_agent_log__add(struct e2e_agent_log_desc *d, const char *fmt,
			      va_list args)
{
	size_t available;
	int ret;

	if (d->reset_idx){
		d->idx = 0;
		d->reset_idx = !d->reset_idx;
	}
	available = sizeof(d->buf) - d->idx;

	if (!available)
		return -ENOBUFS;

	ret = vsnprintf(d->buf + d->idx, available, fmt, args);
	if (ret >= available) {
		d->idx = sizeof(d->buf);
		return -1; // message truncated
	}

	d->idx += ret;

	return ret;
}

static int e2e_agent_sock_init()
{
	int ret;
	int sock;
	int opt = 1;
	struct sockaddr_in address;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock == -1)
		return -errno;

	ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt,
			 sizeof(opt));
	if (ret == -1)
		goto err;

	// Listen on port 65000 on all interfaces
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(PORT);

	ret = bind(sock, (struct sockaddr *)&address, sizeof(address));
	if (ret == -1)
		goto err;

	ret = listen(sock, BACKLOG_SIZE);
	if (ret == -1)
		goto err;

	return sock;
err:
	close(sock);
	return -errno;
}

static int e2e_agent_sock_cleanup(int sock)
{
	int ret = 0;

	ret = close(sock);
	if (ret == -1)
		return errno;

	return ret;
}

// check host(s) done
int update_host_state(struct e2e_agent_client  *c){
	c->host_done = false;
	switch (c->new_cfg.role) {
	case TALK:
		if (c->talker_state != TALKER_STATE_RUNNING)
		{ 
			c->host_done = true;
		}
		break;
	
	case LISTEN:
		if (c->listener_state != LISTENER_STATE_RUNNING)
		{
			c->host_done = true;
		}
		break;

	case LISTEN_AND_TALK:
	case TALK_AND_LISTEN:
	case ROLE_UNDEFINED:
		if (c->listener_state != LISTENER_STATE_RUNNING && c->talker_state != TALKER_STATE_RUNNING)
	 	{
			c->host_done = true;
		}
		break;
	}
	return (0);
}

// handle start_trigger and signoff
static int e2e_agent_parse_message(struct e2e_agent_client *c)
{
	ssize_t bytes;

	// clear buffer
	memset(c->buffer, 0, sizeof(c->buffer));
	
	bytes = read(c->sock, c->buffer, sizeof(c->buffer));
	if (bytes <= 0)
		return -1;

	/* start_trigger can appear at any location */
	if (strstr(c->buffer, "start_trigger")) {
		c->start_trigger = true;
		e2e_info(ESC_INFO, "\nStart_trigger received.");
		
		// e2e_agent_client running
		e2e_client_state = E2E_CLIENT_STATE_RUNNING;
	}

	/* Sign off message is received */
	if (!strcmp(c->buffer, "signoff")) {
		c->signoff = true;
		if (e2e_client_state == E2E_CLIENT_STATE_RUNNING)
		{	
			// accept signoff if host not done
			update_host_state(c);
			if (!c->host_done){
				e2e_info(ESC_INFO, "\nSignoff message received.");
			}
		}
	}
	return 0;
}

// e2e_agent_client threads
int start_client_threads(struct e2e_agent_client *c)
{
	int ret;

	switch (c->new_cfg.role) {
	case ROLE_UNDEFINED: 
		return (-1);
	case TALK:
		// start thread waiting for talker completed
		ret = pthread_create(&get_talker_state, NULL, &e2e_talker_active, c);
		if (ret) 
		{
			e2e_warn("agent: Failed to spawn thread get_talker_state");
			free(c);
			return ret;
		}
		break;
	case LISTEN:
		// start thread waiting for listener completed
		ret = pthread_create(&get_listener_state, NULL, &e2e_listener_active, c);
		if (ret) 
		{
			e2e_warn("agent: Failed to spawn thread get_listener_state");
			free(c);
			return ret;	
		}
		break;
	case LISTEN_AND_TALK:
	case TALK_AND_LISTEN:
		// start threads waiting for talker / listener completed
		ret = pthread_create(&get_talker_state, NULL, &e2e_talker_active, c);
		if (ret) 
		{
			e2e_warn("agent: Failed to spawn thread get_talker_state");
			free(c);
			return ret;	
		}
		ret = pthread_create(&get_listener_state, NULL, &e2e_listener_active, c);
		if (ret) 
		{
			e2e_warn("agent: Failed to spawn thread get_listener_state");
			free(c);
			return ret;	
		}
		break;
	}

	// start thread sending data to client / read data from client if available
	ret = pthread_create(&agent_client_send, NULL, &e2e_agent_sock_send_data, c);
	if (ret) {
		e2e_warn("agent: Failed to spawn thread agent_client_send");
		free(c);
		return ret;	
	}

	// start thread reading data from client
	ret = pthread_create(&agent_client_read, NULL, &e2e_agent_sock_read_data, c);
	if (ret) {
		e2e_warn("agent: Failed to spawn thread agent_client_send");
		free(c);
		return ret;	
	}

	return (0);
	
}

void *e2e_listener_active(void* arg)
{
	struct e2e_agent_client  *c = (struct e2e_agent_client *)arg;
	int ret;

	// waits for signalling from listener when job is done
	ret = e2e_config_lock_wait(&c->agent->cfg->locks.listener_lock);
	if (ret)
		e2e_agent_log_warn(NULL, true, "agent: Failed to wait for listener");
	

	c->listener_state = LISTENER_STATE_DONE;

	return 0;
}

void *e2e_talker_active(void* arg)
{
	struct e2e_agent_client  *c = (struct e2e_agent_client *)arg;
	int ret;

	// waits for signalling from talker job is done
	ret = e2e_config_lock_wait(&c->agent->cfg->locks.talker_lock);
	if (ret)
		e2e_agent_log_warn(NULL, true, "agent: Failed to wait for talker");
	
	c->talker_state = TALKER_STATE_DONE;

	return 0;
}

// read incomming client messages during running test
void *e2e_agent_sock_read_data(void* arg) 
{
	struct e2e_agent_client  *c = (struct e2e_agent_client *)arg;
	int ret;

	while (1) 
	{
		ret = e2e_agent_parse_message(c);
		// finish this thread if socket is closed
		if (ret <0) {
			break;}

		if 	(c->signoff && (c->listener_state == LISTENER_STATE_RUNNING 
				|| c->talker_state == TALKER_STATE_RUNNING)) {
			e2e_client_state = E2E_CLIENT_STATE_STOPPED;
			e2e_info(ESC_INFO, "\nStopping test ... \n");
			break;
		}
	}
	return 0;
}

// send test data to client 
void *e2e_agent_sock_send_data(void *arg) 
{	
	struct e2e_agent_client  *c = (struct e2e_agent_client *)arg;
	int ret;

	// initial waiting for first update
	ret = e2e_config_lock_wait(&c->agent->cfg->locks.agent_lock);
	if (ret)
		e2e_agent_log_warn(NULL, true, "agent: Failed to wait for listener/talker");
	
	// get c->listener_state; c->talker_state
	e2e_agent_get_states(c);

	while (true)
	{
		pthread_mutex_lock(&c->agent->cfg->locks.agent_lock.mutex);
		if (strlen(logs.err.buf)) {
			send(c->sock, logs.err.buf, strlen(logs.err.buf), 0);
			memset(logs.err.buf, 0, strlen(logs.err.buf));
			logs.err.reset_idx = true;
		}
		if (strlen(logs.info.buf)) {
			send(c->sock, logs.info.buf, strlen(logs.info.buf), 0);
			memset(logs.info.buf, 0, strlen(logs.info.buf));
			logs.info.reset_idx = true;
		}
		pthread_mutex_unlock(&c->agent->cfg->locks.agent_lock.mutex);
		// check host state
		update_host_state(c);
		if (!c->host_done) {
			// signal talker/listener, wait for new update
			ret = e2e_config_lock_signal(&c->agent->cfg->locks.agent_lock);
			if (ret)
				e2e_agent_log_warn(NULL, true, "agent: Failed to signal listener/talker");
			ret = e2e_config_lock_wait(&c->agent->cfg->locks.agent_lock);

			if (ret)
				e2e_agent_log_warn(NULL, true, "agent: Failed to wait for listener/talker");
		}
		else {
			break;
		}
	}

	return 0;
}

static enum e2e_agent_action
e2e_agent_read_client_msg(e2e_agent *a, struct e2e_agent_client *c)
{
	bool valid_msg = false;
	int ret;
	int ret_l;
	int ret_t;
	char *ptr, *ptr_2 = NULL;
	char delim[] = " ";


	// Set defaults for the new configuration (=copy current config)
	e2e_agent_copy_cfg(&c->new_cfg, a->cfg);
	
	ret = e2e_agent_parse_message(c);

	if (ret)
		return E2E_AGENT_ACTION_CLIENT_CLOSE;

	if (c->signoff) 
	{	
		e2e_client_state = E2E_CLIENT_STATE_STOPPED;
		e2e_info(ESC_INFO, "Signoff message received.\n");
		return E2E_AGENT_ACTION_CLIENT_CLOSE;
	}

	if (!c->start_trigger)
		return E2E_AGENT_ACTION_CONTINUE;
	
	e2e_info(ESC_INFO, "Client connected ...\n\n");
	ptr = strtok(c->buffer, delim);

	while (ptr != NULL) { // runs over a message

		if (strcmp(ptr, "role") == 0) {
			ptr = strtok(NULL, delim);

			if (strcmp(ptr, "l") == 0) {
				c->new_cfg.role = LISTEN;
				ptr = strtok(NULL, delim);
			} else if (strcmp(ptr, "t") == 0) {
				c->new_cfg.role = TALK;
				ptr = strtok(NULL, delim);
			} else if (strcmp(ptr, "lt") == 0) {
				c->new_cfg.role = LISTEN_AND_TALK;
				ptr = strtok(NULL, delim);
			} else if (strcmp(ptr, "tl") == 0) {
				c->new_cfg.role = TALK_AND_LISTEN;
				ptr = strtok(NULL, delim);
			}
		}

		if (strcmp(ptr, "config_params") == 0) {
			ptr = strtok(NULL, delim);
			e2e_info(ESC_INFO, "\nSetting config parameters:\n");

			while (strcmp(ptr, "start_trigger") != 0) {
				ptr_2 = strtok(NULL, delim);
				e2e_cmdline_set_parameters(&c->new_cfg, *ptr,
							   ptr_2, 1);
				ptr = strtok(NULL, delim);
			}

			if (!strcmp(ptr, "end")) {
				break;
			}
			ptr = strtok(NULL, delim);
		}

		if (!strcmp(ptr, "end") || !strcmp(ptr, "start_trigger")) {
			valid_msg = true;
		} else {
			e2e_agent_log_warn(a,true, 
					   "agent: Failed to parse client msg");
		}

		break;
	}

	ret = start_client_threads(c);
	if (ret)
		return E2E_AGENT_ACTION_CLIENT_CLOSE;

	if (valid_msg) 
	{
		// start test
		e2e_agent_copy_cfg(a->cfg, &c->new_cfg);
		ret = e2e_config_lock_signal(&a->cfg->locks.cfg_lock);
		if (ret)
			e2e_agent_log_warn(a, true, "agent: Failed to signal tests");

		// wait until talker/listener done
		switch (c->new_cfg.role) 
		{
		case TALK:
			pthread_join(get_talker_state, NULL);
			break;

		case LISTEN:
			pthread_join(get_listener_state, NULL);
			break;

		case LISTEN_AND_TALK:
		case TALK_AND_LISTEN:
		case ROLE_UNDEFINED:
			pthread_join(get_talker_state, NULL);
			pthread_join(get_listener_state, NULL);
			break;
		}

		// final signalling ensuring regular finishing of agent_client_send thread
		ret = e2e_config_lock_signal(&c->agent->cfg->locks.agent_lock);
		if (ret)
			e2e_agent_log_warn(NULL, true, "listener: Failed to signal agent client");

		// wait for agent_client_send thread (test done / stopped)
		pthread_join(agent_client_send, NULL);

		// wait for listener and talker threads have ended (signalled in main)
		ret = e2e_config_lock_wait(&a->cfg->locks.test_lock);
		if (ret)
			e2e_agent_log_warn(a,true, 
					   "agent: Failed to wait for tests");

		switch (a->cfg->role) {
		case TALK:
			ret_t = e2e_thread_get_retval(a->talker_thread, NULL);
			ret_l = 0;
			break;
		case LISTEN:
			ret_t = 0;
			ret_l = e2e_thread_get_retval(a->listener_thread, NULL);
			break;
		case LISTEN_AND_TALK:
		case TALK_AND_LISTEN:
		case ROLE_UNDEFINED:
		default:
			ret_t = e2e_thread_get_retval(a->talker_thread, NULL);
			ret_l = e2e_thread_get_retval(a->listener_thread, NULL);
		}

		if (ret_l)
			e2e_agent_log_warn(
				a, true, "agent: Failed to get listener retval");

		if (ret_t)
			e2e_agent_log_warn(
				a, true, "agent: Failed to get talker retval");

		if (!ret_l && !ret_t)
			if (e2e_client_state == E2E_CLIENT_STATE_RUNNING) {
			{
				switch (a->cfg->role) {
				case TALK:
					if (c->talker_state == TALKER_STATE_DONE){
						send(c->sock, "host_done", strlen("host_done"), 0);
					} else {
					send(c->sock, "host_error", strlen("host_error"), 0);}
					break;

				case LISTEN:
					if (c->listener_state == LISTENER_STATE_DONE){
						send(c->sock, "host_done", strlen("host_done"), 0);
					} else {
					send(c->sock, "host_error", strlen("host_error"), 0);}
					break;

				case LISTEN_AND_TALK:
				case TALK_AND_LISTEN:
				case ROLE_UNDEFINED:
					if (c->talker_state == TALKER_STATE_DONE && c->listener_state == LISTENER_STATE_DONE){
						send(c->sock, "host_done", strlen("host_done"), 0);
					} else {
					send(c->sock, "host_error", strlen("host_error"), 0);}

				}
			}
		}
	}
	e2e_info(ESC_INFO, "Closing sockets ...\n\n");

	if (strlen(logs.err.buf))
		send(c->sock, "host_error", strlen("\nhost_error"), 0);
	
	return E2E_AGENT_ACTION_CLIENT_CLOSE;
}

void *e2e_agent_handle_client(void *arg)
{
	struct e2e_agent_client *c = (struct e2e_agent_client *)arg;
	enum e2e_agent_action ret;
	bool client_active = true;
	bool server_active = true;
	
	pthread_detach(pthread_self());
	
	do {
		ret = e2e_agent_read_client_msg(c->agent, c);

		switch (ret) {

		case E2E_AGENT_ACTION_CLIENT_CLOSE:

			ret = e2e_agent_sock_cleanup(c->sock);

			// wait for thread read socket done (client data to server)
			pthread_join(agent_client_read, NULL);
			logs.err.reset_idx = true;
			client_active = false;
			e2e_client_state = E2E_CLIENT_STATE_UNDEFINED;


			e2e_info(ESC_INFO, "Test app running in agent mode.\nWaiting for a test client to connect ...\n\n");
			break;

		default:
			break;
		}
	} while (client_active);

	if (!server_active) {
		e2e_warn("agent: Exiting due to client request");
		e2e_thread_stop(c->agent->agent_thread);
	}

	free(c);
	return NULL;
}

static void *e2e_agent_entry(e2e_thread *thread, void *agent)
{
	e2e_agent *a = (e2e_agent *)agent;
	int ret;
	a->running = true;
	a->main_sock = e2e_agent_sock_init();

	if (a->main_sock < 0) {
		e2e_agent_log_warn(a, true, "agent: sock init failed %d",
				   a->main_sock);
		e2e_agent_failure();
	}
	e2e_info(ESC_INFO, "Test app running in agent mode.\nWaiting for a test client to connect ...\n\n");

	do {
		struct e2e_agent_client *c;
		socklen_t addr_len = sizeof(c->addr);
		pthread_t t;

		c = calloc(1, sizeof(*c));
		if (!c)
			goto err_no_mem;

		a->last_client_heap = c;
		c->agent = a;

		// waits for socket connection (blocking)
		c->sock = accept(a->main_sock, (struct sockaddr *)&c->addr,
				 (socklen_t *)&addr_len);
		if (c->sock == -1) {
			free(c);
			break;
		}

		ret = pthread_create(&t, NULL, e2e_agent_handle_client, c);
		if (ret) {
			e2e_warn("agent: Failed to spawn client thread");
			free(c);
			continue;
		}

	} while (true);

err_no_mem:
	return NULL;
}

static void *e2e_agent_cleanup(e2e_thread *t, void *arg)
{
	e2e_agent *a = (e2e_agent *)arg;
	int ret;

	free(a->last_client_heap);
	e2e_info(ESC_INFO, "Closing sockets and terminating test agent ... \n");

	ret = e2e_agent_sock_cleanup(a->main_sock);
	if (ret)
		e2e_warn("agent: Socket cleanup failed. Ignoring.");

	/*
	 * Signal the test lock once more to make sure the main thread is able
	 * to do a proper shutdown
	 */
	e2e_config_lock_signal(&a->cfg->locks.cfg_lock);

	a->running = false;

	return NULL;
}

int e2e_agent_create(e2e_agent **agent, struct e2e_config *cfg)
{
	e2e_agent *a;
	int ret;
	struct e2e_thread_config tcfg = { 0 };


	if (!cfg)
		return -EINVAL;

	if (cfg->report) {
		struct timespec last_phc;
		struct tm *local_time;
		long sec;
		last_phc = e2e_clock_get(cfg);
		sec = last_phc.tv_sec;
		local_time = localtime(&sec);
		struct utsname name;
		uname(&name);
		mkdir("reports", 0777);
		if (cfg->testcase_name[0] == '\0') {
			snprintf(cfg->report_filename, sizeof(cfg->report_filename),
				"reports/%s_%s:%X_%04d_%02d_%02d__%02d:%02d:%02d.txt",
				name.nodename,
				cfg->iface,
				__bswap_16(cfg->ethertype),
				local_time->tm_year + 1900, local_time->tm_mon + 1,
				local_time->tm_mday, local_time->tm_hour, local_time->tm_min,
				local_time->tm_sec);
		} else {
			snprintf(cfg->report_filename, sizeof(cfg->report_filename),
				"reports/%s_%s_%s_%X_%04d_%02d_%02d__%02d:%02d:%02d.txt",
				cfg->testcase_name,
				name.nodename,
				cfg->iface,
				__bswap_16(cfg->ethertype),
				local_time->tm_year + 1900, local_time->tm_mon + 1,
				local_time->tm_mday, local_time->tm_hour, local_time->tm_min,
				local_time->tm_sec);
		}

		e2e_info(ESC_INFO, "Opening report file %s.\n", cfg->report_filename);

		//l->report = fopen(l->report_filename, "w");
		report_file = fopen(cfg->report_filename, "w");
		if (!report_file)
			e2e_agent_log_crit(NULL, true, "unable to open report file");

		if (cfg->testcase_name[0] != '\0')
			e2e_agent_log_report("Testcase name: %s\n", cfg->testcase_name);	
		e2e_agent_log_report("Device name: %s\n", name.nodename);
		e2e_agent_log_report("Used interface: %s\n", cfg->iface);
		e2e_agent_log_report("Test executed at %02d:%02d:%02d on %02d/%02d/%04d\n",
			local_time->tm_hour, local_time->tm_min, local_time->tm_sec,
			local_time->tm_mon + 1,	local_time->tm_mday, local_time->tm_year + 1900);
		e2e_agent_log_report("\nTestApp configuration:\n");
		switch (cfg->role) {
			case TALK:
				e2e_agent_log_report("\tRole: Talker\n");
				break;
			case LISTEN:
				e2e_agent_log_report("\tRole: Listener\n");
				break;
			case LISTEN_AND_TALK:
				e2e_agent_log_report("\tRole: Listen and talk\n");
				break;
			case TALK_AND_LISTEN:
				e2e_agent_log_report("\tRole: Talk and listen\n");
				break;
			default:
				break;
		}
		if (cfg->bind_listener_ip_given)
			e2e_agent_log_report("\tListener socket bound to: %hhu.%hhu.%hhu.%hhu:%hu\n",
				cfg->bind_listener_ip[0], cfg->bind_listener_ip[1], cfg->bind_listener_ip[2], cfg->bind_listener_ip[3], cfg->bind_listener_port);
		if (cfg->endless_number_of_packets)
			e2e_agent_log_report("\ttesting with infinite number of packets\n"); 
		else
			e2e_agent_log_report("\tTest duration: %d packets\n", cfg->number_of_packets); 
		if (cfg->ptp_running) {
			if (cfg->gm_present) {
				cfg->grandmaster_identity[strcspn(cfg->grandmaster_identity, "\n")] = '\0';
				e2e_agent_log_report("\tPTP running, grand master identity: %s, current GM offset %d ns\n", cfg->grandmaster_identity, cfg->gm_offset); 
			} else {
				e2e_agent_log_report("\tPTP running, no grandmaster detected. Either this device is grandmaster, or it is not synchronzed.\n"); 
			}
		} else {
			e2e_agent_log_report("\tWARNING: no running PTP instance found!\n");
		}
		if (cfg->use_xdp_socket)
			e2e_agent_log_report("\tXDP socket in use\n"); 
		if (cfg->verbose)
			e2e_agent_log_report("\tVerbose output enabled\n");
		if (cfg->quiet_quiet_mode)
			e2e_agent_log_report("\tVery quiet mode enabled\n");
		else {
			if (cfg->quiet_mode)
				e2e_agent_log_report("\tQuiet mode enabled\n");
		}
		if (cfg->file_output)
			e2e_agent_log_report("\tFile output enabled\n");
		
		if(cfg->role == TALK || cfg->role == TALK_AND_LISTEN || cfg->role == LISTEN_AND_TALK) {
			e2e_agent_log_report("\nTalker configuration:\n");
			switch (cfg->send_mode) {
				case HW_BASED:
					e2e_agent_log_report("\tSend mode: HW-based\n");
					break;
				case SW_BASED:
					e2e_agent_log_report("\tSend mode: application-layer-based\n");
					break;
				case ETF:
					e2e_agent_log_report("\tSend mode: ETF offloading\n");
					break;
				default:
					break;
			}
			e2e_agent_log_report("\tLinux send socket priority: %d\n", cfg->socket_prio); 
			e2e_agent_log_report("\tProvision offset %'d ns\n", cfg->provision_time);
			if (cfg->round_trip_delay && cfg->role == TALK_AND_LISTEN)
				e2e_agent_log_report("\tRound-trip delay computation enabled\n"); 
			e2e_agent_log_report("\nTest traffic specification:\n");
			if (cfg->use_udp_socket) {
				e2e_agent_log_report("\tLayer 3 packets sent via UDP socket\n"); 
				if (cfg->dest_port != 0) {
						e2e_info(ESC_INFO, "\tDestination IP address used: %hhu.%hhu.%hhu.%hhu:%hu\n", 
							cfg->dest_ip[0],cfg->dest_ip[1],cfg->dest_ip[2],cfg->dest_ip[3],cfg->dest_port);
					} else {
						e2e_info(ESC_INFO, "\tDestination IP address used: %hhu.%hhu.%hhu.%hhu\n", 
						cfg->dest_ip[0],cfg->dest_ip[1],cfg->dest_ip[2],cfg->dest_ip[3]);
					}
					e2e_agent_log_report("\tDSCP priority %d\n", cfg->dscp_prio);					
			} else {
				e2e_agent_log_report("\tLayer 2 frames sent via raw socket\n");
				if(cfg->dest_mac_set || cfg->role != LISTEN_AND_TALK) 
					e2e_agent_log_report("\tDestination MAC address: %02hhx:%02hhx:%02hhx:%02hhx:%02hhx:%02hhx\n", 
						cfg->dest_mac[0],cfg->dest_mac[1],cfg->dest_mac[2],cfg->dest_mac[3],cfg->dest_mac[4],cfg->dest_mac[5]);
				else
					e2e_agent_log_report("\tUsing received source MAC address as destination MAC address\n");
				e2e_agent_log_report("\tEthertype 0x%X\n", __bswap_16(cfg->ethertype));
				e2e_agent_log_report("\tVLAN ID %d, priority %d\n", cfg->vlan_id, cfg->vlan_prio);
			}
			e2e_agent_log_report("\tcycle time: %'d ns\n", cfg->cycle_time); 
			e2e_agent_log_report("\tframe size: %ld Bytes\n", cfg->use_udp_socket ? cfg->tx_packet_size : cfg->tx_packet_size+LEN_ETH_VLAN_HDR); 
			e2e_agent_log_report("\tburst size: %d\n", cfg->burst_size);
			if(cfg->burst_size > 1)
				e2e_agent_log_report("\tinter frame gap: %'d ns\n", cfg->interframe_gap);
			e2e_agent_log_report("\ttransmission offset: %'d ns\n", cfg->transmission_offset); 
			e2e_agent_log_report("\tintended jitter: %d ns\n", cfg->send_window); 
			e2e_agent_log_report("\tintended missing packet rate: %d / 1000\n", cfg->missed_packet_rate); 
		}
		if(cfg->role == LISTEN || cfg->role == TALK_AND_LISTEN || cfg->role == LISTEN_AND_TALK) {
			e2e_agent_log_report("\nListener configuration:\n"); 
			switch (cfg->receive_mode) {
				case HW_BASED:
					e2e_agent_log_report("\tReceive mode: HW timestamping\n");
					break;
				case SW_BASED:
					e2e_agent_log_report("\tReceive mode: SW timestamping\n");
					break;
				case APP_LAYER:
					e2e_agent_log_report("\tReceive mode: application-layer timestamping\n");
					break;
				default:
					break;
			}
			e2e_agent_log_report("\tRecovery period set to %d packets\n", cfg->recovery_period);
			if(cfg->grafana) {
				e2e_agent_log_report("\tGrafana output send to bucket %s at server %s with a sampling period of %d packets\n", 
					cfg->influx_bucket, cfg->influx_server, cfg->grafana_sampling_period);
			}
			e2e_agent_log_report("\tListener timeout %d seconds\n", cfg->time_out);
			if (cfg->use_udp_socket) {
				e2e_agent_log_report("\tListening for Layer 3 packets via UDP socket\n"); 
			} else {
				e2e_agent_log_report("\tListening for Layer 2 frames with Ethertype 0x%X on raw socket\n", __bswap_16(cfg->ethertype)); 
			}
			if (cfg->reply_mode) {
				e2e_agent_log_report("\tReply mode enabled\n");
				e2e_agent_log_report("\tReply retry rate: %d packets\n", cfg->reply_retry);
			}
		}
		e2e_agent_log_report("\n\nWarnings and errors occured during test:\n\n"); 
	}

	if (!cfg->agent_mode) {
		*agent = NULL;
		return 0;
	}

	a = calloc(1, sizeof(*a));
	if (!a)
		return -ENOMEM;

	a->cfg = cfg;
	tcfg.name = "agent";
	tcfg.entry = e2e_agent_entry;
	tcfg.entry_args = a;
	tcfg.cleanup = e2e_agent_cleanup;
	tcfg.cleanup_args = a;
	*agent = a;

	ret = e2e_thread_create(&a->agent_thread, &tcfg);
	if (ret)
		goto err;

	ret = e2e_thread_start(a->agent_thread);
	if (ret)
		goto err;

	return ret;

err:
	free(a);
	return ret;
}

int e2e_agent_destroy(e2e_agent *a)
{
	int ret;
	void *retval;

	if (!a)
		return 0; // agent disabled

	ret = e2e_thread_join(a->agent_thread, &retval);
	if (ret)
		e2e_warn("agent: Failed to join agent thread");

	ret = e2e_thread_destroy(a->agent_thread);
	if (ret)
		e2e_warn("agent: Failed to destroy agent thread");

	free(a);

	return ret;
}

int e2e_agent_stop(e2e_agent *a)
{
	int ret;

	if (report_file)
		fclose(report_file);

	if (!a)
		return -EINVAL; // agent disabled

	ret = e2e_thread_stop(a->agent_thread);
	if (ret)
		e2e_warn("agent: Failed to cancel agent thread");

	return ret;
}

int e2e_agent_set_threads(e2e_agent *a, e2e_thread *lt, e2e_thread *tt)
{
	if (!a)
		return 0; // agent disabled

	if (!lt || !tt)
		return -EINVAL;

	a->listener_thread = lt;
	a->talker_thread = tt;

	return 0;
}

int e2e_agent_client_get_state(enum e2e_agent_client_state *state)
{
	*state = e2e_client_state;
	return 0;
}


bool e2e_agent_running(e2e_agent *a)
{
	if (!a)
		return false;

	return a->running;
}

int e2e_agent_add_to_log(void *ctx, bool to_stdout,
				struct e2e_agent_log_desc *d, const char *esc,
				const char *prefix, const char *fmt,
				va_list args)
{
	int ret;
	// char fmt_buf[512];
	char fmt_buf[256];
	va_list copy;
	(void)ctx;
	/* Add prefix and always append a newline to the fmt string */
	ret = snprintf(fmt_buf, sizeof(fmt_buf), "%s%s\n", prefix, fmt);
	if (ret >= sizeof(fmt_buf))
		return -1; // format string truncated

	/* Write into the agent log */
	va_copy(copy, args);
	ret = e2e_agent_log__add(d, fmt_buf, copy);
	if (ret < 0)
		return ret;

	if (to_stdout)
		return e2e_log(esc, "", "", fmt_buf, args);

	// if (d->reset_idx) {
	// 	d->reset_idx = false;
	// }

	return 0;
}

/* ctx allows us to be usable as e2e_thread compatible logger */
int e2e_agent_log_warn(void *ctx, bool to_stdout, const char *fmt, ...)
{
	int ret;
	va_list args;
	
	// write to report file
	if (report_file && to_stdout) {
		va_start(args, fmt);
		ret = vfprintf(report_file, fmt, args);
		va_end(args);
	}

	va_start(args, fmt);
	ret = e2e_agent_add_to_log(ctx, to_stdout, &logs.err, "33", "WARNING: ", fmt,
				   args);
	va_end(args);

	return ret;
}

int e2e_agent_log_warnv(void *ctx, const char *fmt, va_list args)
{
	return e2e_agent_add_to_log(ctx, true, &logs.err, "33",
				    "WARNING: ", fmt, args);
}

/* ctx allows us to be usable as e2e_thread compatible logger */
int e2e_agent_log_misc(void *ctx, const char *fmt, ...)
{
	int ret;
	va_list args;

	// write to report file
	if (report_file) {
		va_start(args, fmt);
        ret = vfprintf(report_file, fmt, args);
        va_end(args);
	}


	va_start(args, fmt);
	ret = e2e_agent_add_to_log(ctx, true, &logs.err, "31",
				   "MISCONFIGURATION: ", fmt, args);
	va_end(args);

	return ret;
}

int e2e_agent_log_report(const char *fmt, ...)
{
	int ret;
	va_list args;

	// write to report file
	if (report_file) {
		va_start(args, fmt);
        ret = vfprintf(report_file, fmt, args);
        va_end(args);
        //fflush(report_file); // optional: ensure it's written immediately
	}
	return ret;
}

/* ctx allows us to be usable as e2e_thread compatible logger */
int e2e_agent_log_crit(void *ctx, bool to_stdout, const char *fmt, ...)
{
	int ret;
	va_list args;

	// write to report file
	if (report_file) {
		va_start(args, fmt);
        ret = vfprintf(report_file, fmt, args);
        va_end(args);
	}
	va_start(args, fmt);
	ret = e2e_agent_add_to_log(ctx, to_stdout, &logs.err, "31", "", fmt, args);
	va_end(args);

	return ret;
}

/* ctx allows us to be usable as e2e_thread compatible logger */
int e2e_agent_log_info(void *ctx, bool to_std_out, const char *esc, const char *fmt, ...)
{
	int ret;
	va_list args;
	va_start(args, fmt);
	ret = e2e_agent_add_to_log(ctx, to_std_out, &logs.info, esc, "", fmt, args);
	va_end(args);

	return ret;
}

/* ctx allows us to be usable as e2e_thread compatible logger */
int e2e_agent_log_only(void *ctx, const char *fmt, ...)
{
	int ret;
	va_list args;

	// write to report file
	if (report_file) {
		va_start(args, fmt);
        ret = vfprintf(report_file, fmt, args);
        va_end(args);
	}
	va_start(args, fmt);
	ret = e2e_agent_add_to_log(ctx, false, &logs.info, "", "", fmt, args);
	va_end(args);

	return ret;
}