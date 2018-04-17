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
 * Process Manager.
 */

#include "bouncer.h"

#ifdef HAVE_PMGR

#include <sys/wait.h>

#include <event2/thread.h>
#include <usual/event.h>
#include <usual/pthread.h>
#include <usual/signal.h>


/* Helpers */
static struct Worker *next_worker(void);
static void close_sockets(struct StatList *sock_list);

/* Event Handlers */
static void pmgr_on_accept(int sock, short flags, void *arg);
static void pmgr_on_write(int sock, short flags, void *arg);
static void worker_on_accept(int sock, short flags, void *arg);
static void worker_on_read(int sock, short flags, void *arg);

/* Signal Handlers */
static void on_sigusr1(int sig, short flags, void *arg);
static void on_sigusr2(int sig, short flags, void *arg);
static void on_sighup(int sig, short flags, void *arg);
static void on_sigterm(int sig, short flags, void *arg);
static void on_sigint(int sig, short flags, void *arg);

/* Setup & Cleanup */
static void signals_setup(void);
static void accept_start(void);
static bool pmgr_setup(void);
static void pmgr_cleanup(void);


/*****************************************************************************
 * Types
 *****************************************************************************/

typedef void (*event_cb_t)(int, short, void *);

/*
 * Used by manager, stores info about worker.
 */
struct Worker {
	struct List node;
	struct event ev_write;
	pid_t pid;
	int port;
	int sock;
};

/*
 * Used by worker, stores info about manager.
 */
struct PMgr {
	struct event ev_read;
	int sock;
};


/*****************************************************************************
 * Globals
 *****************************************************************************/

static STATLIST(worker_list);
static STATLIST(socket_list);

static char *cf_pmgr_listen_addr;
static int cf_pmgr_listen_port;

static struct event ev_sigusr1;
static struct event ev_sigusr2;
static struct event ev_sighup;
static struct event ev_sigterm;
static struct event ev_sigint;

static struct PMgr pmgr;


/*****************************************************************************
 * Helpers
 *****************************************************************************/

static struct Worker *next_worker(void)
{
	struct List *el;

	el = statlist_pop(&worker_list);
	statlist_append(&worker_list, el);
	return container_of(el, struct Worker, node);
}

/*
 * This function differs from cleanup_sockets() in that it doesn't unlink()
 * the socket files, just closes the descriptors. This is useful for
 * unreferencing the file descriptor after fork() in the parent.
 */
static void close_sockets(struct StatList *sock_list)
{
	struct ListenSocket *ls;
	struct List *el;

	while ((el = statlist_pop(sock_list)) != NULL) {
		ls = container_of(el, struct ListenSocket, node);

		if (ls->fd > 0) {
			safe_close(ls->fd);
		}
		statlist_remove(sock_list, &ls->node);
		free(ls);
	}
}


/*****************************************************************************
 * Event Handlers
 *****************************************************************************/

static void pmgr_on_accept(int sock, short flags, void *arg)
{
	struct Worker *worker;
	int client_sock;
	int *on_write_arg;

	client_sock = safe_accept(sock, NULL, NULL);
	if (client_sock < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			log_error("accept() failed: %s", strerror(errno));
		return;
	}

	on_write_arg = malloc(sizeof(*on_write_arg));
	if (!on_write_arg) {
		log_error("malloc() failed: %s", strerror(errno));
		goto cleanup;
	}

	*on_write_arg = client_sock;
	worker = next_worker();

	event_set(&worker->ev_write, worker->sock, EV_WRITE,
		  pmgr_on_write, on_write_arg);

	if (event_add(&worker->ev_write, NULL) < 0) {
		log_warning("event_add() failed: %s", strerror(errno));
		goto cleanup;
	}

	log_info("new connection on %d, sending to worker %d",
		 client_sock, worker->pid);
	return;

cleanup:
	if (client_sock >= 0)
		safe_close(client_sock);
	if (on_write_arg)
		free(on_write_arg);
}

static void pmgr_on_write(int sock, short flags, void *arg)
{
	int *client_sock = arg;
	struct Worker *worker;
	struct msghdr msg;
	struct cmsghdr *cmsg;
	struct iovec io;
	char control[CMSG_SPACE(sizeof(int))];
	char iobuf[1];

	memset(&msg, 0, sizeof(msg));
	io.iov_base = iobuf;
	io.iov_len = sizeof(iobuf);
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_control = control;
	msg.msg_controllen = sizeof(control);

	cmsg = CMSG_FIRSTHDR(&msg);
	cmsg->cmsg_level = SOL_SOCKET;
	cmsg->cmsg_type = SCM_RIGHTS;
	cmsg->cmsg_len = CMSG_LEN(sizeof(int));

	memcpy(CMSG_DATA(cmsg), client_sock, sizeof(int));

	if (safe_sendmsg(sock, &msg, 0) < 0) {
		/*
		 * We try to send the socket to another worker.
		 */
		worker = next_worker();

		log_error("sendmsg(client_sock=%d) failed: %s, "
			  "sending to worker %d",
			  *client_sock, strerror(errno), worker->pid);

		event_set(&worker->ev_write, worker->sock, EV_WRITE,
			  pmgr_on_write, client_sock);

		if (event_add(&worker->ev_write, NULL) < 0) {
			log_error("event_add() failed: %s", strerror(errno));
			goto cleanup;
		}
		return;
	}

	log_info("Client socket %d sent to worker", *client_sock);

cleanup:
	safe_close(*client_sock);
	free(client_sock);
}

