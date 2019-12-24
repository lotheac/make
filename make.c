/*	$OpenBSD: make.c,v 1.73 2017/06/22 17:09:10 espie Exp $	*/
/*	$NetBSD: make.c,v 1.10 1996/11/06 17:59:15 christos Exp $	*/

/*
 * Copyright (c) 1988, 1989, 1990, 1993
 *	The Regents of the University of California.  All rights reserved.
 * Copyright (c) 1989 by Berkeley Softworks
 * All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Adam de Boor.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*-
 * make.c --
 *	The functions which perform the examination of targets and
 *	their suitability for creation
 *
 * Interface:
 *	Make_Run		Initialize things for the module and recreate
 *				whatever needs recreating. Returns true if
 *				work was (or would have been) done and
 *				false
 *				otherwise.
 *
 *	Make_Update		Update all parents of a given child. Performs
 *				various bookkeeping chores like finding the
 *				youngest child of the parent, filling
 *				the IMPSRC local variable, etc. It will
 *				place the parent on the to_build queue if it
 *				should be.
 *
 */

#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ohash.h>
#include "config.h"
#include "defines.h"
#include "dir.h"
#include "job.h"
#include "suff.h"
#include "var.h"
#include "error.h"
#include "expandchildren.h"
#include "make.h"
#include "gnode.h"
#include "extern.h"
#include "timestamp.h"
#include "engine.h"
#include "lst.h"
#include "targ.h"
#include "targequiv.h"
#include "garray.h"
#include "memory.h"

/* what gets added each time. Kept as one static array so that it doesn't
 * get resized every time.
 */
static struct growableArray examine;
/* The current fringe of the graph. These are nodes which await examination by
 * MakeOODate. It is added to by Make_Update and subtracted from by
 * MakeStartJobs */
static struct growableArray to_build;	

/* Hold back on nodes where equivalent stuff is already building... */
static struct growableArray heldBack;

static struct ohash targets;	/* stuff we must build */

static void MakeAddChild(void *, void *);
static void MakeHandleUse(void *, void *);
static bool MakeStartJobs(void);
static void MakePrintStatus(void *);

/* Cycle detection functions */
static bool targets_contain_cycles(void);
static void print_unlink_cycle(struct growableArray *, GNode *);
static void break_and_print_cycles(Lst);
static GNode *find_cycle(Lst, struct growableArray *);

static bool try_to_make_node(GNode *);
static void add_targets_to_make(Lst);

static bool has_predecessor_left_to_build(GNode *);
static void requeue_successors(GNode *);
static void random_setup(void);

static bool randomize_queue;
long random_delay = 0;

bool
no_jobs_left()
{
	return Array_IsEmpty(&to_build);
}

static void
random_setup()
{
	randomize_queue = Var_Definedi("RANDOM_ORDER", NULL);

/* no random delay in the new engine for now */
#if 0
	if (Var_Definedi("RANDOM_DELAY", NULL))
		random_delay = strtonum(Var_Value("RANDOM_DELAY"), 0, 1000,
		    NULL) * 1000000;
#endif

}

static void
randomize_garray(struct growableArray *g)
{
	/* This is a fairly standard algorithm to randomize an array. */
	unsigned int i, v;
	GNode *e;

	for (i = g->n; i > 0; i--) {
		v = arc4random_uniform(i);
		if (v == i-1)
			continue;
		else {
			e = g->a[i-1];
			g->a[i-1] = g->a[v];
			g->a[v] = e;
		}
	}
}

static bool
has_predecessor_left_to_build(GNode *gn)
{
	LstNode ln;

	if (Lst_IsEmpty(&gn->predecessors))
		return false;


	for (ln = Lst_First(&gn->predecessors); ln != NULL; ln = Lst_Adv(ln)) {
		GNode	*pgn = Lst_Datum(ln);

		if (pgn->must_make && pgn->built_status == UNKNOWN) {
			if (DEBUG(MAKE))
				printf("predecessor %s not made yet.\n",
				    pgn->name);
			return true;
		}
	}
	return false;
}

