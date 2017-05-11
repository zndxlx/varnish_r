/*-
 * Copyright 2009-2016 UPLEX - Nils Goroll Systemoptimierung
 * All rights reserved.
 *
 * Authors: Julian Wiesener <jw@uplex.de>
 *          Nils Goroll <slink@uplex.de>
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

#include "shard_parse_vcc_enums.h"

struct vbitmap;

struct shard_circlepoint {
	uint32_t		point;
	unsigned int		host;
};

struct shard_backend {
	VCL_BACKEND		backend;
	const char		*ident;	// XXX COPY IN !
	VCL_DURATION		rampup;
	uint32_t		canon_point;
};

#define	SHDBG_LOOKUP	 1
#define	SHDBG_CIRCLE	(1<<1)
#define	SHDBG_RAMPWARM	(1<<2)

struct sharddir {
	unsigned				magic;
#define SHARDDIR_MAGIC				0xdbb7d59f
	uint32_t				debug_flags;

	pthread_rwlock_t			mtx;

	const char				*name;

	unsigned				n_backend;
	unsigned				l_backend;
	struct shard_backend			*backend;

	struct shard_circlepoint		*hashcircle;

	VCL_DURATION				rampup_duration;
	VCL_REAL				warmup;
	VCL_INT					replicas;
};

static inline VCL_BACKEND
sharddir_backend(const struct sharddir *shardd, int id)
{
	assert(id >= 0);
	assert(id < shardd->n_backend);
	return (shardd->backend[id].backend);
}

static inline const char *
sharddir_backend_ident(const struct sharddir *shardd, int host)
{
	assert(host >= 0);
	assert(host < shardd->n_backend);
	return (shardd->backend[host].ident);
}

#define SHDBG(flag, shardd, ...)					\
	do {								\
		if ((shardd)->debug_flags & (flag))			\
			VSL(SLT_Debug, 0, "shard: " __VA_ARGS__);	\
	} while (0)

#define shard_err(ctx, shardd, fmt, ...)				\
	do {								\
		sharddir_err(ctx, SLT_Error, "shard %s: " fmt,		\
		    (shardd)->name, __VA_ARGS__);			\
	} while (0)

#define shard_err0(ctx, shardd, msg)					\
	do {								\
		sharddir_err(ctx, SLT_Error, "shard %s: %s",		\
		    (shardd)->name, (msg));				\
	} while (0)

void sharddir_debug(struct sharddir *shardd, const uint32_t flags);
void sharddir_err(VRT_CTX, enum VSL_tag_e tag,  const char *fmt, ...);
void sharddir_new(struct sharddir **sharddp, const char *vcl_name);
void sharddir_delete(struct sharddir **sharddp);
void sharddir_rdlock(struct sharddir *shardd);
void sharddir_wrlock(struct sharddir *shardd);
void sharddir_unlock(struct sharddir *shardd);
VCL_BACKEND sharddir_pick_be(VRT_CTX, struct sharddir *, uint32_t, VCL_INT,
   VCL_REAL, VCL_BOOL, enum healthy_e);

/* in shard_cfg.c */
void shardcfg_delete(const struct sharddir *shardd);
VCL_DURATION shardcfg_get_rampup(const struct sharddir *shardd, int host);
