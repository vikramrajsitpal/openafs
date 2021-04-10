/*
 * Copyright (c) 2021 Sine Nomine Associates.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <afsconfig.h>
#include <afs/param.h>

#include <roken.h>
#include <afs/afsctl.h>
#include <afs/cellconfig.h>
#include <afs/com_err.h>
#include <afs/cmd.h>
#include <ubik_np.h>

#include <stdarg.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <jansson.h>

enum {
    OPT_output,
    OPT_require_sync,

    OPT_cmd,

    OPT_freezeid,
    OPT_force,
    OPT_timeout_ms,

    OPT_format,

    OPT_reason,
    OPT_ctl_socket,
    OPT_quiet,
};

enum format_type {
    fmt_text = 0,
    fmt_json,
};

struct ctlmain_ctx {
    char *udb_descr;
    char *udb_prefix;

    enum format_type format;

    struct afsctl_clientinfo cinfo;
    int quiet;

    struct ubik_freeze_client *freeze;
    struct ubik_version64 frozen_vers;
    char *db_path;  /**< When freezing the db, the path to the db that the
		     *   server gave us. */

    char *out_path; /**< When dumping the db, the path to dump to. */
};

struct ubikctl_info {
    char *cmd_prefix;
    char *udb_descr;
    char *server_type;
};

static int preamble(struct ubikctl_info *usuite, struct cmd_syndesc *as,
		    struct ctlmain_ctx **a_ctlm);

static void
AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3)
print_nq(struct ctlmain_ctx *ctlm, const char *fmt, ...)
{
    if (!ctlm->quiet) {
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
    }
}

static void
AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3)
print_error(int code, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "\n%s: ", getprogname());
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (code != 0) {
	fprintf(stderr, ": %s\n", afs_error_message(code));
    } else {
	fprintf(stderr, "\n");
    }
}

static void
postamble(struct ctlmain_ctx **a_ctlm)
{
    struct ctlmain_ctx *ctlm = *a_ctlm;
    if (ctlm == NULL) {
	return;
    }
    *a_ctlm = NULL;

    free(ctlm->udb_descr);
    free(ctlm->cinfo.sock_path);
    free(ctlm->cinfo.reason);
    free(ctlm->out_path);
    free(ctlm->db_path);
    ubik_FreezeDestroy(&ctlm->freeze);
    free(ctlm);
}

static int
begin_freeze(struct ctlmain_ctx *ctlm)
{
    int code;
    afs_uint64 freezeid = 0;

    print_nq(ctlm, "Freezing database... ");

    code = ubik_FreezeBegin(ctlm->freeze, &freezeid, &ctlm->frozen_vers,
			    &ctlm->db_path);
    if (code != 0) {
	print_error(code, "Failed to freeze db");
	return code;
    }

    print_nq(ctlm, "done (freezeid %llu, db %lld.%lld).\n", freezeid,
	     ctlm->frozen_vers.epoch64.clunks,
	     ctlm->frozen_vers.counter64);
    return 0;
}

static int
end_freeze(struct ctlmain_ctx *ctlm)
{
    int code;

    print_nq(ctlm, "Ending freeze... ");

    code = ubik_FreezeEnd(ctlm->freeze, NULL);
    if (code != 0) {
	print_error(code, "Error ending freeze");
	return code;
    }

    print_nq(ctlm, "done.\n");

    return 0;
}

static int
print_json(json_t *jobj)
{
    int code = json_dumpf(jobj, stdout, JSON_COMPACT);
    if (code != 0) {
	print_error(0, "Error dumping json data\n");
	return EIO;
    }
    return 0;
}

