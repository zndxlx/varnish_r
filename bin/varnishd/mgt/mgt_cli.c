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
 * The management process' CLI handling
 */

#include "config.h"

#include <sys/types.h>
#include <sys/socket.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mgt/mgt.h"

#include "vcli_serve.h"
#include "vev.h"
#include "vrnd.h"
#include "vsa.h"
#include "vss.h"
#include "vtcp.h"

#define CLI_CMD(U,l,s,h,d,m,M) \
const struct cli_cmd_desc CLICMD_##U[1] = {{ l, s, h, d, m, M }};
#include "tbl/cli_cmds.h"

static const struct cli_cmd_desc *cmds[] = {
#define CLI_CMD(U,l,s,h,d,m,M) CLICMD_##U,
#include "tbl/cli_cmds.h"
};

static const int ncmds = sizeof cmds / sizeof cmds[0];

static int		cli_i = -1, cli_o = -1;
struct VCLS		*mgt_cls;
static const char	*secret_file;

/*--------------------------------------------------------------------*/

static void
mcf_banner(struct cli *cli, const char *const *av, void *priv)
{

	(void)av;
	(void)priv;
	VCLI_Out(cli, "-----------------------------\n");
	VCLI_Out(cli, "Varnish Cache CLI 1.0\n");
	VCLI_Out(cli, "-----------------------------\n");
	VCLI_Out(cli, "%s\n", VSB_data(vident) + 1);
	VCLI_Out(cli, "%s\n", VCS_version);
	VCLI_Out(cli, "\n");
	VCLI_Out(cli, "Type 'help' for command list.\n");
	VCLI_Out(cli, "Type 'quit' to close CLI session.\n");
	if (!MCH_Running())
		VCLI_Out(cli, "Type 'start' to launch worker process.\n");
	VCLI_SetResult(cli, CLIS_OK);
}

/*--------------------------------------------------------------------*/

static struct cli_proto cli_proto[] = {
	{ CLICMD_BANNER,		"", mcf_banner },
	{ NULL }
};

/*--------------------------------------------------------------------*/

static void
mcf_panic(struct cli *cli, const char * const *av, void *priv)
{

	(void)cli;
	(void)av;
	(void)priv;
	AZ(strcmp("", "You asked for it"));
}

static struct cli_proto cli_debug[] = {
	{ CLICMD_DEBUG_PANIC_MASTER,		"d", mcf_panic },
	{ NULL }
};

/*--------------------------------------------------------------------*/

static void
mcf_askchild(struct cli *cli, const char * const *av, void *priv)
{
	int i;
	char *q;
	unsigned u;
	struct vsb *vsb;

	(void)priv;
	/*
	 * Command not recognized in master, try cacher if it is
	 * running.
	 */
	if (cli_o <= 0) {
		VCLI_SetResult(cli, CLIS_UNKNOWN);
		VCLI_Out(cli,
		    "Unknown request in manager process "
		    "(child not running).\n"
		    "Type 'help' for more info.");
		return;
	}
	vsb = VSB_new_auto();
	for (i = 1; av[i] != NULL; i++) {
		VSB_quote(vsb, av[i], strlen(av[i]), 0);
		VSB_putc(vsb, ' ');
	}
	VSB_putc(vsb, '\n');
	AZ(VSB_finish(vsb));
	i = write(cli_o, VSB_data(vsb), VSB_len(vsb));
	if (i != VSB_len(vsb)) {
		VSB_destroy(&vsb);
		VCLI_SetResult(cli, CLIS_COMMS);
		VCLI_Out(cli, "CLI communication error");
		MCH_Cli_Fail();
		return;
	}
	VSB_destroy(&vsb);
	if (VCLI_ReadResult(cli_i, &u, &q, mgt_param.cli_timeout))
		MCH_Cli_Fail();
	VCLI_SetResult(cli, u);
	VCLI_Out(cli, "%s", q);
	free(q);
}

static const struct cli_cmd_desc CLICMD_WILDCARD[1] =
    {{ "*", "<wild-card-entry>", "<fall through to cacher>", "", 0, -1 }};

static struct cli_proto cli_askchild[] = {
	{ CLICMD_WILDCARD, "h*", mcf_askchild },
	{ NULL }
};

/*--------------------------------------------------------------------
 * Ask the child something over CLI, return zero only if everything is
 * happy happy.
 */

