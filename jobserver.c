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

#include <sys/mman.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <semaphore.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "defines.h"
#include "engine.h"
#include "error.h"
#include "extern.h"
#include "gnode.h"
#include "job.h"
#include "jobserver.h"

static sem_t *jobserver;
static int jobfd = -1;
static bool master;

static bool implicit_token_available;

static void jobserver_decrement_token(Job *job);
static void jobserver_disable(void);

static pid_t mypid;

static int
sem_wait_interruptible(sem_t *sem)
{
	int ret, serr;

	disable_sa_restart();
	ret = sem_wait(sem);
	serr = errno;
	enable_sa_restart();
	errno = serr;
	return ret;
}

#define MAKEJOBSEMAPHORE "MAKEJOBSEMAPHORE"

#define JOBSERVER_DEBUG(fmt, ...) do {				\
	if (DEBUG(JOBSERVER)) {					\
		int sv;						\
		sem_getvalue(jobserver, &sv);			\
		fprintf(stderr, "[%ld] jobserver <%d>: "	\
		    fmt "\n",					\
		    (long)mypid, sv, __VA_ARGS__);		\
	}							\
} while(0)

static void
jobserver_shutdown(void)
{
	if (jobserver) {
		sem_destroy(jobserver);
		jobserver = NULL;
	}
	if (jobfd != -1) {
		close(jobfd);
		jobfd = -1;
	}
	usejobserver = false;
}

static void
jobserver_disable(void)
{
	/*
	 * jobserver communication has failed non-fatally; disable the
	 * jobserver and fall back to sequential semantics.
	 */
	fallback_to_sequential();
	jobserver_shutdown();
}

void
jobserver_uninit(void)
{
	if (master)
		jobserver_shutdown();
}

void
jobserver_init(unsigned max_tokens)
{
	char *jobfds;

	if (!usejobserver)
		return;

	mypid = getpid();

	jobfds = getenv(MAKEJOBSEMAPHORE);
	if (jobfds) {
		/* jobserver sem fd number from environment */
		const char *errstr;

		master = false;
		implicit_token_available = true;

		jobfd = strtonum(jobfds, 1, INT_MAX, &errstr);
		if (errstr) {
			warn("disabling jobserver: " MAKEJOBSEMAPHORE "=%s is"
			    " %s", jobfds, errstr);
			jobserver_disable();
			return;
		}
		jobserver = mmap(NULL, sizeof(*jobserver),
		    PROT_READ|PROT_WRITE, MAP_SHARED, jobfd, 0);
		if (jobserver == MAP_FAILED) {
			warn("disabling jobserver: mmap failed");
			jobserver_disable();
			return;
		}
	} else {
		/* create new semaphore */
		char buf[16];
		char tmpl[] = "makejobserver.XXXXXXXXXX";
		master = true;
		if ((jobfd = shm_mkstemp(tmpl)) == -1) {
			warn("jobserver init: shm_mkstemp failed");
			jobserver_shutdown();
			return;
		}
		if (ftruncate(jobfd, sizeof(*jobserver)) == -1) {
			warn("jobserver init: ftruncate failed");
			jobserver_shutdown();
			return;
		}
		jobserver = mmap(NULL, sizeof(*jobserver),
		    PROT_READ|PROT_WRITE, MAP_SHARED, jobfd, 0);
		if (jobserver == MAP_FAILED) {
			warn("jobserver init: mmap failed");
			jobserver_shutdown();
			return;
		}
		if (fcntl(jobfd, F_SETFD, 0) == -1) {
			warn("jobserver init: fcntl");
			jobserver_shutdown();
			return;
		}
		if (sem_init(jobserver, 1, max_tokens) == -1) {
			warn("jobserver init: sem_init");
			jobserver_shutdown();
			return;
		}
		snprintf(buf, sizeof(buf), "%d", jobfd);
		setenv(MAKEJOBSEMAPHORE, buf, 1);
	}

	JOBSERVER_DEBUG("%s initialized in %s", master ? "master" : "slave",
	    shortened_curdir());
}

static bool
use_implicit_token(Job *job)
{
	if (implicit_token_available) {
		implicit_token_available = false;
		job->token_type = JOB_TOKEN_IMPLICIT;
		JOBSERVER_DEBUG("target %s: used implicit token",
		    job->node->name);
		return true;
	}
	return false;
}

void
jobserver_acquire_token(Job *job)
{
	if (job->token_type != JOB_TOKEN_NONE) {
		JOBSERVER_DEBUG("target %s: already have %stoken",
		    job->node->name,
		    job->token_type == JOB_TOKEN_IMPLICIT ? "implicit " : "");
		return;
	}

	if (use_implicit_token(job))
		return;

	if (sem_trywait(jobserver) == -1) {
		while (sem_wait_interruptible(jobserver) == -1) {
			int serr = errno;
			handle_all_signals();
			reap_jobs();
			if (serr != EINTR) {
				warn("disabling jobserver: token sem_wait failed");
				jobserver_disable();
				return;
			}
			/* implicit token may have been returned by reap_jobs() */
			if (use_implicit_token(job))
				return;
		}
	}

	job->token_type = JOB_TOKEN_SEM;
	JOBSERVER_DEBUG("target %s: acquired token", job->node->name);
}


void
jobserver_release_token(Job *job)
{
	unsigned short hadtype = job->token_type;

	job->token_type = JOB_TOKEN_NONE;

	switch (hadtype) {
	case JOB_TOKEN_NONE:
		Punt("cannot release nonexistent token for target %s",
		    job->node->name);
		return;
	case JOB_TOKEN_IMPLICIT:
		assert(!implicit_token_available);
		implicit_token_available = true;
		return;
	case JOB_TOKEN_SEM:
		break;
	}

	/* if releasing a token fails, a token deficit is created. that should
	 * be fatal to the entire make process tree; abort this make and all
	 * jobs without waiting for them to finish.
	 */
	if (sem_post(jobserver) == -1)
		Punt("jobserver token release failed: sem_post: %s",
		    strerror(errno));

	JOBSERVER_DEBUG("target %s: released token", job->node->name);
}
