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

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
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

void
jobserver_shutdown(void)
{
	if (jobserver) {
		sem_close(jobserver);
		if (master)
			sem_unlink(getenv(MAKEJOBSEMAPHORE));
	}
	unsetenv(MAKEJOBSEMAPHORE);
	usejobserver = false;
}

static void
jobserver_disable(void)
{
	/* jobserver communication has failed non-fatally; disable the
	 * jobserver and fall back to sequential semantics.
	 *
	 * we must wait for all running jobs to complete before starting more.
	 * the tokens for those running jobs must be released, or other
	 * processes still using the jobserver could be starved of tokens.
	 *
	 * therefore, before we can disable the jobserver and fall back to
	 * single-job semantics, we need to wait for all running jobs to
	 * complete. of course, while reaping those jobs, we may fail to
	 * release tokens already acquired for them, so we may end up fatally
	 * exiting from jobserver_release_token().
	 */
	Job_Wait();
	jobserver_shutdown();
	sequential = 1;
}

void
jobserver_init(unsigned max_tokens)
{
	int s[2];
	char *semname;

	if (DEBUG(JOBSERVER))
		mypid = getpid();

	if (!usejobserver)
		return;

	semname = getenv(MAKEJOBSEMAPHORE);
	if (semname) {
		/* jobserver sem name from environment */
		master = false;
		implicit_token_available = true;
		jobserver = sem_open(semname, 0);
		if (jobserver == SEM_FAILED) {
			warn("jobserver slave: cannot open semaphore");
			jobserver_disable();
			return;
		}
	} else {
		/* create new semaphore */
		master = true;
		if (asprintf(&semname, "make.%d", mypid) == -1) {
			warn("jobserver init: cannot allocate semaphore name");
			jobserver_shutdown();
			return;
		}
		jobserver = sem_open(semname, O_CREAT|O_EXCL, 0600, max_tokens);
		if (jobserver == SEM_FAILED) {
			warn("jobserver init: cannot create semaphore: sem_open");
			jobserver_shutdown();
		}
		setenv(MAKEJOBSEMAPHORE, semname, 1);
		free(semname);
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
