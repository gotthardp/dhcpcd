/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Privilege Separation for dhcpcd, network proxy
 * Copyright (c) 2006-2020 Roy Marples <roy@marples.name>
 * All rights reserved

 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/socket.h>
#include <sys/types.h>

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "arp.h"
#include "bpf.h"
#include "dhcp.h"
#include "dhcp6.h"
#include "eloop.h"
#include "ipv6nd.h"
#include "logerr.h"
#include "privsep.h"

#ifdef HAVE_CAPSICUM
#include <sys/capsicum.h>
#endif

#ifdef INET
static void
ps_inet_recvbootp(void *arg)
{
	struct dhcpcd_ctx *ctx = arg;

	if (ps_recvmsg(ctx, ctx->udp_rfd, PS_BOOTP, ctx->ps_inet_fd) == -1)
		logerr(__func__);
}
#endif

#ifdef INET6
static void
ps_inet_recvra(void *arg)
{
#ifdef __sun
	struct interface *ifp = arg;
	struct rs_state *state = RS_STATE(ifp);
	struct dhcpcd_ctx *ctx = ifp->ctx;

	if (ps_recvmsg(ctx, state->nd_fd, PS_ND, ctx->ps_inet_fd) == -1)
		logerr(__func__);
#else
	struct dhcpcd_ctx *ctx = arg;

	if (ps_recvmsg(ctx, ctx->nd_fd, PS_ND, ctx->ps_inet_fd) == -1)
		logerr(__func__);
#endif
}
#endif

#ifdef DHCP6
static void
ps_inet_recvdhcp6(void *arg)
{
	struct dhcpcd_ctx *ctx = arg;

	if (ps_recvmsg(ctx, ctx->dhcp6_rfd, PS_DHCP6, ctx->ps_inet_fd) == -1)
		logerr(__func__);
}
#endif

static int
ps_inet_startcb(void *arg)
{
	struct dhcpcd_ctx *ctx = arg;
	int ret = 0;
#ifdef HAVE_CAPSICUM
	cap_rights_t rights;

	cap_rights_init(&rights, CAP_RECV, CAP_EVENT);
#endif

	if (ctx->options & DHCPCD_MASTER)
		setproctitle("[network proxy]");
	else
		setproctitle("[network proxy] %s%s%s",
		    ctx->ifv[0],
		    ctx->options & DHCPCD_IPV4 ? " [ip4]" : "",
		    ctx->options & DHCPCD_IPV6 ? " [ip6]" : "");

	/* This end is the main engine, so it's useless for us. */
	close(ctx->ps_data_fd);
	ctx->ps_data_fd = -1;

	errno = 0;

#ifdef INET
	if ((ctx->options & (DHCPCD_IPV4 | DHCPCD_MASTER)) ==
	    (DHCPCD_IPV4 | DHCPCD_MASTER))
	{
		ctx->udp_rfd = dhcp_openudp(NULL);
		if (ctx->udp_rfd == -1)
			logerr("%s: dhcp_open", __func__);
#ifdef HAVE_CAPSICUM
		else if (cap_rights_limit(ctx->udp_rfd, &rights) == -1
		    && errno != ENOSYS)
		{
			logerr("%s: cap_rights_limit", __func__);
			close(ctx->udp_rfd);
			ctx->udp_rfd = -1;
		}
#endif
		else if (eloop_event_add(ctx->eloop, ctx->udp_rfd,
		    ps_inet_recvbootp, ctx) == -1)
		{
			logerr("%s: eloop_event_add DHCP", __func__);
			close(ctx->udp_rfd);
			ctx->udp_rfd = -1;
		} else
			ret++;
	}
#endif
#if defined(INET6) && !defined(__sun)
	if (ctx->options & DHCPCD_IPV6) {
		ctx->nd_fd = ipv6nd_open(true);
		if (ctx->nd_fd == -1)
			logerr("%s: ipv6nd_open", __func__);
#ifdef HAVE_CAPSICUM
		else if (cap_rights_limit(ctx->nd_fd, &rights) == -1
		    && errno != ENOSYS)
		{
			logerr("%s: cap_rights_limit", __func__);
			close(ctx->nd_fd);
			ctx->nd_fd = -1;
		}
#endif
		else if (eloop_event_add(ctx->eloop, ctx->nd_fd,
		    ps_inet_recvra, ctx) == -1)
		{
			logerr("%s: eloop_event_add RA", __func__);
			close(ctx->nd_fd);
			ctx->nd_fd = -1;
		} else
			ret++;
	}
#endif
#ifdef DHCP6
	if ((ctx->options & (DHCPCD_DHCP6 | DHCPCD_MASTER)) ==
	    (DHCPCD_DHCP6 | DHCPCD_MASTER))
	{
		ctx->dhcp6_rfd = dhcp6_openudp(0, NULL);
		if (ctx->dhcp6_rfd == -1)
			logerr("%s: dhcp6_open", __func__);
#ifdef HAVE_CAPSICUM
		else if (cap_rights_limit(ctx->dhcp6_rfd, &rights) == -1
		    && errno != ENOSYS)
		{
			logerr("%s: cap_rights_limit", __func__);
			close(ctx->dhcp6_rfd);
			ctx->dhcp6_rfd = -1;
		}
#endif
		else if (eloop_event_add(ctx->eloop, ctx->dhcp6_rfd,
		    ps_inet_recvdhcp6, ctx) == -1)
		{
			logerr("%s: eloop_event_add DHCP6", __func__);
			close(ctx->dhcp6_rfd);
			ctx->dhcp6_rfd = -1;
		} else
			ret++;
	}
#endif

	if (ret == 0 && errno == 0) {
		errno = ENXIO;
		return -1;
	}
	return ret;
}

