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

static char *ctl_path;

static void
run_dbinfo(struct ubiktest_cbinfo *cbinfo, struct ubiktest_ops *ops)
{
    struct afstest_cmdinfo cmdinfo;

    memset(&cmdinfo, 0, sizeof(cmdinfo));

    if (cbinfo->ctl_sock == NULL) {
	skip_block(2, "ctl socket not available");
	return;
    }

    cmdinfo.output = "vldb database info:\n"
		     "  type: kv\n"
		     "  engine: lmdb (LMDB version)\n"
		     "  version: 15948756560000000.8\n"
		     "  size: 12\n";
    cmdinfo.fd = STDOUT_FILENO;
    cmdinfo.command = afstest_asprintf("%s vldb-info -ctl-socket %s"
				       " | perl -pe 's/[(]LMDB .*[)]$/(LMDB version)/'",
				       ctl_path, cbinfo->ctl_sock);

    is_command(&cmdinfo, "openafs-ctl vldb-info output matches (text)");

    free(cmdinfo.command);

    memset(&cmdinfo, 0, sizeof(cmdinfo));

    cmdinfo.output = "{\"type\":\"kv\","
		      "\"engine\":{"
		       "\"name\":\"lmdb\","
		       "\"desc\":\"LMDB version\""
		      "},"
		      "\"size\":12,"
		      "\"version\":{"
		       "\"epoch64\":15948756560000000,"
		       "\"counter\":8"
		      "}}";
    cmdinfo.fd = STDOUT_FILENO;
    cmdinfo.command = afstest_asprintf("%s vldb-info -ctl-socket %s "
				       "--format json"
				       " | perl -pe 's/\"desc\":\"LMDB [^\"]*\"/"
						      "\"desc\":\"LMDB version\"/'",
				       ctl_path, cbinfo->ctl_sock);

    is_command(&cmdinfo, "openafs-ctl vldb-info output matches (json)");

    free(cmdinfo.command);
}

static void
add_cruft(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    char *dbdotd = NULL;
    char *cruft = NULL;

    dbdotd = afstest_asprintf("%s/vldb.DB.d", info->confdir);
    opr_Verify(mkdir(dbdotd, 0700) == 0 || errno == EEXIST);

    cruft = afstest_asprintf("%s/vldb.cruft.DB0", dbdotd);
    opr_Verify(mkdir(cruft, 0700) == 0);

    free(dbdotd);
    free(cruft);
}

static void
check_cruft(struct ubiktest_cbinfo *info, struct ubiktest_ops *ops)
{
    char *dbdotd = NULL;
    char *cruft = NULL;
    struct stat st;
    int save_errno;
    int code;

    memset(&st, 0, sizeof(st));

    dbdotd = afstest_asprintf("%s/vldb.DB.d", info->confdir);
    cruft = afstest_asprintf("%s/vldb.cruft.DB0", dbdotd);

    code = lstat(dbdotd, &st);
    is_int(0, code, "lstat(%s) succeeds (errno %d)", dbdotd, errno);
    ok(S_ISDIR(st.st_mode),
       "%s is a dir (mode 0x%x", dbdotd, (unsigned)st.st_mode);

    code = lstat(cruft, &st);
    save_errno = errno;
    is_int(-1, code, "lstat(%s) fails", cruft);
    is_int(save_errno, ENOENT, "lstat(%s) fails with ENOENT", cruft);

    free(dbdotd);
    free(cruft);
}

static char *kv_argv[] = { "-default-db", "vldb4-kv", NULL };

static struct ubiktest_ops scenarios[] = {
    {
	.descr = "created vldb4-kv",
	.create_db = 1,
	.result_kv = 1,
	.server_argv = kv_argv,
    },
    {
	.descr = "existing vldb4-kv",
	.use_db = "vldb4-kv",
	.post_start = run_dbinfo,
	.n_tests = 2,
	.result_kv = 1,
    },
    {
	.descr = "existing vldb4-kv with extra cruft",
	.use_db = "vldb4-kv",
	.pre_start = add_cruft,
	.post_start = check_cruft,
	.n_tests = 4,
	.result_kv = 1,
    },
    {0}
};

int
main(int argc, char **argv)
{
    vltest_init(argv);

    plan(78);

    ctl_path = afstest_obj_path("src/ctl/openafs-ctl");

    ubiktest_runtest_list(&vlsmall, scenarios);

    return 0;
}