/*
 * Manager only has one shot at connecting to worker. If it fails
 * to do so, there's no point keeping the worker alive.
 * Therefore we fatal() on any error.
 */
static void worker_on_accept(int sock, short flags, void *arg)
{
	pmgr.sock = safe_accept(sock, NULL, NULL);
	if (pmgr.sock < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return;
		fatal_perror("accept() failed");
	}

	event_set(&pmgr.ev_read, pmgr.sock, EV_READ | EV_PERSIST,
		  worker_on_read, NULL);

	if (event_add(&pmgr.ev_read, NULL) < 0) {
		safe_close(pmgr.sock);
		fatal_perror("event_add() failed");
	}

	log_info("Established connection to manager");
}

static void worker_on_read(int sock, short flags, void *arg)
{
	struct msghdr msg;
	struct cmsghdr *cmsg;
	struct iovec io;
	PgSocket *client;
	ssize_t len;
	int *socks;
	int client_sock = -1;
	char control[CMSG_SPACE(sizeof(int))];
	char iobuf[1];

	memset(&msg, 0, sizeof(msg));
	io.iov_base = iobuf;
	io.iov_len = sizeof(iobuf);
	msg.msg_iov = &io;
	msg.msg_iovlen = 1;
	msg.msg_controllen = sizeof(control);
	msg.msg_control = control;

	len = safe_recvmsg(sock, &msg, 0);
	if (len < 0) {
		log_error("worker_on_read(): recvmsg() failed");
		return;
	}

	if (!len) {
		/* Manager closed the connection, there's nothing
		   else to do but to shutdown. */
		cf_shutdown = 2;
		return;
	}

	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_RIGHTS) {
			socks = (int *)CMSG_DATA(cmsg);
			client_sock = *socks;
		}
	}

	if (client_sock < 0) {
		log_warning("worker_on_read(): invalid client socket");
		return;
	}

	log_info("New client socket %d from manager", client_sock);

	client = accept_client(client_sock, true);
	if (!client) {
		log_error("worker_on_read(): accept_client() failed");
		safe_close(client_sock);
		return;
	}

	slog_debug(client, "accepted client");
}


/*****************************************************************************
 * Signal Handlers
 *****************************************************************************/
static void on_sigusr1(int sig, short flags, void *arg)
{
	log_info("Got SIGUSR1, forwarding to workers");
	kill(0, sig);
}

static void on_sigusr2(int sig, short flags, void *arg)
{
	log_info("Got SIGUSR2 forwarding to workers");
	kill(0, sig);
}

static void on_sighup(int sig, short flags, void *arg)
{
	log_info("Got SIGHUP, forwarding to workers");
	kill(0, sig);
}

static void on_sigterm(int sig, short flags, void *arg)
{
	log_info("Got SIGTERM, forceful exit");
	exit(1);
}

static void on_sigint(int sig, short flags, void *arg)
{
	log_info("Got SIGINT, graceful exit");
	cf_shutdown = 2;
}


/*****************************************************************************
 * Setup & Cleanup
 *****************************************************************************/

static void signals_setup(void)
{
	int ret;
	sigset_t set;

	/* Block SIGPIPE. */
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	ret = sigprocmask(SIG_BLOCK, &set, NULL);
	if (ret < 0)
		fatal_perror("sigprocmask");

	signal_set(&ev_sigusr1, SIGUSR1, on_sigusr1, NULL);
	ret = signal_add(&ev_sigusr1, NULL);
	if (ret < 0)
		fatal_perror("signal_add");

	signal_set(&ev_sigusr2, SIGUSR2, on_sigusr2, NULL);
	ret = signal_add(&ev_sigusr2, NULL);
	if (ret < 0)
		fatal_perror("signal_add");

	signal_set(&ev_sighup, SIGHUP, on_sighup, NULL);
	ret = signal_add(&ev_sighup, NULL);
	if (ret < 0)
		fatal_perror("signal_add");

	signal_set(&ev_sigterm, SIGTERM, on_sigterm, NULL);
	ret = signal_add(&ev_sigterm, NULL);
	if (ret < 0)
		fatal_perror("signal_add");

	signal_set(&ev_sigint, SIGINT, on_sigint, NULL);
	ret = signal_add(&ev_sigint, NULL);
	if (ret < 0)
		fatal_perror("signal_add");
}

