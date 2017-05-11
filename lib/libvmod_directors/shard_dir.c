/*-
 * Copyright 2009-2016 UPLEX - Nils Goroll Systemoptimierung
 * All rights reserved.
 *
 * Authors: Nils Goroll <nils.goroll@uplex.de>
 *          Geoffrey Simmons <geoff.simmons@uplex.de>
 *          Julian Wiesener <jw@uplex.de>
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
 */

/*lint -e801 */

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "cache/cache.h"
#include "cache/cache_director.h"

#include "vrt.h"
#include "vbm.h"
#include "vrnd.h"

#include "shard_dir.h"

struct shard_be_info {
	int		hostid;
	unsigned	healthy;
	double		changed;	// when
};

/*
 * circle walk state for shard_next
 *
 * pick* cut off the search after having seen all possible backends
 */
struct shard_state {
	const struct vrt_ctx	*ctx;
	struct sharddir	*shardd;
	int			idx;

	struct vbitmap		*picklist;
	int			pickcount;

	struct shard_be_info	previous;
	struct shard_be_info	last;
};

void
sharddir_debug(struct sharddir *shardd, const uint32_t flags)
{
	CHECK_OBJ_NOTNULL(shardd, SHARDDIR_MAGIC);
	shardd->debug_flags = flags;
}

void
sharddir_err(VRT_CTX, enum VSL_tag_e tag,  const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	if (ctx->vsl)
		VSLbv(ctx->vsl, tag, fmt, ap);
	else
		VSLv(tag, 0, fmt, ap);
	va_end(ap);
}

static int
shard_lookup(const struct sharddir *shardd, const uint32_t key)
{
	CHECK_OBJ_NOTNULL(shardd, SHARDDIR_MAGIC);

	const int n = shardd->n_backend * shardd->replicas;
	int idx = -1, high = n, low = 0, i;

	do {
	    i = (high + low) / 2 ;
	    if (shardd->hashcircle[i].point == key)
		idx = i;
	    else if (i == n - 1)
		idx = n - 1;
	    else if (shardd->hashcircle[i].point < key &&
		     shardd->hashcircle[i+1].point >= key)
		idx = i + 1;
	    else if (shardd->hashcircle[i].point > key)
		if (i == 0)
		    idx = 0;
		else
		    high = i;
	    else
		low = i;
	} while (idx == -1);

	return idx;
}

static int
shard_next(struct shard_state *state, VCL_INT skip, VCL_BOOL healthy)
{
	int c, chosen = -1;
	uint32_t ringsz;
	VCL_BACKEND be;
	double changed;
	struct shard_be_info *sbe;

	AN(state);
	assert(state->idx >= 0);
	CHECK_OBJ_NOTNULL(state->shardd, SHARDDIR_MAGIC);

	if (state->pickcount >= state->shardd->n_backend)
		return -1;

	ringsz = state->shardd->n_backend * state->shardd->replicas;

	while (state->pickcount < state->shardd->n_backend && skip >= 0) {

		c = state->shardd->hashcircle[state->idx].host;

		if (!vbit_test(state->picklist, c)) {

			vbit_set(state->picklist, c);
			state->pickcount++;

			sbe = NULL;
			be = state->shardd->backend[c].backend;
			AN(be);
			if (be->healthy(be, state->ctx->bo, &changed)) {
				if (skip-- == 0) {
					chosen = c;
					sbe = &state->last;
				} else {
					sbe = &state->previous;
				}

			} else if (!healthy && skip-- == 0) {
				chosen = c;
				sbe = &state->last;
			}
			if (sbe == &state->last &&
			    state->last.hostid != -1)
				memcpy(&state->previous, &state->last,
				    sizeof(state->previous));

			if (sbe) {
				sbe->hostid = c;
				sbe->healthy = 1;
				sbe->changed = changed;
			}
			if (chosen != -1)
				break;
		}

		if (++(state->idx) == ringsz)
			state->idx = 0;
	}
	return chosen;
}

void
sharddir_new(struct sharddir **sharddp, const char *vcl_name)
{
	struct sharddir *shardd;

	AN(vcl_name);
	AN(sharddp);
	AZ(*sharddp);
	ALLOC_OBJ(shardd, SHARDDIR_MAGIC);
	AN(shardd);
	*sharddp = shardd;
	shardd->name = vcl_name;
	AZ(pthread_rwlock_init(&shardd->mtx, NULL));
}

void
sharddir_delete(struct sharddir **sharddp)
{
	struct sharddir *shardd;

	AN(sharddp);
	shardd = *sharddp;
	*sharddp = NULL;

	CHECK_OBJ_NOTNULL(shardd, SHARDDIR_MAGIC);
	shardcfg_delete(shardd);
	AZ(pthread_rwlock_destroy(&shardd->mtx));
	FREE_OBJ(shardd);
}

void
sharddir_rdlock(struct sharddir *shardd)
{
	CHECK_OBJ_NOTNULL(shardd, SHARDDIR_MAGIC);
	AZ(pthread_rwlock_rdlock(&shardd->mtx));
}

void
sharddir_wrlock(struct sharddir *shardd)
{
	CHECK_OBJ_NOTNULL(shardd, SHARDDIR_MAGIC);
	AZ(pthread_rwlock_wrlock(&shardd->mtx));
}

void
sharddir_unlock(struct sharddir *shardd)
{
	CHECK_OBJ_NOTNULL(shardd, SHARDDIR_MAGIC);
	AZ(pthread_rwlock_unlock(&shardd->mtx));
}