int
mgt_cli_askchild(unsigned *status, char **resp, const char *fmt, ...)
{
	int i, j;
	va_list ap;
	unsigned u;
	char buf[mgt_param.cli_buffer], *p;

	if (resp != NULL)
		*resp = NULL;
	if (status != NULL)
		*status = 0;
	if (cli_i < 0|| cli_o < 0) {
		if (status != NULL)
			*status = CLIS_CANT;
		return (CLIS_CANT);
	}
	va_start(ap, fmt);
	vbprintf(buf, fmt, ap);
	va_end(ap);
	p = strchr(buf, '\0');
	assert(p != NULL && p > buf && p[-1] == '\n');
	i = p - buf;
	j = write(cli_o, buf, i);
	if (j != i) {
		if (status != NULL)
			*status = CLIS_COMMS;
		if (resp != NULL)
			*resp = strdup("CLI communication error");
		MCH_Cli_Fail();
		return (CLIS_COMMS);
	}

	if (VCLI_ReadResult(cli_i, &u, resp, mgt_param.cli_timeout))
		MCH_Cli_Fail();
	if (status != NULL)
		*status = u;
	return (u == CLIS_OK ? 0 : u);
}

/*--------------------------------------------------------------------*/

void
mgt_cli_start_child(int fdi, int fdo)
{

	cli_i = fdi;
	cli_o = fdo;
}

/*--------------------------------------------------------------------*/

void
mgt_cli_stop_child(void)
{

	cli_i = -1;
	cli_o = -1;
	/* XXX: kick any users */
}

/*--------------------------------------------------------------------
 * Generate a random challenge
 */

static void
mgt_cli_challenge(struct cli *cli)
{
	int i;
	uint8_t u;

	AZ(VRND_RandomCrypto(cli->challenge, sizeof cli->challenge - 2));
	for (i = 0; i < (sizeof cli->challenge) - 2; i++) {
		AZ(VRND_RandomCrypto(&u, sizeof u));
		cli->challenge[i] = (u % 26) + 'a';
	}
	cli->challenge[i++] = '\n';
	cli->challenge[i] = '\0';
	VCLI_Out(cli, "%s", cli->challenge);
	VCLI_Out(cli, "\nAuthentication required.\n");
	VCLI_SetResult(cli, CLIS_AUTH);
}

/*--------------------------------------------------------------------
 * Validate the authentication
 */

static void
mcf_auth(struct cli *cli, const char *const *av, void *priv)
{
	int fd;
	char buf[CLI_AUTH_RESPONSE_LEN + 1];

	AN(av[2]);
	(void)priv;
	if (secret_file == NULL) {
		VCLI_Out(cli, "Secret file not configured\n");
		VCLI_SetResult(cli, CLIS_CANT);
		return;
	}
	VJ_master(JAIL_MASTER_FILE);
	fd = open(secret_file, O_RDONLY);
	if (fd < 0) {
		VCLI_Out(cli, "Cannot open secret file (%s)\n",
		    strerror(errno));
		VCLI_SetResult(cli, CLIS_CANT);
		VJ_master(JAIL_MASTER_LOW);
		return;
	}
	VJ_master(JAIL_MASTER_LOW);
	MCH_TrackHighFd(fd);
	VCLI_AuthResponse(fd, cli->challenge, buf);
	closefd(&fd);
	if (strcasecmp(buf, av[2])) {
		MGT_Complain(C_SECURITY,
		    "CLI Authentication failure from %s", cli->ident);
		VCLI_SetResult(cli, CLIS_CLOSE);
		return;
	}
	cli->auth = MCF_AUTH;
	memset(cli->challenge, 0, sizeof cli->challenge);
	VCLI_SetResult(cli, CLIS_OK);
	mcf_banner(cli, av, priv);
}

/*--------------------------------------------------------------------*/

static void __match_proto__(cli_func_t)
mcf_help(struct cli *cli, const char * const *av, void *priv)
{
	if (cli_o <= 0)
		VCLS_func_help(cli, av, priv);
	else
		mcf_askchild(cli, av, priv);
}

static void __match_proto__(cli_func_t)
mcf_help_json(struct cli *cli, const char * const *av, void *priv)
{
	if (cli_o <= 0)
		VCLS_func_help_json(cli, av, priv);
	else
		mcf_askchild(cli, av, priv);
}

