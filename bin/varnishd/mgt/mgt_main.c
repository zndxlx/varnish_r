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
 * The management process and CLI handling
 */

#include "config.h"

#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/stat.h>

#include "mgt/mgt.h"
#include "common/heritage.h"

#include "hash/hash_slinger.h"
#include "vav.h"
#include "vcli_serve.h"
#include "vev.h"
#include "vfil.h"
#include "vin.h"
#include "vpf.h"
#include "vrnd.h"
#include "vsb.h"
#include "vsha256.h"
#include "vsub.h"
#include "vtim.h"
#include "waiter/mgt_waiter.h"

struct heritage		heritage;
unsigned		d_flag = 0;
pid_t			mgt_pid;
struct vev_base		*mgt_evb;
int			exit_status = 0;
struct vsb		*vident;
struct VSC_C_mgt	static_VSC_C_mgt;
struct VSC_C_mgt	*VSC_C_mgt;
static int		I_fd = -1;
static char		Cn_arg[] = "/tmp/varnishd_C_XXXXXXX";

static struct vpf_fh *pfh = NULL;

int optreset;	// Some has it, some doesn't.  Cheaper than auto*

/*--------------------------------------------------------------------*/

static void
usage(void)
{
#define FMT "  %-28s # %s\n"

	printf( "Usage: varnishd [options]\n");

	printf("\nBasic options:\n");

	printf(FMT, "-a address[:port][,proto]",
	    "HTTP listen address and port");
	printf(FMT, "", "Can be specified multiple times.");
	printf(FMT, "", "  default: \":80,HTTP/1\"");
	printf(FMT, "-b address[:port]", "Backend address and port");
	printf(FMT, "", "  default: \":80\"");
	printf(FMT, "-f vclfile", "VCL program");
	printf(FMT, "", "Can be specified multiple times.");
	printf(FMT, "-n dir", "Working directory");

	printf("\n-b can be used only once, and not together with -f\n");

	printf("\nDocumentation options:\n");
	printf(FMT, "-?", "Prints this usage message");
	printf(FMT, "-x parameter", "Parameter documentation");
	printf(FMT, "-x vsl", "VSL record documentation");
	printf(FMT, "-x cli", "CLI command documentation");
	printf(FMT, "-x builtin", "Builtin VCL program");

	printf("\nOperations options:\n");

	printf(FMT, "-F", "Run in foreground");
	printf(FMT, "-T address[:port]", "CLI address");
	printf(FMT, "", "Can be specified multiple times.");
	printf(FMT, "-M address:port", "Reverse CLI destination");
	printf(FMT, "", "Can be specified multiple times.");
	printf(FMT, "-P file", "PID file");
	printf(FMT, "-i identity", "Identity of varnish instance");
	printf(FMT, "-I clifile", "Initialization CLI commands");

	printf("\nTuning options:\n");

	printf(FMT, "-t TTL", "Default TTL");
	printf(FMT, "-p param=value", "set parameter");
	printf(FMT, "", "Can be specified multiple times.");

	printf(FMT, "-s [name=]kind[,options]", "Storage specification");
	printf(FMT, "", "Can be specified multiple times.");
	printf(FMT, "", "  -s malloc");
	printf(FMT, "", "  -s file");

	printf(FMT, "-l vsl[,vsm]", "Size of shared memory file");
	printf(FMT, "", "  vsl: space for VSL records [80m]");
	printf(FMT, "", "  vsm: space for stats counters [1m]");

	printf("\nSecurity options:\n");

	printf(FMT, "-r param[,param...]", "Set parameters read-only from CLI");
	printf(FMT, "", "Can be specified multiple times.");
	printf(FMT, "-S secret-file", "Secret file for CLI authentication");
	printf(FMT, "-j jail[,options]", "Jail specification");
#ifdef HAVE_SETPPRIV
	printf(FMT, "", "  -j solaris");
#endif
	printf(FMT, "", "  -j unix");
	printf(FMT, "", "  -j none");

	printf("\nAdvanced/Dev/Debug options:\n");

	printf(FMT, "-d", "debug mode");
	printf(FMT, "", "Stay in forground, CLI on stdin.");
	printf(FMT, "-C", "Output VCL code compiled to C language");
	printf(FMT, "-V", "version");
	printf(FMT, "-h kind[,options]", "Hash specification");
	printf(FMT, "-W waiter", "Waiter implementation");
}