static inline void
validate_alt(VRT_CTX, const struct sharddir *shardd, VCL_INT *alt)
{
	const VCL_INT alt_max = shardd->n_backend - 1;

	if (*alt < 0) {
		shard_err(ctx, shardd,
		    "invalid negative parameter alt=%ld, set to 0", *alt);
		*alt = 0;
	} else if (*alt > alt_max) {
		shard_err(ctx, shardd,
		    "parameter alt=%ld limited to %ld", *alt, alt_max);
		*alt = alt_max;
	}
}

static inline void
init_state(struct shard_state *state,
    VRT_CTX, struct sharddir *shardd, struct vbitmap *picklist)
{
	AN(picklist);

	state->ctx = ctx;
	state->shardd = shardd;
	state->idx = -1;
	state->picklist = picklist;

	/* healhy and changed only defined for hostid != -1 */
	state->previous.hostid = -1;
	state->last.hostid = -1;
}

/*
 * core function for the director backend method
 *
 * while other directors return a reference to their own backend object (on
 * which varnish will call the resolve method to resolve to a non-director
 * backend), this director immediately reolves in the backend method, to make
 * the director choice visible in VCL
 *
 * consequences:
 * - we need no own struct director
 * - we can only respect a busy object when being called on the backend side,
 *   which probably is, for all practical purposes, only relevant when the
 *   saintmode vmod is used
 *
 * if we wanted to offer delayed resolution, we'd need something like
 * per-request per-director state or we'd need to return a dynamically created
 * director object. That should be straight forward once we got director
 * refcounting #2072. Until then, we could create it on the workspace, but then
 * we'd need to keep other directors from storing any references to our dynamic
 * object for longer than the current task
 *
 */
VCL_BACKEND
sharddir_pick_be(VRT_CTX, struct sharddir *shardd,
    uint32_t key, VCL_INT alt, VCL_REAL warmup, VCL_BOOL rampup,
    enum healthy_e healthy)
{
	VCL_BACKEND be;
	struct shard_state state;
	unsigned picklist_sz = VBITMAP_SZ(shardd->n_backend);
	char picklist_spc[picklist_sz];
	VCL_DURATION chosen_r, alt_r;

	CHECK_OBJ_NOTNULL(shardd, SHARDDIR_MAGIC);
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(ctx->vsl);

	memset(&state, 0, sizeof(state));
	init_state(&state, ctx, shardd, vbit_init(picklist_spc, picklist_sz));

	sharddir_rdlock(shardd);
	if (shardd->n_backend == 0) {
		shard_err0(ctx, shardd, "no backends");
		goto err;
	}

	assert(shardd->hashcircle);

	validate_alt(ctx, shardd, &alt);

	state.idx = shard_lookup(shardd, key);
	assert(state.idx >= 0);

	SHDBG(SHDBG_LOOKUP, shardd, "lookup key %x idx %d host %u",
	    key, state.idx, shardd->hashcircle[state.idx].host);

	if (alt > 0) {
		if (shard_next(&state, alt - 1, healthy == ALL ? 1 : 0) == -1) {
			if (state.previous.hostid != -1) {
				be = sharddir_backend(shardd,
				    state.previous.hostid);
				goto ok;
			}
			goto err;
		}
	}

	if (shard_next(&state, 0, healthy == IGNORE ? 0 : 1) == -1) {
		if (state.previous.hostid != -1) {
			be = sharddir_backend(shardd, state.previous.hostid);
			goto ok;
		}
		goto err;
	}

	be = sharddir_backend(shardd, state.last.hostid);

	if (warmup == -1)
		warmup = shardd->warmup;

	/* short path for cases we dont want ramup/warmup or can't */
	if (alt > 0 || healthy == IGNORE || (!rampup && warmup == 0) ||
	    shard_next(&state, 0, 0) == -1)
		goto ok;

	assert(alt == 0);
	assert(state.previous.hostid >= 0);
	assert(state.last.hostid >= 0);
	assert(state.previous.hostid != state.last.hostid);
	assert(be == sharddir_backend(shardd, state.previous.hostid));

	chosen_r = shardcfg_get_rampup(shardd, state.previous.hostid);
	alt_r = shardcfg_get_rampup(shardd, state.last.hostid);

	SHDBG(SHDBG_RAMPWARM, shardd, "chosen host %d rampup %f changed %f",
	    state.previous.hostid, chosen_r,
	    ctx->now - state.previous.changed);
	SHDBG(SHDBG_RAMPWARM, shardd, "alt host %d rampup %f changed %f",
	    state.last.hostid, alt_r,
	    ctx->now - state.last.changed);

	if (ctx->now - state.previous.changed < chosen_r) {
		/*
		 * chosen host is in rampup
		 * - no change if alternative host is also in rampup or the dice
		 *   has rolled in favour of the chosen host
		 */
		if (!rampup ||
		    ctx->now - state.last.changed < alt_r ||
		    VRND_RandomTestableDouble() * chosen_r <
		     (ctx->now - state.previous.changed))
			goto ok;
	} else {
		/* chosen host not in rampup - warmup ? */
		if (warmup == 0 || VRND_RandomTestableDouble() > warmup)
			goto ok;
	}

	be = sharddir_backend(shardd, state.last.hostid);

  ok:
	AN(be);
	sharddir_unlock(shardd);
	vbit_destroy(state.picklist);
	return (be);
  err:
	sharddir_unlock(shardd);
	vbit_destroy(state.picklist);
	return NULL;
}
