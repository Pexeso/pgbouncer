/*
 * PgBouncer - Lightweight connection pooler for PostgreSQL.
 *
 * Copyright (c) 2007-2009  Marko Kreen, Skype Technologies OÜ
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Handling of pooler listening sockets
 */

#include "bouncer.h"

#include <usual/netdb.h>



static STATLIST(sock_list);

/* should listening sockets be active or suspended? */
static bool need_active = false;
/* is it actually active or suspended? */
static bool pooler_active = false;

/* on accept() failure sleep 5 seconds */
static struct event ev_err;
static struct timeval err_timeout = {5, 0};

void pooler_tune_accept(bool on)
{
	struct List *el;
	struct ListenSocket *ls;
	statlist_for_each(el, &sock_list) {
		ls = container_of(el, struct ListenSocket, node);
		if (!pga_is_unix(&ls->addr))
			tune_accept(ls->fd, on);
	}
}

static void atexit_cb(void)
{
	cleanup_sockets(&sock_list);
}

static void err_wait_func(int sock, short flags, void *arg)
{
	if (cf_pause_mode != P_SUSPEND)
		resume_pooler();
}

static const char *addrpair(const PgAddr *src, const PgAddr *dst)
{
	static char ip1buf[PGADDR_BUF], ip2buf[PGADDR_BUF],
	            buf[2*PGADDR_BUF + 16];
	const char *ip1, *ip2;
	if (pga_is_unix(src))
		return "unix->unix";

	ip1 = pga_ntop(src, ip1buf, sizeof(ip1buf));
	ip2 = pga_ntop(src, ip2buf, sizeof(ip2buf));
	snprintf(buf, sizeof(buf), "%s:%d -> %s:%d",
		 ip1, pga_port(src), ip2, pga_port(dst));
	return buf;
}

static const char *conninfo(const PgSocket *sk)
{
	if (is_server_socket(sk)) {
		return addrpair(&sk->local_addr, &sk->remote_addr);
	} else {
		return addrpair(&sk->remote_addr, &sk->local_addr);
	}
}

/* got new connection, associate it with client struct */
static void pool_accept(int sock, short flags, void *arg)
{
	struct ListenSocket *ls = arg;
	int fd;
	PgSocket *client;
	union {
		struct sockaddr_in in;
		struct sockaddr_in6 in6;
		struct sockaddr_un un;
		struct sockaddr sa;
	} raddr;
	socklen_t len = sizeof(raddr);
	bool is_unix = pga_is_unix(&ls->addr);

	if(!(flags & EV_READ)) {
		log_warning("No EV_READ in pool_accept");
		return;
	}
loop:
	/* get fd */
	fd = safe_accept(sock, &raddr.sa, &len);
	if (fd < 0) {
		if (errno == EAGAIN)
			return;
		else if (errno == ECONNABORTED)
			return;

		/*
		 * probably fd limit, pointless to try often
		 * wait a bit, hope that admin resolves somehow
		 */
		log_error("accept() failed: %s", strerror(errno));
		evtimer_set(&ev_err, err_wait_func, NULL);
		safe_evtimer_add(&ev_err, &err_timeout);
		suspend_pooler();
		return;
	}

	log_noise("new fd from accept=%d", fd);
	if (is_unix) {
		client = accept_client(fd, true);
	} else {
		client = accept_client(fd, false);
	}

	if (client)
		slog_debug(client, "P: got connection: %s", conninfo(client));

	/*
	 * there may be several clients waiting,
	 * avoid context switch by looping
	 */
	goto loop;
}

bool use_pooler_socket(int sock, bool is_unix)
{
	struct ListenSocket *ls;
	int res;
	char buf[PGADDR_BUF];

	if (!tune_socket(sock, is_unix))
		return false;

	ls = calloc(1, sizeof(*ls));
	if (!ls)
		return false;
	ls->fd = sock;
	if (is_unix) {
		pga_set(&ls->addr, AF_UNIX, cf_listen_port);
	} else {
		struct sockaddr_storage ss;
		socklen_t len = sizeof(ss);
		res = getsockname(sock, (struct sockaddr *)&ss, &len);
		if (res < 0) {
			log_error("getsockname failed");
			free(ls);
			return false;
		}
		pga_copy(&ls->addr, (struct sockaddr *)&ss);
	}
	log_info("got pooler socket: %s", pga_str(&ls->addr, buf, sizeof(buf)));
	statlist_append(&sock_list, &ls->node);
	return true;
}

void suspend_pooler(void)
{
	struct List *el;
	struct ListenSocket *ls;

	need_active = false;
	statlist_for_each(el, &sock_list) {
		ls = container_of(el, struct ListenSocket, node);
		if (!ls->active)
			continue;
		if (event_del(&ls->ev) < 0) {
			log_warning("suspend_pooler, event_del: %s", strerror(errno));
			return;
		}
		ls->active = false;
	}
	pooler_active = false;
}

void resume_pooler(void)
{
	struct List *el;
	struct ListenSocket *ls;

	need_active = true;
	statlist_for_each(el, &sock_list) {
		ls = container_of(el, struct ListenSocket, node);
		if (ls->active)
			continue;
		event_set(&ls->ev, ls->fd, EV_READ | EV_PERSIST, pool_accept, ls);
		if (event_add(&ls->ev, NULL) < 0) {
			log_warning("event_add failed: %s", strerror(errno));
			return;
		}
		ls->active = true;
	}
	pooler_active = true;
}

/* retry previously failed suspend_pooler() / resume_pooler() */
void per_loop_pooler_maint(void)
{
	if (need_active && !pooler_active)
		resume_pooler();
	else if (!need_active && pooler_active)
		suspend_pooler();
}

/* listen on socket - should happen after all other initializations */
void pooler_setup(void)
{
	static int init_done = 0;

	if (!init_done) {
		/* remove socket on shutdown */
		atexit(atexit_cb);
		init_done = 1;
	}

	create_sockets(&sock_list);
	if (!statlist_count(&sock_list))
		fatal("nowhere to listen on");

	resume_pooler();
}

bool for_each_pooler_fd(pooler_cb cbfunc, void *arg)
{
	struct List *el;
	struct ListenSocket *ls;
	bool ok;

	statlist_for_each(el, &sock_list) {
		ls = container_of(el, struct ListenSocket, node);
		ok = cbfunc(arg, ls->fd, &ls->addr);
		if (!ok)
			return false;
	}
	return true;
}
