// SPDX-FileCopyrightText: Copyright 2025 Siemens AG
//
// SPDX-License-Identifier: Apache-2.0

#include <errno.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include <xdp/xsk.h>

#include <linux/errqueue.h>
#include <linux/if_packet.h>
#include <linux/net_tstamp.h>

#include "e2e_agent.h"
#include "e2e_bpf.h"
#include "e2e_clock.h"
#include "e2e_common.h"
#include "e2e_sock.h"

#define PKT_SZ_RX (2048) /* Size of iov buffer */
#define RX_PKT_CTRL_SZ (1024) /* Size of control buffer for RX timestamp */
#define INVALID_UMEM_FRAME UINT64_MAX
#define XDP_FRAME_NUM 4096

time_t fct_call_t, fct_ret_t;
long fct_runtime;

struct e2e_sock_umem_info {
	struct xsk_ring_prod fq;
	struct xsk_ring_cons cq;
	struct xsk_umem *umem;
	void *buffer;
};

struct e2e_sock_xdpsock_info {
	struct xsk_ring_cons rx;
	struct xsk_ring_prod tx;
	struct e2e_sock_umem_info *umem;
	struct xsk_socket *xsk;

	uint64_t umem_frame_addr[XDP_FRAME_NUM];
	uint32_t umem_frame_free;
};

typedef struct e2e_sock {
	const struct e2e_config *cfg;
	struct e2e_sock_cfg sock_cfg;
	int (*tx_hook)(void* ctx, const struct timespec *ts);
	void *tx_hook_ctx;
	union 
	{
		struct sockaddr_ll sa;
		struct sockaddr_in sin;
	};
	unsigned char rx_buf[PKT_SZ_RX];
	unsigned char tx_buf[ETH_FRAME_LEN];
	struct {
		struct e2e_sock_umem_info *umem;
		struct e2e_sock_xdpsock_info *info;
		void *packet_buf;
		size_t packet_buf_sz;
		int epoll_fd;
		struct epoll_event epoll_ev;
	} xdp;
	struct {
		struct iovec iov;
		struct msghdr msg;
		char control[CMSG_SPACE(sizeof(uint64_t))];
	} etf;
	struct e2e_sock_stats stats;
	int sock;
} e2e_sock;

static int e2e_sock_check_error_queue(e2e_sock *s, int packet_nr)
{
	uint8_t msg_control[CMSG_SPACE(sizeof(struct sock_extended_err))];
	unsigned char err_buffer[s->cfg->tx_packet_size + 18]; // TODO: Check!
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	struct sock_extended_err *serr;
	__u64 tstamp;

	memset(&msg, 0, sizeof(msg));
	iov.iov_base = err_buffer;
	iov.iov_len = sizeof(err_buffer);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = msg_control;
	msg.msg_controllen = sizeof(msg_control);

	if (recvmsg(s->sock, &msg, MSG_ERRQUEUE) == -1) {
		e2e_agent_log_warn(NULL, true, "recvmsg failed");
		return -1;
	}

	cmsg = CMSG_FIRSTHDR(&msg);
	while (cmsg != NULL) {
		serr = (struct sock_extended_err *)CMSG_DATA(cmsg);
		if (serr->ee_origin == SO_EE_ORIGIN_TXTIME) {
			tstamp = ((__u64)serr->ee_data << 32) + serr->ee_info;

			switch (serr->ee_code) {
			case SO_EE_CODE_TXTIME_INVALID_PARAM:
				e2e_agent_log_warn(
					NULL,
					true,
					"packet nr %d with tstamp %llu dropped due to invalid params\n",
					packet_nr, tstamp);
				s->stats.invalid_param++;
				return 0;
			case SO_EE_CODE_TXTIME_MISSED:
				e2e_agent_log_warn(
					NULL,
					true,
					"packet nr %d with tstamp %llu dropped due to missed deadline\n",
					packet_nr, tstamp);
				s->stats.tx_time_missed++;
				return 0;
			default:
				return -1;
			}
		}

		cmsg = CMSG_NXTHDR(&msg, cmsg);
	}

	return 0;
}

static int e2e_sock_xdp_umem_create(e2e_sock *s)
{
	struct e2e_sock_umem_info *umem;
	int ret;

	umem = calloc(1, sizeof(*umem));
	if (!umem)
		return -ENOMEM;

	ret = xsk_umem__create(&umem->umem, s->xdp.packet_buf,
			       s->xdp.packet_buf_sz, &umem->fq, &umem->cq,
			       NULL);
	if (ret) {
		free(umem);
		return ret;
	}

	umem->buffer = s->xdp.packet_buf;
	s->xdp.umem = umem;

	return 0;
}