/*--------------------------------------------------------------------*/

static void
cli_check(const struct cli *cli)
{
	if (cli->result == CLIS_OK) {
		VSB_clear(cli->sb);
		return;
	}
	AZ(VSB_finish(cli->sb));
	fprintf(stderr, "Error:\n%s\n", VSB_data(cli->sb));
	exit(2);
}

/*--------------------------------------------------------------------
 * This function is called when the CLI on stdin is closed.
 */

static void
mgt_stdin_close(void *priv)
{

	(void)priv;

	if (d_flag) {
		MCH_Stop_Child();
		mgt_cli_close_all();
		if (pfh != NULL)
			(void)VPF_Remove(pfh);
		exit(0);
	} else {
		VFIL_null_fd(STDIN_FILENO);
		VFIL_null_fd(STDOUT_FILENO);
		VFIL_null_fd(STDERR_FILENO);
	}
}

/*--------------------------------------------------------------------
 * Autogenerate a -S file using strong random bits from the kernel.
 */

static void
mgt_secret_atexit(void)
{

	/* Only master process */
	if (getpid() != mgt_pid)
		return;
	VJ_master(JAIL_MASTER_FILE);
	(void)unlink("_.secret");
	VJ_master(JAIL_MASTER_LOW);
}

static const char *
make_secret(const char *dirname)
{
	char *fn;
	int fdo;
	int i;
	unsigned char b;

	assert(asprintf(&fn, "%s/_.secret", dirname) > 0);

	VJ_master(JAIL_MASTER_FILE);
	fdo = open(fn, O_RDWR|O_CREAT|O_TRUNC, 0640);
	if (fdo < 0)
		ARGV_ERR("Cannot create secret-file in %s (%s)\n",
		    dirname, strerror(errno));

	for (i = 0; i < 256; i++) {
		AZ(VRND_RandomCrypto(&b, 1));
		assert(1 == write(fdo, &b, 1));
	}
	closefd(&fdo);
	VJ_master(JAIL_MASTER_LOW);
	AZ(atexit(mgt_secret_atexit));
	return (fn);
}

static void
mgt_Cflag_atexit(void)
{

	/* Only master process */
	if (getpid() != mgt_pid)
		return;
	(void)rmdir("vmod_cache");
	(void)rmdir(Cn_arg);
}

/*--------------------------------------------------------------------*/

static void
init_params(struct cli *cli)
{
	ssize_t def, low;

	MCF_CollectParams();

	MCF_TcpParams();

	if (sizeof(void *) < 8) {		/*lint !e506 !e774  */
		/*
		 * Adjust default parameters for 32 bit systems to conserve
		 * VM space.
		 */
		MCF_ParamConf(MCF_DEFAULT, "workspace_client", "24k");
		MCF_ParamConf(MCF_DEFAULT, "workspace_backend", "16k");
		MCF_ParamConf(MCF_DEFAULT, "http_resp_size", "8k");
		MCF_ParamConf(MCF_DEFAULT, "http_req_size", "12k");
		MCF_ParamConf(MCF_DEFAULT, "gzip_buffer", "4k");
		MCF_ParamConf(MCF_MAXIMUM, "vsl_space", "1G");
		MCF_ParamConf(MCF_MAXIMUM, "vsm_space", "1G");
	}

#if !defined(HAVE_ACCEPT_FILTERS) || defined(__linux)
	MCF_ParamConf(MCF_DEFAULT, "accept_filter", "off");
#endif

	low = sysconf(_SC_THREAD_STACK_MIN);
	MCF_ParamConf(MCF_MINIMUM, "thread_pool_stack", "%jdb", (intmax_t)low);

	def = 48 * 1024;
	if (def < low)
		def = low;
	MCF_ParamConf(MCF_DEFAULT, "thread_pool_stack", "%jdb", (intmax_t)def);

	MCF_ParamConf(MCF_MAXIMUM, "thread_pools", "%d", MAX_THREAD_POOLS);

	MCF_InitParams(cli);
}