static ssize_t
ps_inet_sendmsg(struct dhcpcd_ctx *ctx,
    struct ps_msghdr *psm, struct msghdr *msg)
{
	struct ps_process *psp;
	int s;

	psp = ps_findprocess(ctx, &psm->ps_id);
	if (psp != NULL) {
		s = psp->psp_work_fd;
		goto dosend;
	}

	switch (psm->ps_cmd) {
#ifdef INET
	case PS_BOOTP:
		s = ctx->udp_wfd;
		break;
#endif
#if defined(INET6) && !defined(__sun)
	case PS_ND:
		s = ctx->nd_fd;
		break;
#endif
#ifdef DHCP6
	case PS_DHCP6:
		s = ctx->dhcp6_wfd;
		break;
#endif
	default:
		errno = EINVAL;
		return -1;
	}

dosend:
	return sendmsg(s, msg, 0);
}

static void
ps_inet_recvmsg(void *arg)
{
	struct dhcpcd_ctx *ctx = arg;

	/* Receive shutdown */
	if (ps_recvpsmsg(ctx, ctx->ps_inet_fd, NULL, NULL) == -1)
		logerr(__func__);
}

static void
ps_inet_signalcb(int sig, void *arg)
{
	struct dhcpcd_ctx *ctx = arg;

	/* Ignore SIGINT, respect PS_STOP command or SIGTERM. */
	if (sig == SIGINT)
		return;

	shutdown(ctx->ps_inet_fd, SHUT_RDWR);
	eloop_exit(ctx->eloop, sig == SIGTERM ? EXIT_SUCCESS : EXIT_FAILURE);
}

ssize_t
ps_inet_dispatch(void *arg, struct ps_msghdr *psm, struct msghdr *msg)
{
	struct dhcpcd_ctx *ctx = arg;

	switch (psm->ps_cmd) {
#ifdef INET
	case PS_BOOTP:
		dhcp_recvmsg(ctx, msg);
		break;
#endif
#ifdef INET6
	case PS_ND:
		ipv6nd_recvmsg(ctx, msg);
		break;
#endif
#ifdef DHCP6
	case PS_DHCP6:
		dhcp6_recvmsg(ctx, msg, NULL);
		break;
#endif
	default:
		errno = ENOTSUP;
		return -1;
	}
	return 1;
}

static void
ps_inet_dodispatch(void *arg)
{
	struct dhcpcd_ctx *ctx = arg;

	if (ps_recvpsmsg(ctx, ctx->ps_inet_fd, ps_inet_dispatch, ctx) == -1)
		logerr(__func__);
}