static uint64_t e2e_sock_umem_alloc_frame(struct e2e_sock_xdpsock_info *xsk)
{
	uint64_t frame;

	if (xsk->umem_frame_free == 0)
		return INVALID_UMEM_FRAME;

	frame = xsk->umem_frame_addr[--xsk->umem_frame_free];
	xsk->umem_frame_addr[xsk->umem_frame_free] = INVALID_UMEM_FRAME;
	return frame;
}

static void e2e_sock_umem_free_frame(struct e2e_sock_xdpsock_info *xsk,
				     uint64_t frame)
{
	xsk->umem_frame_addr[xsk->umem_frame_free++] = frame;
}

static uint64_t e2e_sock_umem_get_free_frames(struct e2e_sock_xdpsock_info *xsk)
{
	return xsk->umem_frame_free;
}

static int e2e_sock_xdp_init_epoll(e2e_sock *s)
{
	int sock;
	int ret;

	if (!s->cfg->xdp_use_epoll)
		return 0;

	sock = xsk_socket__fd(s->xdp.info->xsk);
	s->xdp.epoll_fd = epoll_create1(0);
	if (s->xdp.epoll_fd == -1)
		return -errno;

	s->xdp.epoll_ev.events = EPOLLOUT;
	// s->xdp.epoll_ev.data.u64 = (uint64_t) s->xdp.info->xsk;
	ret = epoll_ctl(s->xdp.epoll_fd, EPOLL_CTL_ADD, sock, &s->xdp.epoll_ev);
	if (ret == -1)
		return -errno;

	return ret;
}

static int e2e_sock_xdp_init_sock(e2e_sock *s)
{
	struct e2e_sock_xdpsock_info *xsk_info;
	struct xsk_socket_config xsk_cfg;
	uint32_t ret_res;
	uint32_t idx;
	int ret;
	int i;

	xsk_info = calloc(1, sizeof(*xsk_info));
	if (!xsk_info)
		return -ENOMEM;

	xsk_info->umem = s->xdp.umem;
	xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
	xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
	xsk_cfg.libbpf_flags = s->cfg->libbpf_flags;
	xsk_cfg.xdp_flags = s->cfg->xdp_flags;
	xsk_cfg.bind_flags = s->cfg->xdp_bind_flags;
	ret = xsk_socket__create(&xsk_info->xsk, s->cfg->iface,
				 s->sock_cfg.xdp.rx_queue_id, s->xdp.umem->umem,
				 &xsk_info->rx, &xsk_info->tx, &xsk_cfg);
	if (ret)
		goto error_exit;

	idx = if_nametoindex(s->cfg->iface);

	/* Initialize umem frame allocation */
	for (i = 0; i < XDP_FRAME_NUM; i++)
		xsk_info->umem_frame_addr[i] = i * XSK_UMEM__DEFAULT_FRAME_SIZE;

	xsk_info->umem_frame_free = XDP_FRAME_NUM;

	/* Stuff the receive path with buffers, we assume we have enough */
	ret_res = xsk_ring_prod__reserve(
		&xsk_info->umem->fq, XSK_RING_PROD__DEFAULT_NUM_DESCS, &idx);

	if (ret_res != XSK_RING_PROD__DEFAULT_NUM_DESCS)
		goto error_exit;

	for (i = 0; i < XSK_RING_PROD__DEFAULT_NUM_DESCS; i++)
		*xsk_ring_prod__fill_addr(&xsk_info->umem->fq, idx++) =
			e2e_sock_umem_alloc_frame(xsk_info);

	xsk_ring_prod__submit(&xsk_info->umem->fq,
			      XSK_RING_PROD__DEFAULT_NUM_DESCS);

	s->xdp.info = xsk_info;

	return e2e_sock_xdp_init_epoll(s);

error_exit:
	free(xsk_info);
	return ret;
}

static int e2e_sock_validate_cfg(const struct e2e_sock_cfg *cfg)
{
	if (!cfg)
		return -EINVAL;

	return 0;
}

static int e2e_sock_init_udp(e2e_sock *s)
{
	
    int tos = s->cfg->dscp_prio << 2; // Shift left by 2 bits for ECN

	if (!s)
		return -ENODEV;

	if (s->cfg->tx_packet_size + LEN_UDP_VLAN_HDR > sizeof(s->tx_buf))
		return -EINVAL;

	s->sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (s->sock == -1)
		return -errno;
	
    // Set DSCP via IP_TOS
    if (setsockopt(s->sock, IPPROTO_IP, IP_TOS, &tos, sizeof(tos)) < 0) {
        perror("setsockopt");
        close(s->sock);
        return -errno;;
    }


	return 0;
}


