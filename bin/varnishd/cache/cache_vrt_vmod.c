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

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "vcli_serve.h"
#include "vrt.h"

/*--------------------------------------------------------------------
 * Modules stuff
 */

struct vmod {
	unsigned		magic;
#define VMOD_MAGIC		0xb750219c

	VTAILQ_ENTRY(vmod)	list;

	int			ref;

	char			*nm;
	char			*path;
	char			*backup;
	void			*hdl;
	const void		*funcs;
	int			funclen;
	const char		*abi;
	unsigned		vrt_major;
	unsigned		vrt_minor;
};

static VTAILQ_HEAD(,vmod)	vmods = VTAILQ_HEAD_INITIALIZER(vmods);


int
VRT_Vmod_Init(VRT_CTX, struct vmod **hdl, void *ptr, int len, const char *nm,
    const char *path, const char *file_id, const char *backup)
{
	struct vmod *v;
	const struct vmod_data *d;
	char buf[256];
	void *dlhdl;

	ASSERT_CLI();
	CHECK_OBJ_NOTNULL(ctx, VRT_CTX_MAGIC);
	AN(ctx->msg);
	AN(hdl);
	AZ(*hdl);

	dlhdl = dlopen(backup, RTLD_NOW | RTLD_LOCAL);
	if (dlhdl == NULL) {
		VSB_printf(ctx->msg, "Loading vmod %s from %s:\n", nm, backup);
		VSB_printf(ctx->msg, "dlopen() failed: %s\n", dlerror());
		return (1);
	}

	VTAILQ_FOREACH(v, &vmods, list)
		if (v->hdl == dlhdl)
			break;
	if (v == NULL) {
		ALLOC_OBJ(v, VMOD_MAGIC);
		AN(v);
		REPLACE(v->backup, backup);

		v->hdl = dlhdl;

		bprintf(buf, "Vmod_%s_Data", nm);
		d = dlsym(v->hdl, buf);
		if (d == NULL ||
		    d->file_id == NULL ||
		    strcmp(d->file_id, file_id)) {
			VSB_printf(ctx->msg,
			    "Loading vmod %s from %s:\n", nm, path);
			VSB_printf(ctx->msg,
			    "This is no longer the same file seen by"
			    " the VCL-compiler.\n");
			(void)dlclose(v->hdl);
			FREE_OBJ(v);
			return (1);
		}
		if (d->vrt_major != VRT_MAJOR_VERSION ||
		    d->vrt_minor > VRT_MINOR_VERSION ||
		    d->name == NULL ||
		    strcmp(d->name, nm) ||
		    d->func == NULL ||
		    d->func_len <= 0 ||
		    d->proto == NULL ||
		    d->spec == NULL ||
		    d->abi == NULL) {
			VSB_printf(ctx->msg,
			    "Loading VMOD %s from %s:\n", nm, path);
			VSB_printf(ctx->msg, "VMOD data is mangled.\n");
			(void)dlclose(v->hdl);
			FREE_OBJ(v);
			return (1);
		}

		v->funclen = d->func_len;
		v->funcs = d->func;
		v->abi = d->abi;
		v->vrt_major = d->vrt_major;
		v->vrt_minor = d->vrt_minor;

		REPLACE(v->nm, nm);
		REPLACE(v->path, path);

		VSC_C_main->vmods++;
		VTAILQ_INSERT_TAIL(&vmods, v, list);
	}

	assert(len == v->funclen);
	memcpy(ptr, v->funcs, v->funclen);
	v->ref++;

	*hdl = v;
	return (0);
}

void
VRT_Vmod_Fini(struct vmod **hdl)
{
	struct vmod *v;

	ASSERT_CLI();

	TAKE_OBJ_NOTNULL(v, hdl, VMOD_MAGIC);

#ifndef DONT_DLCLOSE_VMODS
	/*
	 * atexit(3) handlers are not called during dlclose(3).  We don't
	 * normally use them, but we do when running GCOV.  This option
	 * enables us to do that.
	 */
	AZ(dlclose(v->hdl));
#endif
	if (--v->ref != 0)
		return;
	free(v->nm);
	free(v->path);
	free(v->backup);
	VTAILQ_REMOVE(&vmods, v, list);
	VSC_C_main->vmods--;
	FREE_OBJ(v);
}

void
VMOD_Panic(struct vsb *vsb)
{
	struct vmod *v;

	VSB_printf(vsb, "vmods = {\n");
	VSB_indent(vsb, 2);
	VTAILQ_FOREACH(v, &vmods, list)
		VSB_printf(vsb, "%s = {%s, %u.%u},\n",
		    v->nm, v->abi, v->vrt_major, v->vrt_minor);
	VSB_indent(vsb, -2);
	VSB_printf(vsb, "},\n");
}

/*---------------------------------------------------------------------*/

static void
ccf_debug_vmod(struct cli *cli, const char * const *av, void *priv)
{
	struct vmod *v;

	(void)av;
	(void)priv;
	ASSERT_CLI();
	VTAILQ_FOREACH(v, &vmods, list)
		VCLI_Out(cli, "%5d %s (%s)\n", v->ref, v->nm, v->path);
}

static struct cli_proto vcl_cmds[] = {
	{ CLICMD_DEBUG_VMOD,			"d", ccf_debug_vmod },
	{ NULL }
};

void
VMOD_Init(void)
{

	CLI_AddFuncs(vcl_cmds);
}