static int
UbikDbInfoCmd(struct cmd_syndesc *as, void *rock)
{
    struct ubikctl_info *usuite = rock;
    struct ctlmain_ctx *ctlm = NULL;
    int code;
    json_t *jobj = NULL;
    json_error_t jerror;

    code = preamble(usuite, as, &ctlm);
    if (code != 0) {
	goto error;
    }

    code = afsctl_client_call(&ctlm->cinfo, "ubik.dbinfo", json_null(), &jobj);
    if (code != 0) {
	print_error(code, "Failed to get db info");
	goto error;
    }

    if (ctlm->format == fmt_text) {
	json_int_t dbsize_j, epoch_j, counter_j;
	afs_int64 dbsize, epoch64, counter;
	char *dbtype;
	char *engine;
	char *desc;

	code = json_unpack_ex(jobj, &jerror, 0,
			      "{s:s, s:{s:s, s:s}, s:I, s:{s:I, s:I!}}",
			      "type", &dbtype,
			      "engine", "name", &engine,
					"desc", &desc,
			      "size", &dbsize_j,
			      "version", "epoch64", &epoch_j,
					 "counter", &counter_j);
	if (code != 0) {
	    print_error(0, "Error decoding server json data (offset %d): %s",
			jerror.position, jerror.text);
	    goto error;
	}

	dbsize = dbsize_j;
	epoch64 = epoch_j;
	counter = counter_j;

	printf("%s database info:\n", ctlm->udb_descr);
	printf("  type: %s\n", dbtype);
	printf("  engine: %s (%s)\n", engine, desc);
	printf("  version: %lld.%lld\n", epoch64, counter);
	printf("  size: %lld\n", dbsize);

    } else if (ctlm->format == fmt_json) {
	opr_Assert(ctlm->format == fmt_json);

	code = print_json(jobj);
	if (code != 0) {
	    goto error;
	}

    } else {
	print_error(0, "Internal error: unknown format %d", ctlm->format);
	code = EINVAL;
	goto error;
    }

    code = 0;

 done:
    postamble(&ctlm);
    json_decref(jobj);
    return code;

 error:
    code = -1;
    goto done;
}

static int
UbikDumpCmd(struct cmd_syndesc *as, void *rock)
{
    struct ubikctl_info *usuite = rock;
    struct ctlmain_ctx *ctlm = NULL;
    int code;

    code = preamble(usuite, as, &ctlm);
    if (code != 0) {
	goto error;
    }

    opr_Assert(ctlm->freeze != NULL);
    opr_Assert(ctlm->out_path != NULL);

    print_nq(ctlm, "Dumping database... ");

    code = ubik_CopyDB(ctlm->db_path, ctlm->out_path);
    if (code != 0) {
	print_error(code, "Failed to dump db to %s", ctlm->out_path);
	goto error;
    }

    print_nq(ctlm, "done.\n");

    code = end_freeze(ctlm);
    if (code != 0) {
	goto error;
    }

    print_nq(ctlm, "Database dumped to %s, version %lld.%lld\n",
	     ctlm->out_path, ctlm->frozen_vers.epoch64.clunks,
	     ctlm->frozen_vers.counter64);

 done:
    postamble(&ctlm);
    return code;

 error:
    code = -1;
    goto done;
}