static int e2e_sock_init_raw(e2e_sock *s)
{
	if (!s)
		return -ENODEV;

	if (s->cfg->tx_packet_size + LEN_ETH_VLAN_HDR > sizeof(s->tx_buf))
		return -EINVAL;

	s->sock = socket(AF_PACKET, SOCK_RAW, s->sock_cfg.raw.protocol);
	if (s->sock == -1)
		return -errno;

	return 0;
}

static void e2e_sock_destroy_xdp(e2e_sock *s)
{
	if (s->cfg->xdp_use_epoll)
		close(s->xdp.epoll_fd);

	xsk_socket__delete(s->xdp.info->xsk);
	free(s->xdp.info);
	xsk_umem__delete(s->xdp.umem->umem);
	free(s->xdp.umem);
	free(s->xdp.packet_buf);
}

static int e2e_sock_init_xdp(e2e_sock *s)
{
	struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };
	int sock_fd;
	int idx;
	int ret;

	/*
	 * Allow unlimited locking of memory, so all memory needed for packet
	 * buffers can be locked.
	 */
	ret = setrlimit(RLIMIT_MEMLOCK, &rlim);
	if (ret) {
		fprintf(stderr, "ERROR: setrlimit(RLIMIT_MEMLOCK) \"%s\"\n",
			strerror(errno));
		return ret;
	}

	/*
	 * Allocate PAGE_SIZE aligned memory for NUM_FRAMES of the default XDP
	 * frame size
	 */
	s->xdp.packet_buf_sz = XDP_FRAME_NUM * XSK_UMEM__DEFAULT_FRAME_SIZE;
	ret = posix_memalign(&s->xdp.packet_buf, getpagesize(),
			     s->xdp.packet_buf_sz);
	if (ret) {
		fprintf(stderr, "ERROR: Can't allocate buffer memory \"%s\"\n",
			strerror(errno));
		return ret;
	}
	memset(s->xdp.packet_buf, 0, s->xdp.packet_buf_sz);

	/* Initialize shared packet_buffer for umem app_usage */
	ret = e2e_sock_xdp_umem_create(s);
	if (ret) {
		free(s->xdp.packet_buf);
		fprintf(stderr, "ERROR: Can't create umem \"%s\"\n",
			strerror(-ret));
		return ret;
	}

	/* Open and configure the AF_XDP (xsk) socket */
	ret = e2e_sock_xdp_init_sock(s);
	if (ret) {
		xsk_umem__delete(s->xdp.umem->umem);
		free(s->xdp.umem);
		free(s->xdp.packet_buf);
		fprintf(stderr, "ERROR: Can't setup AF_XDP socket \"%s\"\n",
			strerror(-ret));
		return ret;
	}

	sock_fd = xsk_socket__fd(s->xdp.info->xsk);
	idx = s->sock_cfg.xdp.rx_queue_id;

	return e2e_bpf_update_xsk_map(idx, sock_fd);
}

static int e2e_sock_init(e2e_sock *s)
{
	int ret;

	if (!s)
		return -EINVAL;

	if (s->sock_cfg.type == E2E_SOCK_UDP)
	{
		ret = e2e_sock_init_udp(s);
		return ret;
	}

	ret = e2e_sock_init_raw(s);
	if (ret)
		return ret;

	if (s->sock_cfg.type == E2E_SOCK_XDP)
		ret = e2e_sock_init_xdp(s);

	return ret;
}

static int e2e_sock_call_tx_hook(e2e_sock *s, const struct timespec *ts)
{
	if (!s->tx_hook)
		return 0;

	return s->tx_hook(s->tx_hook_ctx, ts);
}

static int e2e_sock_send_hw(e2e_sock *s, unsigned char *buf, size_t buf_sz,
			    const struct timespec *send_time)
{
	ssize_t bytes;
	int ret;

	if (!s || !buf || buf_sz == 0 || !send_time)
		return -EINVAL;

	ret = e2e_sock_call_tx_hook(s, send_time);
	if (ret)
		return ret;

	bytes = e2e_sock_sendto(s, buf, buf_sz, 0);

	return (bytes < 1) ? -errno : 0;
}

static int e2e_sock_send_sw(e2e_sock *s, unsigned char *buf, size_t buf_sz)
{
	struct timespec phc;
	size_t bytes;
	int ret;

	if (!s || !buf || buf_sz == 0)
		return -EINVAL;

	phc = e2e_clock_get(s->cfg);

	ret = e2e_sock_call_tx_hook(s, &phc);
	if (ret)
		return ret;

	bytes = e2e_sock_sendto(s, buf, buf_sz, 0);

	return (bytes < 1) ? -errno : 0;
}

