/*
 * Copyright (c) 2020 Sine Nomine Associates
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS
 * IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <afsconfig.h>
#include <afs/param.h>

#include "vltest.h"

static void
run_upgrade(char *args)
{
    char *vldb_upgrade;
    char *cmd;
    int code;
    int success;

    vldb_upgrade = afstest_obj_path("src/tvlserver/vldb_upgrade");
    cmd = afstest_asprintf("%s %s", vldb_upgrade, args);

    /* Trim off the full path of 'vldb_upgrade', but still show what command
     * we're running. */
    diag("vldb_upgrade%s", &cmd[strlen(vldb_upgrade)]);

    code = system(cmd);

    success = 0;
    if (WIFEXITED(code)) {
	if (WEXITSTATUS(code) == 0) {
	    success = 1;
	} else {
	    diag("vldb_upgrade exited with code %d", WEXITSTATUS(code));
	}
    } else if (WIFSIGNALED(code)) {
	diag("vldb_upgrade died from signal %d", WTERMSIG(code));
    } else {
	diag("vldb_upgrade died with weird status 0x%x", (unsigned)code);
    }

    ok(success, "vldb_upgrade returns success");

    free(vldb_upgrade);
    free(cmd);
}

static void
upgrade_offline(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    char *format = ops->rock;
    char *args;
    char *orig_path;

    /* Move the given vldb.DB0 to vldb.DB0.orig, and then run vldb_upgrade into
     * vldb.DB0 again. */

    orig_path = afstest_asprintf("%s.orig", info->db_path);

    if (rename(info->db_path, orig_path) != 0) {
	sysbail("rename %s -> %s", info->db_path, orig_path);
    }

    args = afstest_asprintf("-input '%s' -output '%s' -to %s -quiet",
			    orig_path, info->db_path, format);
    run_upgrade(args);

    free(orig_path);
    free(args);
}

static void
upgrade_online(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    char *format = ops->rock;
    char *args;

    args = afstest_asprintf("-to %s -online -no-backup -quiet "
			    "-ctl-socket '%s/vl.ctl.sock'",
			    format, info->confdir);
    run_upgrade(args);

    free(args);
}

static void
upgrade_online_bak(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    char *format = ops->rock;
    char *args;
    char *bak_path;

    args = afstest_asprintf("-to %s -online -backup-suffix .orig -quiet "
			    "-ctl-socket '%s/vl.ctl.sock'",
			    format, info->confdir);
    run_upgrade(args);

    bak_path = afstest_asprintf("%s.orig", info->db_path);
    ok(ubiktest_db_equal(info->src_dbpath, bak_path), "backup matches");

    free(args);
    free(bak_path);
}

#ifdef AFS_CTL_ENV
# define SKIP_NOCTL NULL
#else
# define SKIP_NOCTL "Built without afsctl support"
#endif

static struct ubiktest_ops scenarios[] = {
    {
	.descr = "offline vldb4 -> vldb4-kv",
	.use_db = "vldb4",
	.result_kv = 1,
	.pre_start = upgrade_offline,
	.rock = "vldb4-kv",
    },
    {
	.descr = "online vldb4 -> vldb4-kv",
	.skip_reason = SKIP_NOCTL,
	.use_db = "vldb4",
	.result_kv = 1,
	.post_start = upgrade_online,
	.rock = "vldb4-kv",
	.n_tests = 1,
    },
    {
	.descr = "online vldb4 -> vldb4-kv with backup",
	.skip_reason = SKIP_NOCTL,
	.use_db = "vldb4",
	.result_kv = 2,
	.post_start = upgrade_online_bak,
	.rock = "vldb4-kv",
	.n_tests = 2,
    },
    {
	.descr = "offline vldb4-kv -> vldb4",
	.use_db = "vldb4-kv",
	.pre_start = upgrade_offline,
	.rock = "vldb4",
	.n_tests = 1,
    },
    {
	.descr = "online vldb4-kv -> vldb4",
	.skip_reason = SKIP_NOCTL,
	.use_db = "vldb4-kv",
	.post_start = upgrade_online,
	.rock = "vldb4",
	.n_tests = 1,
    },
    {0}
};

int
main(int argc, char **argv)
{
    vltest_init(argv);

    plan(116);

    ubiktest_runtest_list(&vlsmall, scenarios);

    return 0;
}
