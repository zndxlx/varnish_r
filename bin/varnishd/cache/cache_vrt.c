/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2015 Varnish Software AS
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
 * Runtime support for compiled VCL programs
 */

#include "config.h"

#include "cache.h"

#include "cache_director.h"
#include "hash/hash_slinger.h"
#include "vav.h"
#include "vcl.h"
#include "vrt.h"
#include "vrt_obj.h"
#include "vsa.h"
#include "vtcp.h"
#include "vtim.h"

const void * const vrt_magic_string_end = &vrt_magic_string_end;
const void * const vrt_magic_string_unset = &vrt_magic_string_unset;

/*--------------------------------------------------------------------*/

void
VRT_synth(VRT_CTX, unsigned code, const char *reason)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	if (code < 100)
		code = 503;
	ctx->req->err_code = (uint16_t)code;
	ctx->req->err_reason = reason ? reason
	    : http_Status2Reason(ctx->req->err_code % 1000, NULL);
}

/*--------------------------------------------------------------------*/

void
VRT_acl_log(VRT_CTX, const char *msg)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(msg);
	if (ctx->vsl != NULL)
		VSLb(ctx->vsl, SLT_VCL_acl, "%s", msg);
	else
		VSL(SLT_VCL_acl, 0, "%s", msg);
}

int
VRT_acl_match(VRT_CTX, VCL_ACL acl, VCL_IP ip)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(acl, VRT_ACL_MAGIC);
	assert(VSA_Sane(ip));
	return (acl->match(ctx, ip));
}

void
VRT_hit_for_pass(VRT_CTX, VCL_DURATION d)
{
	struct objcore *oc;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (ctx->bo == NULL) {
		VSLb(ctx->vsl, SLT_Error,
		    "Note: Ignoring DURATION argument to return(pass);");
		return;
	}
	CHECK_OBJ_NOTNULL(ctx->bo, BUSYOBJ_MAGIC);
	oc = ctx->bo->fetch_objcore;
	oc->ttl = d;
	oc->grace = 0.0;
	oc->keep = 0.0;
	VSLb(ctx->vsl, SLT_TTL, "HFP %.0f %.0f %.0f %.0f",
	    oc->ttl, oc->grace, oc->keep, oc->t_origin);
}

/*--------------------------------------------------------------------*/

struct http *
VRT_selecthttp(VRT_CTX, enum gethdr_e where)
{
	struct http *hp;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	switch (where) {
	case HDR_REQ:
		hp = ctx->http_req;
		break;
	case HDR_REQ_TOP:
		hp = ctx->http_req_top;
		break;
	case HDR_BEREQ:
		hp = ctx->http_bereq;
		break;
	case HDR_BERESP:
		hp = ctx->http_beresp;
		break;
	case HDR_RESP:
		hp = ctx->http_resp;
		break;
	default:
		WRONG("VRT_selecthttp 'where' invalid");
	}
	return (hp);
}

/*--------------------------------------------------------------------*/

const char *
VRT_GetHdr(VRT_CTX, const struct gethdr_s *hs)
{
	const char *p;
	struct http *hp;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (hs->where == HDR_OBJ) {
		CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
		CHECK_OBJ_NOTNULL(ctx->req->objcore, OBJCORE_MAGIC);
		return(HTTP_GetHdrPack(ctx->req->wrk, ctx->req->objcore,
		    hs->what));
	}
	hp = VRT_selecthttp(ctx, hs->where);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	if (!http_GetHdr(hp, hs->what, &p))
		return (NULL);
	return (p);
}

/*--------------------------------------------------------------------
 * Collapse a STRING_LIST in the space provided, or return NULL
 */

char *
VRT_StringList(char *d, unsigned dl, const char *p, va_list ap)
{
	char *b, *e;
	unsigned x;

	b = d;
	e = b + dl;
	while (p != vrt_magic_string_end && b < e) {
		if (p != NULL) {
			x = strlen(p);
			if (b + x < e)
				memcpy(b, p, x);
			b += x;
		}
		p = va_arg(ap, const char *);
	}
	if (b >= e)
		return (NULL);
	*b++ = '\0';
	return (b);
}

/*--------------------------------------------------------------------
 * Copy and merge a STRING_LIST into a workspace.
 */

const char *
VRT_String(struct ws *ws, const char *h, const char *p, va_list ap)
{
	char *b, *e;
	unsigned u, x;

	u = WS_Reserve(ws, 0);
	e = b = ws->f;
	e += u;
	if (h != NULL) {
		x = strlen(h);
		if (b + x < e)
			memcpy(b, h, x);
		b += x;
		if (b < e)
			*b = ' ';
		b++;
	}
	b = VRT_StringList(b, e > b ? e - b : 0, p, ap);
	if (b == NULL || b == e) {
		WS_Release(ws, 0);
		return (NULL);
	}
	e = b;
	b = ws->f;
	WS_Release(ws, e - b);
	return (b);
}