static int e2e_sock_send_etf(e2e_sock *s, unsigned char *buf, size_t buf_sz,
			     const struct timespec *send_time,
			     uint32_t packet_nr)
{
	struct pollfd p_fd = {
		.fd = s->sock,
	};
	struct cmsghdr *cmsg;
	ssize_t bytes;
	int poll_res;
	int ret;

	if (!s || !buf || !send_time)
		return -EINVAL;

	/* Write send time into packet payload */
	ret = e2e_sock_call_tx_hook(s, send_time);
	if (ret)
		return ret;

	/* Write send time into auxiliary data */
	cmsg = CMSG_FIRSTHDR(&s->etf.msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_TXTIME;
	cmsg->cmsg_len = CMSG_LEN(sizeof(__u64));
	uint64_t send_time_ns_1970 =
		send_time->tv_nsec + (send_time->tv_sec * NSEC_PER_SEC);
	*((__u64 *)CMSG_DATA(cmsg)) = send_time_ns_1970;
	bytes = sendmsg(s->sock, &s->etf.msg, 0);
	if (bytes == -1)
		ret = -errno;

	if (ret == -ENOBUFS)
		s->stats.no_buffer_space++;

	/* Check if errors are pending on the error queue. */
	poll_res = poll(&p_fd, 1, 0);
	if (poll_res == 1 && p_fd.revents & POLLERR) {
		if (!e2e_sock_check_error_queue(s, packet_nr)) {
#ifdef ENABLE_DEBUG_SEND
			printf("process socket error queue error\n");
#endif
		}
	}

	return ret;
}

// struct timeval tv;
struct timeval tv;


static ssize_t e2e_sock_recvmsg(e2e_sock *s, struct msghdr *msg, int time_out)
{	
	if (!s)
		return -EINVAL;
	
	if (time_out) 
	{
		fd_set e2e_socket;
		/* set timeout [s] */
		tv.tv_sec = time_out;
		
		FD_ZERO(&e2e_socket);
		FD_SET(s->sock, &e2e_socket);
		
		// ready_sock = current_sock;
		if (select(s->sock + 1, &e2e_socket, NULL, NULL, &tv) < 0) 
		{
			return (-1);
		}
		if(FD_ISSET(s->sock, &e2e_socket)) 
		{
			return (recvmsg(s->sock, msg, 0));
		}
	}	
	else
	{
		// blocking for time_out == 0
		return (recvmsg(s->sock, msg, 0));
	}
	return (0);
}

static int e2e_sock_raw_extract_ts(struct msghdr *msg, struct timespec *ts)
{
	struct cmsghdr *cmsg;
	struct timespec *t = NULL;

	for (cmsg = CMSG_FIRSTHDR(msg); cmsg; cmsg = CMSG_NXTHDR(msg, cmsg)) {
		if (cmsg->cmsg_level != SOL_SOCKET)
			continue;

		switch (cmsg->cmsg_type) {
		case SO_TIMESTAMPING:
			t = (struct timespec *)CMSG_DATA(cmsg);
			break;
		default:
			/* Ignore other cmsg options */
			break;
		}
	}

	if (!t) {
		e2e_agent_log_crit(NULL, true, "Unable to get receive timestamp");
		e2e_agent_log_crit(NULL, true, "Check settings and synchronization!");
		return -EBADMSG;
	}

#ifdef ENABLE_DEBUG_TS
	printf("ts 0: %d sec, %d nsec, ts 1: %d sec, %d nsec, ts2: %d sec, %d nsec\n",
	       ts[0].tv_sec, ts[0].tv_nsec, ts[1].tv_sec, ts[1].tv_nsec,
	       ts[2].tv_sec, ts[2].tv_nsec);
#endif

	*ts = t[2];

	return 0;
}

static int e2e_sock_raw_rx(e2e_sock *s, struct timespec *ts, ssize_t *rec_bytes, int time_out )
{
	struct sockaddr_in host_address = { 0 };
	const struct e2e_config *cfg = s->cfg;
	char control[RX_PKT_CTRL_SZ];
	struct msghdr msg;
	struct iovec iov;
	int ret = 0;

	host_address.sin_family = AF_INET;
	host_address.sin_port = htons(0);
	host_address.sin_addr.s_addr = INADDR_ANY;

	/* recvmsg header structure */
	iov.iov_base = (void *)&s->rx_buf;
	iov.iov_len = sizeof(s->rx_buf);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_name = &host_address;
	msg.msg_namelen = sizeof(struct sockaddr_in);
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);
	
	*rec_bytes = e2e_sock_recvmsg(s, &msg, time_out);
	if (*rec_bytes <= 0) 
	{
		ret = *rec_bytes;
		if (*rec_bytes == 0)
			ret = -ETIME;
		return ret;
	}
		
	if (cfg->receive_mode == HW_BASED || cfg->receive_mode == SW_BASED)
		ret = e2e_sock_raw_extract_ts(&msg, ts);
	else
		clock_gettime(cfg->clkid, ts);

	return ret;
}