static struct cli_proto cli_auth[] = {
	{ CLICMD_HELP,		"", mcf_help, mcf_help_json },
	{ CLICMD_PING,		"", VCLS_func_ping },
	{ CLICMD_AUTH,		"", mcf_auth },
	{ CLICMD_QUIT,		"", VCLS_func_close },
	{ NULL }
};

/*--------------------------------------------------------------------*/

static void
mgt_cli_cb_before(const struct cli *cli)
{

	if (cli->priv == stderr)
		fprintf(stderr, "> %s\n", cli->cmd);
	MGT_Complain(C_CLI, "CLI %s Rd %s", cli->ident, cli->cmd);
}

static void
mgt_cli_cb_after(const struct cli *cli)
{

	MGT_Complain(C_CLI, "CLI %s Wr %03u %s",
	    cli->ident, cli->result, VSB_data(cli->sb));
	if (cli->priv == stderr &&
	    cli->result != CLIS_OK && cli->cmd[0] != '-') {
		MGT_Complain(C_ERR, "-I file CLI command failed (%d)\n%s\n",
		    cli->result, VSB_data(cli->sb));
		exit(2);
	}
}

/*--------------------------------------------------------------------*/

void
mgt_cli_init_cls(void)
{

	mgt_cls = VCLS_New(mgt_cli_cb_before, mgt_cli_cb_after,
	    &mgt_param.cli_buffer, &mgt_param.cli_limit);
	AN(mgt_cls);
	VCLS_AddFunc(mgt_cls, MCF_NOAUTH, cli_auth);
	VCLS_AddFunc(mgt_cls, MCF_AUTH, cli_proto);
	VCLS_AddFunc(mgt_cls, MCF_AUTH, cli_debug);
	VCLS_AddFunc(mgt_cls, MCF_AUTH, cli_askchild);
}

/*--------------------------------------------------------------------
 * Get rid of all CLI sessions
 */

void
mgt_cli_close_all(void)
{

	VCLS_Destroy(&mgt_cls);
}

/*--------------------------------------------------------------------
 * Callback whenever something happens to the input fd of the session.
 */

static int
mgt_cli_callback2(const struct vev *e, int what)
{
	int i;

	(void)e;
	(void)what;
	i = VCLS_PollFd(mgt_cls, e->fd, 0);
	return (i);
}

/*--------------------------------------------------------------------*/

void
mgt_cli_setup(int fdi, int fdo, int auth, const char *ident,
    mgt_cli_close_f *closefunc, void *priv)
{
	struct cli *cli;
	struct vev *ev;

	cli = VCLS_AddFd(mgt_cls, fdi, fdo, closefunc, priv);

	REPLACE(cli->ident, ident);

	if (!auth && secret_file != NULL) {
		cli->auth = MCF_NOAUTH;
		mgt_cli_challenge(cli);
	} else {
		cli->auth = MCF_AUTH;
		mcf_banner(cli, NULL, NULL);
	}
	AZ(VSB_finish(cli->sb));
	(void)VCLI_WriteResult(fdo, cli->result, VSB_data(cli->sb));

	ev = vev_new();
	AN(ev);
	ev->name = cli->ident;
	ev->fd = fdi;
	ev->fd_flags = EV_RD;
	ev->callback = mgt_cli_callback2;
	ev->priv = cli;
	AZ(vev_add(mgt_evb, ev));
}

/*--------------------------------------------------------------------*/

static struct vsb *
sock_id(const char *pfx, int fd)
{
	struct vsb *vsb;

	char abuf1[VTCP_ADDRBUFSIZE], abuf2[VTCP_ADDRBUFSIZE];
	char pbuf1[VTCP_PORTBUFSIZE], pbuf2[VTCP_PORTBUFSIZE];

	vsb = VSB_new_auto();
	AN(vsb);
	VTCP_myname(fd, abuf1, sizeof abuf1, pbuf1, sizeof pbuf1);
	VTCP_hisname(fd, abuf2, sizeof abuf2, pbuf2, sizeof pbuf2);
	VSB_printf(vsb, "%s %s %s %s %s", pfx, abuf2, pbuf2, abuf1, pbuf1);
	AZ(VSB_finish(vsb));
	return (vsb);
}

