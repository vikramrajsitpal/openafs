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

    OPT_input,
    OPT_backup_suffix,
    OPT_no_backup,
    OPT_dist,

    OPT_cmd,
    OPT_rw,

    OPT_freezeid,
    OPT_force,
    OPT_timeout_ms,

    OPT_format,

    OPT_reason,
    OPT_ctl_socket,
    OPT_quiet,
    OPT_progress,
    OPT_no_progress,
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
    struct opr_progress_opts progopts;

    struct ubik_freeze_client *freeze;
    struct ubik_version64 frozen_vers;
    char *db_path;  /**< When freezing the db, the path to the db that the
		     *   server gave us. */

    char *out_path; /**< When dumping the db, the path to dump to. */
    char *in_path;  /**< When installing/restoring a db, the path to read from */

    char *backup_suffix;
		    /**< When installing a db, backup the existing db to this
		     *   suffix (or NULL to not backup the db) */
    int no_dist;    /**< When installing a db, don't distribute the db to other
		     *   sites */
    int need_dist;  /**< When installing a db, it's an error if we fail to
		     *   distribute the db to other sites */
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
del_db(char *path)
{
    int code;
    code = unlink(path);
    if (code != 0) {
	code = errno;
    }
    if (code == ENOENT) {
	code = 0;
    }
    return code;
}

static int
do_install(struct ubikctl_info *usuite, struct cmd_syndesc *as, int restore)
{
    int code;
    struct ctlmain_ctx *ctlm = NULL;
    char *db_path_free = NULL;
    char *db_path;

    code = preamble(usuite, as, &ctlm);
    if (code != 0) {
	goto error;
    }

    opr_Assert(ctlm->freeze != NULL);

    if (restore) {
	print_nq(ctlm, "Making copy of %s... ", ctlm->in_path);

	if (asprintf(&db_path_free, "%s.TMP", ctlm->db_path) < 0) {
	    code = errno;
	    print_error(code, "Failed to construct db path");
	    goto error;
	}

	db_path = db_path_free;
	code = del_db(db_path);
	if (code != 0) {
	    print_error(code, "Failed to delete tmp path %s", db_path);
	    goto error;
	}

	code = ubik_CopyDB(ctlm->in_path, db_path);
	if (code != 0) {
	    print_error(code, "Failed to copy db to %s", db_path);
	    goto error;
	}

	print_nq(ctlm, "done.\n");

    } else {
	db_path = ctlm->in_path;
    }

    print_nq(ctlm, "Installing db %s... ", db_path);

    code = ubik_FreezeInstall(ctlm->freeze, db_path, ctlm->backup_suffix);
    if (code != 0) {
	print_error(code, "Failed to install db");
	goto error;
    }

    print_nq(ctlm, "done.\n");

    if (!ctlm->no_dist) {
	struct opr_progress_opts progopts = ctlm->progopts;
	struct opr_progress *progress;
	progopts.bkg_spinner = 1;

	progress = opr_progress_start(&progopts, "Distributing db");
	code = ubik_FreezeDistribute(ctlm->freeze);
	opr_progress_done(&progress, code);

	if (code != 0) {
	    print_error(code, "Failed to distribute db");
	    if (ctlm->need_dist) {
		goto error;
	    }

	    fprintf(stderr, "warning: We failed to distribute the new database "
		    "to other ubik sites, but the\n");
	    fprintf(stderr, "warning: database was installed successfully. Ubik "
		    "itself may distribute the db\n");
	    fprintf(stderr, "warning: on its own in the background.\n");
	    fprintf(stderr, "\n");
	}
    }

    code = end_freeze(ctlm);
    if (code != 0) {
	goto error;
    }

    print_nq(ctlm, "\n");
    if (restore) {
	print_nq(ctlm, "Restored ubik database from %s\n", ctlm->in_path);
    } else {
	print_nq(ctlm, "Installed ubik database %s\n", ctlm->in_path);
    }
    if (ctlm->backup_suffix != NULL) {
	print_nq(ctlm, "Existing database backed up to suffix %s\n",
		 ctlm->backup_suffix);
    }

    code = 0;

 done:
    free(db_path_free);
    postamble(&ctlm);
    return code;

 error:
    code = -1;
    goto done;
}

static int
UbikRestoreCmd(struct cmd_syndesc *as, void *rock)
{
    struct ubikctl_info *usuite = rock;

    return do_install(usuite, as, 1);
}

