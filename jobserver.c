/*
 * Copyright (c) 2020 Lauri Tirkkonen <lauri@hacktheplanet.fi>
 *
 * Permission to use, copy, modify, and distribute this software for any
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

#include <sys/socket.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "defines.h"
#include "engine.h"
#include "error.h"
#include "extern.h"
#include "gnode.h"
#include "job.h"
#include "jobserver.h"

struct jobserver {
	int	master;		/* master deposits job tokens into this fd */
	int	slave;		/* slaves consume job tokens from this fd */
	unsigned tokens;	/* number of available tokens. starting a job
				   consumes a token; if this is 0, the current
				   process must wait for a token to start a
				   job. */
	unsigned max_tokens;	/* maximum number of tokens */
	unsigned waiting;	/* master only: number of slaves that have
				   requested a token that has not yet been
				   provided */
	unsigned offered;	/* master only: number of tokens offered to
				   slaves (ie. already sent to socket) */
};
static struct jobserver jobserver;

static void jobserver_decrement_token(Job *job);
static void jobserver_disable(void);
static bool jobserver_token_available(void);

static sigset_t emptyset;
static pid_t mypid;

#define JOBSERVER_DEBUG(fmt, ...) do {				\
	if (DEBUG(JOBSERVER))					\
		fprintf(stderr, "[%ld] job %s: " fmt "\n",	\
		    (long)mypid,				\
		    jobserver_is_master() ? "master" : "slave",	\
		    __VA_ARGS__);				\
} while(0)

enum jobserver_message {
	JMSG_REQ	= '?',
	JMSG_CANCEL	= '!',
	JMSG_OFFER	= '.',
	JMSG_ACK	= '-',
	JMSG_RELEASE	= '+',
};

int
jobserver_get_slave_fd(void)
{
	return jobserver.slave;
}

void
jobserver_shutdown(void)
{
	if (jobserver.master != -1)
		close(jobserver.master);
	if (jobserver.slave != -1)
		close(jobserver.slave);
	usejobserver = false;
}

static void
jobserver_disable(void)
{
	assert(jobserver_is_slave());
	/* jobserver communication has failed non-fatally; disable the
	 * jobserver and fall back to sequential semantics.
	 *
	 * we must wait for all running jobs to complete before starting more.
	 * the tokens for those running jobs must be released to master, or
	 * master will run out of tokens.
	 *
	 * therefore, before we can disable the jobserver and fall back to
	 * single-job semantics, we need to wait for all running jobs to
	 * complete. of course, while reaping those jobs, we may fail to
	 * release tokens already acquired for them, so we may end up fatally
	 * exiting from jobserver_release_token().
	 *
	 * NOTE: we don't edit MAKEFLAGS, so future children of this make will
	 * still receive -J and -j. they should likewise fall back to
	 * sequential once they try to communicate over the invalid socket. */
	Job_Wait();
	jobserver_shutdown();
	sequential = 1;
}

void
jobserver_init(int fd, unsigned max_tokens)
{
	int s[2];

	if (DEBUG(JOBSERVER))
		mypid = getpid();
	sigemptyset(&emptyset);

	if (!usejobserver)
		return;

	jobserver.slave = -1;
	jobserver.master = -1;

	if (fd != -1) {
		/* inherited fd from master via -J */
		jobserver.slave = fd;
		/* a higher-level make (master or slave) has allocated a token
		 * for the job that eventually started this make. make itself
		 * should not consume a job token (it just waits on jobs it
		 * starts), so slaves can start one job "for free". */
		jobserver.tokens = 1;
	} else {
		/* we are the master; create sockets */
		if (socketpair(AF_UNIX, SOCK_STREAM|SOCK_NONBLOCK, 0, s) == -1) {
			warn("jobserver init: cannot create sockets");
			jobserver_shutdown();
			return;
		}
		jobserver.master = s[0];
		jobserver.slave = s[1];
		jobserver.tokens = max_tokens;
		jobserver.max_tokens = max_tokens;

		if (fcntl(jobserver.master, F_SETFD, O_CLOEXEC) == -1) {
			warn("jobserver init: cannot set master fd flags");
			jobserver_shutdown();
			return;
		}
	}

	JOBSERVER_DEBUG("initialized in %s", shortened_curdir());
}

bool
jobserver_is_master(void)
{
	return jobserver.master != -1;
}

bool
jobserver_is_slave(void)
{
	return jobserver.master == -1 && jobserver.slave != -1;
}

static bool
jobserver_token_available(void)
{
	return jobserver.tokens > 0;
}

static bool
jobserver_master_can_offer(void)
{
	return (jobserver.offered < jobserver.waiting &&
	    jobserver.offered < jobserver.tokens);
}

void
jobserver_master_setup_pollfd(struct pollfd *pfd)
{
	pfd->fd = jobserver.master;
	pfd->events = POLLIN;
	if (jobserver_master_can_offer())
		pfd->events |= POLLOUT;
}

