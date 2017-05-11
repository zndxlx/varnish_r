/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2011 Varnish Software AS
 * All rights reserved.
 *
 * Author: Poul-Henning Kamp <phk@phk.freebsd.dk>
 *
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
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * LRU and object timer handling.
 *
 */

#include "config.h"

#include <stdlib.h>

#include "cache.h"

#include "binary_heap.h"
#include "hash/hash_slinger.h"
#include "vtim.h"

struct exp_priv {
	unsigned			magic;
#define EXP_PRIV_MAGIC			0x9db22482
	struct lock			mtx;

	struct worker			*wrk;
	struct vsl_log			vsl;

	VSTAILQ_HEAD(,objcore)		inbox;
	struct binheap			*heap;
	pthread_cond_t			condvar;

	pthread_rwlock_t		cb_rwl;
};

static struct exp_priv *exphdl;

/*--------------------------------------------------------------------
 * Calculate an objects effective ttl time, taking req.ttl into account
 * if it is available.
 */

double
EXP_Ttl(const struct req *req, const struct objcore *oc)
{
	double r;

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);

	r = oc->ttl;
	if (req != NULL && req->d_ttl > 0. && req->d_ttl < r)
		r = req->d_ttl;
	return (oc->t_origin + r);
}

/*--------------------------------------------------------------------
 * Post an objcore to the exp_thread's inbox.
 */

static void
exp_mail_it(struct objcore *oc, uint8_t cmds)
{
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	assert(oc->refcnt > 0);

	Lck_Lock(&exphdl->mtx);
	if ((cmds | oc->exp_flags) & OC_EF_REFD) {
		if (!(oc->exp_flags & OC_EF_POSTED)) {
			if (cmds & OC_EF_REMOVE)
				VSTAILQ_INSERT_HEAD(&exphdl->inbox,
				    oc, exp_list);
			else
				VSTAILQ_INSERT_TAIL(&exphdl->inbox,
				    oc, exp_list);
		}
		oc->exp_flags |= cmds | OC_EF_POSTED;
		AN(oc->exp_flags & OC_EF_REFD);
		VSC_C_main->exp_mailed++;
		AZ(pthread_cond_signal(&exphdl->condvar));
	}
	Lck_Unlock(&exphdl->mtx);
}

/*--------------------------------------------------------------------
 * Call EXP's attention to a an oc
 */

void
EXP_Remove(struct objcore *oc)
{

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	if (oc->exp_flags & OC_EF_REFD)
		exp_mail_it(oc, OC_EF_REMOVE);
}

/*--------------------------------------------------------------------
 * Insert new object.
 *
 * Caller got a oc->refcnt for us.
 */

void
EXP_Insert(struct worker *wrk, struct objcore *oc)
{

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	assert(oc->refcnt >= 2);

	AZ(oc->exp_flags & (OC_EF_INSERT | OC_EF_MOVE));
	AZ(oc->flags & OC_F_DYING);

	ObjSendEvent(wrk, oc, OEV_INSERT);
	exp_mail_it(oc, OC_EF_INSERT | OC_EF_REFD | OC_EF_MOVE);
}

/*--------------------------------------------------------------------
 * We have changed one or more of the object timers, tell the exp_thread
 *
 */

void
EXP_Rearm(struct objcore *oc, double now, double ttl, double grace, double keep)
{
	double when;

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	assert(oc->refcnt > 0);

	if (!(oc->exp_flags & OC_EF_REFD))
		return;

	if (!isnan(ttl))
		oc->ttl = now + ttl - oc->t_origin;
	if (!isnan(grace))
		oc->grace = grace;
	if (!isnan(keep))
		oc->keep = keep;

	when = EXP_WHEN(oc);

	VSL(SLT_ExpKill, 0, "EXP_Rearm p=%p E=%.9f e=%.9f f=0x%x", oc,
	    oc->timer_when, when, oc->flags);

	if (when < oc->t_origin || when < oc->timer_when)
		exp_mail_it(oc, OC_EF_MOVE);
}

/*--------------------------------------------------------------------
 * Handle stuff in the inbox
 */

static void
exp_inbox(struct exp_priv *ep, struct objcore *oc, unsigned flags)
{

	CHECK_OBJ_NOTNULL(ep, EXP_PRIV_MAGIC);
	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	assert(oc->refcnt > 0);

	VSLb(&ep->vsl, SLT_ExpKill, "EXP_Inbox flg=%x p=%p e=%.9f f=0x%x",
	    flags, oc, oc->timer_when, oc->flags);

	if (flags & OC_EF_REMOVE) {
		if (!(flags & OC_EF_INSERT)) {
			assert(oc->timer_idx != BINHEAP_NOIDX);
			binheap_delete(ep->heap, oc->timer_idx);
		}
		assert(oc->timer_idx == BINHEAP_NOIDX);
		assert(oc->refcnt > 0);
		AZ(oc->exp_flags);
		ObjSendEvent(ep->wrk, oc, OEV_EXPIRE);
		(void)HSH_DerefObjCore(ep->wrk, &oc, 0);
		return;
	}

	if (flags & OC_EF_MOVE) {
		oc->timer_when = EXP_WHEN(oc);
		ObjSendEvent(ep->wrk, oc, OEV_TTLCHG);
	}

	VSLb(&ep->vsl, SLT_ExpKill, "EXP_When p=%p e=%.9f f=0x%x", oc,
	    oc->timer_when, flags);

	/*
	 * XXX: There are some pathological cases here, were we
	 * XXX: insert or move an expired object, only to find out
	 * XXX: the next moment and rip them out again.
	 */

	if (flags & OC_EF_INSERT) {
		assert(oc->timer_idx == BINHEAP_NOIDX);
		binheap_insert(exphdl->heap, oc);
		assert(oc->timer_idx != BINHEAP_NOIDX);
	} else if (flags & OC_EF_MOVE) {
		assert(oc->timer_idx != BINHEAP_NOIDX);
		binheap_reorder(exphdl->heap, oc->timer_idx);
		assert(oc->timer_idx != BINHEAP_NOIDX);
	} else {
		WRONG("Objcore state wrong in inbox");
	}
}