/* TODO: check if own function for UDP is necessary */
static int e2e_sock_udp_rx(e2e_sock *s, struct timespec *ts, ssize_t *rec_bytes, int time_out)
{
	struct sockaddr_in host_address = { 0 };
	const struct e2e_config *cfg = s->cfg;
	char control[RX_PKT_CTRL_SZ];
	struct msghdr msg;
	struct iovec iov;
	int ret = 0;

	host_address.sin_family = AF_INET;
	host_address.sin_port = htons(0);
	host_address.sin_addr.s_addr = INADDR_ANY;

	/* recvmsg header structure */
	iov.iov_base = (void *)&s->rx_buf;
	iov.iov_len = sizeof(s->rx_buf);
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_name = &host_address;
	msg.msg_namelen = sizeof(struct sockaddr_in);
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	*rec_bytes = e2e_sock_recvmsg(s, &msg, time_out);
	if (*rec_bytes <= 0) 
	{
		ret = *rec_bytes;
		if (*rec_bytes == 0)
			ret = -ETIME;
		return ret;
	}
	
	if (cfg->receive_mode == HW_BASED || cfg->receive_mode == SW_BASED)
		ret = e2e_sock_raw_extract_ts(&msg, ts);
	else
		clock_gettime(cfg->clkid, ts);

	return ret;
}

static int e2e_sock_xdp_rx(e2e_sock *s, void **p, ssize_t *p_sz,
			   struct timespec *ts)
{
	struct e2e_sock_xdpsock_info *xsk = s->xdp.info;
	struct ether_header *eth;
	struct pollfd fds[2];
	__u32 stock_frames;
	uint64_t data_addr;
	__u32 idx_rx = 0;
	__u32 idx_fq = 0;
	unsigned int i;
	int nfds = 1;
	int pollret;
	__u32 ret;
	__u32 rcvd;

	memset(fds, 0, sizeof(fds));
	fds[0].fd = xsk_socket__fd(xsk->xsk);
	fds[0].events = POLLIN;

	/* Yield the CPU until a packet arrived */
	pollret = poll(fds, nfds, -1);
	if (pollret <= 0 || pollret > 1)
		return -errno;

	rcvd = xsk_ring_cons__peek(&xsk->rx, 1, &idx_rx);
	if (!rcvd) {
		*p = NULL;
		*p_sz = 0;
		return -EAGAIN;
	}

	clock_gettime(s->cfg->clkid, ts);

	/* Stuff the ring with as much frames as possible */
	stock_frames = xsk_prod_nb_free(&xsk->umem->fq,
					e2e_sock_umem_get_free_frames(xsk));

	if (stock_frames > 0) {
		ret = xsk_ring_prod__reserve(&xsk->umem->fq, stock_frames,
					     &idx_fq);

		/* This should not happen, but just in case */
		while (ret != stock_frames)
			ret = xsk_ring_prod__reserve(&xsk->umem->fq, rcvd,
						     &idx_fq);

		for (i = 0; i < stock_frames; i++)
			*xsk_ring_prod__fill_addr(&xsk->umem->fq, idx_fq++) =
				e2e_sock_umem_alloc_frame(xsk);

		xsk_ring_prod__submit(&xsk->umem->fq, stock_frames);
	}

	/* Fetch packet address and size from the descriptor */
	data_addr = xsk_ring_cons__rx_desc(&xsk->rx, idx_rx)->addr;
	*p_sz = (ssize_t)xsk_ring_cons__rx_desc(&xsk->rx, idx_rx++)->len;
	*p = xsk_umem__get_data(xsk->umem->buffer, data_addr);

	/* Remove the 802.1q VLAN tag if we received one */
	eth = *p;
	if (*p_sz >= sizeof(*eth) && eth->ether_type == htons(ETHERTYPE_VLAN)) {
		*p = (unsigned char *)*p + 4;
		*p_sz -= 4;
	}

	e2e_sock_umem_free_frame(xsk, data_addr);
	xsk_ring_cons__release(&xsk->rx, rcvd);

	return 0;
}