static void
requeue_successors(GNode *gn)
{
	LstNode ln;
	/* Deal with successor nodes. If any is marked for making and has an
	 * children_left count of 0, has not been made and isn't in the 
	 * examination queue, it means we need to place it in the queue as 
	 * it restrained itself before.	*/
	for (ln = Lst_First(&gn->successors); ln != NULL; ln = Lst_Adv(ln)) {
		GNode	*succ = Lst_Datum(ln);

		if (succ->must_make && succ->children_left == 0
		    && succ->built_status == UNKNOWN)
			Array_PushNew(&to_build, succ);
	}
}

static void
requeue(GNode *gn)
{
	/* this is where we go inside the array and move things around */
	unsigned int i, j;

	for (i = 0, j = 0; i < heldBack.n; i++, j++) {
		if (heldBack.a[i]->watched == gn) {
			j--;
			heldBack.a[i]->built_status = UNKNOWN;
			if (DEBUG(HELDJOBS))
				printf("%s finished, releasing: %s\n",
				    gn->name, heldBack.a[i]->name);
			Array_Push(&to_build, heldBack.a[i]);
			continue;
		}
		heldBack.a[j] = heldBack.a[i];
	}
	heldBack.n = j;
}

/*-
 *-----------------------------------------------------------------------
 * Make_Update	--
 *	Perform update on the parents of a node. Used by JobFinish once
 *	a node has been dealt with and by MakeStartJobs if it finds an
 *	up-to-date node.
 *
 * Results:
 *	Always returns 0
 *
 * Side Effects:
 *	The children_left field of pgn is decremented and pgn may be placed on
 *	the to_build queue if this field becomes 0.
 *
 *	If the child got built, the parent's child_rebuilt field will be set to
 *	true
 *-----------------------------------------------------------------------
 */
void
Make_Update(GNode *cgn)	/* the child node */
{
	GNode	*pgn;	/* the parent node */
	LstNode	ln;	/* Element in parents list */

	/*
	 * If the child was actually made, see what its modification time is
	 * now -- some rules won't actually update the file. If the file still
	 * doesn't exist, make its mtime now.
	 */
	if (cgn->built_status != UPTODATE) {
		/*
		 * This is what Make does and it's actually a good thing, as it
		 * allows rules like
		 *
		 *	cmp -s y.tab.h parse.h || cp y.tab.h parse.h
		 *
		 * to function as intended. Unfortunately, thanks to the
		 * stateless nature of NFS, there are times when the
		 * modification time of a file created on a remote machine
		 * will not be modified before the local stat() implied by
		 * the Dir_MTime occurs, thus leading us to believe that the
		 * file is unchanged, wreaking havoc with files that depend
		 * on this one.
		 */
		if (noExecute || is_out_of_date(Dir_MTime(cgn)))
			clock_gettime(CLOCK_REALTIME, &cgn->mtime);
		if (DEBUG(MAKE))
			printf("update time: %s\n",
			    time_to_string(&cgn->mtime));
	}

	requeue(cgn);
	/* SIB: this is where I should mark the build as finished */
	for (ln = Lst_First(&cgn->parents); ln != NULL; ln = Lst_Adv(ln)) {
		pgn = Lst_Datum(ln);
		/* SIB: there should be a siblings loop there */
		pgn->children_left--;
		if (pgn->must_make) {
			if (DEBUG(MAKE))
				printf("%s--=%d ",
				    pgn->name, pgn->children_left);

			if ( ! (cgn->type & (OP_EXEC|OP_USE))) {
				if (cgn->built_status == REBUILT)
					pgn->child_rebuilt = true;
				(void)Make_TimeStamp(pgn, cgn);
			}
			if (pgn->children_left == 0) {
				/*
				 * Queue the node up -- any yet-to-build
				 * predecessors will be dealt with in
				 * MakeStartJobs.
				 */
				if (DEBUG(MAKE))
					printf("QUEUING ");
				Array_Push(&to_build, pgn);
			} else if (pgn->children_left < 0) {
				Error("Child %s discovered graph cycles through %s", cgn->name, pgn->name);
			}
		}
	}
	if (DEBUG(MAKE))
		printf("\n");
	requeue_successors(cgn);
}

