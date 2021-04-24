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

#include "prtest.h"

static char *pts;

void
prtest_init(char **argv)
{
    ubiktest_init("afs3-prserver", argv);
    pts = afstest_obj_path("src/ptserver/pts");
}

static void
prtest_dbtest(char *dirname, struct ubiktest_dbtest *test)
{
    char *cmd = NULL;
    char *auth;
    struct afstest_cmdinfo cmdinfo;

    auth = "-noauth";
    if (test->cmd_auth) {
	auth = "-localauth";
    }

    opr_Assert(pts != NULL);
    cmd = afstest_asprintf("%s %s -config %s %s",
			   pts, test->cmd_args, dirname, auth);

    memset(&cmdinfo, 0, sizeof(cmdinfo));
    cmdinfo.command = cmd;
    cmdinfo.exit_code = test->cmd_exitcode;

    if (test->cmd_stdout != 0) {
	cmdinfo.output = test->cmd_stdout;
	cmdinfo.fd = STDOUT_FILENO;

    } else {
	opr_Assert(test->cmd_stderr != NULL);
	cmdinfo.output = test->cmd_stderr;
	cmdinfo.fd = STDERR_FILENO;
    }

    is_command(&cmdinfo, "%s", test->descr);

    free(cmd);
}

static struct ubiktest_dbtest prtiny_tests[] = {
    {
	.descr = "list all entries",
	.cmd_args = "listentries -users -groups",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Name                          ID  Owner Creator\n"
	    "system:administrators       -204   -204    -204 \n"
	    "system:backup               -205   -204    -204 \n"
	    "system:anyuser              -101   -204    -204 \n"
	    "system:authuser             -102   -204    -204 \n"
	    "system:ptsviewers           -203   -204    -204 \n"
	    "anonymous                  32766   -204    -204 \n"
	    "admin                          1   -204    -204 \n"
    },
    {
	.descr = "examine admin",
	.cmd_args = "examine -nameorid admin",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Name: admin, id: 1, owner: system:administrators, creator: system:administrators,\n"
	    "  membership: 1, flags: S----, group quota: unlimited.\n"
    },
    {
	.descr = "examine admin by id",
	.cmd_args = "examine -nameorid 1",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Name: admin, id: 1, owner: system:administrators, creator: system:administrators,\n"
	    "  membership: 1, flags: S----, group quota: unlimited.\n"
    },
    {
	.descr = "list admin groups",
	.cmd_args = "membership -nameorid admin",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Groups admin (id: 1) is a member of:\n"
	    "  system:administrators\n"
    },

    {
	.descr = "examine nonexistent user",
	.cmd_args = "examine -nameorid user",
	.cmd_auth = 1,
	.cmd_exitcode = 1,
	.cmd_stderr =
	    "pts: User or group doesn't exist so couldn't look up id for user\n\r"
    },
    {
	.descr = "examine nonexistent user id",
	.cmd_args = "examine -nameorid 2",
	.cmd_auth = 1,
	.cmd_exitcode = 1,
	.cmd_stderr =
	    "pts: User or group doesn't exist ; unable to find entry for (id: 2)\n\r"
    },
    {
	.descr = "create user",
	.cmd_args = "createuser -name user -id 2",
	.cmd_auth = 1,
	.cmd_stdout = "User user has id 2\n"
    },
    {
	.descr = "examine user",
	.cmd_args = "examine -nameorid user",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Name: user, id: 2, owner: system:administrators, creator: system:administrators,\n"
	    "  membership: 0, flags: S----, group quota: 20.\n"
    },
    {
	.descr = "examine user by id",
	.cmd_args = "examine -nameorid 2",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Name: user, id: 2, owner: system:administrators, creator: system:administrators,\n"
	    "  membership: 0, flags: S----, group quota: 20.\n"
    },
    {
	.descr = "list user groups",
	.cmd_args = "membership -nameorid user",
	.cmd_auth = 1,
	.cmd_stdout = "Groups user (id: 2) is a member of:\n"
    },