static int
UbikFreezeRunCmd(struct cmd_syndesc *as, void *rock)
{
    static struct cmd_item default_cmd = {
	.data = "/bin/sh",
    };

    struct ubikctl_info *usuite = rock;
    struct ctlmain_ctx *ctlm = NULL;
    int code;
    int argc;
    int n_args = 0;
    char **argv = NULL;
    struct cmd_item *cmd_args = NULL;
    struct cmd_item *cur;
    pid_t child;
    int status = 0;
    int nocmd = 0;

    code = preamble(usuite, as, &ctlm);
    if (code != 0) {
	goto error;
    }

    opr_Assert(ctlm->freeze != NULL);

    code = ubik_FreezeSetEnv(ctlm->freeze);
    if (code != 0) {
	print_error(code, "Failed to set freeze env");
	goto error;
    }

    cmd_OptionAsList(as, OPT_cmd, &cmd_args);
    if (cmd_args == NULL) {
	nocmd = 1;
	cmd_args = &default_cmd;
    }

    for (cur = cmd_args; cur != NULL; cur = cur->next) {
	n_args++;
    }

    argv = calloc(n_args+1, sizeof(argv[0]));
    if (argv == NULL) {
	print_error(errno, "calloc");
	goto error;
    }

    argc = 0;
    for (cur = cmd_args; cur != NULL; cur = cur->next) {
	opr_Assert(argc < n_args);
	argv[argc++] = cur->data;
    }

    if (nocmd && !ctlm->quiet) {
	printf("\n");
	printf("No -cmd given; spawning a shell to run for the duration of "
	       "the freeze.\n");
	printf("Exit the shell to end the freeze.\n");
	printf("\n");
	ubik_FreezePrintEnv(ctlm->freeze, stdout);
    }

    child = fork();
    if (child < 0) {
	print_error(code, "fork");
	goto error;
    }

    if (child == 0) {
	execvp(argv[0], argv);
	print_error(errno, "Failed to exec %s", argv[0]);
	exit(1);
    }

    code = waitpid(child, &status, 0);
    if (code < 0) {
	print_error(errno, "waitpid");
	goto error;
    }

    if (WIFEXITED(status)) {
	code = WEXITSTATUS(status);
	if (code != 0 || !ctlm->quiet) {
	    print_error(0, "Command exited with status %d", code);
	}
	if (code != 0) {
	    /* child process failed */
	    goto done;
	}

    } else {
	print_error(0, "Command died with 0x%x", status);
	goto error;
    }

    code = end_freeze(ctlm);
    if (code != 0) {
	goto error;
    }

    code = 0;

 done:
    free(argv);
    postamble(&ctlm);
    return code;

 error:
    code = -1;
    goto done;
}

static int
UbikFreezeAbortCmd(struct cmd_syndesc *as, void *rock)
{
    struct ubikctl_info *usuite = rock;
    struct ctlmain_ctx *ctlm = NULL;
    afs_uint64 freezeid = 0;
    unsigned int opt_freezeid = 0;
    int force = 0;
    int have_freezeid = 0;
    int code;

    code = preamble(usuite, as, &ctlm);
    if (code != 0) {
	goto error;
    }

    if (cmd_OptionAsUint(as, OPT_freezeid, &opt_freezeid) == 0) {
	have_freezeid = 1;
	freezeid = opt_freezeid;
    }
    cmd_OptionAsFlag(as, OPT_force, &force);

    if (have_freezeid && force) {
	print_error(0, "You cannot specify both -freezeid and -abort.");
	goto error;
    }

    if (!have_freezeid && !force) {
	afs_uint64 nested_freezeid = 0;
	if (ubik_FreezeIsNested(ctlm->freeze, &nested_freezeid)) {
	    freezeid = nested_freezeid;
	} else {
	    print_error(0, "You must specify either -freezeid or -abort, if "
			"not running inside '%s %sdb-freeze-run'.",
			getprogname(), ctlm->udb_prefix);
	    goto error;
	}
    }

    if (force) {
	print_nq(ctlm, "Aborting freeze... ");
	code = ubik_FreezeAbortForce(ctlm->freeze, NULL);
    } else {
	print_nq(ctlm, "Aborting freeze %llu... ", freezeid);
	code = ubik_FreezeAbortId(ctlm->freeze, freezeid, NULL);
    }
    if (code != 0) {
	print_error(code, "Error aborting freeze");
	goto error;
    }

    print_nq(ctlm, "done.\n");

 done:
    postamble(&ctlm);
    return code;

 error:
    code = -1;
    goto done;
}

/* Does this command freeze the db? */
static int
creates_freeze(struct cmd_syndesc *as)
{
    if (as->proc == UbikDumpCmd ||
	as->proc == UbikFreezeRunCmd) {
	return 1;
    }
    return 0;
}

/* Does this command interact with db freezes? (does it create a freeze
 * context?) */
static int
freeze_cmd(struct cmd_syndesc *as)
{
    if (creates_freeze(as) ||
	as->proc == UbikFreezeAbortCmd) {
	return 1;
    }
    return 0;
}

/* Does this command use the -quiet option? That is, does it use print_nq? */
static int
can_quiet(struct cmd_syndesc *as)
{
    if (as->proc == UbikDbInfoCmd) {
	return 0;
    }
    return 1;
}