/*--------------------------------------------------------------------
 * Copy and merge a STRING_LIST on the current workspace
 */

const char *
VRT_CollectString(VRT_CTX, const char *p, ...)
{
	va_list ap;
	const char *b;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->ws, WS_MAGIC);
	va_start(ap, p);
	b = VRT_String(ctx->ws, NULL, p, ap);
	va_end(ap);
	return (b);
}

/*--------------------------------------------------------------------*/

void
VRT_SetHdr(VRT_CTX , const struct gethdr_s *hs,
    const char *p, ...)
{
	struct http *hp;
	va_list ap;
	const char *b;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(hs);
	AN(hs->what);
	hp = VRT_selecthttp(ctx, hs->where);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	va_start(ap, p);
	if (p == vrt_magic_string_unset) {
		http_Unset(hp, hs->what);
	} else {
		b = VRT_String(hp->ws, hs->what + 1, p, ap);
		if (b == NULL) {
			VSLb(ctx->vsl, SLT_LostHeader, "%s", hs->what + 1);
		} else {
			http_Unset(hp, hs->what);
			http_SetHeader(hp, b);
		}
	}
	va_end(ap);
}

/*--------------------------------------------------------------------*/

void
VRT_handling(VRT_CTX, unsigned hand)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	assert(hand > 0);
	assert(hand < VCL_RET_MAX);
	// XXX:NOTYET assert(*ctx->handling == 0);
	*ctx->handling = hand;
}

/*--------------------------------------------------------------------*/

void
VRT_fail(VRT_CTX, const char *fmt, ...)
{
	va_list ap;

	assert(ctx->vsl != NULL || ctx->msg != NULL);
	AZ(strchr(fmt, '\n'));
	va_start(ap, fmt);
	if (ctx->vsl != NULL)
		VSLbv(ctx->vsl, SLT_VCL_Error, fmt, ap);
	else {
		VSB_vprintf(ctx->msg, fmt, ap);
		VSB_putc(ctx->msg, '\n');
	}
	va_end(ap);
	VRT_handling(ctx, VCL_RET_FAIL);
}

/*--------------------------------------------------------------------
 * Feed data into the hash calculation
 */

void
VRT_hashdata(VRT_CTX, const char *str, ...)
{
	va_list ap;
	const char *p;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	AN(ctx->specific);
	HSH_AddString(ctx->req, ctx->specific, str);
	va_start(ap, str);
	while (1) {
		p = va_arg(ap, const char *);
		if (p == vrt_magic_string_end)
			break;
		HSH_AddString(ctx->req, ctx->specific, p);
	}
	va_end(ap);
	/*
	 * Add a 'field-separator' to make it more difficult to
	 * manipulate the hash.
	 */
	HSH_AddString(ctx->req, ctx->specific, NULL);
}

/*--------------------------------------------------------------------*/

double
VRT_r_now(VRT_CTX)
{
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	return (ctx->now);
}

/*--------------------------------------------------------------------*/

char *
VRT_IP_string(VRT_CTX, VCL_IP ip)
{
	char *p;
	unsigned len;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	if (ip == NULL)
		return (NULL);
	len = WS_Reserve(ctx->ws, 0);
	if (len == 0) {
		WS_Release(ctx->ws, 0);
		return (NULL);
	}
	p = ctx->ws->f;
	VTCP_name(ip, p, len, NULL, 0);
	WS_Release(ctx->ws, strlen(p) + 1);
	return (p);
}

char *
VRT_INT_string(VRT_CTX, long num)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	return (WS_Printf(ctx->ws, "%ld", num));
}

char *
VRT_REAL_string(VRT_CTX, double num)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	return (WS_Printf(ctx->ws, "%.3f", num));
}

char *
VRT_TIME_string(VRT_CTX, double t)
{
	char *p;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	p = WS_Alloc(ctx->ws, VTIM_FORMAT_SIZE);
	if (p != NULL)
		VTIM_format(t, p);
	return (p);
}

const char * __match_proto__()
VRT_BACKEND_string(VCL_BACKEND d)
{
	if (d == NULL)
		return (NULL);
	CHECK_OBJ_NOTNULL(d, DIRECTOR_MAGIC);
	return (d->vcl_name);
}

const char *
VRT_BOOL_string(unsigned val)
{

	return (val ? "true" : "false");
}

/*--------------------------------------------------------------------*/