    {
	.descr = "examine nonexistent group",
	.cmd_args = "examine -nameorid staff",
	.cmd_auth = 1,
	.cmd_exitcode = 1,
	.cmd_stderr =
	    "pts: User or group doesn't exist so couldn't look up id for staff\n\r"
    },
    {
	.descr = "examine nonexistent group id",
	.cmd_args = "examine -nameorid -210",
	.cmd_auth = 1,
	.cmd_exitcode = 1,
	.cmd_stderr =
	    "pts: User or group doesn't exist ; unable to find entry for (id: -210)\n\r"
    },
    {
	.descr = "create group staff",
	.cmd_args = "creategroup -name staff -id -210",
	.cmd_auth = 1,
	.cmd_stdout = "group staff has id -210\n"
    },
    {
	.descr = "examine staff",
	.cmd_args = "examine -nameorid staff",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Name: staff, id: -210, owner: system:administrators, creator: system:administrators,\n"
	    "  membership: 0, flags: S-M--, group quota: 0.\n"
    },
    {
	.descr = "examine group by id",
	.cmd_args = "examine -nameorid -210",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Name: staff, id: -210, owner: system:administrators, creator: system:administrators,\n"
	    "  membership: 0, flags: S-M--, group quota: 0.\n"
    },
    {
	.descr = "list staff members (empty)",
	.cmd_args = "membership -nameorid staff",
	.cmd_auth = 1,
	.cmd_stdout = "Members of staff (id: -210) are:\n"
    },
    {
	.descr = "add user to staff",
	.cmd_args = "adduser -user user -group staff",
	.cmd_auth = 1,
	.cmd_stdout = ""
    },
    {
	.descr = "list staff members (non-empty)",
	.cmd_args = "membership -nameorid staff",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Members of staff (id: -210) are:\n"
	    "  user\n"
    },
    {
	.descr = "list user groups (again)",
	.cmd_args = "membership -nameorid user",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Groups user (id: 2) is a member of:\n"
	    "  staff\n"
    },

    {
	.descr = "list system:administrators members",
	.cmd_args = "membership system:administrators",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Members of system:administrators (id: -204) are:\n"
	    "  admin\n"
    },
    {
	.descr = "add user to system:administrators",
	.cmd_args = "adduser -user user -group system:administrators",
	.cmd_auth = 1,
	.cmd_stdout = ""
    },
    {
	.descr = "list system:administrators members (with user added)",
	.cmd_args = "membership system:administrators",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Members of system:administrators (id: -204) are:\n"
	    "  admin\n"
	    "  user\n"
    },
    {
	.descr = "remove user from system:administrators",
	.cmd_args = "removeuser -user user -group system:administrators",
	.cmd_auth = 1,
	.cmd_stdout = ""
    },
    {
	.descr = "list system:administrators members (with user removed)",
	.cmd_args = "membership system:administrators",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Members of system:administrators (id: -204) are:\n"
	    "  admin\n"
    },

    {
	.descr = "list all entries (again)",
	.cmd_args = "listentries -users -groups",
	.cmd_auth = 1,
	.cmd_stdout =
	    "Name                          ID  Owner Creator\n"
	    "system:administrators       -204   -204    -204 \n"
	    "system:backup               -205   -204    -204 \n"
	    "system:anyuser              -101   -204    -204 \n"
	    "system:authuser             -102   -204    -204 \n"
	    "system:ptsviewers           -203   -204    -204 \n"
	    "anonymous                  32766   -204    -204 \n"
	    "admin                          1   -204    -204 \n"
	    "user                           2   -204    -204 \n"
	    "staff                       -210   -204    -204 \n"
    },

    {0}
};

static void
prtiny_create(char *dirname)
{
    struct ubiktest_dbtest *cmd;

    static struct ubiktest_dbtest pts_cmds[] = {
	{
	    .descr = "Create user admin",
	    .cmd_args = "createuser -name admin -id 1",
	    .cmd_auth = 1,
	    .cmd_stdout = "User admin has id 1\n",
	},
	{
	    .descr = "Add admin to system:administrators",
	    .cmd_args = "adduser -user admin -group system:administrators",
	    .cmd_auth = 1,
	    .cmd_stdout = "",
	},
	{0}
    };

    for (cmd = pts_cmds; cmd->descr != NULL; cmd++) {
	prtest_dbtest(dirname, cmd);
    }
}

struct ubiktest_dataset prtiny = {
    .descr = "prtiny",
    .server_type = &afstest_server_pt,

    .dbtest_func = prtest_dbtest,
    .tests = prtiny_tests,
    .create_func = prtiny_create,
    .existing_dbs = {
	{
	    .name = "prdb0",
	    .flat_path = "tests/ptserver/db.prtiny/prdb.DB0",
	    .getfile_path = "tests/ptserver/db.prtiny/prdb.getfile",
	},
    },
};