static void accept_start(void)
{
	struct List *el;
	struct ListenSocket *ls;
	event_cb_t cb;

	cb = cf_is_pmgr_worker ? worker_on_accept : pmgr_on_accept;

	statlist_for_each(el, &socket_list) {
		ls = container_of(el, struct ListenSocket, node);

		if (ls->active)
			continue;

		event_set(&ls->ev, ls->fd, EV_READ | EV_PERSIST, cb, NULL);
		if (event_add(&ls->ev, NULL) < 0) {
			log_warning("event_add() failed: %s", strerror(errno));
			continue;
		}
		ls->active = true;
	}
}

static bool connect_worker(struct Worker *worker)
{
	int sock;
	int ret;
	int sa_len;
	struct sockaddr_un sa_un;

	memset(&sa_un, 0, sizeof(sa_un));

	sa_len = sizeof(sa_un);
	sa_un.sun_family = AF_UNIX;
	snprintf(sa_un.sun_path, sizeof(sa_un.sun_path),
		 "%s/.s.PGSQL.%d", cf_unix_socket_dir, worker->port);

	sock = socket(PF_UNIX, SOCK_STREAM, 0);
        if (sock < 0)
                goto fail;

        if (!tune_socket(sock, true))
                goto fail;

        ret = safe_connect(sock, (struct sockaddr *)&sa_un, sa_len);
        if (ret < 0)
		goto fail;

	worker->sock = sock;
	log_info("pmgr connected to worker, pid=%d, sock=%d",
		 worker->pid, worker->sock);
	return true;

fail:
        log_warning("connect_worker() failed: %s", strerror(errno));
        if (sock >= 0)
                safe_close(sock);

	return false;
}

static bool pmgr_setup(void)
{
	int i;
	pid_t pid;
	struct Worker *worker;
	struct List *el;

	/*
	 * Keep original value since the create_sockets() that's going to be
	 * called for each worker uses these global variables.
	 */
	cf_pmgr_listen_addr = cf_listen_addr;
	cf_pmgr_listen_port = cf_listen_port;

	/*
	 * We don't want the workers to listen on any TCP connections,
	 * just unix sockets.
	 */
	cf_listen_addr = NULL;

	/*
	 * Make sure the listen sockets are removed on exit. Call this before
	 * forking since it's safe for workers to inherit the registration.
	 */
	atexit(pmgr_cleanup);

	for (i = 0; i < cf_pmgr_workers; i++) {
		cf_listen_port++;

		create_sockets(&socket_list);

		pid = fork();
		if (pid < 0) {  /* This is error. */
			fatal_perror("pmgr_setup");
		} else if (pid == 0) {  /* This is worker. */
			cf_is_pmgr_worker = true;
			return false;
		}

		/* This is manager. */
		worker = malloc(sizeof(*worker));
		if (!worker)
			fatal_perror("pmgr_setup");
		worker->pid = pid;
		worker->port = cf_listen_port;
		worker->sock = -1;
		statlist_append(&worker_list, &worker->node);

		/*
		 * We want to close the worker sockets in manager to avoid
		 * leaking them since they're not going to be used.
		 */
		close_sockets(&socket_list);
	}

	/*
	 * Make sure the variables are back at their original value
	 * when calling create_sockets() on the manager.
	 */
	cf_listen_addr = cf_pmgr_listen_addr;
	cf_listen_port = cf_pmgr_listen_port;

	/*
	 * Establish connection to the workers. We want to do it here
	 * to give the workers some time to initialize after forking
	 * before trying to connect.
	 */
	statlist_for_each(el, &worker_list) {
		worker = container_of(el, struct Worker, node);
		if (!connect_worker(worker))
			fatal("failed to connect to worker");
	}

	if (!event_init())
		fatal("event_init() failed");

	create_sockets(&socket_list);
	signals_setup();
	accept_start();
	return true;
}

static void pmgr_cleanup(void)
{
	if (!cf_is_pmgr_worker)
		kill(0, SIGINT);
	cleanup_sockets(&socket_list);
}


/*****************************************************************************
 * Public
 *****************************************************************************/

void pmgr_worker_setup(void)
{
	accept_start();
}

void pmgr_run(void)
{
	if (cf_reboot)
		fatal("cf_reboot currently not supported");
	if (cf_daemon)
		fatal("cf_daemon currently not supported");
	if (cf_logfile && *cf_logfile)
		fatal("cf_logfile currently not supported");
	if (!cf_unix_socket_dir || !*cf_unix_socket_dir)
		fatal("cf_unix_socket_dir must be set");

	if (!pmgr_setup())
		return; /* This is worker. */

	while (cf_shutdown < 2)
		event_loop(EVLOOP_ONCE);

	/* Cleanup is registered with atexit(). */

	exit(0);
}

#else /* !HAVE_PMGR */

void pmgr_worker_setup(void)
{
	fatal("PMGR not supported");
}

void pmgr_run(void)
{
	fatal("PMGR not supported");
}

#endif