static int e2e_sock_xdp_tx_kick(e2e_sock *s)
{
	struct e2e_sock_xdpsock_info *xsk;
	struct epoll_event events[1];
	ssize_t bytes;
	int sock;
	int fds;

	if (s->cfg->xdp_use_epoll) {
		fds = epoll_wait(s->xdp.epoll_fd, events, 1, -1);
		return (fds == -1) ? -errno : 0;
	}

	xsk = s->xdp.info;
	sock = xsk_socket__fd(xsk->xsk);
	bytes = sendto(sock, NULL, 0, MSG_DONTWAIT, NULL, 0);

	return (bytes < 0) ? -errno : 0;
}

static int e2e_sock_xdp_send(e2e_sock *s, unsigned char *buf, size_t buf_sz,
			     const struct timespec *send_time,
			     uint32_t packet_nr)
{
	struct e2e_sock_xdpsock_info *xsk;
	struct timespec ts = *send_time;
	uint32_t completed;
	uint32_t cq_idx;
	uint32_t tx_idx;
	uint32_t tx_cnt;
	uint64_t addr;
	int ret;

	if (!s)
		return -EINVAL;

	xsk = s->xdp.info;

	/* Collect/free completed TX buffers */
	completed = xsk_ring_cons__peek(
		&xsk->umem->cq, XSK_RING_CONS__DEFAULT_NUM_DESCS, &cq_idx);

	/*
	 * Note: Normally we would iterate over the number of completed entries
	 * of the completion queue and free all frames by calling
	 * e2e_sock_umem_free_frame().
	 *
	 * We are always using the first slot of the umem area, so no
	 * iteration necessary.
	 */
	if (completed)
		xsk_ring_cons__release(&xsk->umem->cq, completed);
	else if (packet_nr > 1)
		return -EALREADY; /* The umem area is already queued */

	/* Reserve desc inside TX queue */
	tx_cnt = xsk_ring_prod__reserve(&xsk->tx, 1, &tx_idx);
	if (tx_cnt != 1)
		return -EBUSY; /* TX queue full, drop the packet */

	if (s->cfg->send_mode == SW_BASED)
		ts = e2e_clock_get(s->cfg);

	ret = e2e_sock_call_tx_hook(s, &ts);
	if (ret)
		return ret;

	addr = (uint64_t)buf - (uint64_t)s->xdp.packet_buf;
	xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->addr = addr;
	xsk_ring_prod__tx_desc(&xsk->tx, tx_idx)->len = buf_sz;
	xsk_ring_prod__submit(&xsk->tx, 1);

	return e2e_sock_xdp_tx_kick(s);
}

static int e2e_sock_raw_send(e2e_sock *s, unsigned char *buf, size_t buf_sz,
			     const struct timespec *send_time,
			     uint32_t packet_nr)
{
	int ret;

	if (s->cfg->send_mode == HW_BASED)
		ret = e2e_sock_send_hw(s, buf, buf_sz, send_time);
	else if (s->cfg->send_mode == ETF)
		ret = e2e_sock_send_etf(s, buf, buf_sz, send_time, packet_nr);
	else /* s->cfg->send_mode == SW_BASED */
		ret = e2e_sock_send_sw(s, buf, buf_sz);

	return ret;
}

int e2e_sock_create(e2e_sock **s, const struct e2e_sock_cfg *sock_cfg,
		    const struct e2e_config *cfg)
{
	e2e_sock *new;
	int ret;

	/* Make sure we do not overwrite/leak something */
	if (*s)
		return -EINVAL;

	ret = e2e_sock_validate_cfg(sock_cfg);
	if (ret)
		return ret;

	new = calloc(1, sizeof(e2e_sock));
	if (!new)
		return -ENOMEM;

	new->cfg = cfg;
	new->sock_cfg = *sock_cfg;

	ret = e2e_sock_init(new);
	if (ret) {
		free(new);
		return ret;
	}

	*s = new;

	return 0;
}

int e2e_sock_destroy(e2e_sock *s)
{
	if (!s)
		return -EINVAL;

	close(s->sock);

	if (s->sock_cfg.type == E2E_SOCK_XDP)
		e2e_sock_destroy_xdp(s);

	free(s);

	return 0;
}

int e2e_sock_setopt(e2e_sock *s, int level, int optname, const void *optval,
		    socklen_t len)
{
	if (!s)
		return -EINVAL;

	return setsockopt(s->sock, level, optname, optval, len);
}

int e2e_sock_ioctl(e2e_sock *s, unsigned long int req, void *resp)
{
	if (!s)
		return -EINVAL;

	return ioctl(s->sock, req, resp);
}

ssize_t e2e_sock_sendto(e2e_sock *s, const void *buf, size_t buf_sz, int flags)
{
	if (!s)
		return -EINVAL;

	return sendto(s->sock, buf, buf_sz, flags, (struct sockaddr *)&s->sa,
		      sizeof(s->sa));
}