pid_t
ps_inet_start(struct dhcpcd_ctx *ctx)
{
	pid_t pid;

	pid = ps_dostart(ctx, &ctx->ps_inet_pid, &ctx->ps_inet_fd,
	    ps_inet_recvmsg, ps_inet_dodispatch, ctx,
	    ps_inet_startcb, ps_inet_signalcb,
	    PSF_DROPPRIVS);

#ifdef HAVE_CAPSICUM
	if (pid == 0 && cap_enter() == -1 && errno != ENOSYS)
		logerr("%s: cap_enter", __func__);
#endif
#ifdef HAVE_PLEDGE
	if (pid == 0 && pledge("stdio", NULL) == -1)
		logerr("%s: pledge", __func__);
#endif

	return pid;
}

int
ps_inet_stop(struct dhcpcd_ctx *ctx)
{

	return ps_dostop(ctx, &ctx->ps_inet_pid, &ctx->ps_inet_fd);
}

#ifdef INET
static void
ps_inet_recvinbootp(void *arg)
{
	struct ps_process *psp = arg;

	if (ps_recvmsg(psp->psp_ctx, psp->psp_work_fd,
	    PS_BOOTP, psp->psp_ctx->ps_data_fd) == -1)
		logerr(__func__);
}

static int
ps_inet_listenin(void *arg)
{
	struct ps_process *psp = arg;
	struct in_addr *ia = &psp->psp_id.psi_addr.psa_in_addr;
	char buf[INET_ADDRSTRLEN];
#ifdef HAVE_CAPSICUM
	cap_rights_t rights;

	cap_rights_init(&rights, CAP_RECV, CAP_EVENT);
#endif

	inet_ntop(AF_INET, ia, buf, sizeof(buf));
	setproctitle("[network proxy] %s", buf);

	psp->psp_work_fd = dhcp_openudp(ia);
	if (psp->psp_work_fd == -1) {
		logerr(__func__);
		return -1;
	}

#ifdef HAVE_CAPSICUM
	if (cap_rights_limit(psp->psp_work_fd, &rights) == -1 &&
	    errno != ENOSYS)
	{
		logerr("%s: cap_rights_limit", __func__);
		return -1;
	}
#endif

	if (eloop_event_add(psp->psp_ctx->eloop, psp->psp_work_fd,
	    ps_inet_recvinbootp, psp) == -1)
	{
		logerr("%s: eloop_event_add DHCP", __func__);
		return -1;
	}

	logdebugx("spawned listener %s on PID %d", buf, getpid());
	return 0;
}
#endif

#if defined(INET6) && defined(__sun)
static void
ps_inet_recvin6nd(void *arg)
{
	struct ps_process *psp = arg;

	if (ps_recvmsg(psp->psp_ctx, psp->psp_work_fd,
	    PS_ND, psp->psp_ctx->ps_data_fd) == -1)
		logerr(__func__);
}

static int
ps_inet_listennd(void *arg)
{
	struct ps_process *psp = arg;
#ifdef HAVE_CAPSICUM
	cap_rights_t rights;

	cap_rights_init(&rights, CAP_RECV, CAP_EVENT);
#endif

	setproctitle("[ND network proxy]");

	psp->psp_work_fd = ipv6nd_open(&psp->psp_ifp);
	if (psp->psp_work_fd == -1) {
		logerr(__func__);
		return -1;
	}

#ifdef HAVE_CAPSICUM
	if (cap_rights_limit(psp->psp_work_fd, &rights) == -1 &&
	    errno != ENOSYS)
	{
		logerr("%s: cap_rights_limit", __func__);
		return -1;
	}
#endif

	if (eloop_event_add(psp->psp_ctx->eloop, psp->psp_work_fd,
	    ps_inet_recvin6nd, psp) == -1)
	{
		logerr(__func__);
		return -1;
	}

	logdebugx("spawned ND listener on PID %d", getpid());
	return 0;
}
#endif

