/*-
 * Copyright (c) 2006 Verdens Gang AS
 * Copyright (c) 2006-2017 Varnish Software AS
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
 * This file contains the request-handling state engine, which is intended to
 * (over time) be(ome) protocol agnostic.
 * We already use this now with ESI:includes, which are for all relevant
 * purposes a different "protocol"
 *
 * A special complication is the fact that we can suspend processing of
 * a request when hash-lookup finds a busy objhdr.
 *
 */

#include "config.h"

#include "cache.h"
#include "cache_director.h"
#include "cache_filter.h"
#include "cache_transport.h"

#include "hash/hash_slinger.h"
#include "storage/storage.h"
#include "vcl.h"
#include "vsha256.h"
#include "vtim.h"

/*--------------------------------------------------------------------
 * Handle "Expect:" and "Connection:" on incoming request
 */

static enum req_fsm_nxt
cnt_transport(struct worker *wrk, struct req *req)
{
	const char *p;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(req->http, HTTP_MAGIC);
	CHECK_OBJ_NOTNULL(req->transport, TRANSPORT_MAGIC);
	assert(req->req_body_status != REQ_BODY_INIT);
	AN(req->transport->minimal_response);

	if (http_GetHdr(req->http, H_Expect, &p)) {
		if (strcasecmp(p, "100-continue")) {
			req->doclose = SC_RX_JUNK;
			(void)req->transport->minimal_response(req, 417);
			wrk->stats->client_req_417++;
			return (REQ_FSM_DONE);
		}
		if (req->http->protover >= 11 &&
		    req->htc->pipeline_b == NULL)	// XXX: HTTP1 vs 2 ?
			req->want100cont = 1;
		http_Unset(req->http, H_Expect);
	}

	wrk->stats->client_req++;
	wrk->stats->s_req++;

	AZ(req->err_code);
	req->ws_req = WS_Snapshot(req->ws);

	req->doclose = http_DoConnection(req->http);
	if (req->doclose == SC_RX_BAD) {
		(void)req->transport->minimal_response(req, 400);
		return (REQ_FSM_DONE);
	}

	if (req->req_body_status < REQ_BODY_TAKEN) {
		AN(req->transport->req_body != NULL);
		VFP_Setup(req->vfc);
		req->vfc->http = req->http;
		req->vfc->wrk = wrk;
		req->transport->req_body(req);
	}

	HTTP_Copy(req->http0, req->http);	// For ESI & restart
	req->req_step = R_STP_RECV;
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * Deliver an object to client
 */

static enum req_fsm_nxt
cnt_deliver(struct worker *wrk, struct req *req)
{

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(req->objcore, OBJCORE_MAGIC);
	CHECK_OBJ_NOTNULL(req->objcore->objhead, OBJHEAD_MAGIC);
	AN(req->vcl);

	assert(req->objcore->refcnt > 0);

	ObjTouch(req->wrk, req->objcore, req->t_prev);

	HTTP_Setup(req->resp, req->ws, req->vsl, SLT_RespMethod);
	if (HTTP_Decode(req->resp,
	    ObjGetAttr(req->wrk, req->objcore, OA_HEADERS, NULL))) {
		req->err_code = 500;
		req->req_step = R_STP_SYNTH;
		return (REQ_FSM_MORE);
	}
	http_ForceField(req->resp, HTTP_HDR_PROTO, "HTTP/1.1");

	if (req->is_hit)
		http_PrintfHeader(req->resp,
		    "X-Varnish: %u %u", VXID(req->vsl->wid),
		    ObjGetXID(wrk, req->objcore));
	else
		http_PrintfHeader(req->resp,
		    "X-Varnish: %u", VXID(req->vsl->wid));

	/*
	 * We base Age calculation upon the last timestamp taken during
	 * client request processing. This gives some inaccuracy, but
	 * since Age is only full second resolution that shouldn't
	 * matter. (Last request timestamp could be a Start timestamp
	 * taken before the object entered into cache leading to negative
	 * age. Truncate to zero in that case).
	 */
	http_PrintfHeader(req->resp, "Age: %.0f",
	    floor(fmax(0., req->t_prev - req->objcore->t_origin)));

	http_SetHeader(req->resp, "Via: 1.1 varnish (Varnish/5.1)");

	if (cache_param->http_gzip_support &&
	    ObjCheckFlag(req->wrk, req->objcore, OF_GZIPED) &&
	    !RFC2616_Req_Gzip(req->http))
		RFC2616_Weaken_Etag(req->resp);

	VCL_deliver_method(req->vcl, wrk, req, NULL, NULL);
	VSLb_ts_req(req, "Process", W_TIM_real(wrk));

	/* Stop the insanity before it turns "Hotel California" on us */
	if (req->restarts >= cache_param->max_restarts)
		wrk->handling = VCL_RET_DELIVER;

	if (wrk->handling != VCL_RET_DELIVER) {
		(void)HSH_DerefObjCore(wrk, &req->objcore, HSH_RUSH_POLICY);
		http_Teardown(req->resp);

		switch (wrk->handling) {
		case VCL_RET_RESTART:
			req->req_step = R_STP_RESTART;
			break;
		case VCL_RET_FAIL:
			req->req_step = R_STP_VCLFAIL;
			break;
		case VCL_RET_SYNTH:
			req->req_step = R_STP_SYNTH;
			break;
		default:
			WRONG("Illegal return from vcl_deliver{}");
		}

		return (REQ_FSM_MORE);
	}

	assert(wrk->handling == VCL_RET_DELIVER);

	if (req->esi_level == 0
	    && http_IsStatus(req->resp, 200)
	    && req->http->conds && RFC2616_Do_Cond(req))
		http_PutResponse(req->resp, "HTTP/1.1", 304, NULL);

	req->req_step = R_STP_TRANSMIT;
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * VCL failed, die horribly
 */

static enum req_fsm_nxt
cnt_vclfail(const struct worker *wrk, struct req *req)
{

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	HTTP_Copy(req->http, req->http0);
	WS_Reset(req->ws, req->ws_req);
	req->err_code = 503;
	req->err_reason = "VCL failed";
	req->req_step = R_STP_SYNTH;
	req->doclose = SC_VCL_FAILURE;
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * Emit a synthetic response
 */

static enum req_fsm_nxt
cnt_synth(struct worker *wrk, struct req *req)
{
	struct http *h;
	double now;
	struct vsb *synth_body;
	ssize_t sz, szl;
	uint8_t *ptr;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	wrk->stats->s_synth++;

	now = W_TIM_real(wrk);
	VSLb_ts_req(req, "Process", now);

	if (req->err_code < 100)
		req->err_code = 501;

	HTTP_Setup(req->resp, req->ws, req->vsl, SLT_RespMethod);
	h = req->resp;
	http_TimeHeader(h, "Date: ", now);
	http_SetHeader(h, "Server: Varnish");
	http_PrintfHeader(req->resp, "X-Varnish: %u", VXID(req->vsl->wid));
	http_PutResponse(h, "HTTP/1.1", req->err_code, req->err_reason);

	/*
	 * For late 100-continue, we suggest to VCL to close the connection to
	 * neither send a 100-continue nor drain-read the request. But VCL has
	 * the option to veto by removing Connection: close
	 */
	if (req->want100cont) {
		http_SetHeader(h, "Connection: close");
	}

	synth_body = VSB_new_auto();
	AN(synth_body);

	VCL_synth_method(req->vcl, wrk, req, NULL, synth_body);

	AZ(VSB_finish(synth_body));

	if (wrk->handling == VCL_RET_FAIL) {
		VSB_destroy(&synth_body);
		req->doclose = SC_VCL_FAILURE;
		VSLb_ts_req(req, "Resp", W_TIM_real(wrk));
		http_Teardown(req->resp);
		return (REQ_FSM_DONE);
	}

	if (wrk->handling == VCL_RET_RESTART) {
		/*
		 * XXX: Should we reset req->doclose = SC_VCL_FAILURE
		 * XXX: If so, to what ?
		 */
		HTTP_Setup(h, req->ws, req->vsl, SLT_RespMethod);
		VSB_destroy(&synth_body);
		req->req_step = R_STP_RESTART;
		return (REQ_FSM_MORE);
	}
	assert(wrk->handling == VCL_RET_DELIVER);

	http_Unset(h, H_Content_Length);
	http_PrintfHeader(req->resp, "Content-Length: %zd",
	    VSB_len(synth_body));

	if (!req->doclose && http_HdrIs(req->resp, H_Connection, "close"))
		req->doclose = SC_RESP_CLOSE;

	/* Discard any lingering request body before delivery */
	(void)VRB_Ignore(req);

	req->objcore = HSH_Private(wrk);
	CHECK_OBJ_NOTNULL(req->objcore, OBJCORE_MAGIC);
	szl = -1;
	if (STV_NewObject(wrk, req->objcore, stv_transient, 1024)) {
		szl = VSB_len(synth_body);
		assert(szl >= 0);
		sz = szl;
		if (sz > 0 &&
		    ObjGetSpace(wrk, req->objcore, &sz, &ptr) && sz >= szl) {
			memcpy(ptr, VSB_data(synth_body), szl);
			ObjExtend(wrk, req->objcore, szl);
		} else if (sz > 0) {
			szl = -1;
		}
	}

	if (szl >= 0)
		AZ(ObjSetU64(wrk, req->objcore, OA_LEN, szl));
	HSH_DerefBoc(wrk, req->objcore);
	VSB_destroy(&synth_body);

	if (szl < 0) {
		VSLb(req->vsl, SLT_Error, "Could not get storage");
		req->doclose = SC_OVERLOAD;
		VSLb_ts_req(req, "Resp", W_TIM_real(wrk));
		(void)HSH_DerefObjCore(wrk, &req->objcore, 1);
		http_Teardown(req->resp);
		return (REQ_FSM_DONE);
	}

	req->req_step = R_STP_TRANSMIT;
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * The mechanics of sending a response (from deliver or synth)
 */

static enum req_fsm_nxt
cnt_transmit(struct worker *wrk, struct req *req)
{
	struct boc *boc;
	const char *r;
	uint16_t status;
	int sendbody;
	intmax_t clval;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(req->transport, TRANSPORT_MAGIC);

	/* Grab a ref to the bo if there is one */
	boc = HSH_RefBoc(req->objcore);

	clval = http_GetContentLength(req->resp);
	if (boc != NULL)
		req->resp_len = clval;
	else
		req->resp_len = ObjGetLen(req->wrk, req->objcore);

	req->res_mode = 0;

	/* RFC 7230, 3.3.3 */
	status = http_GetStatus(req->resp);
	if (!strcmp(req->http0->hd[HTTP_HDR_METHOD].b, "HEAD")) {
		if (req->objcore->flags & OC_F_PASS)
			sendbody = -1;
		else
			sendbody = 0;
	} else if (status < 200 || status == 204 || status == 304) {
		req->resp_len = -1;
		sendbody = 0;
	} else
		sendbody = 1;

	if (sendbody >= 0) {
		if (!req->disable_esi && req->resp_len != 0 &&
		    ObjHasAttr(wrk, req->objcore, OA_ESIDATA))
			VDP_push(req, VDP_ESI, NULL, 0, "ESI");

		if (cache_param->http_gzip_support &&
		    ObjCheckFlag(req->wrk, req->objcore, OF_GZIPED) &&
		    !RFC2616_Req_Gzip(req->http))
			VDP_push(req, VDP_gunzip, NULL, 1, "GUZ");

		if (cache_param->http_range_support &&
		    http_IsStatus(req->resp, 200)) {
			http_ForceHeader(req->resp, H_Accept_Ranges, "bytes");
			if (sendbody && http_GetHdr(req->http, H_Range, &r))
				VRG_dorange(req, r);
		}
	}

	if (sendbody < 0) {
		/* Don't touch pass+HEAD C-L */
		sendbody = 0;
	} else if (clval >= 0 && clval == req->resp_len) {
		/* Reuse C-L header */
	} else {
		http_Unset(req->resp, H_Content_Length);
		if (req->resp_len >= 0 && sendbody)
			http_PrintfHeader(req->resp,
			    "Content-Length: %jd", req->resp_len);
	}

	req->transport->deliver(req, boc, sendbody);

	VSLb_ts_req(req, "Resp", W_TIM_real(wrk));

	if (req->objcore->flags & (OC_F_PRIVATE | OC_F_PASS)) {
		if (boc != NULL) {
			HSH_Abandon(req->objcore);
			ObjWaitState(req->objcore, BOS_FINISHED);
		}
		ObjSlim(wrk, req->objcore);
	}

	if (boc != NULL)
		HSH_DerefBoc(wrk, req->objcore);

	(void)HSH_DerefObjCore(wrk, &req->objcore, HSH_RUSH_POLICY);
	http_Teardown(req->resp);

	return (REQ_FSM_DONE);
}

/*--------------------------------------------------------------------
 * Initiated a fetch (pass/miss) which we intend to deliver
 */

static enum req_fsm_nxt
cnt_fetch(struct worker *wrk, struct req *req)
{

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	CHECK_OBJ_NOTNULL(req->objcore, OBJCORE_MAGIC);

	wrk->stats->s_fetch++;
	(void)VRB_Ignore(req);

	if (req->objcore->flags & OC_F_FAILED) {
		req->err_code = 503;
		req->req_step = R_STP_SYNTH;
		(void)HSH_DerefObjCore(wrk, &req->objcore, 1);
		AZ(req->objcore);
		return (REQ_FSM_MORE);
	}

	req->req_step = R_STP_DELIVER;
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * Attempt to lookup objhdr from hash.  We disembark and reenter
 * this state if we get suspended on a busy objhdr.
 */

static enum req_fsm_nxt
cnt_lookup(struct worker *wrk, struct req *req)
{
	struct objcore *oc, *busy;
	enum lookup_e lr;
	int had_objhead = 0;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	AZ(req->objcore);

	AN(req->vcl);

	VRY_Prep(req);

	AZ(req->objcore);
	if (req->hash_objhead)
		had_objhead = 1;
	lr = HSH_Lookup(req, &oc, &busy, req->hash_always_miss ? 1 : 0);
	if (lr == HSH_BUSY) {
		/*
		 * We lost the session to a busy object, disembark the
		 * worker thread.   We return to STP_LOOKUP when the busy
		 * object has been unbusied, and still have the objhead
		 * around to restart the lookup with.
		 */
		return (REQ_FSM_DISEMBARK);
	}
	if (had_objhead)
		VSLb_ts_req(req, "Waitinglist", W_TIM_real(wrk));

	if (busy == NULL) {
		VRY_Finish(req, DISCARD);
	} else {
		AN(busy->flags & OC_F_BUSY);
		VRY_Finish(req, KEEP);
	}

	AZ(req->objcore);
	if (lr == HSH_MISS) {
		/* Found nothing */
		AZ(oc);
		if (busy != NULL) {
			AN(busy->flags & OC_F_BUSY);
			req->objcore = busy;
			req->req_step = R_STP_MISS;
		} else {
			req->req_step = R_STP_PASS;
		}
		return (REQ_FSM_MORE);
	}

	CHECK_OBJ_NOTNULL(oc, OBJCORE_MAGIC);
	AZ(oc->flags & OC_F_BUSY);
	req->objcore = oc;
	AZ(oc->flags & OC_F_PASS);

	VSLb(req->vsl, SLT_Hit, "%u", ObjGetXID(wrk, req->objcore));

	VCL_hit_method(req->vcl, wrk, req, NULL, NULL);

	switch (wrk->handling) {
	case VCL_RET_DELIVER:
		if (busy != NULL) {
			AZ(oc->flags & OC_F_PASS);
			CHECK_OBJ_NOTNULL(busy->boc, BOC_MAGIC);
			VBF_Fetch(wrk, req, busy, oc, VBF_BACKGROUND);
		} else {
			(void)VRB_Ignore(req);// XXX: handle err
		}
		wrk->stats->cache_hit++;
		req->is_hit = 1;
		req->req_step = R_STP_DELIVER;
		return (REQ_FSM_MORE);
	case VCL_RET_MISS:
		if (busy != NULL) {
			req->objcore = busy;
			req->stale_oc = oc;
			req->req_step = R_STP_MISS;
		} else {
			(void)HSH_DerefObjCore(wrk, &req->objcore,
			    HSH_RUSH_POLICY);
			/*
			 * We don't have a busy object, so treat this
			 * like a pass
			 */
			VSLb(req->vsl, SLT_VCL_Error,
			    "vcl_hit{} returns miss without busy object."
			    "  Doing pass.");
			req->req_step = R_STP_PASS;
		}
		return (REQ_FSM_MORE);
	case VCL_RET_RESTART:
		req->req_step = R_STP_RESTART;
		break;
	case VCL_RET_FAIL:
		req->req_step = R_STP_VCLFAIL;
		break;
	case VCL_RET_SYNTH:
		req->req_step = R_STP_SYNTH;
		break;
	case VCL_RET_PASS:
		wrk->stats->cache_hit++;
		req->is_hit = 1;
		req->req_step = R_STP_PASS;
		break;
	default:
		WRONG("Illegal return from vcl_hit{}");
	}

	/* Drop our object, we won't need it */
	(void)HSH_DerefObjCore(wrk, &req->objcore, HSH_RUSH_POLICY);

	if (busy != NULL) {
		(void)HSH_DerefObjCore(wrk, &busy, 0);
		VRY_Clear(req);
	}

	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * Cache miss.
 */

static enum req_fsm_nxt
cnt_miss(struct worker *wrk, struct req *req)
{

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	AN(req->vcl);
	CHECK_OBJ_NOTNULL(req->objcore, OBJCORE_MAGIC);

	VCL_miss_method(req->vcl, wrk, req, NULL, NULL);
	switch (wrk->handling) {
	case VCL_RET_FETCH:
		wrk->stats->cache_miss++;
		VBF_Fetch(wrk, req, req->objcore, req->stale_oc, VBF_NORMAL);
		if (req->stale_oc != NULL)
			(void)HSH_DerefObjCore(wrk, &req->stale_oc, 0);
		req->req_step = R_STP_FETCH;
		return (REQ_FSM_MORE);
	case VCL_RET_FAIL:
		req->req_step = R_STP_VCLFAIL;
		break;
	case VCL_RET_SYNTH:
		req->req_step = R_STP_SYNTH;
		break;
	case VCL_RET_RESTART:
		req->req_step = R_STP_RESTART;
		break;
	case VCL_RET_PASS:
		req->req_step = R_STP_PASS;
		break;
	default:
		WRONG("Illegal return from vcl_miss{}");
	}
	VRY_Clear(req);
	if (req->stale_oc != NULL)
		(void)HSH_DerefObjCore(wrk, &req->stale_oc, 0);
	AZ(HSH_DerefObjCore(wrk, &req->objcore, 1));
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * Pass processing
 */

static enum req_fsm_nxt
cnt_pass(struct worker *wrk, struct req *req)
{

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	AN(req->vcl);
	AZ(req->objcore);

	VCL_pass_method(req->vcl, wrk, req, NULL, NULL);
	switch (wrk->handling) {
	case VCL_RET_FAIL:
		req->req_step = R_STP_VCLFAIL;
		break;
	case VCL_RET_SYNTH:
		req->req_step = R_STP_SYNTH;
		break;
	case VCL_RET_RESTART:
		req->req_step = R_STP_RESTART;
		break;
	case VCL_RET_FETCH:
		wrk->stats->s_pass++;
		req->objcore = HSH_Private(wrk);
		CHECK_OBJ_NOTNULL(req->objcore, OBJCORE_MAGIC);
		VBF_Fetch(wrk, req, req->objcore, NULL, VBF_PASS);
		req->req_step = R_STP_FETCH;
		break;
	default:
		WRONG("Illegal return from cnt_pass{}");
	}
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * Pipe mode
 */

static enum req_fsm_nxt
cnt_pipe(struct worker *wrk, struct req *req)
{
	struct busyobj *bo;
	enum req_fsm_nxt nxt;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	AN(req->vcl);

	wrk->stats->s_pipe++;
	bo = VBO_GetBusyObj(wrk, req);
	CHECK_OBJ_NOTNULL(bo, BUSYOBJ_MAGIC);
	VSLb(bo->vsl, SLT_Begin, "bereq %u pipe", VXID(req->vsl->wid));
	VSLb(req->vsl, SLT_Link, "bereq %u pipe", VXID(bo->vsl->wid));
	THR_SetBusyobj(bo);

	HTTP_Setup(bo->bereq, bo->ws, bo->vsl, SLT_BereqMethod);
	http_FilterReq(bo->bereq, req->http, 0);	// XXX: 0 ?
	http_PrintfHeader(bo->bereq, "X-Varnish: %u", VXID(req->vsl->wid));
	http_SetHeader(bo->bereq, "Connection: close");

	if (req->want100cont) {
		http_SetHeader(bo->bereq, "Expect: 100-continue");
		req->want100cont = 0;
	}

	VCL_pipe_method(req->vcl, wrk, req, bo, NULL);

	switch (wrk->handling) {
	case VCL_RET_FAIL:
		req->req_step = R_STP_VCLFAIL;
		nxt = REQ_FSM_MORE;
		break;
	case VCL_RET_SYNTH:
		req->req_step = R_STP_SYNTH;
		nxt = REQ_FSM_MORE;
		break;
	case VCL_RET_PIPE:
		AZ(bo->req);
		bo->req = req;
		bo->wrk = wrk;
		SES_Close(req->sp, VDI_Http1Pipe(req, bo));
		nxt = REQ_FSM_DONE;
		break;
	default:
		WRONG("Illegal return from vcl_pipe{}");
	}
	http_Teardown(bo->bereq);
	VBO_ReleaseBusyObj(wrk, &bo);
	THR_SetBusyobj(NULL);
	return (nxt);
}

/*--------------------------------------------------------------------
 * Handle restart events
 */

static enum req_fsm_nxt
cnt_restart(struct worker *wrk, struct req *req)
{

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	req->director_hint = NULL;
	if (++req->restarts >= cache_param->max_restarts) {
		VSLb(req->vsl, SLT_VCL_Error, "Too many restarts");
		req->err_code = 503;
		req->req_step = R_STP_SYNTH;
	} else {
		// XXX: ReqEnd + ReqAcct ?
		VSLb_ts_req(req, "Restart", W_TIM_real(wrk));
		VSL_ChgId(req->vsl, "req", "restart",
		    VXID_Get(wrk, VSL_CLIENTMARKER));
		VSLb_ts_req(req, "Start", req->t_prev);
		req->err_code = 0;
		req->req_step = R_STP_RECV;
	}
	req->is_hit = 0;
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * We have a complete request, set everything up and start it.
 * We can come here both with a request from the client and with
 * a interior request during ESI delivery.
 */

static enum req_fsm_nxt
cnt_recv(struct worker *wrk, struct req *req)
{
	unsigned recv_handling;
	struct SHA256Context sha256ctx;
	const char *xff;
	const char *ci, *cp;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	AN(req->vcl);
	AZ(req->objcore);
	AZ(req->err_code);

	AZ(isnan(req->t_first));
	AZ(isnan(req->t_prev));
	AZ(isnan(req->t_req));

	ci = SES_Get_String_Attr(req->sp, SA_CLIENT_IP);
	cp = SES_Get_String_Attr(req->sp, SA_CLIENT_PORT);
	VSLb(req->vsl, SLT_ReqStart, "%s %s", ci, cp);

	http_VSL_log(req->http);

	if (req->restarts == 0) {
		/*
		 * This really should be done earlier, but we want to capture
		 * it in the VSL log.
		 */
		http_CollectHdr(req->http, H_X_Forwarded_For);
		if (http_GetHdr(req->http, H_X_Forwarded_For, &xff)) {
			http_Unset(req->http, H_X_Forwarded_For);
			http_PrintfHeader(req->http, "X-Forwarded-For: %s, %s",
			    xff, ci);
		} else {
			http_PrintfHeader(req->http, "X-Forwarded-For: %s", ci);
		}
	}

	/* By default we use the first backend */
	AZ(req->director_hint);
	req->director_hint = VCL_DefaultDirector(req->vcl);
	AN(req->director_hint);

	req->vdp_retval = 0;
	req->d_ttl = -1;
	req->disable_esi = 0;
	req->hash_always_miss = 0;
	req->hash_ignore_busy = 0;
	req->client_identity = NULL;
	req->storage = NULL;

	http_CollectHdr(req->http, H_Cache_Control);

	if (req->req_body_status == REQ_BODY_FAIL) {
		req->doclose = SC_OVERLOAD;
		return (REQ_FSM_DONE);
	}

	VCL_recv_method(req->vcl, wrk, req, NULL, NULL);
	if (wrk->handling == VCL_RET_VCL && req->restarts == 0) {
		req->director_hint = VCL_DefaultDirector(req->vcl);
		HTTP_Copy(req->http, req->http0);
		WS_Reset(req->ws, req->ws_req);
		AN(req->director_hint);
		VCL_recv_method(req->vcl, wrk, req, NULL, NULL);
	}

	if (req->want100cont && !req->late100cont) {
		req->want100cont = 0;
		if (req->transport->minimal_response(req, 100)) {
			req->doclose = SC_REM_CLOSE;
			return (REQ_FSM_DONE);
		}
	}

	/* Attempts to cache req.body may fail */
	if (req->req_body_status == REQ_BODY_FAIL) {
		req->doclose = SC_RX_BODY;
		return (REQ_FSM_DONE);
	}

	recv_handling = wrk->handling;

	/* We wash the A-E header here for the sake of VRY */
	if (cache_param->http_gzip_support &&
	     (recv_handling != VCL_RET_PIPE) &&
	     (recv_handling != VCL_RET_PASS)) {
		if (RFC2616_Req_Gzip(req->http)) {
			http_ForceHeader(req->http, H_Accept_Encoding, "gzip");
		} else {
			http_Unset(req->http, H_Accept_Encoding);
		}
	}

	SHA256_Init(&sha256ctx);
	VCL_hash_method(req->vcl, wrk, req, NULL, &sha256ctx);
	if (wrk->handling == VCL_RET_FAIL)
		recv_handling = wrk->handling;
	else
		assert(wrk->handling == VCL_RET_LOOKUP);
	SHA256_Final(req->digest, &sha256ctx);

	switch (recv_handling) {
	case VCL_RET_VCL:
		VSLb(req->vsl, SLT_VCL_Error,
		    "Illegal return(vcl): %s",
		    req->restarts ? "Not after restarts" :
		    "Only from active VCL");
		req->err_code = 503;
		req->req_step = R_STP_SYNTH;
		return (REQ_FSM_MORE);
	case VCL_RET_PURGE:
		req->req_step = R_STP_PURGE;
		return (REQ_FSM_MORE);
	case VCL_RET_HASH:
		req->req_step = R_STP_LOOKUP;
		return (REQ_FSM_MORE);
	case VCL_RET_PIPE:
		if (req->esi_level > 0) {
			VSLb(req->vsl, SLT_VCL_Error,
			    "vcl_recv{} returns pipe for ESI included object."
			    "  Doing pass.");
			req->req_step = R_STP_PASS;
		} else if (req->http0->protover > 11) {
			VSLb(req->vsl, SLT_VCL_Error,
			    "vcl_recv{} returns pipe for HTTP/2 request."
			    "  Doing pass.");
			req->req_step = R_STP_PASS;
		} else {
			req->req_step = R_STP_PIPE;
		}
		return (REQ_FSM_MORE);
	case VCL_RET_PASS:
		req->req_step = R_STP_PASS;
		return (REQ_FSM_MORE);
	case VCL_RET_SYNTH:
		req->req_step = R_STP_SYNTH;
		return (REQ_FSM_MORE);
	case VCL_RET_FAIL:
		req->req_step = R_STP_VCLFAIL;
		return (REQ_FSM_MORE);
	default:
		WRONG("Illegal return from vcl_recv{}");
	}
}

/*--------------------------------------------------------------------
 * Find the objhead, purge it.
 *
 * XXX: We should ask VCL if we should fetch a new copy of the object.
 */

static enum req_fsm_nxt
cnt_purge(struct worker *wrk, struct req *req)
{
	struct objcore *oc, *boc;
	enum lookup_e lr;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);
	AZ(req->objcore);

	AN(req->vcl);

	VRY_Prep(req);

	AZ(req->objcore);
	lr = HSH_Lookup(req, &oc, &boc, 1);
	assert (lr == HSH_MISS);
	AZ(oc);
	CHECK_OBJ_NOTNULL(boc, OBJCORE_MAGIC);
	VRY_Finish(req, DISCARD);

	HSH_Purge(wrk, boc->objhead, 0, 0, 0);

	AZ(HSH_DerefObjCore(wrk, &boc, 1));

	VCL_purge_method(req->vcl, wrk, req, NULL, NULL);
	switch (wrk->handling) {
	case VCL_RET_RESTART:
		req->req_step = R_STP_RESTART;
		break;
	case VCL_RET_FAIL:
		req->req_step = R_STP_VCLFAIL;
		break;
	case VCL_RET_SYNTH:
		req->req_step = R_STP_SYNTH;
		break;
	default:
		WRONG("Illegal return from vcl_purge{}");
	}
	return (REQ_FSM_MORE);
}

/*--------------------------------------------------------------------
 * Central state engine dispatcher.
 *
 * Kick the session around until it has had enough.
 *
 */

static void
cnt_diag(struct req *req, const char *state)
{

	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	VSLb(req->vsl,  SLT_Debug, "vxid %u STP_%s sp %p vcl %p",
	    req->vsl->wid, state, req->sp, req->vcl);
	VSL_Flush(req->vsl, 0);
}

enum req_fsm_nxt
CNT_Request(struct worker *wrk, struct req *req)
{
	enum req_fsm_nxt nxt;

	CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
	CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

	/*
	 * Possible entrance states
	 */
	assert(
	    req->req_step == R_STP_LOOKUP ||
	    req->req_step == R_STP_TRANSPORT ||
	    req->req_step == R_STP_RECV);

	AN(req->vsl->wid & VSL_CLIENTMARKER);

	req->vfc->wrk = req->wrk = wrk;
	wrk->vsl = req->vsl;
	for (nxt = REQ_FSM_MORE; nxt == REQ_FSM_MORE; ) {
		/*
		 * This is a good place to be paranoid about the various
		 * pointers still pointing to the things we expect.
		 */
		CHECK_OBJ_NOTNULL(wrk, WORKER_MAGIC);
		CHECK_OBJ_ORNULL(wrk->nobjhead, OBJHEAD_MAGIC);
		CHECK_OBJ_NOTNULL(req, REQ_MAGIC);

		/*
		 * We don't want the thread workspace to be used for
		 * anything of long duration, so mandate that it be
		 * empty on state-transitions.
		 */
		WS_Assert(wrk->aws);
		AZ(WS_Snapshot(wrk->aws));

		switch (req->req_step) {
#define REQ_STEP(l,u,arg) \
		    case R_STP_##u: \
			if (DO_DEBUG(DBG_REQ_STATE)) \
				cnt_diag(req, #u); \
			nxt = cnt_##l arg; \
			break;
#include "tbl/steps.h"
		default:
			WRONG("State engine misfire");
		}
		WS_Assert(wrk->aws);
		CHECK_OBJ_ORNULL(wrk->nobjhead, OBJHEAD_MAGIC);
	}
	wrk->vsl = NULL;
	if (nxt == REQ_FSM_DONE) {
		AN(req->vsl->wid);
		VRB_Free(req);
		req->wrk = NULL;
	}
	assert(nxt == REQ_FSM_DISEMBARK || req->ws->r == NULL);
	return (nxt);
}
