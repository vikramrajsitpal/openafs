/*
 * Copyright (c) 2021 Sine Nomine Associates
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

#include "common.h"
#include "prtest.h"

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

    cmdinfo.output = "ptdb database info:\n"
		     "  type: flat\n"
		     "  engine: udisk (traditional udisk/uphys storage)\n"
		     "  version: 16168939430000000.2\n"
		     "  size: 66944\n";
    cmdinfo.fd = STDOUT_FILENO;
    cmdinfo.command = afstest_asprintf("%s ptdb-info -ctl-socket %s", ctl_path,
				       cbinfo->ctl_sock);
    is_command(&cmdinfo, "openafs-ctl ptdb-info output matches [text]");

    free(cmdinfo.command);

    memset(&cmdinfo, 0, sizeof(cmdinfo));

    cmdinfo.output = "{\"type\":\"flat\","
		      "\"engine\":{"
		       "\"name\":\"udisk\","
		       "\"desc\":\"traditional udisk/uphys storage\""
		      "},"
		      "\"size\":66944,"
		      "\"version\":{"
		       "\"epoch64\":16168939430000000,"
		       "\"counter\":2"
		     "}}";
    cmdinfo.fd = STDOUT_FILENO;
    cmdinfo.command = afstest_asprintf("%s ptdb-info -ctl-socket %s "
				       "--format json", ctl_path,
				       cbinfo->ctl_sock);
    is_command(&cmdinfo, "openafs-ctl ptdb-info output matches [json]");

    free(cmdinfo.command);
}

static struct ubiktest_ops scenarios[] = {
    {
	.descr = "created prdb",
	.create_db = 1,
    },
    {
	.descr = "existing prdb",
	.use_db = "prdb0",
	.post_start = run_dbinfo,
    },
    {0}
};

int
main(int argc, char **argv)
{
    prtest_init(argv);

    plan(62);

    ctl_path = afstest_obj_path("src/ctl/openafs-ctl");

    ubiktest_runtest_list(&prtiny, scenarios);

    return 0;
}