/*--------------------------------------------------------------------*/

static void
identify(const char *i_arg)
{
	char id[17], *p;
	int i;

	strcpy(id, "varnishd");

	if (i_arg != NULL) {
		if (strlen(i_arg) + 1 > 1024)
			ARGV_ERR("Identity (-i) name too long (max 1023).\n");
		heritage.identity = strdup(i_arg);
		AN(heritage.identity);
		i = strlen(id);
		id[i++] = '/';
		for (; i < (sizeof(id) - 1L); i++) {
			if (!isalnum(*i_arg))
				break;
			id[i] = *i_arg++;
		}
		id[i] = '\0';
	}
	p = strdup(id);
	AN(p);

	openlog(p, LOG_PID, LOG_LOCAL0);
}

static void
mgt_tests(void)
{
	assert(VTIM_parse("Sun, 06 Nov 1994 08:49:37 GMT") == 784111777);
	assert(VTIM_parse("Sunday, 06-Nov-94 08:49:37 GMT") == 784111777);
	assert(VTIM_parse("Sun Nov  6 08:49:37 1994") == 784111777);

	/* Check that our SHA256 works */
	SHA256_Test();
}

static void
mgt_initialize(struct cli *cli)
{
	static unsigned clilim = 32768;

	/* for ASSERT_MGT() */
	mgt_pid = getpid();

	/* Create a cli for convenience in otherwise CLI functions */
	INIT_OBJ(cli, CLI_MAGIC);
	cli[0].sb = VSB_new_auto();
	AN(cli[0].sb);
	cli[0].result = CLIS_OK;
	cli[0].limit = &clilim;

	mgt_cli_init_cls();		// CLI commands can be registered

	init_params(cli);
	cli_check(cli);
}

static void
mgt_x_arg(const char *x_arg)
{
	if (!strcmp(x_arg, "parameter"))
		MCF_DumpRstParam();
	else if (!strcmp(x_arg, "vsl"))
		mgt_DumpRstVsl();
	else if (!strcmp(x_arg, "cli"))
		mgt_DumpRstCli();
	else if (!strcmp(x_arg, "builtin"))
		mgt_DumpBuiltin();
	else
		ARGV_ERR("Invalid -x argument\n");
}

/*--------------------------------------------------------------------*/

#define ERIC_MAGIC 0x2246988a		/* Eric is not random */

static int
mgt_eric(void)
{
	int eric_pipes[2];
	unsigned u;
	ssize_t sz;

	AZ(pipe(eric_pipes));

	switch (fork()) {
	case -1:
		fprintf(stderr, "Fork() failed: %s\n", strerror(errno));
		exit(-1);
	case 0:
		closefd(&eric_pipes[0]);
		assert(setsid() > 1);

		VFIL_null_fd(STDIN_FILENO);
		return (eric_pipes[1]);
	default:
		break;
	}
	closefd(&eric_pipes[1]);
	sz = read(eric_pipes[0], &u, sizeof u);
	if (sz == sizeof u && u == ERIC_MAGIC)
		exit(0);
	else if (sz == sizeof u && u != 0)
		exit(u);
	else
		exit(-1);
}

static void
mgt_eric_im_done(int eric_fd, unsigned u)
{

	if (u == 0)
		u = ERIC_MAGIC;

	VFIL_null_fd(STDIN_FILENO);
	VFIL_null_fd(STDOUT_FILENO);
	VFIL_null_fd(STDERR_FILENO);

	assert(write(eric_fd, &u, sizeof u) == sizeof u);
	closefd(&eric_fd);
}