static int
UbikInstallCmd(struct cmd_syndesc *as, void *rock)
{
    struct ubikctl_info *usuite = rock;

    return do_install(usuite, as, 0);
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
UbikFreezeDistCmd(struct cmd_syndesc *as, void *rock)
{
    struct ubikctl_info *usuite = rock;
    struct ctlmain_ctx *ctlm = NULL;
    int code;

    code = preamble(usuite, as, &ctlm);
    if (code != 0) {
	goto error;
    }

    print_nq(ctlm, "Distributing restored database (may take a while)... ");

    code = ubik_FreezeDistribute(ctlm->freeze);
    if (code != 0) {
	print_error(code, "Failed to distribute db");
	goto error;
    }

    print_nq(ctlm, "done.\n");

    code = 0;

 done:
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
	as->proc == UbikRestoreCmd ||
	as->proc == UbikInstallCmd ||
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
	as->proc == UbikFreezeDistCmd ||
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
    if (as->proc == UbikRestoreCmd || as->proc == UbikInstallCmd) {
	fopts.fi_needrw = 1;
    }
    if (as->proc == UbikFreezeRunCmd) {
	cmd_OptionAsFlag(as, OPT_rw, &fopts.fi_needrw);
	fopts.fi_nonest = 1;
    }
    if (as->proc == UbikFreezeDistCmd) {
	fopts.fi_forcenest = 1;
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
check_db(char *path)
{
    int code;
    struct ubik_dbase *dbase = NULL;
    struct ubik_trans *trans = NULL;
    struct ubik_version vers;

    code = ubik_RawInit(path, NULL, &dbase);
    if (code != 0) {
	print_error(code, "Failed to init raw handle");
	goto error;
    }

    code = ubik_BeginTransReadAny(dbase, UBIK_READTRANS, &trans);
    if (code != 0) {
	print_error(code, "Failed to begin trans");
	goto error;
    }

    code = ubik_RawGetVersion(trans, &vers);
    if (code != 0) {
	print_error(code, "Failed to get header version");
	goto error;
    }

    code = 0;

 error:
    if (trans != NULL) {
	ubik_AbortTrans(trans);
    }
    ubik_RawClose(&dbase);
    return code;
}

static int
preamble(struct ubikctl_info *usuite, struct cmd_syndesc *as,
	 struct ctlmain_ctx **a_ctlm)
{
    int code;
    char *format_str = NULL;
    char *dist = NULL;
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
	cmd_OptionAsFlag(as, OPT_progress, &ctlm->progopts.force_enable);
	cmd_OptionAsFlag(as, OPT_no_progress, &ctlm->progopts.force_disable);

	ctlm->progopts.quiet = ctlm->quiet;
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

    if (as->proc == UbikRestoreCmd || as->proc == UbikInstallCmd) {
	int no_backup = 0;

	cmd_OptionAsString(as, OPT_input, &ctlm->in_path);
	opr_Assert(ctlm->in_path != NULL);

	cmd_OptionAsString(as, OPT_backup_suffix, &ctlm->backup_suffix);
	cmd_OptionAsFlag(as, OPT_no_backup, &no_backup);
	cmd_OptionAsString(as, OPT_dist, &dist);

	if (ctlm->backup_suffix == NULL && !no_backup) {
	    print_error(0, "You must specify either -backup-suffix or "
			"-no-backup");
	    goto error;
	}
	if (no_backup) {
	    free(ctlm->backup_suffix);
	    ctlm->backup_suffix = NULL;
	}

	if (dist == NULL || strcmp(dist, "try") == 0) {
	    /* noop; this is the default */
	} else if (strcmp(dist, "skip") == 0) {
	    ctlm->no_dist = 1;
	} else if (strcmp(dist, "required") == 0) {
	    ctlm->need_dist = 1;
	} else {
	    print_error(0, "Bad value for -dist: %s", dist);
	    goto error;
	}

	/* Check that we can open the input db. */
	code = check_db(ctlm->in_path);
	if (code != 0) {
	    print_error(code, "Failed to open %s", ctlm->in_path);
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

    code = 0;

done:
    free(dist);
    return code;

 enomem:
    print_error(ENOMEM, "Failed alloc");
 error:
    postamble(&ctlm);
    code = -1;
    goto done;

}

static void
install_opts(struct cmd_syndesc *as)
{
    cmd_AddParmAtOffset(as, OPT_input, "-input", CMD_SINGLE, CMD_REQUIRED,
			"input db path");
    cmd_AddParmAtOffset(as, OPT_backup_suffix, "-backup-suffix", CMD_SINGLE,
			CMD_OPTIONAL,
			"backup db suffix");
    cmd_AddParmAtOffset(as, OPT_no_backup, "-no-backup", CMD_FLAG,
			CMD_OPTIONAL,
			"do not generate db backup");
    cmd_AddParmAtOffset(as, OPT_dist, "-dist", CMD_SINGLE, CMD_OPTIONAL,
			"try | skip | required");
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
    if (can_quiet(as)) {
	cmd_AddParmAtOffset(as, OPT_quiet, "-quiet", CMD_FLAG, CMD_OPTIONAL,
			    "talk less");
	cmd_AddParmAtOffset(as, OPT_progress, "-progress", CMD_FLAG,
			    CMD_OPTIONAL,
			    "Enable progress reporting");
	cmd_AddParmAtOffset(as, OPT_no_progress, "-no-progress", CMD_FLAG,
			    CMD_OPTIONAL,
			    "Disable progress reporting");
    }
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

    ts = cmd_CreateSyntax(do_snprintf(&name, "%sdb-restore", prefix),
			  UbikRestoreCmd, usuite, 0,
			  do_snprintf(&descr, "restore %s", db));
    install_opts(ts);
    common_opts(ts);

    ts = cmd_CreateSyntax(do_snprintf(&name, "%sdb-install", prefix),
			  UbikInstallCmd, usuite, 0,
			  do_snprintf(&descr, "install %s", db));
    install_opts(ts);
    common_opts(ts);

    ts = cmd_CreateSyntax(do_snprintf(&name, "%sdb-freeze-run", prefix),
			  UbikFreezeRunCmd, usuite, 0,
			  do_snprintf(&descr, "freeze %s during command", db));
    cmd_AddParmAtOffset(ts, OPT_cmd, "-cmd", CMD_LIST, CMD_OPTIONAL,
			"command (and args) to run during freeze");
    cmd_AddParmAtOffset(ts, OPT_rw, "-rw", CMD_FLAG, CMD_OPTIONAL,
			"allow database to be modified during freeze");
    cmd_AddParmAtOffset(ts, OPT_require_sync, "-require-sync", CMD_FLAG,
			CMD_OPTIONAL,
			"fail if using non-sync-site");
    common_opts(ts);

    ts = cmd_CreateSyntax(do_snprintf(&name, "%sdb-freeze-dist", prefix),
			  UbikFreezeDistCmd, usuite, 0,
			  do_snprintf(&descr,
				      "distribute installed %s during a freeze",
				      db));
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