void
jobserver_master_communicate(const struct pollfd *pfd)
{
	ssize_t r;
	char tok[64];
	unsigned requested = 0, cancelled = 0, acked = 0, reclaimed = 0;

	assert(jobserver_is_master());

	if (pfd->revents & (POLLHUP|POLLERR|POLLNVAL)) {
		warnx("jobserver master: error on socket %d, disabling "
		    "jobserver", jobserver.master);
		jobserver_shutdown();
		return;
	}

	if (pfd->revents & POLLIN) {
		if ((r = recv(jobserver.master, tok, sizeof(tok), 0)) == -1)
			Punt("jobserver token reclaim failed: recv: %s",
			    strerror(errno));
		for (const char *c = tok; c < tok + r; c++) {
			enum jobserver_message msg = *c;
			switch (msg) {
			case JMSG_REQ:
				requested++;
				break;
			case JMSG_CANCEL:
				cancelled++;
				break;
			case JMSG_ACK:
				acked++;
				break;
			case JMSG_RELEASE:
				reclaimed++;
				break;
			default:
				Punt("unknown job token '%c' received from "
				    "slave", *c);
			}
		}
	}

	jobserver.tokens += reclaimed;

	if (jobserver.tokens > jobserver.max_tokens)
		Punt("jobserver master reclaimed too many tokens: total %u "
		    "exceeds maximum %u", jobserver.tokens,
		    jobserver.max_tokens);

	jobserver.waiting += requested;
	/* slaves may send cancel messages after we've already made an offer
	 * for their request, but before sending an ack. in that case we will
	 * have extra offer(s) in the socket buffers until some slave receives
	 * them. if that happens, set waiting to 0. */
	if (jobserver.waiting < cancelled)
		jobserver.waiting = 0;
	else
		jobserver.waiting -= cancelled;
	if (jobserver.tokens < acked || jobserver.offered < acked)
		Punt("jobserver master got too many acks: %u tokens "
		    "available, %u offers, %u acks received", jobserver.tokens,
		    jobserver.offered, acked);
	jobserver.tokens -= acked;
	jobserver.offered -= acked;

	while (jobserver_master_can_offer()) {
		enum jobserver_message msg = JMSG_OFFER;
		if ((r = send(jobserver.master, &msg, 1, 0)) == -1) {
			if (errno == EAGAIN)
				break;
			Punt("jobserver master token offer failed: %s",
			    strerror(errno));
		}
		jobserver.offered += r;
		jobserver.waiting -= r;
	}

	JOBSERVER_DEBUG("%u req, %u cancel, %u ack, %u reclaim. %u tokens "
	    "available, %u jobs waiting, %u unacked offers",
	    requested, cancelled, acked, reclaimed, jobserver.tokens,
	    jobserver.waiting, jobserver.offered);
}

static void
jobserver_decrement_token(Job *job)
{
	assert(jobserver_token_available());
	jobserver.tokens--;
	job->token_type = JOB_TOKEN_LOCAL;

	if (jobserver_is_master())
		JOBSERVER_DEBUG("target %s: used token, %u available",
		    job->node->name, jobserver.tokens);
	else
		JOBSERVER_DEBUG("target %s: used local token",
		    job->node->name);
}