/*--------------------------------------------------------------------*/

static int __match_proto__(vev_cb_f)
mgt_sigint(const struct vev *e, int what)
{

	(void)e;
	(void)what;
	MGT_Complain(C_ERR, "Manager got SIGINT");
	(void)fflush(stdout);
	if (MCH_Running())
		MCH_Stop_Child();
	exit(0);
}

/*--------------------------------------------------------------------*/

static int __match_proto__(vev_cb_f)
mgt_uptime(const struct vev *e, int what)
{
	static double mgt_uptime_t0 = 0;

	(void)e;
	(void)what;
	AN(VSC_C_mgt);
	if (mgt_uptime_t0 == 0)
		mgt_uptime_t0 = VTIM_real();
	VSC_C_mgt->uptime = static_VSC_C_mgt.uptime =
	    (uint64_t)(VTIM_real() - mgt_uptime_t0);
	if (heritage.vsm != NULL)
		VSM_common_ageupdate(heritage.vsm);
	return (0);
}

/*--------------------------------------------------------------------*/

static void
mgt_I_close(void *priv)
{
	(void)priv;
	fprintf(stderr, "END of -I file processing\n");
	closefd(&I_fd);
}

/*--------------------------------------------------------------------*/

struct f_arg {
	unsigned		magic;
#define F_ARG_MAGIC		0x840649a8
	char			*farg;
	char			*src;
	VTAILQ_ENTRY(f_arg)	list;
};

static const char opt_spec[] = "a:b:Cdf:Fh:i:I:j:l:M:n:P:p:r:S:s:T:t:VW:x:";

