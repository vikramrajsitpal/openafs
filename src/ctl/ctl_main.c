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
    OPT_format,

    OPT_reason,
    OPT_ctl_socket,
};

enum format_type {
    fmt_text = 0,
    fmt_json,
};

struct ctlmain_ctx {
    char *udb_descr;

    enum format_type format;

    struct afsctl_clientinfo cinfo;
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
    free(ctlm);
}

static int
preamble(struct ubikctl_info *usuite, struct cmd_syndesc *as,
	 struct ctlmain_ctx **a_ctlm)
{
    char *format_str = NULL;
    struct ctlmain_ctx *ctlm;

    ctlm = calloc(1, sizeof(*ctlm));
    if (ctlm == NULL) {
	goto enomem;
    }

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

    *a_ctlm = ctlm;

    return 0;

 enomem:
    print_error(ENOMEM, "Failed alloc");
 error:
    postamble(&ctlm);
    return -1;

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

static void
common_opts(struct cmd_syndesc *as)
{
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