void
jobserver_acquire_token(Job *job)
{
	ssize_t r;
	char c;
	bool acquired = false, requested = false;
	enum state {
		STATE_INIT,
		STATE_REQD,
		STATE_RECVD,
		STATE_ACKED,
		STATE_NEEDCANCEL,
	};
	enum state state = STATE_INIT;

	if (job->token_type != JOB_TOKEN_NONE) {
		JOBSERVER_DEBUG("target %s: already have token (%s)",
		    job->node->name,
		    job->token_type == JOB_TOKEN_LOCAL ? "local" : "remote");
		return;
	}

	if (jobserver_is_master()) {
		jobserver_decrement_token(job);
		return;
	}

	while (state != STATE_ACKED) {
		struct pollfd pfd = {
			.fd = jobserver.slave,
		};

		switch (state) {
		case STATE_INIT:
			/* use local token if available */
			if (jobserver_token_available()) {
				jobserver_decrement_token(job);
				return;
			}
			JOBSERVER_DEBUG("target %s: requesting remote token",
			    job->node->name);
			pfd.events = POLLOUT;
			break;
		case STATE_REQD:
			/* if reap_jobs() returned the local token while we
			 * were waiting for a remote one, cancel the request
			 * and use the local token */
			if (jobserver_token_available()) {
				state = STATE_NEEDCANCEL;
				continue;
			}
			pfd.events = POLLIN;
			break;
		case STATE_RECVD:
		case STATE_NEEDCANCEL:
			pfd.events = POLLOUT;
			break;
		case STATE_ACKED:
			continue;
		}

		/* if either ppoll or recv fails below, we are no longer able
		 * to communicate to the jobserver master. we must disable the
		 * jobserver and execute future jobs sequentially.
		 *
		 * one job token has been allocated to this make by a
		 * higher-level make, but it has already been used to start
		 * another job (or we would not be trying to acquire a token).
		 * we must wait for all running jobs to complete before
		 * starting more. the tokens for those running jobs must be
		 * released to master, or master will run out of tokens.
		 *
		 * therefore, before we can disable the jobserver and fall back
		 * to single-job semantics, we keep usejobserver = true and
		 * wait for all running jobs to complete. of course, they may
		 * fail to release their tokens, which
		 * jobserver_release_token() considers fatal. */

		if ((r = ppoll(&pfd, 1, NULL, &emptyset)) == -1) {
			if (errno == EINTR) {
				handle_all_signals();
				reap_jobs();
				continue;
			}
			warn("disabling jobserver: token poll failed");
			jobserver_disable();
			return;
		}

		if (pfd.revents & POLLHUP) {
			warnx("disabling jobserver: socket disconnected");
			jobserver_disable();
			return;
		}

		if (pfd.revents & (POLLERR|POLLNVAL)) {
			warnx("disabling jobserver: bad fd %d",
			    jobserver.slave);
			jobserver_disable();
			return;
		}

		r = -1;

		if (pfd.revents & POLLOUT) {
			enum jobserver_message msg;

			if (state == STATE_INIT)
				msg = JMSG_REQ;
			else if (state == STATE_RECVD)
				msg = JMSG_ACK;
			else if (state == STATE_NEEDCANCEL)
				msg = JMSG_CANCEL;

			if ((r = send(jobserver.slave, &msg, 1, 0)) == -1) {
				if (errno == EAGAIN)
					continue;
				warn("disabling jobserver: communication "
				    "failed");
				jobserver_disable();
				return;
			}

			if (state == STATE_NEEDCANCEL) {
				/* request cancelled, go back to init state */
				state = STATE_INIT;
				continue;
			}
		} else if (pfd.revents & POLLIN) {
			if ((r = recv(jobserver.slave, &c, 1, 0)) == -1) {
				if (errno == EAGAIN)
					continue;
				warn("disabling jobserver: token receive "
				    "failed");
				jobserver_disable();
				return;
			}
		}
		if (r == 1)
			state++;
	}

	job->token_type = JOB_TOKEN_REMOTE;
	JOBSERVER_DEBUG("target %s: acquired token", job->node->name);
}


void
jobserver_release_token(Job *job)
{
	ssize_t r;
	bool released = false;
	unsigned short hadtype = job->token_type;

	job->token_type = JOB_TOKEN_NONE;

	switch (hadtype) {
	case JOB_TOKEN_NONE:
		return;
	case JOB_TOKEN_LOCAL:
		jobserver.tokens++;
		if (jobserver_is_master()) {
			JOBSERVER_DEBUG("target %s: returned token, %u "
			    "available", job->node->name, jobserver.tokens);
			if (jobserver.tokens > jobserver.max_tokens)
				Punt("jobserver master: token release: total %u "
				    "exceeds maximum %u", jobserver.tokens,
				    jobserver.max_tokens);
		} else {
			JOBSERVER_DEBUG("target %s: local token returned",
			    job->node->name);
			assert(jobserver.tokens == 1);
		}
		return;
	case JOB_TOKEN_REMOTE:
		break;
	}

	while (!released) {
		struct pollfd pfd = {
			.fd = jobserver.slave,
			.events = POLLOUT,
		};

		/* if releasing a token fails for any reason, a token deficit
		 * is created in the jobserver master; that should be fatal to
		 * the entire make process tree rooted at the jobserver master.
		 * in that scenario, by definition we are unable to communicate
		 * to the master via the jobserver socket, so the only thing we
		 * can do is abort this slave make and all its jobs, without
		 * waiting for them to finish.
		 *
		 * this enables a pathological case: if other makes are waiting
		 * for tokens, but the job that started this make does not
		 * terminate (perhaps because an intermediate process is
		 * waiting on a sibling make), or terminates with zero exit
		 * status, we have effectively lost a job token. if this causes
		 * the total number of tokens to go to 0, we deadlock: no jobs
		 * can be started, and all existing slaves will keep waiting
		 * forever.
		 */

		if ((r = ppoll(&pfd, 1, NULL, &emptyset)) == -1) {
			if (errno == EINTR) {
				/* handle_all_signals will exit if the signal
				 * was fatal; keep trying to release token if
				 * not */
				handle_all_signals();
				continue;
			}
			Punt("jobserver token release failed: ppoll: %s",
			    strerror(errno));
		}

		if (pfd.events & (POLLERR|POLLNVAL)) {
			Punt("jobserver token release failed: bad fd %d",
			    jobserver.slave);
		}

		if (pfd.revents & POLLOUT) {
			enum jobserver_message msg = JMSG_RELEASE;
			if ((r = send(jobserver.slave, &msg, 1, 0)) == -1) {
				if (errno == EAGAIN)
					continue;
				Punt("jobserver token release failed: send: "
				    "%s", strerror(errno));
			}
			released = (r == 1);
		}
	}

	JOBSERVER_DEBUG("target %s: released token", job->node->name);
}