void
VRT_Rollback(VRT_CTX, const struct http *hp)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(hp, HTTP_MAGIC);
	if (hp == ctx->http_req) {
		CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
		HTTP_Copy(ctx->req->http, ctx->req->http0);
		WS_Reset(ctx->req->ws, ctx->req->ws_req);
	} else if (hp == ctx->http_bereq) {
		CHECK_OBJ_NOTNULL(ctx->bo, BUSYOBJ_MAGIC);
		HTTP_Copy(ctx->bo->bereq, ctx->bo->bereq0);
		WS_Reset(ctx->bo->bereq->ws, ctx->bo->ws_bo);
		WS_Reset(ctx->bo->ws, ctx->bo->ws_bo);
	} else
		WRONG("VRT_Rollback 'hp' invalid");
}

/*--------------------------------------------------------------------*/

void
VRT_synth_page(VRT_CTX, const char *str, ...)
{
	va_list ap;
	const char *p;
	struct vsb *vsb;

	CAST_OBJ_NOTNULL(vsb, ctx->specific, VSB_MAGIC);
	va_start(ap, str);
	p = str;
	while (p != vrt_magic_string_end) {
		if (p == NULL)
			p = "(null)";
		VSB_cat(vsb, p);
		p = va_arg(ap, const char *);
	}
	va_end(ap);
}

/*--------------------------------------------------------------------*/

void
VRT_ban_string(VRT_CTX, const char *str)
{
	char *a1, *a2, *a3;
	char **av;
	struct ban_proto *bp;
	const char *err;
	int i;

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(ctx->vsl);
	AN(str);

	bp = BAN_Build();
	if (bp == NULL) {
		VSLb(ctx->vsl, SLT_VCL_Error, "ban(): Out of Memory");
		return;
	}
	av = VAV_Parse(str, NULL, ARGV_NOESC);
	AN(av);
	if (av[0] != NULL) {
		VSLb(ctx->vsl, SLT_VCL_Error, "ban(): %s", av[0]);
		VAV_Free(av);
		BAN_Abandon(bp);
		return;
	}
	for (i = 0; ;) {
		a1 = av[++i];
		if (a1 == NULL) {
			VSLb(ctx->vsl, SLT_VCL_Error,
			    "ban(): No ban conditions found.");
			break;
		}
		a2 = av[++i];
		if (a2 == NULL) {
			VSLb(ctx->vsl, SLT_VCL_Error,
			    "ban(): Expected comparison operator.");
			break;
		}
		a3 = av[++i];
		if (a3 == NULL) {
			VSLb(ctx->vsl, SLT_VCL_Error,
			    "ban(): Expected second operand.");
			break;
		}
		err = BAN_AddTest(bp, a1, a2, a3);
		if (err) {
			VSLb(ctx->vsl, SLT_VCL_Error, "ban(): %s", err);
			break;
		}
		if (av[++i] == NULL) {
			err = BAN_Commit(bp);
			if (err == NULL)
				bp = NULL;
			else
				VSLb(ctx->vsl, SLT_VCL_Error, "ban(): %s", err);
			break;
		}
		if (strcmp(av[i], "&&")) {
			VSLb(ctx->vsl, SLT_VCL_Error,
			    "ban(): Expected && between conditions,"
			    " found \"%s\"", av[i]);
			break;
		}
	}
	if (bp != NULL)
		BAN_Abandon(bp);
	VAV_Free(av);
}

VCL_BYTES
VRT_CacheReqBody(VRT_CTX, VCL_BYTES maxsize)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	if (ctx->method != VCL_MET_RECV) {
		VSLb(ctx->vsl, SLT_VCL_Error,
		    "req.body can only be cached in vcl_recv{}");
		return (-1);
	}
	return (VRB_Cache(ctx->req, maxsize));
}

/*--------------------------------------------------------------------
 * purges
 */

void
VRT_purge(VRT_CTX, double ttl, double grace, double keep)
{

	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(ctx->req->wrk, WORKER_MAGIC);
	if (ctx->method == VCL_MET_HIT || ctx->method == VCL_MET_MISS)
		HSH_Purge(ctx->req->wrk, ctx->req->objcore->objhead,
		    ttl, grace, keep);
}

/*--------------------------------------------------------------------
 * Simple stuff
 */

int
VRT_strcmp(const char *s1, const char *s2)
{
	if (s1 == NULL || s2 == NULL)
		return(1);
	return (strcmp(s1, s2));
}

void
VRT_memmove(void *dst, const void *src, unsigned len)
{

	(void)memmove(dst, src, len);
}

int
VRT_ipcmp(const struct suckaddr *sua1, const struct suckaddr *sua2)
{
	if (sua1 == NULL || sua2 == NULL)
		return(1);
	return (VSA_Compare_IP(sua1, sua2));
}