/*--------------------------------------------------------------------*/

struct telnet {
	unsigned		magic;
#define TELNET_MAGIC		0x53ec3ac0
	int			fd;
	struct vev		*ev;
};

static void
telnet_close(void *priv)
{
	struct telnet *tn;

	CAST_OBJ_NOTNULL(tn, priv, TELNET_MAGIC);
	(void)close(tn->fd);
	FREE_OBJ(tn);
}

static struct telnet *
telnet_new(int fd)
{
	struct telnet *tn;

	ALLOC_OBJ(tn, TELNET_MAGIC);
	AN(tn);
	tn->fd = fd;
	return (tn);
}

static int
telnet_accept(const struct vev *ev, int what)
{
	struct vsb *vsb;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	struct telnet *tn;
	int i;

	(void)what;
	addrlen = sizeof addr;
	i = accept(ev->fd, (void *)&addr, &addrlen);
	if (i < 0 && errno == EBADF)
		return (1);
	if (i < 0)
		return (0);

	MCH_TrackHighFd(i);
	tn = telnet_new(i);
	vsb = sock_id("telnet", i);
	mgt_cli_setup(i, i, 0, VSB_data(vsb), telnet_close, tn);
	VSB_destroy(&vsb);
	return (0);
}

void
mgt_cli_secret(const char *S_arg)
{
	int i, fd;
	char buf[BUFSIZ];

	/* Save in shmem */
	mgt_SHM_static_alloc(S_arg, strlen(S_arg) + 1L, "Arg", "-S", "");

	VJ_master(JAIL_MASTER_FILE);
	fd = open(S_arg, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "Can not open secret-file \"%s\"\n", S_arg);
		exit(2);
	}
	VJ_master(JAIL_MASTER_LOW);
	MCH_TrackHighFd(fd);
	i = read(fd, buf, sizeof buf);
	if (i == 0) {
		fprintf(stderr, "Empty secret-file \"%s\"\n", S_arg);
		exit(2);
	}
	if (i < 0) {
		fprintf(stderr, "Can not read secret-file \"%s\"\n", S_arg);
		exit(2);
	}
	closefd(&fd);
	secret_file = S_arg;
}

static int __match_proto__(vss_resolved_f)
mct_callback(void *priv, const struct suckaddr *sa)
{
	int sock;
	struct vsb *vsb = priv;
	const char *err;
	char abuf[VTCP_ADDRBUFSIZE];
	char pbuf[VTCP_PORTBUFSIZE];
	struct telnet *tn;

	VJ_master(JAIL_MASTER_PRIVPORT);
	sock = VTCP_listen(sa, 10, &err);
	VJ_master(JAIL_MASTER_LOW);
	assert(sock != 0);		// We know where stdin is
	if (sock > 0) {
		VTCP_myname(sock, abuf, sizeof abuf, pbuf, sizeof pbuf);
		VSB_printf(vsb, "%s %s\n", abuf, pbuf);
		tn = telnet_new(sock);
		tn->ev = vev_new();
		AN(tn->ev);
		tn->ev->fd = sock;
		tn->ev->fd_flags = POLLIN;
		tn->ev->callback = telnet_accept;
		tn->ev->priv = tn;
		AZ(vev_add(mgt_evb, tn->ev));
	}
	return (0);
}

void
mgt_cli_telnet(const char *T_arg)
{
	int error;
	const char *err;
	struct vsb *vsb;

	AN(T_arg);
	vsb = VSB_new_auto();
	AN(vsb);
	error = VSS_resolver(T_arg, NULL, mct_callback, vsb, &err);
	if (err != NULL)
		ARGV_ERR("Could not resolve -T argument to address\n\t%s\n", err);
	AZ(error);
	AZ(VSB_finish(vsb));
	if (VSB_len(vsb) == 0)
		ARGV_ERR("-T %s could not be listened on.\n", T_arg);
	/* Save in shmem */
	mgt_SHM_static_alloc(VSB_data(vsb), VSB_len(vsb) + 1, "Arg", "-T", "");
	VSB_destroy(&vsb);
}

/* Reverse CLI ("Master") connections --------------------------------*/

struct m_addr {
	unsigned		magic;
#define M_ADDR_MAGIC		0xbc6217ed
	struct suckaddr		*sa;
	VTAILQ_ENTRY(m_addr)	list;
};