/*--------------------------------------------------------------------
 * Expire stuff from the binheap
 */

static double
exp_expire(struct exp_priv *ep, double now)
{
	struct objcore *oc;

	CHECK_OBJ_NOTNULL(ep, EXP_PRIV_MAGIC);

	oc = binheap_root(ep->heap);
	if (oc == NULL)
		return (now + 355./113.);
	VSLb(&ep->vsl, SLT_ExpKill, "EXP_expire p=%p e=%.9f f=0x%x", oc,
	    oc->timer_when - now, oc->flags);

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);

	/* Ready ? */
	if (oc->timer_when > now)
		return (oc->timer_when);

	VSC_C_main->n_expired++;

	Lck_Lock(&ep->mtx);
	if (oc->exp_flags & OC_EF_POSTED) {
		oc->exp_flags |= OC_EF_REMOVE;
		oc = NULL;
	} else {
		oc->exp_flags &= ~OC_EF_REFD;
	}
	Lck_Unlock(&ep->mtx);
	if (oc != NULL) {
		if (!(oc->flags & OC_F_DYING))
			HSH_Kill(oc);

		/* Remove from binheap */
		assert(oc->timer_idx != BINHEAP_NOIDX);
		binheap_delete(ep->heap, oc->timer_idx);
		assert(oc->timer_idx == BINHEAP_NOIDX);

		CHECK_OBJ_NOTNULL(oc->objhead, OBJHEAD_MAGIC);
		VSLb(&ep->vsl, SLT_ExpKill, "EXP_Expired x=%u t=%.0f",
		    ObjGetXID(ep->wrk, oc), EXP_Ttl(NULL, oc) - now);
		ObjSendEvent(ep->wrk, oc, OEV_EXPIRE);
		(void)HSH_DerefObjCore(ep->wrk, &oc, 0);
	}
	return (0);
}

/*--------------------------------------------------------------------
 * This thread monitors the root of the binary heap and whenever an
 * object expires, accounting also for graceability, it is killed.
 */

static int __match_proto__(binheap_cmp_t)
object_cmp(void *priv, const void *a, const void *b)
{
	const struct objcore *aa, *bb;

	(void)priv;
	CAST_OBJ_NOTNULL(aa, a, OBJCORE_MAGIC);
	CAST_OBJ_NOTNULL(bb, b, OBJCORE_MAGIC);
	return (aa->timer_when < bb->timer_when);
}

static void __match_proto__(binheap_update_t)
object_update(void *priv, void *p, unsigned u)
{
	struct objcore *oc;

	(void)priv;
	CAST_OBJ_NOTNULL(oc, p, OBJCORE_MAGIC);
	oc->timer_idx = u;
}

static void * __match_proto__(bgthread_t)
exp_thread(struct worker *wrk, void *priv)
{
	struct objcore *oc;
	double t = 0, tnext = 0;
	struct exp_priv *ep;
	unsigned flags = 0;

	CAST_OBJ_NOTNULL(ep, priv, EXP_PRIV_MAGIC);
	ep->wrk = wrk;
	VSL_Setup(&ep->vsl, NULL, 0);
	ep->heap = binheap_new(NULL, object_cmp, object_update);
	AN(ep->heap);
	while (1) {

		Lck_Lock(&ep->mtx);
		oc = VSTAILQ_FIRST(&ep->inbox);
		CHECK_OBJ_ORNULL(oc, OBJCORE_MAGIC);
		if (oc != NULL) {
			assert(oc->refcnt >= 1);
			VSTAILQ_REMOVE(&ep->inbox, oc, objcore, exp_list);
			VSC_C_main->exp_received++;
			tnext = 0;
			flags = oc->exp_flags;
			if (flags & OC_EF_REMOVE)
				oc->exp_flags = 0;
			else
				oc->exp_flags &= OC_EF_REFD;
		} else if (tnext > t) {
			VSL_Flush(&ep->vsl, 0);
			Pool_Sumstat(wrk);
			(void)Lck_CondWait(&ep->condvar, &ep->mtx, tnext);
		}
		Lck_Unlock(&ep->mtx);

		t = VTIM_real();

		if (oc != NULL)
			exp_inbox(ep, oc, flags);
		else
			tnext = exp_expire(ep, t);
	}
	NEEDLESS(return NULL);
}

/*--------------------------------------------------------------------*/

void
EXP_Init(void)
{
	struct exp_priv *ep;
	pthread_t pt;

	ALLOC_OBJ(ep, EXP_PRIV_MAGIC);
	AN(ep);

	Lck_New(&ep->mtx, lck_exp);
	AZ(pthread_cond_init(&ep->condvar, NULL));
	VSTAILQ_INIT(&ep->inbox);
	AZ(pthread_rwlock_init(&ep->cb_rwl, NULL));
	exphdl = ep;
	WRK_BgThread(&pt, "cache-timeout", exp_thread, ep);
}