#ifdef DHCP6
static void
ps_inet_recvin6dhcp6(void *arg)
{
	struct ps_process *psp = arg;

	if (ps_recvmsg(psp->psp_ctx, psp->psp_work_fd,
	    PS_DHCP6, psp->psp_ctx->ps_data_fd) == -1)
		logerr(__func__);
}

static int
ps_inet_listenin6(void *arg)
{
	struct ps_process *psp = arg;
	struct in6_addr *ia = &psp->psp_id.psi_addr.psa_in6_addr;
	char buf[INET6_ADDRSTRLEN];
#ifdef HAVE_CAPSICUM
	cap_rights_t rights;

	cap_rights_init(&rights, CAP_RECV, CAP_EVENT);
#endif

	inet_ntop(AF_INET6, ia, buf, sizeof(buf));
	setproctitle("[network proxy] %s", buf);

	psp->psp_work_fd = dhcp6_openudp(psp->psp_id.psi_ifindex, ia);
	if (psp->psp_work_fd == -1) {
		logerr(__func__);
		return -1;
	}

#ifdef HAVE_CAPSICUM
	if (cap_rights_limit(psp->psp_work_fd, &rights) == -1 &&
	    errno != ENOSYS)
	{
		logerr("%s: cap_rights_limit", __func__);
		return -1;
	}
#endif

	if (eloop_event_add(psp->psp_ctx->eloop, psp->psp_work_fd,
	    ps_inet_recvin6dhcp6, psp) == -1)
	{
		logerr("%s: eloop_event_add DHCP", __func__);
		return -1;
	}

	logdebugx("spawned listener %s on PID %d", buf, getpid());
	return 0;
}
#endif

static void
ps_inet_recvmsgpsp(void *arg)
{
	struct ps_process *psp = arg;

	/* Receive shutdown. */
	if (ps_recvpsmsg(psp->psp_ctx, psp->psp_fd, NULL, NULL) == -1)
		logerr(__func__);
}

ssize_t
ps_inet_cmd(struct dhcpcd_ctx *ctx, struct ps_msghdr *psm, struct msghdr *msg)
{
	uint16_t cmd;
	struct ps_process *psp;
	int (*start_func)(void *);
	pid_t start;

	cmd = (uint16_t)(psm->ps_cmd & ~(PS_START | PS_STOP));
	if (cmd == psm->ps_cmd)
		return ps_inet_sendmsg(ctx, psm, msg);

	psp = ps_findprocess(ctx, &psm->ps_id);

#ifdef PRIVSEP_DEBUG
	logerrx("%s: IN cmd %x, psp %p", __func__, psm->ps_cmd, psp);
#endif

	if (psm->ps_cmd & PS_STOP) {
		assert(psp == NULL);
		return 0;
	}

	switch (cmd) {
#ifdef INET
	case PS_BOOTP:
		start_func = ps_inet_listenin;
		break;
#endif
#ifdef INET6
#ifdef __sun
	case PS_ND:
		start_func = ps_inet_listennd;
		break;
#endif
#ifdef DHCP6
	case PS_DHCP6:
		start_func = ps_inet_listenin6;
		break;
#endif
#endif
	default:
		logerrx("%s: unknown command %x", __func__, psm->ps_cmd);
		errno = ENOTSUP;
		return -1;
	}

	if (!(psm->ps_cmd & PS_START)) {
		errno = EINVAL;
		return -1;
	}

	if (psp != NULL)
		return 1;

	psp = ps_newprocess(ctx, &psm->ps_id);
	if (psp == NULL)
		return -1;

	start = ps_dostart(ctx,
	    &psp->psp_pid, &psp->psp_fd,
	    ps_inet_recvmsgpsp, NULL, psp,
	    start_func, ps_inet_signalcb,
	    PSF_DROPPRIVS);
	switch (start) {
	case -1:
		ps_freeprocess(psp);
		return -1;
	case 0:
#ifdef HAVE_CAPSICUM
		if (cap_enter() == -1 && errno != ENOSYS)
			logerr("%s: cap_enter", __func__);
#endif
#ifdef HAVE_PLEDGE
		if (pledge("stdio", NULL) == -1)
			logerr("%s: pledge", __func__);
#endif
		break;
	default:
		break;
	}
	return start;
}