int e2e_sock_bind(e2e_sock *s)
{
	if (!s)
		return -EINVAL;

	/* Print bind target for visibility */
	if (s->sock_cfg.type == E2E_SOCK_UDP) {
		char addrstr[INET_ADDRSTRLEN] = "";
		struct in_addr in = s->sin.sin_addr;
		inet_ntop(AF_INET, &in, addrstr, sizeof(addrstr));
		e2e_info(ESC_INFO, "Binding UDP socket to %s:%d\n", addrstr, ntohs(s->sin.sin_port));
		{
			int ret = bind(s->sock, (struct sockaddr *)&s->sin, sizeof(s->sin));
			if (ret == -1) {
				e2e_agent_log_crit(NULL, true, "Failed to bind UDP socket to %s:%d: %s\n", addrstr, ntohs(s->sin.sin_port), strerror(errno));
				return -errno;
			}
			return 0;
		}
	} else {
		char ifname[IFNAMSIZ] = "";
		if (if_indextoname(s->sa.sll_ifindex, ifname)) {
			e2e_info(ESC_INFO, "Binding raw socket on iface %s, proto 0x%x\n", ifname, ntohs(s->sa.sll_protocol));
		} else {
			e2e_info(ESC_INFO, "Binding raw socket on ifindex %d, proto 0x%x\n", s->sa.sll_ifindex, ntohs(s->sa.sll_protocol));
		}
		{
			int ret = bind(s->sock, (struct sockaddr *)&s->sa, sizeof(s->sa));
			if (ret == -1) {
				char ifname[IFNAMSIZ] = "";
				if (if_indextoname(s->sa.sll_ifindex, ifname)) {
					e2e_agent_log_crit(NULL, true, "Failed to bind raw socket on iface %s: %s\n", ifname, strerror(errno));
				} else {
					e2e_agent_log_crit(NULL, true, "Failed to bind raw socket on ifindex %d: %s\n", s->sa.sll_ifindex, strerror(errno));
				}
				return -errno;
			}
			return 0;
		}
	}
}

int e2e_sock_init_sockaddr_tx(e2e_sock *s)
{
	struct ifreq if_idx = { 0 };
	int ret = 0;

	if (!s)
		return -EINVAL;

	if (s->sock_cfg.type == E2E_SOCK_UDP) {
		memcpy((void*)&(s->sin.sin_addr), s->cfg->dest_ip, sizeof(s->cfg->dest_ip));
		s->sin.sin_port = htons(s->cfg->dest_port);

		if (s->cfg->send_mode == ETF) {
			s->etf.iov.iov_base = &s->tx_buf;
			s->etf.iov.iov_len = s->cfg->tx_packet_size + LEN_UDP_VLAN_HDR;

			s->etf.msg.msg_name = &s->sin;
			s->etf.msg.msg_namelen = sizeof(s->sin);
			s->etf.msg.msg_iov = &s->etf.iov;
			s->etf.msg.msg_iovlen = 1;

			/* Specify the transmission time in the CMSG. */
			s->etf.msg.msg_control = &s->etf.control;
			s->etf.msg.msg_controllen = sizeof(s->etf.control);
		}
	} else {
		strncpy(if_idx.ifr_name, s->cfg->iface, IFNAMSIZ - 1);

		ret = e2e_sock_ioctl(s, SIOCGIFINDEX, &if_idx);
		if (ret == -1) {
			perror("SIOCGIFINDEX");
			return -errno;
		}

		/* Index of the network device */
		s->sa.sll_ifindex = if_idx.ifr_ifindex;

		/* Address length*/
		s->sa.sll_halen = ETH_ALEN;

		memcpy(s->sa.sll_addr, s->cfg->dest_mac, sizeof(s->cfg->dest_mac));

		if (s->cfg->send_mode == ETF) {
			s->sa.sll_family = AF_PACKET;

			s->etf.iov.iov_base = &s->tx_buf;
			s->etf.iov.iov_len = s->cfg->tx_packet_size + LEN_ETH_VLAN_HDR;

			s->etf.msg.msg_name = &s->sa;
			s->etf.msg.msg_namelen = sizeof(s->sa);
			s->etf.msg.msg_iov = &s->etf.iov;
			s->etf.msg.msg_iovlen = 1;

			/* Specify the transmission time in the CMSG. */
			s->etf.msg.msg_control = &s->etf.control;
			s->etf.msg.msg_controllen = sizeof(s->etf.control);
		}
	}

	return ret;
}