int
main(int argc, char * const *argv)
{
	int o, eric_fd = -1;
	unsigned C_flag = 0;
	unsigned f_flag = 0;
	unsigned F_flag = 0;
	const char *b_arg = NULL;
	const char *i_arg = NULL;
	const char *j_arg = NULL;
	const char *h_arg = "critbit";
	const char *M_arg = NULL;
	const char *n_arg = NULL;
	const char *P_arg = NULL;
	const char *S_arg = NULL;
	const char *s_arg = "malloc,100m";
	const char *W_arg = NULL;
	int s_arg_given = 0;
	int novcl = 0;
	const char *T_arg = "localhost:0";
	char *p;
	struct cli cli[1];
	char *dirname;
	char **av;
	unsigned u;
	struct sigaction sac;
	struct vev *e;
	struct f_arg *fa;
	struct vsb *vsb;
	VTAILQ_HEAD(,f_arg) f_args = VTAILQ_HEAD_INITIALIZER(f_args);

	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	mgt_tests();

	mgt_initialize(cli);

	/* Check if first argument is a special flag */

	o = getopt(argc, argv, opt_spec);
	switch (o) {
	case 'x':
		if (argc != 3)
			ARGV_ERR("Too many arguments for -x\n");
		mgt_x_arg(optarg);
		exit(0);
	case 'V':
		if (argc != 2)
			ARGV_ERR("Too many arguments for -V\n");
		VCS_Message("varnishd");
		exit(0);
	default:
		break;
	}

	/* First pass over arguments to determine overall configuration */

	do {
		switch (o) {
		case '?':
			if (optopt == '?')
				usage();
			exit(2);
		case 'V':
		case 'x':
			ARGV_ERR("-%c must be the first argument\n", o);
			break;
		case 'b':
			b_arg = optarg;
			break;
		case 'C':
			C_flag = 1;
			break;
		case 'd':
			d_flag++;
			break;
		case 'f':
			f_flag = 1;
			break;
		case 'F':
			F_flag = 1;
			break;
		case 'j':
			j_arg = optarg;
			break;
		default:
			break;
		}
		o = getopt(argc, argv, opt_spec);
	} while (o != -1);

	if (argc != optind)
		ARGV_ERR("Too many arguments (%s...)\n", argv[optind]);

	if (b_arg != NULL && f_flag)
		ARGV_ERR("Only one of -b or -f can be specified\n");

	if (d_flag && F_flag)
		ARGV_ERR("Only one of -d or -F can be specified\n");

	if (C_flag && b_arg == NULL && !f_flag)
		ARGV_ERR("-C needs either -b <backend> or -f <vcl_file>\n");

	if (d_flag && C_flag)
		ARGV_ERR("-d makes no sense with -C\n");

	if (F_flag && C_flag)
		ARGV_ERR("-F makes no sense with -C\n");

	if (!d_flag && b_arg == NULL && !f_flag)
		ARGV_ERR("Neither -b nor -f given. (use -f '' to override)\n");

	/*
	 * Start out by closing all unwanted file descriptors we might
	 * have inherited from sloppy process control daemons.
	 */
	VSUB_closefrom(STDERR_FILENO + 1);
	MCH_TrackHighFd(STDERR_FILENO);

	/*
	 * Have Eric Daemonize us if need be
	 */
	if (!C_flag && !d_flag && !F_flag) {
		eric_fd = mgt_eric();
		MCH_TrackHighFd(eric_fd);
		mgt_pid = getpid();
	}

	/* Set up the mgt counters */
	memset(&static_VSC_C_mgt, 0, sizeof static_VSC_C_mgt);
	VSC_C_mgt = &static_VSC_C_mgt;

	VRND_SeedAll();

	vident = mgt_BuildVident();

	/* Various initializations */
	VTAILQ_INIT(&heritage.socks);
	mgt_evb = vev_new_base();
	AN(mgt_evb);

	/* Initialize transport protocols */
	XPORT_Init();

	VJ_Init(j_arg);

	optind = 1;
	optreset = 1;
	while ((o = getopt(argc, argv, opt_spec)) != -1) {
		switch (o) {
		case 'C':
		case 'd':
		case 'F':
		case 'j':
			/* Handled in first pass */
			break;
		case 'a':
			MAC_Arg(optarg);
			break;
		case 'b':
			ALLOC_OBJ(fa, F_ARG_MAGIC);
			AN(fa);
			REPLACE(fa->farg, "<-b argument>");
			vsb = VSB_new_auto();
			AN(vsb);
			VSB_printf(vsb, "vcl 4.0;\n");
			VSB_printf(vsb, "backend default {\n");
			VSB_printf(vsb, "    .host = \"%s\";\n", optarg);
			VSB_printf(vsb, "}\n");
			AZ(VSB_finish(vsb));
			fa->src = strdup(VSB_data(vsb));
			AN(fa->src);
			VSB_destroy(&vsb);
			VTAILQ_INSERT_TAIL(&f_args, fa, list);
			break;
		case 'f':
			if (*optarg == '\0') {
				novcl = 1;
				break;
			}
			ALLOC_OBJ(fa, F_ARG_MAGIC);
			AN(fa);
			REPLACE(fa->farg, optarg);
			fa->src = VFIL_readfile(NULL, fa->farg, NULL);
			if (fa->src == NULL)
				ARGV_ERR("Cannot read -f file (%s): %s\n",
				    fa->farg, strerror(errno));
			VTAILQ_INSERT_TAIL(&f_args, fa, list);
			break;
		case 'h':
			h_arg = optarg;
			break;
		case 'i':
			i_arg = optarg;
			break;
		case 'I':
			if (I_fd >= 0)
				ARGV_ERR("\tOnly one -I allowed\n");
			I_fd = open(optarg, O_RDONLY);
			if (I_fd < 0)
				ARGV_ERR("\tCant open %s: %s\n",
				    optarg, strerror(errno));
			break;
		case 'l':
			av = VAV_Parse(optarg, NULL, ARGV_COMMA);
			AN(av);
			if (av[0] != NULL)
				ARGV_ERR("\t-l ...: %s\n", av[0]);
			if (av[1] != NULL) {
				MCF_ParamSet(cli, "vsl_space", av[1]);
				cli_check(cli);
			}
			if (av[1] != NULL && av[2] != NULL) {
				MCF_ParamSet(cli, "vsm_space", av[2]);
				cli_check(cli);
			}
			VAV_Free(av);
			break;
		case 'M':
			M_arg = optarg;
			break;
		case 'n':
			n_arg = optarg;
			break;
		case 'P':
			P_arg = optarg;
			break;
		case 'p':
			p = strchr(optarg, '=');
			if (p == NULL)
				ARGV_ERR("\t-p lacks '='\n");
			AN(p);
			*p++ = '\0';
			MCF_ParamSet(cli, optarg, p);
			*--p = '=';
			cli_check(cli);
			break;
		case 'r':
			MCF_ParamProtect(cli, optarg);
			cli_check(cli);
			break;
		case 'S':
			S_arg = optarg;
			break;
		case 's':
			s_arg_given = 1;
			STV_Config(optarg);
			break;
		case 'T':
			if (!strcmp(optarg, "none"))
				T_arg = NULL;
			else
				T_arg = optarg;
			break;
		case 't':
			MCF_ParamSet(cli, "default_ttl", optarg);
			break;
		case 'W':
			W_arg = optarg;
			break;
		default:
			WRONG("Error in argument parsing");
		}
	}
	assert(argc == optind);

	/* XXX: we can have multiple CLI actions above, is this enough ? */
	if (cli[0].result != CLIS_OK) {
		AZ(VSB_finish(cli[0].sb));
		ARGV_ERR("Failed parameter creation:\n%s\n",
		    VSB_data(cli[0].sb));
	}

	assert(d_flag == 0 || F_flag == 0);

	if (C_flag) {
		if (n_arg == NULL) {
			AN(mkdtemp(Cn_arg));
			AZ(chmod(Cn_arg, 0755));
			AZ(atexit(mgt_Cflag_atexit));
			n_arg = Cn_arg;
		}
	}

	if (S_arg != NULL && !strcmp(S_arg, "none")) {
		fprintf(stderr,
		    "Warning: CLI authentication disabled.\n");
	} else if (S_arg != NULL) {
		VJ_master(JAIL_MASTER_FILE);
		o = open(S_arg, O_RDONLY, 0);
		if (o < 0)
			ARGV_ERR("Cannot open -S file (%s): %s\n",
			    S_arg, strerror(errno));
		closefd(&o);
		VJ_master(JAIL_MASTER_LOW);
	}

	if (VIN_N_Arg(n_arg, &heritage.name, &dirname, NULL) != 0)
		ARGV_ERR("Invalid instance (-n) name: %s\n", strerror(errno));

#ifdef HAVE_SETPROCTITLE
	setproctitle("Varnish-Mgr %s", heritage.name);
#endif

	identify(i_arg);

	if (VJ_make_workdir(dirname))
		ARGV_ERR("Cannot create working directory (%s): %s\n",
		    dirname, strerror(errno));

	if (VJ_make_vcldir("vmod_cache")) {
		ARGV_ERR(
		    "Cannot create vmod directory (%s/vmod_cache): %s\n",
		    dirname, strerror(errno));
	}

	VJ_master(JAIL_MASTER_FILE);
	if (P_arg && (pfh = VPF_Open(P_arg, 0644, NULL)) == NULL)
		ARGV_ERR("Could not open pid/lock (-P) file (%s): %s\n",
		    P_arg, strerror(errno));
	VJ_master(JAIL_MASTER_LOW);

	/* If no -s argument specified, process default -s argument */
	if (!s_arg_given)
		STV_Config(s_arg);

	/* Configure Transient storage, if user did not */
	STV_Config_Transient();

	mgt_vcl_init();

	if (C_flag) {
		VTAILQ_FOREACH(fa, &f_args, list) {
			mgt_vcl_startup(cli, fa->src,
			    VTAILQ_NEXT(fa, list) == NULL ? "boot" : NULL,
			    fa->farg, 1);
			AZ(VSB_finish(cli->sb));
			fprintf(stderr, "%s\n", VSB_data(cli->sb));
			VSB_clear(cli->sb);
		}
		exit(cli->result == CLIS_OK ? 0 : 2);
	} else {
		while (!VTAILQ_EMPTY(&f_args)) {
			fa = VTAILQ_FIRST(&f_args);
			VTAILQ_REMOVE(&f_args, fa, list);
			mgt_vcl_startup(cli, fa->src,
			    VTAILQ_EMPTY(&f_args) ? "boot" : NULL,
			    fa->farg, 0);
			cli_check(cli);
			free(fa->farg);
			free(fa->src);
			FREE_OBJ(fa);
		}
	}

	if (VTAILQ_EMPTY(&heritage.socks))
		MAC_Arg(":80");

	assert(!VTAILQ_EMPTY(&heritage.socks));

	HSH_config(h_arg);

	Wait_config(W_arg);

	mgt_SHM_Init();

	if (M_arg != NULL)
		mgt_cli_master(M_arg);
	if (T_arg != NULL)
		mgt_cli_telnet(T_arg);

	AZ(VSB_finish(vident));

	if (S_arg == NULL)
		S_arg = make_secret(dirname);
	AN(S_arg);

	assert(pfh == NULL || !VPF_Write(pfh));

	MGT_Complain(C_DEBUG, "Platform: %s", VSB_data(vident) + 1);

	if (d_flag)
		mgt_cli_setup(0, 1, 1, "debug", mgt_stdin_close, NULL);

	if (strcmp(S_arg, "none"))
		mgt_cli_secret(S_arg);

	mgt_SHM_Create();

	memset(&sac, 0, sizeof sac);
	sac.sa_handler = SIG_IGN;
	sac.sa_flags = SA_RESTART;

	AZ(sigaction(SIGPIPE, &sac, NULL));
	AZ(sigaction(SIGHUP, &sac, NULL));

	MCH_Init();

	if (I_fd >= 0) {
		fprintf(stderr, "BEGIN of -I file processing\n");
		mgt_cli_setup(I_fd, 2, 1, "-I file", mgt_I_close, stderr);
		while (I_fd >= 0) {
			o = vev_schedule_one(mgt_evb);
			if (o != 1)
				MGT_Complain(C_ERR,
				    "vev_schedule_one() = %d", o);
		}
	}

	if (!d_flag && !mgt_has_vcl() && !novcl)
		MGT_Complain(C_ERR, "No VCL loaded yet");

	if (mgt_has_vcl() && ! d_flag)
		u = MCH_Start_Child();
	else
		u = 0;

	if (eric_fd >= 0)
		mgt_eric_im_done(eric_fd, u);

	if (u)
		exit(u);

	/* Failure is no longer an option */

	if (F_flag)
		VFIL_null_fd(STDIN_FILENO);

	e = vev_new();
	AN(e);
	e->callback = mgt_uptime;
	e->timeout = 1.0;
	e->name = "mgt_uptime";
	AZ(vev_add(mgt_evb, e));

	e = vev_new();
	AN(e);
	e->sig = SIGTERM;
	e->callback = mgt_sigint;
	e->name = "mgt_sigterm";
	AZ(vev_add(mgt_evb, e));

	e = vev_new();
	AN(e);
	e->sig = SIGINT;
	e->callback = mgt_sigint;
	e->name = "mgt_sigint";
	AZ(vev_add(mgt_evb, e));

	o = vev_schedule(mgt_evb);
	if (o != 0)
		MGT_Complain(C_ERR, "vev_schedule() = %d", o);

	MGT_Complain(C_INFO, "manager dies");
	if (pfh != NULL)
		(void)VPF_Remove(pfh);
	exit(exit_status);
}