static int M_fd = -1;
static struct vev *M_poker, *M_conn;
static double M_poll = 0.1;

static VTAILQ_HEAD(,m_addr)	m_addr_list =
    VTAILQ_HEAD_INITIALIZER(m_addr_list);

static void
Marg_closer(void *priv)
{

	(void)priv;
	(void)close(M_fd);
	M_fd = -1;
}

static int __match_proto__(vev_cb_f)
Marg_connect(const struct vev *e, int what)
{
	struct vsb *vsb;
	struct m_addr *ma;

	assert(e == M_conn);
	(void)what;

	M_fd = VTCP_connected(M_fd);
	if (M_fd < 0) {
		MGT_Complain(C_INFO, "Could not connect to CLI-master: %m");
		ma = VTAILQ_FIRST(&m_addr_list);
		AN(ma);
		VTAILQ_REMOVE(&m_addr_list, ma, list);
		VTAILQ_INSERT_TAIL(&m_addr_list, ma, list);
		if (M_poll < 10)
			M_poll++;
		return (1);
	}
	vsb = sock_id("master", M_fd);
	mgt_cli_setup(M_fd, M_fd, 0, VSB_data(vsb), Marg_closer, NULL);
	VSB_destroy(&vsb);
	M_poll = 1;
	return (1);
}

static int __match_proto__(vev_cb_f)
Marg_poker(const struct vev *e, int what)
{
	int s;
	struct m_addr *ma;

	assert(e == M_poker);
	(void)what;

	M_poker->timeout = M_poll;	/* XXX nasty ? */
	if (M_fd > 0)
		return (0);

	ma = VTAILQ_FIRST(&m_addr_list);
	AN(ma);

	/* Try to connect asynchronously */
	s = VTCP_connect(ma->sa, -1);
	if (s < 0)
		return (0);

	MCH_TrackHighFd(s);

	M_conn = vev_new();
	AN(M_conn);
	M_conn->callback = Marg_connect;
	M_conn->name = "-M connector";
	M_conn->fd_flags = EV_WR;
	M_conn->fd = s;
	M_fd = s;
	AZ(vev_add(mgt_evb, M_conn));
	return (0);
}

static int __match_proto__(vss_resolved_f)
marg_cb(void *priv, const struct suckaddr *sa)
{
	struct m_addr *ma;

	(void)priv;
	ALLOC_OBJ(ma, M_ADDR_MAGIC);
	AN(ma);
	ma->sa = VSA_Clone(sa);
	VTAILQ_INSERT_TAIL(&m_addr_list, ma, list);
	return(0);
}

void
mgt_cli_master(const char *M_arg)
{
	const char *err;
	int error;

	AN(M_arg);

	error = VSS_resolver(M_arg, NULL, marg_cb, NULL, &err);
	if (err != NULL)
		ARGV_ERR("Could not resolve -M argument to address\n\t%s\n", err);
	AZ(error);
	if (VTAILQ_EMPTY(&m_addr_list))
		ARGV_ERR("Could not resolve -M argument to address\n");
	AZ(M_poker);
	M_poker = vev_new();
	AN(M_poker);
	M_poker->timeout = M_poll;
	M_poker->callback = Marg_poker;
	M_poker->name = "-M poker";
	AZ(vev_add(mgt_evb, M_poker));
}

static int
cli_cmp(const void *a, const void *b)
{
	struct cli_cmd_desc * const * const aa = a;
	struct cli_cmd_desc * const * const bb = b;

	return (strcmp((*aa)->request, (*bb)->request));
}

void
mgt_DumpRstCli(void)
{
	const struct cli_cmd_desc *cp;
	int i, j;

	qsort(cmds, ncmds, sizeof cmds[0], cli_cmp);
	for (i = 0; i < ncmds; i++, cp++) {
		cp = cmds[i];
		if (!strncmp(cp->request, "debug.", 6))
			continue;
		printf(".. _ref_cli_%s:\n\n", cp->syntax);
		printf("%s\n", cp->syntax);
		for (j = 0; j < strlen(cp->syntax); j++)
			printf("~");
		printf("\n");
		printf("  %s\n", cp->help);
		if (*cp->doc != '\0')
			printf("\n%s\n", cp->doc);

		printf("\n");
	}
}