static void
MakePrintStatus(void *gnp)
{
	GNode	*gn = gnp;
	if (gn->built_status == UPTODATE) {
		printf("`%s' is up to date.\n", gn->name);
	} else if (gn->children_left != 0) {
		printf("`%s' not remade because of errors.\n", gn->name);
	}
}

static void
MakeAddChild(void *to_addp, void *ap)
{
	GNode *gn = to_addp;
	struct growableArray *a = ap;

	if (!gn->must_make && !(gn->type & OP_USE))
		Array_Push(a, gn);
}

static void
MakeHandleUse(void *cgnp, void *pgnp)
{
	GNode *cgn = cgnp;
	GNode *pgn = pgnp;

	if (cgn->type & OP_USE)
		Make_HandleUse(cgn, pgn);
}


/* round-about detection: assume make is bug-free, if there are targets
 * that have not been touched, it means they never were reached, so we can
 * look for a cycle
 */
static bool
targets_contain_cycles(void)
{
	GNode *gn;
	unsigned int i;
	bool cycle = false;
	bool first = true;

	for (gn = ohash_first(&targets, &i); gn != NULL;
	    gn = ohash_next(&targets, &i)) {
	    	if (has_been_built(gn))
			continue;
		cycle = true;
		if (first)
			printf("Error target(s) unaccounted for: ");
		printf("%s ", gn->name);
		first = false;
	}
	if (!first)
		printf("\n");
	return cycle;
}

static void
print_unlink_cycle(struct growableArray *l, GNode *c)
{
	LstNode ln;
	GNode *gn = NULL;
	unsigned int i;
	
	printf("Cycle found: ");

	for (i = 0; i != l->n; i++) {
		gn = l->a[i];
		if (gn == c)
			printf("(");
		printf("%s -> ", gn->name);
	}
	printf("%s)\n", c->name);
	assert(gn);

	/* So the first element is tied to our node, find and kill the link */
	for (ln = Lst_First(&gn->children); ln != NULL; ln = Lst_Adv(ln)) {
		GNode *gn2 = Lst_Datum(ln);
		if (gn2 == c) {
			Lst_Remove(&gn->children, ln);
			return;
		}
	}
	/* this shouldn't happen ever */
	assert(0);
}

/* each call to find_cycle records a cycle in cycle, to break at node c.
 * this will stop eventually.
 */
static void
break_and_print_cycles(Lst t)
{
	struct growableArray cycle;

	Array_Init(&cycle, 16); /* cycles are generally shorter */
	while (1) {
		GNode *c;

		Array_Reset(&cycle);
		c = find_cycle(t, &cycle);
		if (c)
			print_unlink_cycle(&cycle, c);
		else
			break;
	}
	free(cycle.a);
}


static GNode *
find_cycle(Lst l, struct growableArray *cycle)
{
	LstNode ln;

	for (ln = Lst_First(l); ln != NULL; ln = Lst_Adv(ln)) {
		GNode *gn = Lst_Datum(ln);
		if (gn->in_cycle) {
			/* we should print the cycle and not do more */
			return gn;
		}
		
		if (gn->built_status == UPTODATE)
			continue;
		if (gn->children_left != 0) {
			GNode *c;

			gn->in_cycle = true;
			Array_Push(cycle, gn);
			c = find_cycle(&gn->children, cycle);
			gn->in_cycle = false;
			if (c)
				return c;
			Array_Pop(cycle);
		}
	}
	return NULL;
}