#ifdef INET
static ssize_t
ps_inet_in_docmd(struct ipv4_addr *ia, uint16_t cmd, const struct msghdr *msg)
{
	assert(ia != NULL);
	struct dhcpcd_ctx *ctx = ia->iface->ctx;
	struct ps_msghdr psm = {
		.ps_cmd = cmd,
		.ps_id = {
			.psi_cmd = (uint8_t)(cmd & ~(PS_START | PS_STOP)),
			.psi_ifindex = ia->iface->index,
			.psi_addr.psa_in_addr = ia->addr,
		},
	};

	return ps_sendpsmmsg(ctx, ctx->ps_root_fd, &psm, msg);
}

ssize_t
ps_inet_openbootp(struct ipv4_addr *ia)
{

	return ps_inet_in_docmd(ia, PS_START | PS_BOOTP, NULL);
}

ssize_t
ps_inet_closebootp(struct ipv4_addr *ia)
{

	return ps_inet_in_docmd(ia, PS_STOP | PS_BOOTP, NULL);
}

ssize_t
ps_inet_sendbootp(struct interface *ifp, const struct msghdr *msg)
{

	return ps_sendmsg(ifp->ctx, ifp->ctx->ps_root_fd, PS_BOOTP, 0, msg);
}
#endif /* INET */

#ifdef INET6
#ifdef __sun
static ssize_t
ps_inet_ifp_docmd(struct interface *ifp, uint16_t cmd, const struct msghdr *msg)
{
	struct dhcpcd_ctx *ctx = ifp->ctx;
	struct ps_msghdr psm = {
		.ps_cmd = cmd,
		.ps_id = {
			.psi_cmd = (uint8_t)(cmd & ~(PS_START | PS_STOP)),
			.psi_ifindex = ifp->index,
		},
	};

	return ps_sendpsmmsg(ctx, ctx->ps_root_fd, &psm, msg);
}

ssize_t
ps_inet_opennd(struct interface *ifp)
{

	return ps_inet_ifp_docmd(ifp, PS_ND | PS_START, NULL);
}

ssize_t
ps_inet_closend(struct interface *ifp)
{

	return ps_inet_ifp_docmd(ifp, PS_ND | PS_STOP, NULL);
}

ssize_t
ps_inet_sendnd(struct interface *ifp, const struct msghdr *msg)
{

	return ps_inet_ifp_docmd(ifp, PS_ND, msg);
}
#else
ssize_t
ps_inet_sendnd(struct interface *ifp, const struct msghdr *msg)
{

	return ps_sendmsg(ifp->ctx, ifp->ctx->ps_root_fd, PS_ND, 0, msg);
}
#endif

#ifdef DHCP6
static ssize_t
ps_inet_in6_docmd(struct ipv6_addr *ia, uint16_t cmd, const struct msghdr *msg)
{
	struct dhcpcd_ctx *ctx = ia->iface->ctx;
	struct ps_msghdr psm = {
		.ps_cmd = cmd,
		.ps_id = {
			.psi_cmd = (uint8_t)(cmd & ~(PS_START | PS_STOP)),
			.psi_ifindex = ia->iface->index,
			.psi_addr.psa_in6_addr = ia->addr,
		},
	};

	return ps_sendpsmmsg(ctx, ctx->ps_root_fd, &psm, msg);
}

ssize_t
ps_inet_opendhcp6(struct ipv6_addr *ia)
{

	return ps_inet_in6_docmd(ia, PS_DHCP6 | PS_START, NULL);
}

ssize_t
ps_inet_closedhcp6(struct ipv6_addr *ia)
{

	return ps_inet_in6_docmd(ia, PS_DHCP6 | PS_STOP, NULL);
}

ssize_t
ps_inet_senddhcp6(struct interface *ifp, const struct msghdr *msg)
{

	return ps_sendmsg(ifp->ctx, ifp->ctx->ps_root_fd, PS_DHCP6, 0, msg);
}
#endif /* DHCP6 */
#endif /* INET6 */