static int
preamble_freeze(struct ctlmain_ctx *ctlm, struct cmd_syndesc *as)
{
    struct ubik_freezeinit_opts fopts;
    int code;
    unsigned int timeout_ms = 0;

    memset(&fopts, 0, sizeof(fopts));

    if (as->proc == UbikDumpCmd || as->proc == UbikFreezeRunCmd) {
	cmd_OptionAsFlag(as, OPT_require_sync, &fopts.fi_needsync);
    }
    if (as->proc == UbikFreezeRunCmd) {
	fopts.fi_nonest = 1;
    }

    if (creates_freeze(as)) {
	if (cmd_OptionAsUint(as, OPT_timeout_ms, &timeout_ms) == 0) {
	    fopts.fi_timeout_ms = timeout_ms;
	}
    }

    fopts.fi_cinfo = &ctlm->cinfo;

    code = ubik_FreezeInit(&fopts, &ctlm->freeze);
    if (code != 0) {
	print_error(code, "Failed to initialize freeze");
	goto error;
    }

    if (creates_freeze(as)) {
	code = begin_freeze(ctlm);
	if (code != 0) {
	    goto error;
	}
    }

    code = 0;

 done:
    return code;

 error:
    code = -1;
    goto done;
}

static int
preamble(struct ubikctl_info *usuite, struct cmd_syndesc *as,
	 struct ctlmain_ctx **a_ctlm)
{
    int code;
    char *format_str = NULL;
    struct ctlmain_ctx *ctlm;

    ctlm = calloc(1, sizeof(*ctlm));
    if (ctlm == NULL) {
	goto enomem;
    }

    ctlm->udb_prefix = usuite->cmd_prefix;
    ctlm->udb_descr = strdup(usuite->udb_descr);
    if (ctlm->udb_descr == NULL) {
	goto enomem;
    }

    ctlm->cinfo.server_type = usuite->server_type;

    cmd_OptionAsString(as, OPT_reason, &ctlm->cinfo.reason);
    cmd_OptionAsString(as, OPT_ctl_socket, &ctlm->cinfo.sock_path);

    if (cmd_OptionAsString(as, OPT_format, &format_str) == 0) {
	if (strcmp(format_str, "text") == 0) {
	    ctlm->format = fmt_text;
	} else if (strcmp(format_str, "json") == 0) {
	    ctlm->format = fmt_json;
	} else {
	    print_error(0, "Bad argument to -format: '%s'\n", format_str);
	    goto error;
	}
    }

    if (can_quiet(as)) {
	cmd_OptionAsFlag(as, OPT_quiet, &ctlm->quiet);
    }

    if (as->proc == UbikDumpCmd) {
	/* Check that we can create the output db. */
	cmd_OptionAsString(as, OPT_output, &ctlm->out_path);
	opr_Assert(ctlm->out_path != NULL);

	code = mkdir(ctlm->out_path, 0700);
	if (code != 0) {
	    print_error(errno, "Could not create %s", ctlm->out_path);
	    goto error;
	}

	code = rmdir(ctlm->out_path);
	if (code != 0) {
	    print_error(errno, "Could not rmdir %s", ctlm->out_path);
	    goto error;
	}
    }

    if (freeze_cmd(as)) {
	code = preamble_freeze(ctlm, as);
	if (code != 0) {
	    goto error;
	}
    }

    *a_ctlm = ctlm;

    return 0;

 enomem:
    print_error(ENOMEM, "Failed alloc");
 error:
    postamble(&ctlm);
    return -1;

}

static void
common_opts(struct cmd_syndesc *as)
{
    if (creates_freeze(as)) {
	cmd_AddParmAtOffset(as, OPT_timeout_ms, "-timeout-ms", CMD_SINGLE,
			    CMD_OPTIONAL,
			    "max time for db freeze (in ms)");
    }
    if (can_quiet(as)) {
	cmd_AddParmAtOffset(as, OPT_quiet, "-quiet", CMD_FLAG, CMD_OPTIONAL,
			    "talk less");
    }

    cmd_AddParmAtOffset(as, OPT_reason, "-reason", CMD_SINGLE, CMD_OPTIONAL,
			"reason to log for operation");
    cmd_AddParmAtOffset(as, OPT_ctl_socket, "-ctl-socket", CMD_SINGLE,
			CMD_OPTIONAL,
			"path to ctl unix socket");
}