int e2e_sock_init_sockaddr_rx(e2e_sock *s)
{
	unsigned idx;

	if (!s)
		return -EINVAL;

	if (s->sock_cfg.type == E2E_SOCK_UDP) {
		/* If configured to bind listener IP, always use it regardless of
		 * whether the destination is multicast. Use bind_listener_port when
		 * configured; otherwise fall back to dest_port.
		 */
		/* Always honor the configured bind_listener_ip and bind_listener_port
		 * for UDP listeners. If the user didn't specify a port, bind_listener_port
		 * will contain the default (set in config defaults).
		 */
		{
			uint8_t *b = (uint8_t *)s->cfg->bind_listener_ip;
			uint32_t host_ip = (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
			s->sin.sin_family = AF_INET;
			s->sin.sin_addr.s_addr = htonl(host_ip);
			s->sin.sin_port = htons(s->cfg->bind_listener_port);
		}
	} else {
		idx = if_nametoindex(s->cfg->iface);

		s->sa.sll_ifindex = (int)idx;
		s->sa.sll_family = AF_PACKET;
		s->sa.sll_protocol = htons(s->sock_cfg.raw.protocol);
	}

	return 0;
}

int e2e_sock_send(e2e_sock *s, unsigned char *buf, size_t buf_sz,
		  const struct timespec *send_time, uint32_t packet_nr)
{
	if (!s)
		return -EINVAL;

	if ((s->sock_cfg.type == E2E_SOCK_RAW) || (s->sock_cfg.type == E2E_SOCK_UDP))
		return e2e_sock_raw_send(s, buf, buf_sz, send_time, packet_nr);

	return e2e_sock_xdp_send(s, buf, buf_sz, send_time, packet_nr);
}

int e2e_sock_rx(e2e_sock *s, void **p, ssize_t *p_sz, struct timespec *ts, int time_out, void *arg)
{
	struct ether_header *eth;
	struct e2e_config *cfg = (struct e2e_config *)arg;
	int ret = 0;
	if (!s)
		return -EINVAL;

	if (s->sock_cfg.type == E2E_SOCK_UDP) {
		ret = e2e_sock_udp_rx(s, ts, p_sz, time_out);
		*p = s->rx_buf;
	}

	if (s->sock_cfg.type == E2E_SOCK_RAW) {
		ret = e2e_sock_raw_rx(s, ts, p_sz, time_out);
		*p = s->rx_buf;
	}

	if (s->sock_cfg.type == E2E_SOCK_XDP)
		ret = e2e_sock_xdp_rx(s, p, p_sz, ts);

	if (ret)
		return ret;

	if ((s->sock_cfg.type == E2E_SOCK_RAW) || (s->sock_cfg.type == E2E_SOCK_XDP))
	{
		/* Make sure that at least the EtherType field was received */
		if (unlikely(*p_sz < sizeof(struct ether_header)))
			return -EBADMSG;

		/*
		* Check that we received packets of our own protocol by reading
		* the EtherType field
		*/
		eth = *p;
		if (unlikely(eth->ether_type != cfg->ethertype)) {
			printf("Wrong ethretype: %x <> %x\n", eth->ether_type, cfg->ethertype);
			return -EBADMSG;
		}
	}

	return ret;
}

const struct e2e_sock_stats *e2e_sock_get_stats(e2e_sock *s)
{
	if (!s)
		return NULL;

	return &s->stats;
}

int e2e_sock_get_tx_buf(e2e_sock *s, void **buf, size_t *buf_sz)
{
	unsigned char *pkt;
	uint64_t frame;
	int ret = 0;
	size_t sz;

	if (!s || !buf) /* buf_sz is optional */
		return -EINVAL;

	if (s->sock_cfg.type == E2E_SOCK_UDP) {
		*buf = &s->tx_buf;
		sz = sizeof(s->tx_buf);
	}

	if (s->sock_cfg.type == E2E_SOCK_RAW) {
		*buf = &s->tx_buf;
		sz = sizeof(s->tx_buf);
	}

	if (s->sock_cfg.type == E2E_SOCK_XDP) {
		frame = e2e_sock_umem_alloc_frame(s->xdp.info);
		pkt = xsk_umem__get_data(s->xdp.umem->buffer, frame);
		*buf = pkt;
		sz = XSK_UMEM__DEFAULT_FRAME_SIZE;
	}

	if (buf_sz)
		*buf_sz = sz;

	return ret;
}

int e2e_sock_set_tx_hook(e2e_sock *s, void *ctx,
			 int (*hook)(void *ctx, const struct timespec *ts))
{
	if (!s)
		return -EINVAL;

	s->tx_hook = hook;
	s->tx_hook_ctx = ctx;

	return 0;
}