static char *
AFS_ATTRIBUTE_FORMAT(__printf__, 2, 3)
do_snprintf(struct rx_opaque *buf, const char *fmt, ...)
{
    va_list ap;
    int nbytes;

    va_start(ap, fmt);
    nbytes = vsnprintf(buf->val, buf->len, fmt, ap);
    va_end(ap);

    opr_Assert(nbytes < buf->len);
    return buf->val;
}

static int
create_ubik_syntax(struct ubikctl_info *usuite)
{
    static char buf_name[64];
    static char buf_descr[128];

    struct rx_opaque name = {
	.val = buf_name,
	.len = sizeof(buf_name),
    };
    struct rx_opaque descr = {
	.val = buf_descr,
	.len = sizeof(buf_descr),
    };
    char *prefix = usuite->cmd_prefix;
    char *db = usuite->udb_descr;
    struct cmd_syndesc *ts;

    ts = cmd_CreateSyntax(do_snprintf(&name, "%sdb-info", prefix),
			  UbikDbInfoCmd, usuite, 0,
			  do_snprintf(&descr, "get %s info", db));
    cmd_AddParmAtOffset(ts, OPT_format, "-format", CMD_SINGLE, CMD_OPTIONAL,
			"text | json");
    common_opts(ts);

    ts = cmd_CreateSyntax(do_snprintf(&name, "%sdb-dump", prefix),
			  UbikDumpCmd, usuite, 0,
			  do_snprintf(&descr, "dump %s", db));
    cmd_AddParmAtOffset(ts, OPT_output, "-output", CMD_SINGLE, CMD_REQUIRED,
			"output db path");
    cmd_AddParmAtOffset(ts, OPT_require_sync, "-require-sync", CMD_FLAG,
			CMD_OPTIONAL,
			"fail if using non-sync-site");
    common_opts(ts);

    ts = cmd_CreateSyntax(do_snprintf(&name, "%sdb-freeze-run", prefix),
			  UbikFreezeRunCmd, usuite, 0,
			  do_snprintf(&descr, "freeze %s during command", db));
    cmd_AddParmAtOffset(ts, OPT_cmd, "-cmd", CMD_LIST, CMD_OPTIONAL,
			"command (and args) to run during freeze");
    cmd_AddParmAtOffset(ts, OPT_require_sync, "-require-sync", CMD_FLAG,
			CMD_OPTIONAL,
			"fail if using non-sync-site");
    common_opts(ts);

    ts = cmd_CreateSyntax(do_snprintf(&name, "%sdb-freeze-abort", prefix),
			  UbikFreezeAbortCmd, usuite, 0, "abort a running freeze");
    cmd_AddParmAtOffset(ts, OPT_freezeid, "-freezeid", CMD_SINGLE,
			CMD_OPTIONAL,
			"freezeid to abort");
    cmd_AddParmAtOffset(ts, OPT_force, "-force", CMD_FLAG, CMD_OPTIONAL,
			"abort whatever freeze is running");
    common_opts(ts);

    return 0;
}

static struct ubikctl_info ubik_suites[] = {
    {
	.cmd_prefix = "pt",
	.udb_descr = "ptdb",
	.server_type = "ptserver",
    },
    {
	.cmd_prefix = "vl",
	.udb_descr = "vldb",
	.server_type = "vlserver",
    },
    {0}
};

int
main(int argc, char *argv[])
{
    int code;
    struct ubikctl_info *usuite;

    setprogname(argv[0]);

    initialize_CMD_error_table();
    initialize_U_error_table();

    for (usuite = ubik_suites; usuite->cmd_prefix != NULL; usuite++) {
	code = create_ubik_syntax(usuite);
	if (code != 0) {
	    return code;
	}
    }

    return cmd_Dispatch(argc, argv);
}
